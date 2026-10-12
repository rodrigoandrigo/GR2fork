#pragma once

#include <map>
#include "video_core/renderer_d3d12/resource_cache.h"
#include "video_core/texture_cache/image_info.h"

namespace D3D12 {
class ImageCache {
public:
    struct Image {
        VideoCore::ImageInfo info;
        D3D12Resource native, upload, readback;
        D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_COMMON};
        std::vector<u8> guest, linear;
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
        std::vector<UINT> rows;
        std::vector<UINT64> row_bytes;
        u64 age{};
        u64 batch{};
        bool written{};
    };
    void Initialize(D3D12DeviceContext& device, ResourceCache::Read read,
                    ResourceCache::Write write);
    void BeginBatch();
    Image& GetImage(const VideoCore::ImageInfo& info, DXGI_FORMAT format,
                    D3D12_RESOURCE_FLAGS flags);
    void Upload(Image& image, ID3D12GraphicsCommandList* commands);
    void Barrier(Image& image, ID3D12GraphicsCommandList* commands, D3D12_RESOURCE_STATES state);
    void PrepareDownloads(ID3D12GraphicsCommandList* commands);
    void CommitDownloads();

private:
    void CopyRows(Image& image, void* data, bool upload);
    D3D12DeviceContext* device{};
    D3D12ResourceAllocator allocator;
    ResourceCache::Read read;
    ResourceCache::Write write;
    std::map<std::vector<u64>, std::unique_ptr<Image>> images;
    u64 batch{}, age{};
};
DXGI_FORMAT ConvertFormat(vk::Format format);
} // namespace D3D12
