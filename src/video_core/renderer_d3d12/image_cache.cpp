#include "video_core/renderer_d3d12/image_cache.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>
#include <fmt/format.h>
#include <winrt/base.h>
#include "video_core/amdgpu/resource.h"
#include "video_core/renderer_d3d12/profile.h"
#include "video_core/renderer_d3d12/logic_op_format.h"
#include "video_core/renderer_d3d12/shader_cache.h"
#include "video_core/texture_cache/cpu_tiling.h"

namespace D3D12 {
ImageCache::Image* ImageCache::FindPresentation(const VideoCore::ImageInfo& info) {
    Image* result{};
    for (auto& [key, image] : images) {
        const auto& source = image->info;
        if (!image->resident || image->cpu_dirty || image->logic_op || source.props.is_depth ||
            source.props.is_volume || source.num_samples != 1 || image->base_layer != 0 ||
            source.size.width != info.size.width || source.size.height != info.size.height ||
            source.num_bits != info.num_bits || source.pitch != info.pitch ||
            source.mips_layout[0].pitch != info.mips_layout[0].pitch ||
            ((source.micro_tiled_mips ^ info.micro_tiled_mips) & 1) ||
            source.tile_mode != info.tile_mode || source.alt_tile != info.alt_tile ||
            source.bank_swizzle != info.bank_swizzle)
            continue;
        auto first = canonicalize ? canonicalize(source.guest_address, info.guest_size)
                                  : ResourceCache::Identity{source.guest_address, false};
        auto second = canonicalize ? canonicalize(info.guest_address, info.guest_size)
                                   : ResourceCache::Identity{info.guest_address, false};
        if (first.address != second.address || first.physical != second.physical)
            continue;
        const auto format = image->Native()->GetDesc().Format;
        const bool compatible = info.num_bits == 64
                                    ? format == DXGI_FORMAT_R16G16B16A16_FLOAT
                                    : format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                                          format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                                          format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
                                          format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                                          format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                                          format == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
                                          format == DXGI_FORMAT_R10G10B10A2_UNORM ||
                                          format == DXGI_FORMAT_R10G10B10A2_TYPELESS;
        if (compatible &&
            (!result ||
             (image->written != result->written ? image->written : image->age > result->age)))
            result = image.get();
    }
    return result;
}

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
    case vk::Format::eR5G6B5UnormPack16:
        return DXGI_FORMAT_B5G6R5_UNORM;
    case vk::Format::eA1R5G5B5UnormPack16:
        return DXGI_FORMAT_B5G5R5A1_UNORM;
    case vk::Format::eB4G4R4A4UnormPack16:
        return DXGI_FORMAT_B4G4R4A4_UNORM;
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
    case vk::Format::eD32SfloatS8Uint:
        return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    case vk::Format::eD16UnormS8Uint:
        return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    case vk::Format::eD24UnormS8Uint:
        return DXGI_FORMAT_D24_UNORM_S8_UINT;
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
        throw std::runtime_error(
            fmt::format("Guest image format has no D3D12 mapping: {}", u32(format)));
    }
}
void ImageCache::Initialize(D3D12DeviceContext& context, ResourceCache::Read reader,
                            ResourceCache::Write writer,
                            std::function<ResourceCache::Identity(VAddr, u64)> resolver,
                            ShaderCache* shader_cache, std::shared_ptr<TransferPool> pool,
                            const ResourceCache* aliases) {
    device = &context;
    alias_cache = aliases;
    transfer_pool = std::move(pool);
    if (!transfer_pool) {
        transfer_pool = std::make_shared<TransferPool>();
        transfer_pool->Initialize(context);
    }
    read = std::move(reader);
    write = std::move(writer);
    canonicalize = std::move(resolver);
    allocator.Initialize(context.Device(), 256ULL * 1024 * 1024);
    transfers.Initialize(context.Device());
    if (shader_cache) {
        tilizer = std::make_unique<TilingRuntime>();
        tilizer->Initialize(context, *shader_cache, transfer_pool);
    }
}
void ImageCache::ConfigureResidency(bool enabled, std::function<void(VAddr, u64)> callback) {
    if (enabled && !persistent)
        InvalidateCpu({}, true);
    persistent = enabled;
    synchronize = std::move(callback);
}
bool ImageCache::Overlaps(const Image& image, VAddr address, u64 size) const {
    const auto overlap = [&](VAddr base, u64 bytes) {
        if (!base || !bytes || !address || !size)
            return false;
        return alias_cache ? alias_cache->Aliases(base, bytes, address, size)
                           : base <= UINT64_MAX - bytes && address <= UINT64_MAX - size &&
                                 base < address + size && address < base + bytes;
    };
    return overlap(image.info.guest_address, image.info.guest_size) ||
           overlap(image.info.stencil_addr, image.info.stencil_size);
}
void ImageCache::Synchronize(Image& image) {
    if (!(image.written || image.stencil_written))
        return;
    if (!synchronize)
        throw std::runtime_error("Dirty D3D12 image requires a synchronization callback");
    if (image.info.guest_address)
        synchronize(image.info.guest_address, image.info.guest_size);
    if (image.info.stencil_addr)
        synchronize(image.info.stencil_addr, image.info.stencil_size);
}
std::vector<ImageCache::Watch> ImageCache::Watches() const {
    std::vector<Watch> result;
    if (!persistent)
        return result;
    for (const auto& [key, image] : images) {
        if (!image->resident || image->cpu_dirty)
            continue;
        const bool dirty = image->written || image->stencil_written;
        if (image->has_depth)
            result.push_back({image->info.guest_address, image->info.guest_size, dirty});
        if (image->has_stencil)
            result.push_back({image->info.stencil_addr, image->info.stencil_size, dirty});
    }
    return result;
}
bool ImageCache::HasDirtyImages() const {
    return std::ranges::any_of(images, [](const auto& entry) {
        return entry.second->written || entry.second->stencil_written;
    });
}
bool ImageCache::RequestDownloads(std::span<const ResourceCache::Range> ranges, bool all) {
    bool found{};
    for (auto& [key, image] : images) {
        if (!(image->written || image->stencil_written))
            continue;
        if (all || std::ranges::any_of(ranges, [&](const auto& range) {
                return Overlaps(*image, range.address, range.size);
            })) {
            image->download_requested = true;
            found = true;
        }
    }
    return found;
}
void ImageCache::InvalidateCpu(std::span<const ResourceCache::Range> ranges, bool all) {
    for (auto& [key, image] : images) {
        if (!(all || std::ranges::any_of(ranges, [&](const auto& range) {
                  return Overlaps(*image, range.address, range.size);
              })))
            continue;
        if (image->written || image->stencil_written)
            throw std::runtime_error("CPU invalidation requires GPU image writeback first");
        image->cpu_dirty = true;
        image->resident = false;
        image->guest.clear();
        image->stencil_guest.clear();
        ++statistics.cpu_invalidations;
    }
}
void ImageCache::BeginBatch() {
    device->WaitForGpu();
    if (tilizer)
        tilizer->BeginBatch();
    ++batch;
}
ImageCache::Image& ImageCache::GetImage(const VideoCore::ImageInfo& info, DXGI_FORMAT format,
                                        D3D12_RESOURCE_FLAGS flags, u32 base_layer,
                                        u32 layer_count, bool logic_op, u32 logic_mask) {
    if (info.num_bits == 96 && info.props.is_tiled)
        throw std::runtime_error("RGB96 guest images require linear tiling");
    if (!logic_op)
        logic_mask = 0;
    const bool has_stencil =
        info.props.is_depth && info.props.has_stencil && info.stencil_addr != 0;
    const bool has_depth = !info.props.is_depth || info.guest_address != 0;
    if (!info.props.is_depth && (info.stencil_addr || info.stencil_size)) {
        throw std::runtime_error(
            "D3D12 guest image requires unsupported metadata, samples or layout");
    }
    if (!has_depth && !has_stencil) {
        throw std::runtime_error(
            "D3D12 guest image requires unsupported metadata, samples or layout");
    }
    if (has_stencil) {
        if (!info.stencil_size || info.stencil_size > 64 * 1024 * 1024 || info.props.is_volume ||
            info.resources.levels != 1) {
            throw std::runtime_error(
                "D3D12 guest stencil requires unsupported metadata, samples or layout");
        }
    }
    if (has_depth) {
        if (!info.guest_address || !info.guest_size || info.guest_size > 64 * 1024 * 1024)
            throw std::runtime_error(
                "D3D12 guest image requires unsupported metadata, samples or layout");
    } else if (!info.props.is_depth) {
        throw std::runtime_error(
            "D3D12 guest image requires unsupported metadata, samples or layout");
    }
    if (!info.size.width || !info.size.height || info.size.width > 16384 ||
        info.size.height > 16384 || !info.size.depth || info.size.depth > 2048 ||
        (!info.props.is_volume && info.size.depth != 1) || !info.resources.levels ||
        info.resources.levels > 16 || !info.resources.layers || info.resources.layers > 2048 ||
        !std::has_single_bit(info.num_samples) || info.num_samples > 16)
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
    const DXGI_FORMAT view_format =
        logic_op ? DXGI_FORMAT_R32G32B32A32_UINT : ConvertFormat(info.pixel_format);
    if (logic_op) {
        GetLogicOpFormat(ConvertFormat(info.pixel_format));
        format = view_format;
    }
    const bool promoted_depth = has_stencil && info.pixel_format == vk::Format::eD16UnormS8Uint;
    DXGI_FORMAT dsv_format = view_format;
    DXGI_FORMAT resource_format = format;
    DXGI_FORMAT depth_footprint = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT stencil_footprint = DXGI_FORMAT_UNKNOWN;
    if (has_stencil) {
        if (view_format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT) {
            resource_format = DXGI_FORMAT_R32G8X24_TYPELESS;
            depth_footprint = DXGI_FORMAT_R32_TYPELESS;
            stencil_footprint = DXGI_FORMAT_R8_TYPELESS;
        } else if (view_format == DXGI_FORMAT_D24_UNORM_S8_UINT && !multisampled) {
            resource_format = DXGI_FORMAT_R24G8_TYPELESS;
            depth_footprint = DXGI_FORMAT_R32_TYPELESS;
            stencil_footprint = DXGI_FORMAT_R8_TYPELESS;
        } else {
            throw std::runtime_error("Guest stencil format has no D3D12 mapping");
        }
        dsv_format = view_format;
        format = resource_format;
        flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    }
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
    const auto stencil_identity =
        has_stencil ? (canonicalize ? canonicalize(info.stencil_addr, info.stencil_size)
                                    : ResourceCache::Identity{info.stencil_addr, false})
                    : ResourceCache::Identity{0, false};
    const auto identity = has_depth
                              ? (canonicalize ? canonicalize(info.guest_address, info.guest_size)
                                              : ResourceCache::Identity{info.guest_address, false})
                              : ResourceCache::Identity{0, false};
    std::vector<u64> key{identity.address,      info.guest_size,   info.size.width,
                         info.size.height,      info.pitch,        info.resources.levels,
                         info.resources.layers, u64(format),       u64(flags),
                         u64(info.tile_mode),   info.bank_swizzle, u64(info.alt_tile)};
    key.insert(key.end(), {base_layer, layer_count, u64(logic_op), logic_mask});
    if (logic_op)
        key.push_back(u64(info.pixel_format));
    key.insert(key.end(), {info.size.depth, u64(info.type), info.props.is_volume});
    key.insert(key.end(), {info.num_samples, u64(view_format)});
    key.push_back(identity.physical);
    key.insert(key.end(), {stencil_identity.address, u64(info.stencil_size)});
    key.push_back(stencil_identity.physical);
    key.insert(key.end(), {u64(has_stencil), u64(has_depth), u64(promoted_depth)});
    key.insert(key.end(), {u64(info.meta_info.cmask_addr), u64(info.meta_info.fmask_addr),
                           u64(info.meta_info.htile_addr)});
    for (u32 mip = 0; mip < info.resources.levels; ++mip) {
        const auto& layout = info.mips_layout[mip];
        key.insert(key.end(), {layout.size, layout.pitch, layout.height, layout.offset});
    }
    auto found = images.find(key);
    if (persistent) {
        const auto priority = [](const Image& image) {
            return (image.written || image.stencil_written) ? 2
                   : image.resident && !image.cpu_dirty     ? 1
                                                            : 0;
        };
        auto compatible = key;
        compatible[7] = compatible[8] = 0;
        for (auto candidate = images.begin(); candidate != images.end(); ++candidate) {
            auto other = candidate->first;
            other[7] = other[8] = 0;
            const auto description = candidate->second->Native()->GetDesc();
            if (other == compatible && (description.Flags & flags) == flags &&
                (description.Format == format || description.Format == view_format ||
                 format == view_format) &&
                (found == images.end() ||
                 priority(*candidate->second) > priority(*found->second))) {
                found = candidate;
            }
        }
    }
    if (persistent) {
        for (auto& [other_key, other] : images)
            if ((found == images.end() || other.get() != found->second.get()) &&
                (Overlaps(*other, info.guest_address, info.guest_size) ||
                 Overlaps(*other, info.stencil_addr, info.stencil_size)))
                Synchronize(*other);
    }
    if (found != images.end()) {
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
    result->logic_op = logic_op;
    result->logic_mask = logic_mask;
    result->base_layer = base_layer;
    result->native_layers = layer_count;
    result->has_stencil = has_stencil;
    result->has_depth = has_depth;
    result->promoted_depth = promoted_depth;
    result->dsv_format = has_stencil ? dsv_format : DXGI_FORMAT_UNKNOWN;
    result->depth_footprint_format = depth_footprint;
    result->stencil_footprint_format = stencil_footprint;
    D3D12_RESOURCE_DESC staging = desc;
    D3D12_RESOURCE_DESC stencil_planes_desc{};
    if (multisampled) {
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
            throw std::runtime_error("D3D12 multisampling requires a 2D image");
        staging.SampleDesc.Count = 1;
        staging.DepthOrArraySize = layer_count * info.num_samples;
        staging.Format = msaa_format.transfer;
        staging.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        result->msaa = std::make_unique<MsaaTransfer::Resources>();
        if (has_stencil) {
            stencil_planes_desc = staging;
            stencil_planes_desc.Format = DXGI_FORMAT_R8_UINT;
        }
    }
    const UINT count = info.resources.levels * layer_count * info.num_samples;
    u64 bytes = 0;
    u64 stencil_bytes = 0;
    if (has_stencil && !multisampled) {
        const UINT plane_count = info.resources.levels * layer_count;
        result->footprints.resize(has_depth ? plane_count : 0);
        result->rows.resize(has_depth ? plane_count : 0);
        result->row_bytes.resize(has_depth ? plane_count : 0);
        result->stencil_footprints.resize(plane_count);
        result->stencil_rows.resize(plane_count);
        result->stencil_row_bytes.resize(plane_count);
        if (has_depth) {
            D3D12_RESOURCE_DESC depth_staging = desc;
            depth_staging.Format = depth_footprint;
            depth_staging.Flags = D3D12_RESOURCE_FLAG_NONE;
            device->Device()->GetCopyableFootprints(&depth_staging, 0, plane_count, 0,
                                                    result->footprints.data(), result->rows.data(),
                                                    result->row_bytes.data(), &bytes);
        }
        D3D12_RESOURCE_DESC stencil_staging = desc;
        stencil_staging.Format = stencil_footprint;
        stencil_staging.Flags = D3D12_RESOURCE_FLAG_NONE;
        device->Device()->GetCopyableFootprints(
            &stencil_staging, 0, plane_count, 0, result->stencil_footprints.data(),
            result->stencil_rows.data(), result->stencil_row_bytes.data(), &stencil_bytes);
    } else if (has_stencil) {
        result->footprints.resize(has_depth ? count : 0);
        result->rows.resize(has_depth ? count : 0);
        result->row_bytes.resize(has_depth ? count : 0);
        result->stencil_footprints.resize(count);
        result->stencil_rows.resize(count);
        result->stencil_row_bytes.resize(count);
        if (has_depth) {
            device->Device()->GetCopyableFootprints(&staging, 0, count, 0,
                                                    result->footprints.data(), result->rows.data(),
                                                    result->row_bytes.data(), &bytes);
        }
        device->Device()->GetCopyableFootprints(
            &stencil_planes_desc, 0, count, 0, result->stencil_footprints.data(),
            result->stencil_rows.data(), result->stencil_row_bytes.data(), &stencil_bytes);
    } else {
        result->footprints.resize(count);
        result->rows.resize(count);
        result->row_bytes.resize(count);
        device->Device()->GetCopyableFootprints(&staging, 0, count, 0, result->footprints.data(),
                                                result->rows.data(), result->row_bytes.data(),
                                                &bytes);
    }
    const u64 allocation = device->Device()->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
    u64 extra =
        multisampled ? device->Device()->GetResourceAllocationInfo(0, 1, &staging).SizeInBytes : 0;
    u64 stencil_extra = 0;
    if (multisampled && has_stencil) {
        stencil_extra =
            device->Device()->GetResourceAllocationInfo(0, 1, &stencil_planes_desc).SizeInBytes;
    }
    if (allocation == UINT64_MAX || extra == UINT64_MAX || stencil_extra == UINT64_MAX ||
        bytes > 256ULL * 1024 * 1024 || stencil_bytes > 256ULL * 1024 * 1024)
        throw std::runtime_error(fmt::format(
            "Unsupported D3D12 image allocation or footprint: {}x{}x{} levels={} layers={} "
            "samples={} format={:#x} view={:#x} flags={:#x} tile={} guest={:#x}:{} "
            "stencil={:#x}:{} "
            "allocation={} extra={} stencil_extra={} bytes={} stencil_bytes={}",
            info.size.width, info.size.height, info.size.depth, info.resources.levels,
            info.resources.layers, info.num_samples, u32(desc.Format), u32(view_format),
            static_cast<u32>(desc.Flags), u64(info.tile_mode), info.guest_address, info.guest_size,
            info.stencil_addr, info.stencil_size, allocation, extra, stencil_extra, bytes,
            stencil_bytes));
    result->staging_bytes = bytes;
    result->stencil_staging_bytes = stencil_bytes;
    const u64 cpu_required =
        (has_depth ? u64(info.guest_size) * 2 : 0) + (has_stencil ? u64(info.stencil_size) * 2 : 0);
    EnsureSpace(allocation + extra + stencil_extra, cpu_required, true);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = has_stencil ? dsv_format : view_format;
    clear.DepthStencil.Depth = 1.f;
    winrt::check_hresult(device->Device()->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
        (multisampled && info.props.is_depth) || has_stencil ? &clear : nullptr,
        IID_PPV_ARGS(&result->native)));
    result->allocation_bytes = allocation + extra + stencil_extra;
    if (multisampled) {
        winrt::check_hresult(device->Device()->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &staging, D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&result->msaa->planes)));
        if (has_stencil) {
            winrt::check_hresult(device->Device()->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &stencil_planes_desc, D3D12_RESOURCE_STATE_COMMON,
                nullptr, IID_PPV_ARGS(&result->msaa->stencil_planes)));
        }
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
        if (has_stencil) {
            resources.stencil_descriptors.Initialize(
                device->Device(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, true);
            static_cast<void>(resources.stencil_descriptors.Allocate(2));
        }
    }
    result->age = ++age;
    result->batch = batch;
    auto& output = *result;
    images.emplace(std::move(key), std::move(result));
    native_bytes += output.allocation_bytes;
    cpu_bytes +=
        (has_depth ? u64(info.guest_size) * 2 : 0) + (has_stencil ? u64(info.stencil_size) * 2 : 0);
    return output;
}
void ImageCache::EnsureSpace(u64 required, u64 cpu_required, bool inserting) {
    const auto exceeds_budget = [&] {
        const auto stats = allocator.Stats();
        return stats.live_bytes > stats.budget_bytes ||
               native_bytes > stats.budget_bytes - stats.live_bytes ||
               required > stats.budget_bytes - stats.live_bytes - native_bytes;
    };
    while ((inserting && images.size() >= 64) || exceeds_budget() ||
           cpu_required > 256ULL * 1024 * 1024 - cpu_bytes) {
        auto oldest = images.end();
        for (auto it = images.begin(); it != images.end(); ++it)
            if (it->second->batch != batch &&
                (persistent || (!it->second->written && !it->second->stencil_written)) &&
                (oldest == images.end() || it->second->age < oldest->second->age))
                oldest = it;
        if (oldest == images.end())
            throw std::runtime_error("D3D12 image cache budget exhausted");
        if (persistent)
            Synchronize(*oldest->second);
        cpu_bytes -= (oldest->second->has_depth ? u64(oldest->second->info.guest_size) * 2 : 0) +
                     (oldest->second->has_stencil ? u64(oldest->second->info.stencil_size) * 2 : 0);
        native_bytes -= oldest->second->allocation_bytes;
        images.erase(oldest);
    }
}
void ImageCache::EnsureStaging(Image& image, bool download) {
    const auto heap = download ? D3D12_HEAP_TYPE_READBACK : D3D12_HEAP_TYPE_UPLOAD;
    auto& resource = download ? image.readback : image.upload;
    if (image.has_depth && !resource.Get())
        resource = transfer_pool->Acquire(image.staging_bytes, heap);
    auto& stencil_resource = download ? image.stencil_readback : image.stencil_upload;
    if (image.has_stencil && !stencil_resource.Get())
        stencil_resource = transfer_pool->Acquire(image.stencil_staging_bytes, heap);
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
    const auto logic_format =
        image.logic_op ? GetLogicOpFormat(ConvertFormat(image.info.pixel_format)) : LogicOpFormat{};
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
            const u64 guest_row_bytes = image.logic_op
                                            ? u64(footprint.Footprint.Width) * logic_format.bytes
                                            : image.row_bytes[subresource];
            if (guest_row_bytes > pitch || image.rows[subresource] > height)
                throw std::runtime_error("D3D12 footprint exceeds guest mip layout");
            for (u32 slice = 0; slice < footprint.Footprint.Depth; ++slice) {
                for (u32 row = 0; row < image.rows[subresource]; ++row) {
                    const u64 guest_offset =
                        mip_offset +
                        (image.base_layer * image.info.num_samples + layer + slice) * pitch *
                            height +
                        row * pitch;
                    if (guest_offset + guest_row_bytes > image.linear.size())
                        throw std::runtime_error("D3D12 image row exceeds guest backing");
                    auto* guest = image.linear.data() + guest_offset;
                    auto* host =
                        static_cast<u8*>(data) + footprint.Offset +
                        (slice * image.rows[subresource] + row) * footprint.Footprint.RowPitch;
                    if (image.logic_op) {
                        for (u32 x = 0; x < footprint.Footprint.Width; ++x)
                            CopyLogicOpTexel(logic_format, guest + u64(x) * logic_format.bytes,
                                             host + u64(x) * 16, upload, image.logic_mask);
                    } else if (image.info.pixel_format == vk::Format::eB4G4R4A4UnormPack16) {
                        for (u32 x = 0; x < footprint.Footprint.Width; ++x) {
                            u16 value;
                            std::memcpy(&value, (upload ? guest : host) + x * 2, 2);
                            value = ((value & 0xf) << 12) | ((value & 0xf0) << 4) |
                                    ((value & 0xf00) >> 4) | ((value & 0xf000) >> 12);
                            std::memcpy((upload ? host : guest) + x * 2, &value, 2);
                        }
                    } else {
                        std::memcpy(upload ? host : guest, upload ? guest : host,
                                    image.row_bytes[subresource]);
                    }
                }
            }
        }
        mip_offset += layout.size;
    }
}
namespace {
Profile profile;
void CopyStencilRows(ImageCache::Image& image, void* data, bool upload) {
    u64 mip_offset{};
    for (u32 mip = 0; mip < image.info.resources.levels; ++mip) {
        const auto& layout = image.info.mips_layout[mip];
        const u64 pitch = layout.pitch;
        const u64 height = layout.height;
        for (u32 layer = 0; layer < image.native_layers * image.info.num_samples; ++layer) {
            const u32 subresource = mip + layer * image.info.resources.levels;
            const auto& footprint = image.stencil_footprints[subresource];
            if (image.stencil_row_bytes[subresource] > pitch ||
                image.stencil_rows[subresource] > height)
                throw std::runtime_error("D3D12 footprint exceeds guest mip layout");
            for (u32 slice = 0; slice < footprint.Footprint.Depth; ++slice) {
                for (u32 row = 0; row < image.stencil_rows[subresource]; ++row) {
                    const u64 guest_offset =
                        mip_offset +
                        (image.base_layer * image.info.num_samples + layer + slice) * pitch *
                            height +
                        row * pitch;
                    if (guest_offset + image.stencil_row_bytes[subresource] >
                        image.stencil_linear.size())
                        throw std::runtime_error("D3D12 image row exceeds guest backing");
                    auto* guest = image.stencil_linear.data() + guest_offset;
                    auto* host = static_cast<u8*>(data) + footprint.Offset +
                                 (slice * image.stencil_rows[subresource] + row) *
                                     footprint.Footprint.RowPitch;
                    std::memcpy(upload ? host : guest, upload ? guest : host,
                                image.stencil_row_bytes[subresource]);
                }
            }
        }
        mip_offset += image.info.stencil_size / image.native_layers;
    }
}
u32 DepthSubresource(u32 mip, u32 layer, u32 levels) {
    return mip + layer * levels;
}
VideoCore::ImageInfo StencilImageInfo(const VideoCore::ImageInfo& info) {
    VideoCore::ImageInfo stencil_info = info;
    stencil_info.num_bits = 8;
    stencil_info.guest_size = info.stencil_size;
    stencil_info.props.is_block = 0;
    const u32 depth_bytes = std::max<u32>(info.num_bits / 8, 1);
    for (u32 mip = 0; mip < info.resources.levels; ++mip) {
        auto& layout = stencil_info.mips_layout[mip];
        layout.size /= depth_bytes;
        layout.offset /= depth_bytes;
    }
    return stencil_info;
}
u32 StencilSubresource(u32 mip, u32 layer, u32 levels, u32 layers) {
    return mip + layer * levels + levels * layers;
}
} // namespace
void ImageCache::Upload(Image& image, ID3D12GraphicsCommandList* commands, VAddr read_address,
                        VAddr stencil_read_address) {
    if (persistent && image.resident && !image.cpu_dirty &&
        (!read_address || read_address == image.info.guest_address) &&
        (!stencil_read_address || stencil_read_address == image.info.stencil_addr)) {
        ++statistics.resident_hits;
        return;
    }
    image.resident = true;
    image.cpu_dirty = false;
    ++statistics.uploads;
    if (image.has_stencil) {
        if (image.msaa)
            UploadMsaaStencil(image, commands, read_address, stencil_read_address);
        else
            UploadStencil(image, commands, read_address, stencil_read_address);
        return;
    }
    bool changed{};
    bool gpu_detile{};
    ProfileSample(profile, "upload_read", 128, [&] {
        std::vector<u8> guest(image.info.guest_size);
        read(read_address ? read_address : image.info.guest_address, guest);
        changed = image.msaa || guest != image.guest;
        if (!changed)
            return;
        image.guest = std::move(guest);
        const auto entries = TilingRuntime::Entries(image.info, image.footprints);
        if (!image.logic_op && tilizer && TilingRuntime::Supported(image.info, entries.size()) &&
            tilizer->CanRecord(image.info, entries, false)) {
            gpu_detile = true;
            return;
        }
        image.linear.resize(image.guest.size());
        VideoCore::ConvertTiling(image.guest, image.linear, image.info, false);
        EnsureStaging(image, false);
        void* mapped;
        const D3D12_RANGE empty{0, 0};
        winrt::check_hresult(image.upload->Map(0, &empty, &mapped));
        CopyRows(image, mapped, true);
        image.upload->Unmap(0, nullptr);
    });
    if (!changed) {
        return;
    }
    if (gpu_detile) {
        ProfileSample(profile, "upload_copy", 128, [&] {
            Barrier(image, commands, D3D12_RESOURCE_STATE_COPY_DEST);
            const auto entries = TilingRuntime::Entries(image.info, image.footprints);
            tilizer->Detile(commands, image.info, entries, image.guest, image.Native(),
                            image.footprints);
        });
        return;
    }
    ProfileSample(profile, "upload_copy", 128, [&] {
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
                             image.logic_op ? DXGI_FORMAT_R32G32B32A32_UINT
                                            : ConvertFormat(image.info.pixel_format),
                             image.info.props.is_depth, true);
        }
    });
}
void ImageCache::UploadStencil(Image& image, ID3D12GraphicsCommandList* commands,
                               VAddr read_address, VAddr stencil_read_address) {
    const VAddr depth_source = read_address ? read_address : image.info.guest_address;
    const VAddr stencil_source =
        stencil_read_address ? stencil_read_address : image.info.stencil_addr;
    bool depth_changed = false;
    bool stencil_changed = false;
    std::vector<u8> depth_float_bytes;
    const u8* depth_linear_bytes = nullptr;
    if (image.has_depth) {
        std::vector<u8> guest(image.info.guest_size);
        read(depth_source, guest);
        depth_changed = guest != image.guest;
        if (depth_changed) {
            image.guest = std::move(guest);
            image.linear.resize(image.guest.size());
            VideoCore::ConvertTiling(image.guest, image.linear, image.info, false);
        }
        if (image.promoted_depth) {
            const size_t pixels = image.info.stencil_size;
            depth_float_bytes.resize(pixels * 4);
            const auto* src = reinterpret_cast<const u16*>(image.linear.data());
            auto* dst = reinterpret_cast<float*>(depth_float_bytes.data());
            for (size_t i = 0; i < pixels; ++i) {
                dst[i] = float(src[i]) * (1.f / 65535.f);
            }
            depth_linear_bytes = depth_float_bytes.data();
        } else {
            depth_linear_bytes = image.linear.data();
        }
    }
    if (image.has_stencil) {
        std::vector<u8> guest(image.info.stencil_size);
        read(stencil_source, guest);
        stencil_changed = guest != image.stencil_guest;
        if (stencil_changed) {
            image.stencil_guest = std::move(guest);
            image.stencil_linear.resize(image.stencil_guest.size());
            const VideoCore::ImageInfo stencil_info = StencilImageInfo(image.info);
            VideoCore::ConvertTiling(image.stencil_guest, image.stencil_linear, stencil_info,
                                     false);
        }
    }
    if (!depth_changed && !stencil_changed) {
        return;
    }
    EnsureStaging(image, false);
    const D3D12_RANGE empty{0, 0};
    if (image.has_depth && depth_changed) {
        void* mapped;
        winrt::check_hresult(image.upload->Map(0, &empty, &mapped));
        if (image.promoted_depth) {
            u64 mip_offset = 0;
            for (u32 mip = 0; mip < image.info.resources.levels; ++mip) {
                const auto& layout = image.info.mips_layout[mip];
                const u64 pitch = layout.pitch * 4;
                for (u32 layer = 0; layer < image.native_layers; ++layer) {
                    const u32 subresource = mip + layer * image.info.resources.levels;
                    const auto& footprint = image.footprints[subresource];
                    for (u32 row = 0; row < image.rows[subresource]; ++row) {
                        const u64 guest_offset =
                            mip_offset + (image.base_layer + layer) * pitch * layout.height +
                            row * pitch;
                        auto* guest = depth_float_bytes.data() + guest_offset;
                        auto* host = static_cast<u8*>(mapped) + footprint.Offset +
                                     row * footprint.Footprint.RowPitch;
                        std::memcpy(host, guest, image.row_bytes[subresource]);
                    }
                }
                mip_offset += image.info.stencil_size * 4 / image.native_layers;
            }
        } else {
            CopyRows(image, mapped, true);
        }
        image.upload->Unmap(0, nullptr);
    }
    if (image.has_stencil && stencil_changed) {
        void* mapped;
        winrt::check_hresult(image.stencil_upload->Map(0, &empty, &mapped));
        CopyStencilRows(image, mapped, true);
        image.stencil_upload->Unmap(0, nullptr);
    }
    Barrier(image, commands, D3D12_RESOURCE_STATE_COPY_DEST);
    const u32 levels = image.info.resources.levels;
    for (u32 layer = 0; layer < image.native_layers; ++layer) {
        if (image.has_depth && depth_changed) {
            D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
            source.pResource = image.upload.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = image.footprints[layer];
            destination.pResource = image.Native();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.SubresourceIndex = DepthSubresource(0, layer, levels);
            commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }
        if (image.has_stencil && stencil_changed) {
            D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
            source.pResource = image.stencil_upload.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = image.stencil_footprints[layer];
            destination.pResource = image.Native();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.SubresourceIndex =
                StencilSubresource(0, layer, levels, image.native_layers);
            commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }
    }
}
void ImageCache::UploadMsaaStencil(Image& image, ID3D12GraphicsCommandList* commands,
                                   VAddr read_address, VAddr stencil_read_address) {
    const VAddr depth_source = read_address ? read_address : image.info.guest_address;
    const VAddr stencil_source =
        stencil_read_address ? stencil_read_address : image.info.stencil_addr;
    bool depth_changed = false;
    bool stencil_changed = false;
    if (image.has_depth) {
        std::vector<u8> guest(image.info.guest_size);
        read(depth_source, guest);
        depth_changed = guest != image.guest;
        if (depth_changed) {
            image.guest = std::move(guest);
            image.linear.resize(image.guest.size());
            VideoCore::ConvertTiling(image.guest, image.linear, image.info, false);
        }
    }
    if (image.has_stencil) {
        std::vector<u8> guest(image.info.stencil_size);
        read(stencil_source, guest);
        stencil_changed = guest != image.stencil_guest;
        if (stencil_changed) {
            image.stencil_guest = std::move(guest);
            image.stencil_linear.resize(image.stencil_guest.size());
            const VideoCore::ImageInfo stencil_info = StencilImageInfo(image.info);
            VideoCore::ConvertTiling(image.stencil_guest, image.stencil_linear, stencil_info,
                                     false);
        }
    }
    if (!depth_changed && !stencil_changed) {
        return;
    }
    std::vector<u8> depth_float_bytes;
    if (image.has_depth && image.promoted_depth && depth_changed) {
        depth_float_bytes.resize(image.info.stencil_size * 4);
        const auto* src = reinterpret_cast<const u16*>(image.linear.data());
        auto* dst = reinterpret_cast<float*>(depth_float_bytes.data());
        for (size_t i = 0; i < image.info.stencil_size; ++i) {
            dst[i] = float(src[i]) * (1.f / 65535.f);
        }
    }
    EnsureStaging(image, false);
    const D3D12_RANGE empty{0, 0};
    const auto& layout = image.info.mips_layout[0];
    if (image.has_depth && depth_changed) {
        void* mapped;
        winrt::check_hresult(image.upload->Map(0, &empty, &mapped));
        if (image.promoted_depth) {
            const u64 pitch = u64(layout.pitch) * 4;
            for (u32 ls = 0; ls < image.native_layers * image.info.num_samples; ++ls) {
                const auto& footprint = image.footprints[ls];
                for (u32 row = 0; row < image.rows[ls]; ++row) {
                    const u64 guest_offset =
                        (image.base_layer * image.info.num_samples + ls) * pitch * layout.height +
                        row * pitch;
                    auto* guest = depth_float_bytes.data() + guest_offset;
                    auto* host = static_cast<u8*>(mapped) + footprint.Offset +
                                 row * footprint.Footprint.RowPitch;
                    std::memcpy(host, guest, image.row_bytes[ls]);
                }
            }
        } else {
            CopyRows(image, mapped, true);
        }
        image.upload->Unmap(0, nullptr);
    }
    if (image.has_stencil && stencil_changed) {
        void* mapped;
        winrt::check_hresult(image.stencil_upload->Map(0, &empty, &mapped));
        CopyStencilRows(image, mapped, true);
        image.stencil_upload->Unmap(0, nullptr);
    }
    MsaaTransfer::Barrier(*image.msaa, commands, D3D12_RESOURCE_STATE_COPY_DEST);
    MsaaTransfer::StencilBarrier(*image.msaa, commands, D3D12_RESOURCE_STATE_COPY_DEST);
    for (u32 ls = 0; ls < image.native_layers * image.info.num_samples; ++ls) {
        if (image.has_depth && depth_changed) {
            D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
            source.pResource = image.upload.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = image.footprints[ls];
            destination.pResource = image.msaa->planes.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.SubresourceIndex = ls;
            commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }
        if (image.has_stencil && stencil_changed) {
            D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
            source.pResource = image.stencil_upload.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = image.stencil_footprints[ls];
            destination.pResource = image.msaa->stencil_planes.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.SubresourceIndex = ls;
            commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }
    }
    SetSamplePositions(commands, image.info.num_samples, image.sample_positions);
    Barrier(image, commands, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    if (image.has_depth && depth_changed) {
        transfers.Record(commands, image.Native(), *image.msaa,
                         ConvertFormat(image.info.pixel_format), true, true);
    }
    if (image.has_stencil && stencil_changed) {
        transfers.RecordStencil(commands, image.Native(), *image.msaa, image.dsv_format, true);
    }
}
void ImageCache::PrepareStencilDownloads(Image& image, ID3D12GraphicsCommandList* commands) {
    EnsureStaging(image, true);
    const u32 levels = image.info.resources.levels;
    for (u32 layer = 0; layer < image.native_layers; ++layer) {
        if (image.has_depth && image.written) {
            D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
            source.pResource = image.Native();
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            source.SubresourceIndex = DepthSubresource(0, layer, levels);
            destination.pResource = image.readback.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint = image.footprints[layer];
            commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }
        if (image.has_stencil && image.stencil_written) {
            D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
            source.pResource = image.Native();
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            source.SubresourceIndex = StencilSubresource(0, layer, levels, image.native_layers);
            destination.pResource = image.stencil_readback.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint = image.stencil_footprints[layer];
            commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }
    }
}
void ImageCache::CommitStencilDownloads(Image& image) {
    if (image.has_depth && image.written) {
        image.guest.resize(image.info.guest_size);
        image.linear.resize(image.guest.size());
        read(image.info.guest_address, image.guest);
        VideoCore::ConvertTiling(image.guest, image.linear, image.info, false);
        void* mapped;
        winrt::check_hresult(image.readback->Map(0, nullptr, &mapped));
        if (image.promoted_depth) {
            std::vector<u8> float_bytes(image.info.stencil_size * 4);
            const auto* depth = reinterpret_cast<const u16*>(image.linear.data());
            auto* initial = reinterpret_cast<float*>(float_bytes.data());
            for (size_t i = 0; i < image.info.stencil_size; ++i)
                initial[i] = float(depth[i]) * (1.f / 65535.f);
            u64 mip_offset = 0;
            for (u32 mip = 0; mip < image.info.resources.levels; ++mip) {
                const auto& layout = image.info.mips_layout[mip];
                const u64 pitch = layout.pitch * 4;
                for (u32 layer = 0; layer < image.native_layers; ++layer) {
                    const u32 subresource = mip + layer * image.info.resources.levels;
                    const auto& footprint = image.footprints[subresource];
                    for (u32 row = 0; row < image.rows[subresource]; ++row) {
                        const u64 guest_offset =
                            mip_offset + (image.base_layer + layer) * pitch * layout.height +
                            row * pitch;
                        auto* guest = float_bytes.data() + guest_offset;
                        auto* host = static_cast<u8*>(mapped) + footprint.Offset +
                                     row * footprint.Footprint.RowPitch;
                        std::memcpy(guest, host, image.row_bytes[subresource]);
                    }
                }
                mip_offset += image.info.stencil_size * 4 / image.native_layers;
            }
            const auto* src = reinterpret_cast<const float*>(float_bytes.data());
            auto* dst = reinterpret_cast<u16*>(image.linear.data());
            for (size_t i = 0; i < image.info.stencil_size; ++i) {
                const float clamped = std::clamp(src[i], 0.f, 1.f);
                dst[i] = u16(clamped * 65535.f + 0.5f);
            }
        } else {
            CopyRows(image, mapped, false);
        }
        const D3D12_RANGE empty{0, 0};
        image.readback->Unmap(0, &empty);
        VideoCore::ConvertTiling(image.linear, image.guest, image.info, true);
        write(image.info.guest_address, image.guest);
        image.written = false;
    }
    if (image.has_stencil && image.stencil_written) {
        image.stencil_guest.resize(image.info.stencil_size);
        image.stencil_linear.resize(image.stencil_guest.size());
        read(image.info.stencil_addr, image.stencil_guest);
        const VideoCore::ImageInfo stencil_info = StencilImageInfo(image.info);
        VideoCore::ConvertTiling(image.stencil_guest, image.stencil_linear, stencil_info, false);
        void* mapped;
        winrt::check_hresult(image.stencil_readback->Map(0, nullptr, &mapped));
        CopyStencilRows(image, mapped, false);
        const D3D12_RANGE empty{0, 0};
        image.stencil_readback->Unmap(0, &empty);
        VideoCore::ConvertTiling(image.stencil_linear, image.stencil_guest, stencil_info, true);
        write(image.info.stencil_addr, image.stencil_guest);
        image.stencil_written = false;
    }
}
void ImageCache::PrepareMsaaStencilDownloads(Image& image, ID3D12GraphicsCommandList* commands) {
    EnsureStaging(image, true);
    Barrier(image, commands, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    SetSamplePositions(commands, 1, {});
    if (image.has_depth && image.written) {
        transfers.Record(commands, image.Native(), *image.msaa,
                         ConvertFormat(image.info.pixel_format), true, false);
    }
    if (image.has_stencil && image.stencil_written) {
        transfers.RecordStencil(commands, image.Native(), *image.msaa, image.dsv_format, false);
    }
    if (image.has_depth && image.written) {
        MsaaTransfer::Barrier(*image.msaa, commands, D3D12_RESOURCE_STATE_COPY_SOURCE);
        for (u32 ls = 0; ls < image.native_layers * image.info.num_samples; ++ls) {
            D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
            source.pResource = image.msaa->planes.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            source.SubresourceIndex = ls;
            destination.pResource = image.readback.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint = image.footprints[ls];
            commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }
    }
    if (image.has_stencil && image.stencil_written) {
        MsaaTransfer::StencilBarrier(*image.msaa, commands, D3D12_RESOURCE_STATE_COPY_SOURCE);
        for (u32 ls = 0; ls < image.native_layers * image.info.num_samples; ++ls) {
            D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
            source.pResource = image.msaa->stencil_planes.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            source.SubresourceIndex = ls;
            destination.pResource = image.stencil_readback.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint = image.stencil_footprints[ls];
            commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }
    }
}
void ImageCache::CommitMsaaStencilDownloads(Image& image) {
    if (image.has_depth && image.written) {
        image.guest.resize(image.info.guest_size);
        image.linear.resize(image.guest.size());
        read(image.info.guest_address, image.guest);
        VideoCore::ConvertTiling(image.guest, image.linear, image.info, false);
        void* mapped;
        winrt::check_hresult(image.readback->Map(0, nullptr, &mapped));
        if (image.promoted_depth) {
            std::vector<u8> float_bytes(image.info.stencil_size * 4);
            const auto* depth = reinterpret_cast<const u16*>(image.linear.data());
            auto* initial = reinterpret_cast<float*>(float_bytes.data());
            for (size_t i = 0; i < image.info.stencil_size; ++i)
                initial[i] = float(depth[i]) * (1.f / 65535.f);
            const auto& layout = image.info.mips_layout[0];
            const u64 pitch = u64(layout.pitch) * 4;
            for (u32 ls = 0; ls < image.native_layers * image.info.num_samples; ++ls) {
                const auto& footprint = image.footprints[ls];
                for (u32 row = 0; row < image.rows[ls]; ++row) {
                    const u64 guest_offset =
                        (image.base_layer * image.info.num_samples + ls) * pitch * layout.height +
                        row * pitch;
                    auto* guest = float_bytes.data() + guest_offset;
                    auto* host = static_cast<u8*>(mapped) + footprint.Offset +
                                 row * footprint.Footprint.RowPitch;
                    std::memcpy(guest, host, image.row_bytes[ls]);
                }
            }
            const auto* src = reinterpret_cast<const float*>(float_bytes.data());
            auto* dst = reinterpret_cast<u16*>(image.linear.data());
            for (size_t i = 0; i < image.info.stencil_size; ++i) {
                const float clamped = std::clamp(src[i], 0.f, 1.f);
                dst[i] = u16(clamped * 65535.f + 0.5f);
            }
        } else {
            CopyRows(image, mapped, false);
        }
        const D3D12_RANGE empty{0, 0};
        image.readback->Unmap(0, &empty);
        VideoCore::ConvertTiling(image.linear, image.guest, image.info, true);
        write(image.info.guest_address, image.guest);
        image.written = false;
    }
    if (image.has_stencil && image.stencil_written) {
        image.stencil_guest.resize(image.info.stencil_size);
        image.stencil_linear.resize(image.stencil_guest.size());
        read(image.info.stencil_addr, image.stencil_guest);
        const VideoCore::ImageInfo stencil_info = StencilImageInfo(image.info);
        VideoCore::ConvertTiling(image.stencil_guest, image.stencil_linear, stencil_info, false);
        void* mapped;
        winrt::check_hresult(image.stencil_readback->Map(0, nullptr, &mapped));
        CopyStencilRows(image, mapped, false);
        const D3D12_RANGE empty{0, 0};
        image.stencil_readback->Unmap(0, &empty);
        VideoCore::ConvertTiling(image.stencil_linear, image.stencil_guest, stencil_info, true);
        write(image.info.stencil_addr, image.stencil_guest);
        image.stencil_written = false;
    }
}
void ImageCache::PrepareDownloads(ID3D12GraphicsCommandList* commands) {
    if (persistent) {
        for (auto& [key, owner] : images) {
            if (!(owner->written || owner->stencil_written))
                continue;
            for (auto& [other_key, other] : images) {
                if (owner.get() == other.get() ||
                    !(Overlaps(*other, owner->info.guest_address, owner->info.guest_size) ||
                      Overlaps(*other, owner->info.stencil_addr, owner->info.stencil_size)))
                    continue;
                if (other->written || other->stencil_written) {
                    owner->download_requested = true;
                    other->download_requested = true;
                    owner->invalidate_after_download = true;
                    other->invalidate_after_download = true;
                } else {
                    other->cpu_dirty = true;
                    other->resident = false;
                    other->guest.clear();
                    other->stencil_guest.clear();
                }
            }
        }
    }
    for (auto& [key, image] : images) {
        if (persistent && !image->download_requested)
            continue;
        if (image->has_stencil) {
            if (image->written || image->stencil_written) {
                if (image->msaa)
                    PrepareMsaaStencilDownloads(*image, commands);
                else {
                    Barrier(*image, commands, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    PrepareStencilDownloads(*image, commands);
                }
            }
        } else if (image->written) {
            EnsureStaging(*image, true);
            if (image->msaa) {
                Barrier(*image, commands, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                SetSamplePositions(commands, 1, {});
                transfers.Record(commands, image->Native(), *image->msaa,
                                 image->logic_op ? DXGI_FORMAT_R32G32B32A32_UINT
                                                 : ConvertFormat(image->info.pixel_format),
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
            if (!image->msaa) {
                const auto entries = TilingRuntime::Entries(image->info, image->footprints);
                if (!image->logic_op && tilizer &&
                    TilingRuntime::Supported(image->info, entries.size()) &&
                    tilizer->CanRecord(image->info, entries, true)) {
                    image->gpu_tiled = true;
                    std::vector<u8> guest(image->info.guest_size);
                    read(image->info.guest_address, guest);
                    image->tiled_readback =
                        tilizer->Tile(commands, image->info, entries, image->Native(),
                                      image->footprints, guest);
                }
            }
        }
        Barrier(*image, commands, D3D12_RESOURCE_STATE_COMMON);
        if (image->msaa) {
            MsaaTransfer::Barrier(*image->msaa, commands, D3D12_RESOURCE_STATE_COMMON);
            if (image->has_stencil)
                MsaaTransfer::StencilBarrier(*image->msaa, commands, D3D12_RESOURCE_STATE_COMMON);
        }
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
void ImageCache::RetireUploads(u64 fence) {
    transfer_fence = fence;
    for (auto& [key, image] : images) {
        transfer_pool->Retire(std::move(image->upload), fence);
        transfer_pool->Retire(std::move(image->stencil_upload), fence);
    }
    if (tilizer)
        tilizer->RetireUploads(fence);
}
void ImageCache::CommitDownloads() {
    const u64 fence = std::max(transfer_fence, device->LastSignaledValue());
    if (device->CompletedValue() < fence)
        throw std::runtime_error("D3D12 image readback requested before transfer fence");
    for (auto& [key, image] : images) {
        if (persistent && !image->download_requested)
            continue;
        if (image->written || image->stencil_written)
            ++statistics.downloads;
        image->download_requested = false;
        if (image->has_stencil) {
            if (image->msaa)
                CommitMsaaStencilDownloads(*image);
            else
                CommitStencilDownloads(*image);
            continue;
        }
        if (!image->written)
            continue;
        void* mapped;
        const auto entries = TilingRuntime::Entries(image->info, image->footprints);
        const bool tiled =
            image->gpu_tiled && TilingRuntime::Supported(image->info, entries.size());
        if (tiled) {
            winrt::check_hresult(image->tiled_readback->Map(0, nullptr, &mapped));
        } else {
            winrt::check_hresult(image->readback->Map(0, nullptr, &mapped));
        }
        if (tiled) {
            image->guest.resize(image->info.guest_size);
            std::memcpy(image->guest.data(), mapped, image->guest.size());
        } else {
            image->guest.resize(image->info.guest_size);
            read(image->info.guest_address, image->guest);
            image->linear.resize(image->guest.size());
            VideoCore::ConvertTiling(image->guest, image->linear, image->info, false);
            CopyRows(*image, mapped, false);
            VideoCore::ConvertTiling(image->linear, image->guest, image->info, true);
        }
        image->gpu_tiled = false;
        const D3D12_RANGE empty{0, 0};
        if (tiled)
            image->tiled_readback->Unmap(0, &empty);
        else
            image->readback->Unmap(0, &empty);
        write(image->info.guest_address, image->guest);
        image->written = false;
    }
    for (auto& [key, image] : images) {
        if (image->invalidate_after_download) {
            image->invalidate_after_download = false;
            image->resident = false;
            image->cpu_dirty = true;
            image->guest.clear();
            image->stencil_guest.clear();
        }
    }
    RetireUploads(fence);
    for (auto& [key, image] : images) {
        transfer_pool->Retire(std::move(image->readback), fence);
        transfer_pool->Retire(std::move(image->stencil_readback), fence);
        image->tiled_readback = nullptr;
        if (persistent) {
            std::vector<u8>{}.swap(image->guest);
            std::vector<u8>{}.swap(image->linear);
            std::vector<u8>{}.swap(image->stencil_guest);
            std::vector<u8>{}.swap(image->stencil_linear);
        }
    }
    if (tilizer)
        tilizer->ReleaseReadbacks(fence);
}
} // namespace D3D12
