// SPDX-License-Identifier: GPL-2.0-or-later

#include "d3d12_resource_allocator.h"

#include <winrt/base.h>

#include <algorithm>
#include <limits>
#include <utility>

struct D3D12ResourceAccounting {
  Microsoft::WRL::ComPtr<ID3D12Device> device;
  D3D12ResourceStats stats;

  UINT64 &HeapBytes(D3D12_HEAP_TYPE heap_type) noexcept {
    switch (heap_type) {
    case D3D12_HEAP_TYPE_UPLOAD:
      return stats.upload_bytes;
    case D3D12_HEAP_TYPE_READBACK:
      return stats.readback_bytes;
    default:
      return stats.default_bytes;
    }
  }
};

D3D12Resource::~D3D12Resource() { Reset(); }

D3D12Resource::D3D12Resource(D3D12Resource &&other) noexcept {
  *this = std::move(other);
}

D3D12Resource &D3D12Resource::operator=(D3D12Resource &&other) noexcept {
  if (this != &other) {
    Reset();
    accounting_ = std::move(other.accounting_);
    resource_ = std::move(other.resource_);
    allocation_bytes_ = std::exchange(other.allocation_bytes_, 0U);
    heap_type_ = other.heap_type_;
  }
  return *this;
}

void D3D12Resource::Reset() noexcept {
  resource_.Reset();
  if (accounting_) {
    accounting_->stats.live_bytes -= allocation_bytes_;
    accounting_->HeapBytes(heap_type_) -= allocation_bytes_;
    --accounting_->stats.live_resources;
    accounting_.reset();
  }
  allocation_bytes_ = 0U;
}

void D3D12ResourceAllocator::Initialize(ID3D12Device *device, UINT64 budget_bytes) {
  if (device == nullptr || budget_bytes == 0U) {
    winrt::throw_hresult(E_INVALIDARG);
  }
  if (accounting_) {
    winrt::throw_hresult(E_UNEXPECTED);
  }
  accounting_ = std::make_shared<D3D12ResourceAccounting>();
  accounting_->device = device;
  accounting_->stats.budget_bytes = budget_bytes;
}

HRESULT D3D12ResourceAllocator::Reject(HRESULT error) noexcept {
  if (accounting_) {
    ++accounting_->stats.failed_allocations;
    accounting_->stats.last_error = error;
  }
  return error;
}

HRESULT D3D12ResourceAllocator::CreateBuffer(
    UINT64 size, D3D12_HEAP_TYPE heap_type,
    D3D12_RESOURCE_STATES initial_state, D3D12Resource &output) noexcept {
  if (size == 0U ||
      (heap_type != D3D12_HEAP_TYPE_DEFAULT && heap_type != D3D12_HEAP_TYPE_UPLOAD &&
       heap_type != D3D12_HEAP_TYPE_READBACK) ||
      (heap_type == D3D12_HEAP_TYPE_UPLOAD &&
       initial_state != D3D12_RESOURCE_STATE_GENERIC_READ) ||
      (heap_type == D3D12_HEAP_TYPE_READBACK &&
       initial_state != D3D12_RESOURCE_STATE_COPY_DEST)) {
    return Reject(E_INVALIDARG);
  }
  D3D12_RESOURCE_DESC description{};
  description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  description.Width = size;
  description.Height = 1;
  description.DepthOrArraySize = 1;
  description.MipLevels = 1;
  description.SampleDesc.Count = 1;
  description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  return Create(description, heap_type, initial_state, output);
}

HRESULT D3D12ResourceAllocator::CreateTexture2D(
    const D3D12_RESOURCE_DESC &description,
    D3D12_RESOURCE_STATES initial_state, D3D12Resource &output) noexcept {
  if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
      description.Width == 0U || description.Height == 0U ||
      description.DepthOrArraySize == 0U || description.MipLevels == 0U ||
      description.Format == DXGI_FORMAT_UNKNOWN ||
      description.SampleDesc.Count == 0U ||
      description.Layout != D3D12_TEXTURE_LAYOUT_UNKNOWN) {
    return Reject(E_INVALIDARG);
  }
  return Create(description, D3D12_HEAP_TYPE_DEFAULT, initial_state, output);
}

HRESULT D3D12ResourceAllocator::Create(
    const D3D12_RESOURCE_DESC &description, D3D12_HEAP_TYPE heap_type,
    D3D12_RESOURCE_STATES initial_state, D3D12Resource &output) noexcept {
  if (!accounting_) {
    return E_UNEXPECTED;
  }
  const auto allocation =
      accounting_->device->GetResourceAllocationInfo(0, 1, &description);
  if (allocation.SizeInBytes == 0U ||
      allocation.SizeInBytes == std::numeric_limits<UINT64>::max()) {
    return Reject(E_INVALIDARG);
  }
  if (allocation.SizeInBytes >
      accounting_->stats.budget_bytes - accounting_->stats.live_bytes) {
    return Reject(E_OUTOFMEMORY);
  }
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = heap_type;
  D3D12Resource created;
  const HRESULT result = accounting_->device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &description, initial_state, nullptr,
      IID_PPV_ARGS(created.resource_.ReleaseAndGetAddressOf()));
  if (FAILED(result)) {
    return Reject(result);
  }
  created.accounting_ = accounting_;
  created.allocation_bytes_ = allocation.SizeInBytes;
  created.heap_type_ = heap_type;
  auto &stats = accounting_->stats;
  stats.live_bytes += allocation.SizeInBytes;
  accounting_->HeapBytes(heap_type) += allocation.SizeInBytes;
  ++stats.live_resources;
  ++stats.created_resources;
  stats.peak_bytes = std::max(stats.peak_bytes, stats.live_bytes);
  // Preserve the previous owner if creation failed; replacement requires the
  // caller to have retired every GPU use of that previous resource.
  output = std::move(created);
  return S_OK;
}

D3D12ResourceStats D3D12ResourceAllocator::Stats() const noexcept {
  return accounting_ ? accounting_->stats : D3D12ResourceStats{};
}
