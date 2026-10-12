// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

// Fixed slots: callers must drain GPU use before rewriting a descriptor.
// This is not a fence-retired allocator and does not recycle slots.
class D3D12DescriptorArena final {
public:
  D3D12DescriptorArena() = default;
  D3D12DescriptorArena(const D3D12DescriptorArena &) = delete;
  D3D12DescriptorArena &operator=(const D3D12DescriptorArena &) = delete;
  void Initialize(ID3D12Device *device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                  UINT capacity, bool shader_visible);
  [[nodiscard]] UINT Allocate(UINT count = 1);
  [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE Cpu(UINT index) const;
  [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE Gpu(UINT index) const;
  [[nodiscard]] ID3D12DescriptorHeap *Heap() const noexcept { return heap_.Get(); }
  [[nodiscard]] UINT Used() const noexcept { return used_; }
  [[nodiscard]] UINT Capacity() const noexcept { return capacity_; }

private:
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
  UINT increment_{};
  UINT capacity_{};
  UINT used_{};
  bool shader_visible_{};
};

class D3D12RootLayout final {
public:
  [[nodiscard]] ID3D12RootSignature *Get() const noexcept { return root_.Get(); }

private:
  friend class D3D12PipelineCache;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
  std::vector<std::uint8_t> serialized_;
};

struct D3D12PipelineStats {
  UINT64 root_creations{};
  UINT64 graphics_creations{};
  UINT64 compute_creations{};
  UINT64 cache_hits{};
};

// Device-local bounded cache. Exact byte keys, never pointer identities or raw
// native structures (padding). Host graphics subset: triangle, one color RT,
// no vertex inputs/depth/MSAA; guest state support must extend the key and API.
class D3D12PipelineCache final {
public:
  using Root = std::shared_ptr<const D3D12RootLayout>;
  D3D12PipelineCache() = default;
  D3D12PipelineCache(const D3D12PipelineCache &) = delete;
  D3D12PipelineCache &operator=(const D3D12PipelineCache &) = delete;
  void Initialize(ID3D12Device *device);
  [[nodiscard]] Root RootSignature(const D3D12_ROOT_SIGNATURE_DESC &description);
  [[nodiscard]] Microsoft::WRL::ComPtr<ID3D12PipelineState>
  Graphics(const Root &root, D3D12_SHADER_BYTECODE vertex,
           D3D12_SHADER_BYTECODE pixel, DXGI_FORMAT color_format,
           bool premultiplied_blend);
  [[nodiscard]] Microsoft::WRL::ComPtr<ID3D12PipelineState>
  Compute(const Root &root, D3D12_SHADER_BYTECODE shader);
  [[nodiscard]] D3D12PipelineStats Stats() const noexcept { return stats_; }

private:
  void ValidateRoot(const Root &root) const;
  static constexpr std::size_t EntryLimit = 64;
  Microsoft::WRL::ComPtr<ID3D12Device> device_;
  std::map<std::vector<std::uint8_t>, Root> roots_;
  std::map<std::vector<std::uint8_t>, Microsoft::WRL::ComPtr<ID3D12PipelineState>> graphics_;
  std::map<std::vector<std::uint8_t>, Microsoft::WRL::ComPtr<ID3D12PipelineState>> compute_;
  D3D12PipelineStats stats_;
};
