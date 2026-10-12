// SPDX-License-Identifier: GPL-2.0-or-later
#include "d3d12_videocore_bridge.h"

#include <winrt/base.h>

#include <cstring>
#include <limits>
#include <utility>

namespace {
constexpr UINT64 MaxTransferBytes = 16ULL * 1024ULL * 1024ULL;
constexpr std::size_t MaxBindings = 16;
} // namespace

D3D12VideoCoreBridge::D3D12VideoCoreBridge(D3D12DeviceContext &context,
                                           D3D12ResourceAllocator &allocator,
                                           D3D12TransferStats &transfers)
    : context_(context), allocator_(allocator), transfers_(transfers) {}

D3D12VideoCoreBridge::~D3D12VideoCoreBridge() {
  try {
    // A discarded recording must not execute on scope exit. Only drain work
    // that Flush already submitted before releasing its staging/references.
    context_.Wait(last_ticket_);
  } catch (...) {
    // Device-loss teardown is owned by the host; destructors cannot throw.
  }
}

[[noreturn]] void D3D12VideoCoreBridge::Reject() {
  ++stats_.rejected_requests;
  winrt::throw_hresult(E_INVALIDARG);
}

[[noreturn]] void D3D12VideoCoreBridge::Unsupported() {
  ++stats_.unsupported_requests;
  winrt::throw_hresult(E_NOTIMPL);
}

void D3D12VideoCoreBridge::RegisterBuffer(VAddr base, ID3D12Resource *resource) {
  if (!resource || base == 0 || bindings_.size() >= MaxBindings || encoder_ ||
      (last_ticket_ != 0 && context_.CompletedValue() < last_ticket_)) {
    Reject();
  }
  const auto desc = resource->GetDesc();
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || desc.Width == 0 ||
      desc.Width > std::numeric_limits<VAddr>::max() - base) {
    Reject();
  }
  D3D12_HEAP_PROPERTIES heap{};
  winrt::check_hresult(resource->GetHeapProperties(&heap, nullptr));
  Microsoft::WRL::ComPtr<ID3D12Device> device;
  winrt::check_hresult(resource->GetDevice(IID_PPV_ARGS(device.ReleaseAndGetAddressOf())));
  if (heap.Type != D3D12_HEAP_TYPE_DEFAULT || device.Get() != context_.Device()) {
    Reject();
  }
  const VAddr end = base + static_cast<VAddr>(desc.Width);
  for (const auto &binding : bindings_) {
    const VAddr other_end = binding.base + static_cast<VAddr>(binding.size);
    if (binding.resource.Get() == resource || (base < other_end && binding.base < end)) {
      Reject();
    }
  }
  Binding binding;
  binding.base = base;
  binding.size = desc.Width;
  binding.resource = resource;
  bindings_.push_back(std::move(binding));
}

D3D12VideoCoreBridge::Range D3D12VideoCoreBridge::Find(VAddr address, UINT64 bytes) {
  if (bytes == 0 || bytes > MaxTransferBytes) {
    Reject();
  }
  for (const auto &binding : bindings_) {
    if (address >= binding.base) {
      const UINT64 offset = address - binding.base;
      if (offset < binding.size && bytes <= binding.size - offset) {
        return {binding.resource.Get(), offset};
      }
    }
  }
  Reject();
}

void D3D12VideoCoreBridge::BeginRecording() {
  if (encoder_) {
    return;
  }
  // Conservative single-batch ownership: never recycle an allocator/staging
  // before its previous real GPU ticket completes.
  context_.Wait(last_ticket_);
  staging_.clear();
  pending_images_.clear();
  staged_bytes_ = 0;
  if (!commands_) {
    context_.CreateDirectCommands(commands_allocator_, commands_);
  } else {
    winrt::check_hresult(commands_allocator_->Reset());
    winrt::check_hresult(commands_->Reset(commands_allocator_.Get(), nullptr));
  }
  encoder_ = std::make_unique<D3D12CommandEncoder>(commands_.Get(), transfers_);
  for (const auto &binding : bindings_) {
    // DEFAULT buffers decay to COMMON at ExecuteCommandLists boundaries.
    encoder_->Track(binding.resource.Get(), D3D12_RESOURCE_STATE_COMMON);
  }
}

void D3D12VideoCoreBridge::FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds) {
  if (is_gds) {
    Unsupported();
  }
  const auto destination = Find(address, num_bytes);
  if (num_bytes % sizeof(u32) != 0 || destination.offset % sizeof(u32) != 0) {
    Reject();
  }
  BeginRecording();
  if (num_bytes > MaxTransferBytes - staged_bytes_) {
    Reject();
  }
  D3D12Resource upload;
  winrt::check_hresult(allocator_.CreateBuffer(num_bytes, D3D12_HEAP_TYPE_UPLOAD,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, upload));
  void *mapped{};
  const D3D12_RANGE no_read{0, 0};
  winrt::check_hresult(upload->Map(0, &no_read, &mapped));
  for (UINT offset = 0; offset < num_bytes; offset += static_cast<UINT>(sizeof(u32))) {
    std::memcpy(static_cast<std::uint8_t *>(mapped) + offset, &value, sizeof(value));
  }
  const D3D12_RANGE written{0, num_bytes};
  upload->Unmap(0, &written);
  // Retain the owner before issuing any command that can reference it.
  staging_.push_back(std::move(upload));
  encoder_->Track(staging_.back().Get(), D3D12_RESOURCE_STATE_GENERIC_READ);
  encoder_->CopyBuffer(destination.resource, destination.offset, staging_.back().Get(), 0, num_bytes);
  staged_bytes_ += num_bytes;
  ++stats_.fills;
}

