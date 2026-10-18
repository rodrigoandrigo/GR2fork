#pragma once

#include <map>
#include <memory>
#include "video_core/renderer_d3d12/msaa_transfer.h"
#include "video_core/renderer_d3d12/resource_cache.h"
#include "video_core/renderer_d3d12/tiling_runtime.h"
#include "video_core/texture_cache/image_info.h"

namespace D3D12 {
class ImageCache {
public:
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
        std::vector<u8> guest_scratch;
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
                    class ShaderCache* shader_cache = nullptr);
    void BeginBatch();
    Image& GetImage(const VideoCore::ImageInfo& info, DXGI_FORMAT format,
                    D3D12_RESOURCE_FLAGS flags, u32 base_layer = 0, u32 layer_count = 0, bool logic_op = false, u32 logic_mask = 15);
    void Upload(Image& image, ID3D12GraphicsCommandList* commands, VAddr read_address = 0,
                VAddr stencil_read_address = 0);
    void Barrier(Image& image, ID3D12GraphicsCommandList* commands, D3D12_RESOURCE_STATES state);
    void PrepareDownloads(ID3D12GraphicsCommandList* commands);
    void CommitDownloads();
    void Resolve(Image& source, Image& destination, ID3D12GraphicsCommandList* commands);
    const std::vector<u8>& MaskShader() const {
        return transfers.MaskShader();
    }

private:
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
