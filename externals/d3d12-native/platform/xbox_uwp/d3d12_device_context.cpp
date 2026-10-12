// SPDX-License-Identifier: GPL-2.0-or-later

#include "d3d12_device_context.h"

#include <dxgi1_4.h>
#include <winrt/base.h>

#include <limits>

using Microsoft::WRL::ComPtr;

D3D12DeviceContext::~D3D12DeviceContext() {
  if (fence_event_ != INVALID_HANDLE_VALUE) {
    CloseHandle(fence_event_);
  }
}

void D3D12DeviceContext::Initialize(IUnknown *adapter) {
  if (device_) {
    winrt::throw_hresult(E_UNEXPECTED);
  }
  winrt::check_hresult(D3D12CreateDevice(
      adapter, D3D_FEATURE_LEVEL_11_0,
      IID_PPV_ARGS(device_.ReleaseAndGetAddressOf())));

  D3D12_COMMAND_QUEUE_DESC queue_description{};
  queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  winrt::check_hresult(device_->CreateCommandQueue(
      &queue_description,
      IID_PPV_ARGS(direct_queue_.ReleaseAndGetAddressOf())));

  winrt::check_hresult(device_->CreateFence(
      0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence_.ReleaseAndGetAddressOf())));
  fence_event_ = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
  if (fence_event_ == nullptr) {
    fence_event_ = INVALID_HANDLE_VALUE;
    winrt::throw_last_error();
  }
}

void D3D12DeviceContext::CreateDirectCommands(
    ComPtr<ID3D12CommandAllocator> &allocator,
    ComPtr<ID3D12GraphicsCommandList> &list) const {
  winrt::check_hresult(device_->CreateCommandAllocator(
      D3D12_COMMAND_LIST_TYPE_DIRECT,
      IID_PPV_ARGS(allocator.ReleaseAndGetAddressOf())));
  winrt::check_hresult(device_->CreateCommandList(
      0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
      IID_PPV_ARGS(list.ReleaseAndGetAddressOf())));
}

UINT64 D3D12DeviceContext::Signal() {
  winrt::check_hresult(submission_error_);
  // UINT64_MAX is reserved by D3D12 to report device removal.
  if (fence_value_ >= std::numeric_limits<UINT64>::max() - 1U) {
    winrt::throw_hresult(E_UNEXPECTED);
  }
  const UINT64 value = fence_value_ + 1U;
  submission_error_ = direct_queue_->Signal(fence_.Get(), value);
  // A failed signal leaves already-executed lists without a completion ticket.
  // Do not allow later calls to recycle their allocators using an older ticket.
  winrt::check_hresult(submission_error_);
  fence_value_ = value;
  return value;
}

UINT64 D3D12DeviceContext::Submit(ID3D12CommandList *list) {
  winrt::check_hresult(submission_error_);
  if (list == nullptr) {
    winrt::throw_hresult(E_INVALIDARG);
  }
  ID3D12CommandList *lists[]{list};
  direct_queue_->ExecuteCommandLists(1, lists);
  ++submitted_lists_;
  return Signal();
}

UINT64 D3D12DeviceContext::CompletedValue() const {
  winrt::check_hresult(submission_error_);
  const UINT64 completed = fence_->GetCompletedValue();
  if (completed == std::numeric_limits<UINT64>::max()) {
    winrt::check_hresult(device_->GetDeviceRemovedReason());
    winrt::throw_hresult(DXGI_ERROR_DEVICE_REMOVED);
  }
  return completed;
}

void D3D12DeviceContext::Wait(UINT64 ticket) {
  if (ticket > fence_value_) {
    winrt::throw_hresult(E_INVALIDARG);
  }
  if (CompletedValue() >= ticket) {
    return;
  }

  winrt::check_hresult(fence_->SetEventOnCompletion(ticket, fence_event_));
  ++blocking_waits_;
  const DWORD wait_result = WaitForSingleObjectEx(fence_event_, INFINITE, FALSE);
  if (wait_result == WAIT_FAILED) {
    winrt::throw_last_error();
  }
  if (wait_result != WAIT_OBJECT_0) {
    winrt::throw_hresult(E_UNEXPECTED);
  }
  if (CompletedValue() < ticket) {
    winrt::throw_hresult(E_UNEXPECTED);
  }
}

void D3D12DeviceContext::WaitForGpu() {
  Wait(Signal());
}

bool D3D12DeviceContext::TryTrim() {
  ComPtr<IDXGIDevice3> dxgi_device;
  const HRESULT result = device_.As(&dxgi_device);
  if (result == E_NOINTERFACE) {
    return false;
  }
  winrt::check_hresult(result);
  dxgi_device->Trim();
  return true;
}
