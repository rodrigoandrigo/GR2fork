#pragma once

#include "video_core/renderer_d3d12/transfer_pool.h"

#include <span>
#include <functional>
#include <unordered_map>
#include <wrl/client.h>
#include "d3d12_device_context.h"
#include "d3d12_resource_allocator.h"
#include "video_core/texture_cache/image_info.h"

namespace D3D12 {
struct TilingEntry {
    u32 mip;
    u32 pitch;
    u32 height;
    u32 texels;
    u32 tiled_offset;
    u32 linear_offset;
    u32 row_pitch;
    u32 width;
    u32 visible_height;
    u32 depth{1}, slice{}, sample{}, depth_pitch{};
};
class TilingRuntime {
  public:
    using Reader = std::function<void(std::span<u8>)>;
    void Initialize(D3D12DeviceContext& device, class ShaderCache& shaders,
                    std::shared_ptr<TransferPool> transfers = {});
    u64 MemoryBytes() const { return allocator.Stats().live_bytes + pipelines.size() * 65536; }
    void TrimMemory();
    void BeginBatch();
    void RetireUploads(u64 fence);
    void ReleaseReadbacks(u64 fence);
    bool CanRecord(const VideoCore::ImageInfo& info, std::span<const TilingEntry> entries,
                   bool tile) const;
    static bool Supported(const VideoCore::ImageInfo& info, size_t entries);
    static std::vector<TilingEntry>
    Entries(const VideoCore::ImageInfo& info,
            std::span<const D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints, u32 base_layer = 0);
    void Detile(ID3D12GraphicsCommandList* commands, const VideoCore::ImageInfo& info,
                std::span<const TilingEntry> entries, std::span<const u8> tiled_bytes,
                ID3D12Resource* image,
                std::span<const D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints,
                bool logic_op = false, u32 logic_mask = 15, const Reader& reader = {});
    ID3D12Resource* Tile(ID3D12GraphicsCommandList* commands, const VideoCore::ImageInfo& info,
                         std::span<const TilingEntry> entries, ID3D12Resource* image,
                         std::span<const D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints,
                         std::span<const u8> guest, bool logic_op = false, u32 logic_mask = 15,
                         const Reader& reader = {});

  private:
    ID3D12PipelineState* Pipeline(const VideoCore::ImageInfo& info, bool is_tiler, bool logic_op);
    void Dispatch(ID3D12GraphicsCommandList* commands, const VideoCore::ImageInfo& info,
                  std::span<const TilingEntry> entries, bool is_tiler, bool logic_op,
                  u32 logic_mask);
    void CopyFromGuest(ID3D12GraphicsCommandList* commands, std::span<const u8> bytes,
                       const Reader& reader = {}, u64 size = 0);
    void OutputToImage(ID3D12GraphicsCommandList* commands, std::span<const TilingEntry> entries,
                       ID3D12Resource* image,
                       std::span<const D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints);
    void OutputToBuffer(ID3D12GraphicsCommandList* commands, u64 bytes);
    D3D12Resource& RequestBuffer(D3D12Resource& buffer, u64 size, D3D12_HEAP_TYPE heap,
                                 D3D12_RESOURCE_STATES initial_state, D3D12_RESOURCE_STATES& state,
                                 D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE);
    void BeginOperation();
    void Transition(ID3D12GraphicsCommandList* commands, D3D12Resource& buffer,
                    D3D12_RESOURCE_STATES& state, D3D12_RESOURCE_STATES next);
    D3D12DeviceContext* device{};
    class ShaderCache* shaders{};
    D3D12ResourceAllocator allocator;
    std::shared_ptr<TransferPool> transfer_pool;
    u64 transfer_fence{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    std::unordered_map<u64, Microsoft::WRL::ComPtr<ID3D12PipelineState>> pipelines;
    struct Operation {
        D3D12Resource staging, input, output, tile_readback;
        D3D12_RESOURCE_STATES heap_state{}, input_state{}, output_state{};
    };
    std::vector<std::unique_ptr<Operation>> operations;
    Operation* active{};
};
} // namespace D3D12