void D3D12VideoCoreBridge::CopyBuffer(VAddr dst, VAddr src, u32 num_bytes,
                                     bool dst_gds, bool src_gds) {
  if (dst_gds || src_gds) {
    Unsupported();
  }
  const auto destination = Find(dst, num_bytes);
  const auto source = Find(src, num_bytes);
  if (destination.resource == source.resource) {
    Reject();
  }
  BeginRecording();
  encoder_->CopyBuffer(destination.resource, destination.offset,
                        source.resource, source.offset, num_bytes);
  ++stats_.copies;
}

u64 D3D12VideoCoreBridge::Flush() {
  if (!encoder_) {
    return last_ticket_;
  }
  if (marker_depth_ != 0) {
    Reject();
  }
  winrt::check_hresult(commands_->Close());
  last_ticket_ = context_.Submit(commands_.Get());
  encoder_.reset();
  ++stats_.flushes;
  return last_ticket_;
}

void D3D12VideoCoreBridge::Finish() {
  const u64 ticket = Flush();
  context_.Wait(ticket);
  staging_.clear();
  pending_images_.clear();
  staged_bytes_ = 0;
  ++stats_.synchronizations;
}

void D3D12VideoCoreBridge::CpSync() { Finish(); }

void D3D12VideoCoreBridge::OnSubmit() {
  ++stats_.host_submit_events;
  if (!encoder_ && last_ticket_ != 0 && context_.CompletedValue() >= last_ticket_) {
    staging_.clear();
    pending_images_.clear();
    staged_bytes_ = 0;
  }
}

void D3D12VideoCoreBridge::DownloadBuffer(VAddr address, std::span<std::uint8_t> output) {
  const auto source = Find(address, output.size());
  Finish();
  D3D12Resource readback;
  winrt::check_hresult(allocator_.CreateBuffer(output.size(), D3D12_HEAP_TYPE_READBACK,
                                               D3D12_RESOURCE_STATE_COPY_DEST, readback));
  BeginRecording();
  encoder_->Track(readback.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
  encoder_->CopyBuffer(readback.Get(), 0, source.resource, source.offset, output.size());
  Finish();
  void *mapped{};
  const D3D12_RANGE range{0, output.size()};
  winrt::check_hresult(readback->Map(0, &range, &mapped));
  std::memcpy(output.data(), mapped, output.size());
  const D3D12_RANGE no_write{0, 0};
  readback->Unmap(0, &no_write);
  ++stats_.downloads;
}

void D3D12VideoCoreBridge::CreateLinearFrame(VAddr address, UINT width, UINT height,
                                            UINT pitch, D3D12Resource &output) {
  if (width == 0 || height == 0 || width > 4096 || height > 4096 || output.Get()) {
    Reject();
  }
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  const auto source = Find(address, static_cast<UINT64>(pitch) * height);
  if (source.offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT != 0) {
    Reject();
  }
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  context_.Device()->GetCopyableFootprints(&desc, 0, 1, source.offset, &footprint,
                                           nullptr, nullptr, nullptr);
  if (footprint.Offset != source.offset || footprint.Footprint.RowPitch != pitch ||
      pitch < width * 4U) {
    Reject();
  }
  D3D12Resource frame;
  winrt::check_hresult(allocator_.CreateTexture2D(desc, D3D12_RESOURCE_STATE_COPY_DEST, frame));
  BeginRecording();
  pending_images_.emplace_back(frame.Get());
  encoder_->Track(frame.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
  encoder_->CopyBufferToTexture(frame.Get(), source.resource, footprint);
  encoder_->Transition(frame.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  output = std::move(frame);
  ++stats_.linear_frames;
}

std::string D3D12VideoCoreBridge::Marker(std::string_view label, bool from_guest) {
  if (label.empty() || label.size() > 1024) {
    Reject();
  }
  return (from_guest ? "guest:" : "host:") + std::string(label);
}

void D3D12VideoCoreBridge::ScopeMarkerBegin(std::string_view label, bool from_guest) {
  const auto text = Marker(label, from_guest);
  BeginRecording();
  // Direct BeginEvent/SetMarker payloads are reserved for tooling. Keep
  // diagnostic host metadata; GPU PIX annotations require a separate adapter.
  OutputDebugStringA(text.c_str());
  ++marker_depth_;
  ++stats_.markers;
}

void D3D12VideoCoreBridge::ScopeMarkerEnd(bool) {
  if (!encoder_ || marker_depth_ == 0) {
    Reject();
  }
  --marker_depth_;
}

void D3D12VideoCoreBridge::ScopedMarkerInsertColor(std::string_view label, u32 color,
                                                  bool from_guest) {
  // Color is diagnostic metadata, not a GPU/PIX color annotation.
  const auto text = Marker(label, from_guest) + ";color=" + std::to_string(color);
  BeginRecording();
  OutputDebugStringA(text.c_str());
  ++stats_.markers;
}

void D3D12VideoCoreBridge::Draw(bool, u32) { Unsupported(); }
void D3D12VideoCoreBridge::DrawIndirect(bool, VAddr, u32, u32, u32, VAddr) { Unsupported(); }
void D3D12VideoCoreBridge::DispatchDirect() { Unsupported(); }
void D3D12VideoCoreBridge::DispatchIndirect(VAddr, u32, u32) { Unsupported(); }
u32 D3D12VideoCoreBridge::ReadDataFromGds(u32) { Unsupported(); }
void D3D12VideoCoreBridge::ProcessDownloadImages() { Unsupported(); }
