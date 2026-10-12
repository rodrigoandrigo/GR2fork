// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "d3d12_command_encoder.h"
#include "d3d12_device_context.h"
#include "d3d12_resource_allocator.h"
#include "video_core/gpu_command_sink.h"

#include <memory>
#include <span>
#include <string>
#include <vector>

struct D3D12VideoCoreStats {
  UINT64 fills{};
  UINT64 copies{};
  UINT64 flushes{};
  UINT64 synchronizations{};
  UINT64 host_submit_events{};
  UINT64 markers{};
  UINT64 downloads{};
  UINT64 linear_frames{};
  UINT64 rejected_requests{};
  UINT64 unsupported_requests{};
};

// Initial consumer of the actual VideoCore interface. Registered addresses are
// identifiers, NEVER dereferenced host pointers. No guest MMU/cache coherence,
// Liverpool binding, guest shaders, GDS or image-download service is implied.
// Calls are serialized; registration is immutable for this object's lifetime.
class D3D12VideoCoreBridge final : public VideoCore::GpuCommandSink {
public:
  D3D12VideoCoreBridge(D3D12DeviceContext &context,
                       D3D12ResourceAllocator &allocator,
                       D3D12TransferStats &transfers);
  ~D3D12VideoCoreBridge() override;
  D3D12VideoCoreBridge(const D3D12VideoCoreBridge &) = delete;
  D3D12VideoCoreBridge &operator=(const D3D12VideoCoreBridge &) = delete;
  // DEFAULT buffers on the same device, known initial state COMMON. No alias,
  // overlap, replacement or mutation while recording/in flight.
  void RegisterBuffer(VAddr base, ID3D12Resource *resource);
  void DownloadBuffer(VAddr address, std::span<std::uint8_t> output);
  // Linear BGRA8 only; pitch and offset must match the D3D12 footprint.
  // Caller owns the output through its fence and subsequent presentation.
  void CreateLinearFrame(VAddr address, UINT width, UINT height, UINT pitch,
                          D3D12Resource &output);
  [[nodiscard]] D3D12VideoCoreStats Stats() const noexcept { return stats_; }

  void Draw(bool is_indexed, u32 index_offset = 0) override;
  void DrawIndirect(bool is_indexed, VAddr arg_address, u32 offset, u32 stride,
                    u32 max_count, VAddr count_address) override;
  void DispatchDirect() override;
  void DispatchIndirect(VAddr address, u32 offset, u32 size) override;
  void FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds) override;
  void CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds) override;
  u32 ReadDataFromGds(u32 gds_offset) override;
  void ProcessDownloadImages() override;
  void CpSync() override;
  u64 Flush() override;
  void Finish() override;
  void OnSubmit() override;
  void ScopeMarkerBegin(std::string_view label, bool from_guest = false) override;
  void ScopeMarkerEnd(bool from_guest = false) override;
  void ScopedMarkerInsertColor(std::string_view label, u32 color,
                               bool from_guest = false) override;

private:
  struct Binding {
    VAddr base{};
    UINT64 size{};
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
  };
  struct Range {
    ID3D12Resource *resource{};
    UINT64 offset{};
  };
  [[noreturn]] void Reject();
  [[noreturn]] void Unsupported();
  Range Find(VAddr address, UINT64 bytes);
  void BeginRecording();
  std::string Marker(std::string_view label, bool from_guest);

  D3D12DeviceContext &context_;
  D3D12ResourceAllocator &allocator_;
  D3D12TransferStats &transfers_;
  D3D12VideoCoreStats stats_;
  std::vector<Binding> bindings_;
  Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commands_allocator_;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commands_;
  std::unique_ptr<D3D12CommandEncoder> encoder_;
  std::vector<D3D12Resource> staging_;
  std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> pending_images_;
  UINT64 last_ticket_{};
  UINT64 staged_bytes_{};
  UINT marker_depth_{};
};
