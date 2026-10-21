#pragma once

#include "video_core/renderer_d3d12/transfer_pool.h"

#include <map>
#include <memory>
#include "video_core/renderer_d3d12/msaa_transfer.h"
#include "video_core/renderer_d3d12/resource_cache.h"
#include "video_core/renderer_d3d12/tiling_runtime.h"
#include "video_core/texture_cache/image_info.h"

namespace D3D12 {
class ImageCache {
public:
    void SetAsyncRecording(bool enabled, u32 limit = 64) {
        async_recording = enabled;
        image_limit = limit;
    }
    struct Image {
        VideoCore::ImageInfo info;
        D3D12Resource upload, readback;
        D3D12Resource stencil_upload, stencil_readback;
        Microsoft::WRL::ComPtr<ID3D12Resource> native;
        u64 allocation_bytes{};
        u64 staging_bytes{};
        u64 stencil_staging_bytes{};
        std::unique_ptr<MsaaTransfer::Resources> msaa;
        std::vector<D3D12_SAMPLE_POSITION> sample_positions;
        ID3D12Resource* Native() const {
            return native.Get();
        }
        D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_COMMON};
        std::vector<u8> guest, linear;
        std::vector<u8> stencil_guest, stencil_linear;
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
        bool logic_op{};
        u32 logic_mask{};
        bool gpu_tiled{};
        ID3D12Resource* tiled_readback{};
        std::vector<UINT> rows;
        std::vector<UINT64> row_bytes;
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> stencil_footprints;
        std::vector<UINT> stencil_rows;
        std::vector<UINT64> stencil_row_bytes;
        u64 age{};
        u64 batch{};
        u32 base_layer{}, native_layers{};
        bool cpu_dirty{true};
        bool resident{};
        bool download_requested{};
        bool invalidate_after_download{};
        bool written{};
        bool stencil_written{};
        bool has_stencil{};
        bool has_depth{true};
        bool promoted_depth{};
        DXGI_FORMAT dsv_format{DXGI_FORMAT_UNKNOWN};
        DXGI_FORMAT stencil_footprint_format{DXGI_FORMAT_UNKNOWN};
        DXGI_FORMAT depth_footprint_format{DXGI_FORMAT_UNKNOWN};
    };
    void Initialize(D3D12DeviceContext& device, ResourceCache::Read read,
                    ResourceCache::Write write,
                    std::function<ResourceCache::Identity(VAddr, u64)> canonicalize = {},
                    class ShaderCache* shader_cache = nullptr,
                    std::shared_ptr<TransferPool> transfer_pool = {},
                    const ResourceCache* alias_cache = nullptr);
    struct Watch {
        VAddr address;
        u64 size;
        bool dirty;
    };
    struct Statistics {
        u64 uploads{}, resident_hits{}, downloads{}, cpu_invalidations{};
    };
    void ConfigureResidency(bool enabled, std::function<void(VAddr, u64)> synchronize = {});
    std::vector<Watch> Watches() const;
    bool RequestDownloads(std::span<const ResourceCache::Range> ranges, bool all);
    void InvalidateCpu(std::span<const ResourceCache::Range> ranges, bool all = false);
    bool HasDirtyImages() const;
    const Statistics& ResidencyStatistics() const {
        return statistics;
    }
    Image* FindPresentation(const VideoCore::ImageInfo& info);
    u64 MemoryBytes() const;
    void TrimMemory(u64 target, bool writeback);
    void BeginBatch();
    Image& GetImage(const VideoCore::ImageInfo& info, DXGI_FORMAT format,
                    D3D12_RESOURCE_FLAGS flags, u32 base_layer = 0, u32 layer_count = 0,
                    bool logic_op = false, u32 logic_mask = 15);
    void Upload(Image& image, ID3D12GraphicsCommandList* commands, VAddr read_address = 0,
                VAddr stencil_read_address = 0);
    void Barrier(Image& image, ID3D12GraphicsCommandList* commands, D3D12_RESOURCE_STATES state);
    void PrepareDownloads(ID3D12GraphicsCommandList* commands);
    void CommitDownloads();
    void RetireUploads(u64 fence);
    void Resolve(Image& source, Image& destination, ID3D12GraphicsCommandList* commands);
    const std::vector<u8>& MaskShader() const {
        return transfers.MaskShader();
    }

private:
    bool async_recording{};
    u32 image_limit{64};
    bool Overlaps(const Image& image, VAddr address, u64 size) const;
    void Synchronize(Image& image);
    bool persistent{};
    Statistics statistics{};
    std::function<void(VAddr, u64)> synchronize;
    const ResourceCache* alias_cache{};
    void EnsureSpace(u64 required, u64 cpu_required = 0, bool inserting = false);
    void EnsureStaging(Image& image, bool download);
    void CopyRows(Image& image, void* data, bool upload);
    void UploadStencil(Image& image, ID3D12GraphicsCommandList* commands, VAddr read_address,
                       VAddr stencil_read_address);
    void UploadMsaaStencil(Image& image, ID3D12GraphicsCommandList* commands, VAddr read_address,
                           VAddr stencil_read_address);
    void PrepareStencilDownloads(Image& image, ID3D12GraphicsCommandList* commands);
    void PrepareMsaaStencilDownloads(Image& image, ID3D12GraphicsCommandList* commands);
    void CommitStencilDownloads(Image& image);
    void CommitMsaaStencilDownloads(Image& image);
    D3D12DeviceContext* device{};
    D3D12ResourceAllocator allocator;
    std::shared_ptr<TransferPool> transfer_pool;
    u64 transfer_fence{};
    MsaaTransfer transfers;
    std::unique_ptr<TilingRuntime> tilizer;
    ResourceCache::Read read;
    ResourceCache::Write write;
    std::function<ResourceCache::Identity(VAddr, u64)> canonicalize;
    std::map<std::vector<u64>, std::unique_ptr<Image>> images;
    u64 batch{}, age{};
    u64 cpu_bytes{}, native_bytes{};
};
DXGI_FORMAT ConvertFormat(vk::Format format);
} // namespace D3D12
