// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <d3d12.h>
#include <wrl/client.h>

// Owns the direct queue and its synchronization primitives. This is the UWP
// shell's D3D12 foundation, not yet a renderer for PS4 command streams.
class D3D12DeviceContext final {
public:
  D3D12DeviceContext() = default;
  ~D3D12DeviceContext();

  D3D12DeviceContext(const D3D12DeviceContext &) = delete;
  D3D12DeviceContext &operator=(const D3D12DeviceContext &) = delete;

  // Passing an adapter is used by the native D3D12/WARP CI test.
  void Initialize(IUnknown *adapter = nullptr);
  [[nodiscard]] ID3D12Device *Device() const noexcept { return device_.Get(); }
  [[nodiscard]] ID3D12CommandQueue *DirectQueue() const noexcept {
    return direct_queue_.Get();
  }
  void CreateDirectCommands(
      Microsoft::WRL::ComPtr<ID3D12CommandAllocator> &allocator,
      Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> &list) const;
  // The caller owns the list/allocator/resources until this ticket completes.
  // Calls are serialized by the host's UI thread; this is not a multi-queue API.
  [[nodiscard]] UINT64 Submit(ID3D12CommandList *list);
  void Wait(UINT64 ticket);
  void WaitForGpu();
  [[nodiscard]] UINT64 CompletedValue() const;
  [[nodiscard]] UINT64 LastSignaledValue() const noexcept { return fence_value_; }
  [[nodiscard]] UINT64 SubmittedLists() const noexcept { return submitted_lists_; }
  [[nodiscard]] UINT64 BlockingWaits() const noexcept { return blocking_waits_; }
  [[nodiscard]] bool TryTrim();

private:
  [[nodiscard]] UINT64 Signal();

  Microsoft::WRL::ComPtr<ID3D12Device> device_;
  Microsoft::WRL::ComPtr<ID3D12CommandQueue> direct_queue_;
  Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
  HANDLE fence_event_{INVALID_HANDLE_VALUE};
  UINT64 fence_value_{};
  UINT64 submitted_lists_{};
  UINT64 blocking_waits_{};
  HRESULT submission_error_{S_OK};
};
