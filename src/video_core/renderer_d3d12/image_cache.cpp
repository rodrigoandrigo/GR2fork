#include "video_core/renderer_d3d12/image_cache.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>
#include <winrt/base.h>
#include "video_core/amdgpu/resource.h"
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
    case vk::Format::eR8G8Snorm:
        return DXGI_FORMAT_R8G8_SNORM;
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
    case vk::Format::eR16Snorm:
        return DXGI_FORMAT_R16_SNORM;
    case vk::Format::eR16Uint:
        return DXGI_FORMAT_R16_UINT;
    case vk::Format::eR16Sint:
        return DXGI_FORMAT_R16_SINT;
    case vk::Format::eR16G16Sfloat:
        return DXGI_FORMAT_R16G16_FLOAT;
    case vk::Format::eR16G16Unorm:
        return DXGI_FORMAT_R16G16_UNORM;
    case vk::Format::eR16G16Snorm:
        return DXGI_FORMAT_R16G16_SNORM;
    case vk::Format::eR16G16Uint:
        return DXGI_FORMAT_R16G16_UINT;
    case vk::Format::eR16G16Sint:
        return DXGI_FORMAT_R16G16_SINT;
    case vk::Format::eR16G16B16A16Sfloat:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case vk::Format::eR16G16B16A16Unorm:
        return DXGI_FORMAT_R16G16B16A16_UNORM;
    case vk::Format::eR16G16B16A16Snorm:
        return DXGI_FORMAT_R16G16B16A16_SNORM;
    case vk::Format::eR16G16B16A16Uint:
        return DXGI_FORMAT_R16G16B16A16_UINT;
    case vk::Format::eR16G16B16A16Sint:
        return DXGI_FORMAT_R16G16B16A16_SINT;
    case vk::Format::eR32Sfloat:
        return DXGI_FORMAT_R32_FLOAT;
    case vk::Format::eR32Uint:
        return DXGI_FORMAT_R32_UINT;
    case vk::Format::eR32Sint:
        return DXGI_FORMAT_R32_SINT;
    case vk::Format::eR32G32Sfloat:
        return DXGI_FORMAT_R32G32_FLOAT;
    case vk::Format::eR32G32Uint:
        return DXGI_FORMAT_R32G32_UINT;
    case vk::Format::eR32G32Sint:
        return DXGI_FORMAT_R32G32_SINT;
    case vk::Format::eR32G32B32Sfloat:
        return DXGI_FORMAT_R32G32B32_FLOAT;
    case vk::Format::eR32G32B32Uint:
        return DXGI_FORMAT_R32G32B32_UINT;
    case vk::Format::eR32G32B32Sint:
        return DXGI_FORMAT_R32G32B32_SINT;
    case vk::Format::eR32G32B32A32Sfloat:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case vk::Format::eR32G32B32A32Uint:
        return DXGI_FORMAT_R32G32B32A32_UINT;
    case vk::Format::eR32G32B32A32Sint:
        return DXGI_FORMAT_R32G32B32A32_SINT;
    case vk::Format::eB10G11R11UfloatPack32:
        return DXGI_FORMAT_R11G11B10_FLOAT;
    case vk::Format::eE5B9G9R9UfloatPack32:
        return DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
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
                            ResourceCache::Write writer,
                            std::function<ResourceCache::Identity(VAddr, u64)> resolver) {
    device = &context;
    read = std::move(reader);
    write = std::move(writer);
    canonicalize = std::move(resolver);
    allocator.Initialize(context.Device(), 256ULL * 1024 * 1024);
    transfers.Initialize(context.Device());
}
void ImageCache::BeginBatch() {
    device->WaitForGpu();
    ++batch;
}
ImageCache::Image& ImageCache::GetImage(const VideoCore::ImageInfo& info, DXGI_FORMAT format,
                                        D3D12_RESOURCE_FLAGS flags, u32 base_layer,
                                        u32 layer_count) {
    if (!info.guest_address || !info.guest_size || info.guest_size > 64 * 1024 * 1024 ||
        !info.size.width || !info.size.height || info.size.width > 16384 ||
        info.size.height > 16384 || !info.size.depth || info.size.depth > 2048 ||
        (!info.props.is_volume && info.size.depth != 1) || !info.resources.levels ||
        info.resources.levels > 16 || !info.resources.layers || info.resources.layers > 2048 ||
        !std::has_single_bit(info.num_samples) || info.num_samples > 16 ||
        info.meta_info.cmask_addr || info.meta_info.fmask_addr || info.meta_info.htile_addr ||
        info.stencil_addr || info.alt_tile)
        throw std::runtime_error(
            "D3D12 guest image requires unsupported metadata, samples or layout");
    if (base_layer >= info.resources.layers)
        throw std::runtime_error("Guest image base layer exceeds allocation");
    if (!layer_count)
        layer_count = info.resources.layers - base_layer;
    if (layer_count > info.resources.layers - base_layer)
        throw std::runtime_error("Guest image layer count exceeds allocation");
    if (info.props.is_volume && (base_layer || layer_count != 1 || info.resources.layers != 1))
        throw std::runtime_error("Invalid D3D12 volume image layer selection");
    const bool multisampled = info.num_samples > 1;
    const DXGI_FORMAT view_format = ConvertFormat(info.pixel_format);
    MsaaFormat msaa_format{};
    if (multisampled) {
        if (info.resources.levels != 1 || info.props.is_volume || info.size.height == 0 ||
            (flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ||
            layer_count * info.num_samples > 2048)
            throw std::runtime_error("Invalid D3D12 multisample image dimensions or UAV usage");
        msaa_format = GetMsaaFormat(view_format, info.props.is_depth);
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS quality{view_format, info.num_samples};
        winrt::check_hresult(device->Device()->CheckFeatureSupport(
            D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &quality, sizeof(quality)));
        if (!quality.NumQualityLevels)
            throw std::runtime_error("Guest sample count is unsupported for this D3D12 format");
        flags |= info.props.is_depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
                                     : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        format = msaa_format.resource;
    }
    const auto identity = canonicalize ? canonicalize(info.guest_address, info.guest_size)
                                       : ResourceCache::Identity{info.guest_address, false};
    std::vector<u64> key{identity.address,      info.guest_size,  info.size.width,
                         info.size.height,      info.pitch,       info.resources.levels,
                         info.resources.layers, u64(format),      u64(flags),
                         u64(info.tile_mode),   info.bank_swizzle};
    key.insert(key.end(), {base_layer, layer_count});
    key.insert(key.end(), {info.size.depth, u64(info.type), info.props.is_volume});
    key.insert(key.end(), {info.num_samples, u64(view_format)});
    key.push_back(identity.physical);
    for (u32 mip = 0; mip < info.resources.levels; ++mip) {
        const auto& layout = info.mips_layout[mip];
        key.insert(key.end(), {layout.size, layout.pitch, layout.height, layout.offset});
    }
    if (auto found = images.find(key); found != images.end()) {
        found->second->batch = batch;
        found->second->age = ++age;
        found->second->info = info;
        return *found->second;
    }
    D3D12_RESOURCE_DESC desc{};
    const bool one_dimensional =
        info.type == AmdGpu::ImageType::Color1D || info.type == AmdGpu::ImageType::Color1DArray;
    if (one_dimensional && info.size.height != 1)
        throw std::runtime_error("Invalid D3D12 1D image height");
    desc.Dimension = info.props.is_volume ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
                                          : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = info.size.width;
    desc.Height = info.size.height;
    desc.DepthOrArraySize = info.props.is_volume ? info.size.depth : layer_count;
    desc.MipLevels = info.resources.levels;
    desc.Format = format;
    desc.SampleDesc.Count = info.num_samples;
    desc.Flags = flags;
    auto result = std::make_unique<Image>();
    result->info = info;
    result->base_layer = base_layer;
    result->native_layers = layer_count;
    D3D12_RESOURCE_DESC staging = desc;
    if (multisampled) {
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
            throw std::runtime_error("D3D12 multisampling requires a 2D image");
        staging.SampleDesc.Count = 1;
        staging.DepthOrArraySize = layer_count * info.num_samples;
        staging.Format = msaa_format.transfer;
        staging.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        result->msaa = std::make_unique<MsaaTransfer::Resources>();
    }
    const UINT count = info.resources.levels * layer_count * info.num_samples;
    result->footprints.resize(count);
    result->rows.resize(count);
    result->row_bytes.resize(count);
    u64 bytes;
    device->Device()->GetCopyableFootprints(&staging, 0, count, 0, result->footprints.data(),
                                            result->rows.data(), result->row_bytes.data(), &bytes);
    const u64 allocation = device->Device()->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
    const u64 extra =
        multisampled ? device->Device()->GetResourceAllocationInfo(0, 1, &staging).SizeInBytes : 0;
    if (allocation == UINT64_MAX || extra == UINT64_MAX || bytes > 256ULL * 1024 * 1024)
        throw std::runtime_error("Unsupported D3D12 image allocation or footprint");
    const u64 required = allocation + extra + ((bytes + 65535) & ~u64{65535}) * 2;
    while (images.size() >= 64 ||
           required >
               allocator.Stats().budget_bytes - allocator.Stats().live_bytes - native_bytes ||
           cpu_bytes + u64(info.guest_size) * 2 > 256ULL * 1024 * 1024) {
        auto oldest = images.end();
        for (auto it = images.begin(); it != images.end(); ++it)
            if (it->second->batch != batch && !it->second->written &&
                (oldest == images.end() || it->second->age < oldest->second->age))
                oldest = it;
        if (oldest == images.end())
            throw std::runtime_error("D3D12 image cache budget exhausted");
        cpu_bytes -= u64(oldest->second->info.guest_size) * 2;
        native_bytes -= oldest->second->allocation_bytes;
        images.erase(oldest);
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = view_format;
    clear.DepthStencil.Depth = 1.f;
    winrt::check_hresult(device->Device()->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
        multisampled && info.props.is_depth ? &clear : nullptr, IID_PPV_ARGS(&result->native)));
    result->allocation_bytes = allocation + extra;
    if (multisampled) {
        winrt::check_hresult(device->Device()->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &staging, D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&result->msaa->planes)));
        auto& resources = *result->msaa;
        resources.descriptors.Initialize(device->Device(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                         2, true);
        resources.targets.Initialize(device->Device(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, count,
                                     false);
        resources.depth_targets.Initialize(device->Device(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
                                           layer_count, false);
        static_cast<void>(resources.descriptors.Allocate(2));
        static_cast<void>(resources.targets.Allocate(count));
        static_cast<void>(resources.depth_targets.Allocate(layer_count));
    }
    winrt::check_hresult(allocator.CreateBuffer(bytes, D3D12_HEAP_TYPE_UPLOAD,
                                                D3D12_RESOURCE_STATE_GENERIC_READ, result->upload));
    winrt::check_hresult(allocator.CreateBuffer(bytes, D3D12_HEAP_TYPE_READBACK,
                                                D3D12_RESOURCE_STATE_COPY_DEST, result->readback));
    result->age = ++age;
    result->batch = batch;
    auto& output = *result;
    images.emplace(std::move(key), std::move(result));
    native_bytes += output.allocation_bytes;
    cpu_bytes += u64(info.guest_size) * 2;
    return output;
}
void ImageCache::Barrier(Image& image, ID3D12GraphicsCommandList* commands,
                         D3D12_RESOURCE_STATES state) {
    if (image.msaa && image.info.props.is_depth)
        SetSamplePositions(commands, image.info.num_samples, image.sample_positions);
    if (image.state == state) {
        if (state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barrier.UAV.pResource = image.Native();
            commands->ResourceBarrier(1, &barrier);
        }
        return;
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {image.Native(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, image.state,
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
        for (u32 layer = 0; layer < image.native_layers * image.info.num_samples; ++layer) {
            const u32 subresource = mip + layer * image.info.resources.levels;
            const auto& footprint = image.footprints[subresource];
            if (image.row_bytes[subresource] > pitch || image.rows[subresource] > height)
                throw std::runtime_error("D3D12 footprint exceeds guest mip layout");
            for (u32 slice = 0; slice < footprint.Footprint.Depth; ++slice) {
                for (u32 row = 0; row < image.rows[subresource]; ++row) {
                    const u64 guest_offset =
                        mip_offset +
                        (image.base_layer * image.info.num_samples + layer + slice) * pitch *
                            height +
                        row * pitch;
                    if (guest_offset + image.row_bytes[subresource] > image.linear.size())
                        throw std::runtime_error("D3D12 image row exceeds guest backing");
                    auto* guest = image.linear.data() + guest_offset;
                    auto* host =
                        static_cast<u8*>(data) + footprint.Offset +
                        (slice * image.rows[subresource] + row) * footprint.Footprint.RowPitch;
                    std::memcpy(upload ? host : guest, upload ? guest : host,
                                image.row_bytes[subresource]);
                }
            }
        }
        mip_offset += layout.size;
    }
}
void ImageCache::Upload(Image& image, ID3D12GraphicsCommandList* commands, VAddr read_address) {
    std::vector<u8> guest(image.info.guest_size);
    read(read_address ? read_address : image.info.guest_address, guest);
    if (guest == image.guest && !image.msaa)
        return;
    image.guest = std::move(guest);
    image.linear.resize(image.guest.size());
    VideoCore::ConvertTiling(image.guest, image.linear, image.info, false);
    void* mapped;
    const D3D12_RANGE empty{0, 0};
    winrt::check_hresult(image.upload->Map(0, &empty, &mapped));
    CopyRows(image, mapped, true);
    image.upload->Unmap(0, nullptr);
    if (image.msaa)
        MsaaTransfer::Barrier(*image.msaa, commands, D3D12_RESOURCE_STATE_COPY_DEST);
    else
        Barrier(image, commands, D3D12_RESOURCE_STATE_COPY_DEST);
    for (u32 subresource = 0; subresource < image.footprints.size(); ++subresource) {
        D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
        source.pResource = image.upload.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = image.footprints[subresource];
        destination.pResource = image.msaa ? image.msaa->planes.Get() : image.Native();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = subresource;
        commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    }
    if (image.msaa) {
        SetSamplePositions(commands, image.info.num_samples, image.sample_positions);
        Barrier(image, commands,
                image.info.props.is_depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                                          : D3D12_RESOURCE_STATE_RENDER_TARGET);
        transfers.Record(commands, image.Native(), *image.msaa,
                         ConvertFormat(image.info.pixel_format), image.info.props.is_depth, true);
    }
}
void ImageCache::PrepareDownloads(ID3D12GraphicsCommandList* commands) {
    for (auto& [key, image] : images) {
        if (image->written) {
            if (image->msaa) {
                Barrier(*image, commands, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                SetSamplePositions(commands, 1, {});
                transfers.Record(commands, image->Native(), *image->msaa,
                                 ConvertFormat(image->info.pixel_format),
                                 image->info.props.is_depth, false);
                MsaaTransfer::Barrier(*image->msaa, commands, D3D12_RESOURCE_STATE_COPY_SOURCE);
            } else
                Barrier(*image, commands, D3D12_RESOURCE_STATE_COPY_SOURCE);
            for (u32 subresource = 0; subresource < image->footprints.size(); ++subresource) {
                D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
                source.pResource = image->msaa ? image->msaa->planes.Get() : image->Native();
                source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                source.SubresourceIndex = subresource;
                destination.pResource = image->readback.Get();
                destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                destination.PlacedFootprint = image->footprints[subresource];
                commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            }
        }
        Barrier(*image, commands, D3D12_RESOURCE_STATE_COMMON);
        if (image->msaa)
            MsaaTransfer::Barrier(*image->msaa, commands, D3D12_RESOURCE_STATE_COMMON);
    }
}
void ImageCache::Resolve(Image& source, Image& destination, ID3D12GraphicsCommandList* commands) {
    if (!source.msaa || destination.msaa || source.info.props.is_depth ||
        destination.info.props.is_depth || source.info.size.width != destination.info.size.width ||
        source.info.size.height != destination.info.size.height ||
        source.native_layers != destination.native_layers ||
        source.info.pixel_format != destination.info.pixel_format)
        throw std::runtime_error("Invalid D3D12 color resolve source or destination");
    const DXGI_FORMAT format = ConvertFormat(source.info.pixel_format);
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
    winrt::check_hresult(device->Device()->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,
                                                               &support, sizeof(support)));
    if (!(support.Support1 & D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE))
        throw std::runtime_error("D3D12 format does not support multisample color resolve");
    Upload(source, commands);
    Upload(destination, commands);
    Barrier(source, commands, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    Barrier(destination, commands, D3D12_RESOURCE_STATE_RESOLVE_DEST);
    for (u32 layer = 0; layer < source.native_layers; ++layer)
        commands->ResolveSubresource(destination.Native(), layer, source.Native(), layer, format);
    destination.written = true;
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
