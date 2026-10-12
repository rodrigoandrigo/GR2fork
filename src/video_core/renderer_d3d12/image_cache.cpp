#include "video_core/renderer_d3d12/image_cache.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <winrt/base.h>
#include "video_core/texture_cache/cpu_tiling.h"

namespace D3D12 {
DXGI_FORMAT ConvertFormat(vk::Format format) {
    switch (format) {
    case vk::Format::eR8Unorm:
        return DXGI_FORMAT_R8_UNORM;
    case vk::Format::eR8Snorm:
        return DXGI_FORMAT_R8_SNORM;
    case vk::Format::eR8Uint:
        return DXGI_FORMAT_R8_UINT;
    case vk::Format::eR8Sint:
        return DXGI_FORMAT_R8_SINT;
    case vk::Format::eR8G8Unorm:
        return DXGI_FORMAT_R8G8_UNORM;
    case vk::Format::eR8G8Uint:
        return DXGI_FORMAT_R8G8_UINT;
    case vk::Format::eR8G8Sint:
        return DXGI_FORMAT_R8G8_SINT;
    case vk::Format::eR8G8B8A8Unorm:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case vk::Format::eR8G8B8A8Srgb:
        return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case vk::Format::eR8G8B8A8Uint:
        return DXGI_FORMAT_R8G8B8A8_UINT;
    case vk::Format::eR8G8B8A8Sint:
        return DXGI_FORMAT_R8G8B8A8_SINT;
    case vk::Format::eR8G8B8A8Snorm:
        return DXGI_FORMAT_R8G8B8A8_SNORM;
    case vk::Format::eB8G8R8A8Unorm:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case vk::Format::eB8G8R8A8Srgb:
        return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case vk::Format::eA2B10G10R10UnormPack32:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case vk::Format::eA2B10G10R10UintPack32:
        return DXGI_FORMAT_R10G10B10A2_UINT;
    case vk::Format::eR16Sfloat:
        return DXGI_FORMAT_R16_FLOAT;
    case vk::Format::eR16Unorm:
        return DXGI_FORMAT_R16_UNORM;
    case vk::Format::eR16Uint:
        return DXGI_FORMAT_R16_UINT;
    case vk::Format::eR16Sint:
        return DXGI_FORMAT_R16_SINT;
    case vk::Format::eR16G16Sfloat:
        return DXGI_FORMAT_R16G16_FLOAT;
    case vk::Format::eR16G16B16A16Sfloat:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case vk::Format::eR32Sfloat:
        return DXGI_FORMAT_R32_FLOAT;
    case vk::Format::eR32Uint:
        return DXGI_FORMAT_R32_UINT;
    case vk::Format::eR32Sint:
        return DXGI_FORMAT_R32_SINT;
    case vk::Format::eR32G32Sfloat:
        return DXGI_FORMAT_R32G32_FLOAT;
    case vk::Format::eR32G32B32Sfloat:
        return DXGI_FORMAT_R32G32B32_FLOAT;
    case vk::Format::eR32G32B32A32Sfloat:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case vk::Format::eD32Sfloat:
        return DXGI_FORMAT_D32_FLOAT;
    case vk::Format::eD16Unorm:
        return DXGI_FORMAT_D16_UNORM;
    case vk::Format::eBc1RgbaUnormBlock:
        return DXGI_FORMAT_BC1_UNORM;
    case vk::Format::eBc1RgbaSrgbBlock:
        return DXGI_FORMAT_BC1_UNORM_SRGB;
    case vk::Format::eBc2UnormBlock:
        return DXGI_FORMAT_BC2_UNORM;
    case vk::Format::eBc2SrgbBlock:
        return DXGI_FORMAT_BC2_UNORM_SRGB;
    case vk::Format::eBc3UnormBlock:
        return DXGI_FORMAT_BC3_UNORM;
    case vk::Format::eBc3SrgbBlock:
        return DXGI_FORMAT_BC3_UNORM_SRGB;
    case vk::Format::eBc4UnormBlock:
        return DXGI_FORMAT_BC4_UNORM;
    case vk::Format::eBc4SnormBlock:
        return DXGI_FORMAT_BC4_SNORM;
    case vk::Format::eBc5UnormBlock:
        return DXGI_FORMAT_BC5_UNORM;
    case vk::Format::eBc5SnormBlock:
        return DXGI_FORMAT_BC5_SNORM;
    case vk::Format::eBc6HUfloatBlock:
        return DXGI_FORMAT_BC6H_UF16;
    case vk::Format::eBc6HSfloatBlock:
        return DXGI_FORMAT_BC6H_SF16;
    case vk::Format::eBc7UnormBlock:
        return DXGI_FORMAT_BC7_UNORM;
    case vk::Format::eBc7SrgbBlock:
        return DXGI_FORMAT_BC7_UNORM_SRGB;
    default:
        throw std::runtime_error("Guest image format has no D3D12 mapping");
    }
}
void ImageCache::Initialize(D3D12DeviceContext& context, ResourceCache::Read reader,
                            ResourceCache::Write writer) {
    device = &context;
    read = std::move(reader);
    write = std::move(writer);
    allocator.Initialize(context.Device(), 256ULL * 1024 * 1024);
}
void ImageCache::BeginBatch() {
    device->WaitForGpu();
    ++batch;
}
ImageCache::Image& ImageCache::GetImage(const VideoCore::ImageInfo& info, DXGI_FORMAT format,
                                        D3D12_RESOURCE_FLAGS flags) {
    if (!info.guest_address || !info.guest_size || info.guest_size > 64 * 1024 * 1024 ||
        !info.size.width || !info.size.height || info.size.width > 16384 ||
        info.size.height > 16384 || info.size.depth != 1 || !info.resources.levels ||
        info.resources.levels > 16 || !info.resources.layers || info.resources.layers > 2048 ||
        info.num_samples != 1 || info.meta_info.cmask_addr || info.meta_info.fmask_addr ||
        info.meta_info.htile_addr || info.stencil_addr || info.alt_tile)
        throw std::runtime_error(
            "D3D12 guest image requires unsupported metadata, samples or layout");
    std::vector<u64> key{info.guest_address,    info.guest_size,  info.size.width,
                         info.size.height,      info.pitch,       info.resources.levels,
                         info.resources.layers, u64(format),      u64(flags),
                         u64(info.tile_mode),   info.bank_swizzle};
    for (u32 mip = 0; mip < info.resources.levels; ++mip) {
        const auto& layout = info.mips_layout[mip];
        key.insert(key.end(), {layout.size, layout.pitch, layout.height, layout.offset});
    }
    if (auto found = images.find(key); found != images.end()) {
        found->second->batch = batch;
        found->second->age = ++age;
        return *found->second;
    }
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = info.size.width;
    desc.Height = info.size.height;
    desc.DepthOrArraySize = info.resources.layers;
    desc.MipLevels = info.resources.levels;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = flags;
    auto result = std::make_unique<Image>();
    result->info = info;
    const UINT count = info.resources.levels * info.resources.layers;
    result->footprints.resize(count);
    result->rows.resize(count);
    result->row_bytes.resize(count);
    u64 bytes;
    device->Device()->GetCopyableFootprints(&desc, 0, count, 0, result->footprints.data(),
                                            result->rows.data(), result->row_bytes.data(), &bytes);
    const u64 required = device->Device()->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes +
                         ((bytes + 65535) & ~u64{65535}) * 2;
    while (images.size() >= 64 ||
           required > allocator.Stats().budget_bytes - allocator.Stats().live_bytes) {
        auto oldest = images.end();
        for (auto it = images.begin(); it != images.end(); ++it)
            if (it->second->batch != batch && !it->second->written &&
                (oldest == images.end() || it->second->age < oldest->second->age))
                oldest = it;
        if (oldest == images.end())
            throw std::runtime_error("D3D12 image cache budget exhausted");
        images.erase(oldest);
    }
    winrt::check_hresult(
        allocator.CreateTexture2D(desc, D3D12_RESOURCE_STATE_COMMON, result->native));
    winrt::check_hresult(allocator.CreateBuffer(bytes, D3D12_HEAP_TYPE_UPLOAD,
                                                D3D12_RESOURCE_STATE_GENERIC_READ, result->upload));
    winrt::check_hresult(allocator.CreateBuffer(bytes, D3D12_HEAP_TYPE_READBACK,
                                                D3D12_RESOURCE_STATE_COPY_DEST, result->readback));
    result->age = ++age;
    result->batch = batch;
    auto& output = *result;
    images.emplace(std::move(key), std::move(result));
    return output;
}
void ImageCache::Barrier(Image& image, ID3D12GraphicsCommandList* commands,
                         D3D12_RESOURCE_STATES state) {
    if (image.state == state) {
        if (state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barrier.UAV.pResource = image.native.Get();
            commands->ResourceBarrier(1, &barrier);
        }
        return;
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {image.native.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, image.state,
                          state};
    commands->ResourceBarrier(1, &barrier);
    image.state = state;
}
void ImageCache::CopyRows(Image& image, void* data, bool upload) {
    u64 mip_offset{};
    for (u32 mip = 0; mip < image.info.resources.levels; ++mip) {
        const auto& layout = image.info.mips_layout[mip];
        const u64 pitch =
            (image.info.props.is_block ? std::max<u32>((layout.pitch + 3) / 4, 1) : layout.pitch) *
            (image.info.num_bits / 8);
        const u64 height =
            image.info.props.is_block ? std::max<u32>((layout.height + 3) / 4, 1) : layout.height;
        for (u32 layer = 0; layer < image.info.resources.layers; ++layer) {
            const u32 subresource = mip + layer * image.info.resources.levels;
            const auto& footprint = image.footprints[subresource];
            if (image.row_bytes[subresource] > pitch || image.rows[subresource] > height)
                throw std::runtime_error("D3D12 footprint exceeds guest mip layout");
            for (u32 row = 0; row < image.rows[subresource]; ++row) {
                const u64 guest_offset = mip_offset + layer * pitch * height + row * pitch;
                if (guest_offset + image.row_bytes[subresource] > image.linear.size())
                    throw std::runtime_error("D3D12 image row exceeds guest backing");
                auto* guest = image.linear.data() + guest_offset;
                auto* host =
                    static_cast<u8*>(data) + footprint.Offset + row * footprint.Footprint.RowPitch;
                std::memcpy(upload ? host : guest, upload ? guest : host,
                            image.row_bytes[subresource]);
            }
        }
        mip_offset += layout.size;
    }
}
void ImageCache::Upload(Image& image, ID3D12GraphicsCommandList* commands) {
    std::vector<u8> guest(image.info.guest_size);
    read(image.info.guest_address, guest);
    if (guest == image.guest)
        return;
    image.guest = std::move(guest);
    image.linear.resize(image.guest.size());
    VideoCore::ConvertTiling(image.guest, image.linear, image.info, false);
    void* mapped;
    const D3D12_RANGE empty{0, 0};
    winrt::check_hresult(image.upload->Map(0, &empty, &mapped));
    CopyRows(image, mapped, true);
    image.upload->Unmap(0, nullptr);
    Barrier(image, commands, D3D12_RESOURCE_STATE_COPY_DEST);
    for (u32 subresource = 0; subresource < image.footprints.size(); ++subresource) {
        D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
        source.pResource = image.upload.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = image.footprints[subresource];
        destination.pResource = image.native.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = subresource;
        commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    }
}
void ImageCache::PrepareDownloads(ID3D12GraphicsCommandList* commands) {
    for (auto& [key, image] : images) {
        if (image->written) {
            Barrier(*image, commands, D3D12_RESOURCE_STATE_COPY_SOURCE);
            for (u32 subresource = 0; subresource < image->footprints.size(); ++subresource) {
                D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
                source.pResource = image->native.Get();
                source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                source.SubresourceIndex = subresource;
                destination.pResource = image->readback.Get();
                destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                destination.PlacedFootprint = image->footprints[subresource];
                commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            }
        }
        Barrier(*image, commands, D3D12_RESOURCE_STATE_COMMON);
    }
}
void ImageCache::CommitDownloads() {
    for (auto& [key, image] : images) {
        if (!image->written)
            continue;
        void* mapped;
        winrt::check_hresult(image->readback->Map(0, nullptr, &mapped));
        CopyRows(*image, mapped, false);
        const D3D12_RANGE empty{0, 0};
        image->readback->Unmap(0, &empty);
        VideoCore::ConvertTiling(image->linear, image->guest, image->info, true);
        write(image->info.guest_address, image->guest);
        image->written = false;
    }
}
} // namespace D3D12
