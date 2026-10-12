// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <memory>

struct D3D12ResourceStats {
  UINT64 budget_bytes{};
  UINT64 live_bytes{};
  UINT64 peak_bytes{};
  UINT64 default_bytes{};
  UINT64 upload_bytes{};
  UINT64 readback_bytes{};
  UINT64 live_resources{};
  UINT64 created_resources{};
  UINT64 failed_allocations{};
  HRESULT last_error{S_OK};
};

struct D3D12ResourceAccounting;

// Move-only ownership. Get() is borrowed; the caller must keep this owner alive
// until all commands referencing the resource complete on the GPU.
class D3D12Resource final {
public:
  D3D12Resource() = default;
  ~D3D12Resource();
  D3D12Resource(const D3D12Resource &) = delete;
  D3D12Resource &operator=(const D3D12Resource &) = delete;
  D3D12Resource(D3D12Resource &&other) noexcept;
  D3D12Resource &operator=(D3D12Resource &&other) noexcept;

  [[nodiscard]] ID3D12Resource *Get() const noexcept { return resource_.Get(); }
  [[nodiscard]] ID3D12Resource *operator->() const noexcept { return Get(); }
  [[nodiscard]] UINT64 AllocationBytes() const noexcept { return allocation_bytes_; }
  void Reset() noexcept;

private:
  friend class D3D12ResourceAllocator;
  std::shared_ptr<D3D12ResourceAccounting> accounting_;
  Microsoft::WRL::ComPtr<ID3D12Resource> resource_;
  UINT64 allocation_bytes_{};
  D3D12_HEAP_TYPE heap_type_{D3D12_HEAP_TYPE_DEFAULT};
};

// Committed resources with a host-defined cap. Accounting uses allocation info,
// not logical image bytes, and is not a measurement of physical GPU residency.
// Calls and resource destruction are serialized by the host's UI thread.
class D3D12ResourceAllocator final {
public:
  void Initialize(ID3D12Device *device, UINT64 budget_bytes);
  [[nodiscard]] HRESULT CreateBuffer(UINT64 size, D3D12_HEAP_TYPE heap_type,
                                     D3D12_RESOURCE_STATES initial_state,
                                     D3D12Resource &output) noexcept;
  [[nodiscard]] HRESULT CreateTexture2D(const D3D12_RESOURCE_DESC &description,
                                        D3D12_RESOURCE_STATES initial_state,
                                        D3D12Resource &output) noexcept;
  [[nodiscard]] D3D12ResourceStats Stats() const noexcept;

private:
  [[nodiscard]] HRESULT Create(const D3D12_RESOURCE_DESC &description,
                               D3D12_HEAP_TYPE heap_type,
                               D3D12_RESOURCE_STATES initial_state,
                               D3D12Resource &output) noexcept;
  [[nodiscard]] HRESULT Reject(HRESULT error) noexcept;
  std::shared_ptr<D3D12ResourceAccounting> accounting_;
};
