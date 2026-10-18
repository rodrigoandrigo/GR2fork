#include "video_core/renderer_d3d12/tiling_runtime.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <utility>
#include <winrt/base.h>
#include "video_core/amdgpu/tiling.h"
#include "video_core/renderer_d3d12/shader_cache.h"

namespace D3D12 {
namespace {

constexpr std::string_view TilingShader = R"(
struct TilingInfo {
    uint bank_swizzle;
    uint micro_tiled_mips;
    uint num_entries;
    uint pad;
    uint4 entries_a[16];
    uint4 entries_b[16];
};

ByteAddressBuffer source_data : register(t0);
RWByteAddressBuffer dest_data : register(u0);
ConstantBuffer<TilingInfo> info : register(b0);

#define BYTES_PER_PIXEL (BITS_PER_PIXEL / 8u)
#define MICRO_TILE_WIDTH 8
#define MICRO_TILE_HEIGHT 8
#define MICRO_TILE_PIXELS (MICRO_TILE_WIDTH * MICRO_TILE_HEIGHT)
#define MICRO_TILE_BITS (MICRO_TILE_PIXELS * MICRO_TILE_THICKNESS * BITS_PER_PIXEL * NUM_SAMPLES)
#define MICRO_TILE_BYTES (MICRO_TILE_BITS / 8u)
#define NUM_PIPE_INTERLEAVE_BITS 8

#if IS_MACRO_TILED
#define NUM_PIPES (PIPE_CONFIG == 0 ? 2u : PIPE_CONFIG == 16 ? 16u : 8u)
#define NUM_PIPE_BITS (PIPE_CONFIG == 0 ? 1u : PIPE_CONFIG == 16 ? 4u : 3u)
#endif

#define ADDR_SURF_DISPLAY_MICRO_TILING 0
#define ADDR_SURF_THIN_MICRO_TILING 1
#define ADDR_SURF_DEPTH_MICRO_TILING 2
#define ADDR_SURF_ROTATED_MICRO_TILING 3

#define ARRAY_LINEAR_GENERAL 0
#define ARRAY_LINEAR_ALIGNED 1
#define ARRAY_1D_TILED_THIN1 2
#define ARRAY_1D_TILED_THICK 3
#define ARRAY_2D_TILED_THIN1 4
#define ARRAY_PRT_TILED_THIN1 5
#define ARRAY_PRT_2D_TILED_THIN1 6
#define ARRAY_2D_TILED_THICK 7
#define ARRAY_2D_TILED_XTHICK 8
#define ARRAY_PRT_TILED_THICK 9
#define ARRAY_PRT_2D_TILED_THICK 10
#define ARRAY_PRT_3D_TILED_THIN1 11
#define ARRAY_3D_TILED_THIN1 12
#define ARRAY_3D_TILED_THICK 13
#define ARRAY_3D_TILED_XTHICK 14
#define ARRAY_PRT_3D_TILED_THICK 15

#define ADDR_SURF_P2 0
#define ADDR_SURF_P8_32x32_8x16 10
#define ADDR_SURF_P8_32x32_16x16 12

#define BITS_TO_BYTES(x) (((x) + 7u) / 8u)
#define BITOF(v, b) ((v >> (b)) & 1u)

uint ComputePixelIndexWithinMicroTile(uint x, uint y, uint z, uint thickness) {
    uint p0 = 0, p1 = 0, p2 = 0, p3 = 0, p4 = 0, p5 = 0, p6 = 0, p7 = 0, p8 = 0;
    uint x0 = BITOF(x, 0), x1 = BITOF(x, 1), x2 = BITOF(x, 2);
    uint y0 = BITOF(y, 0), y1 = BITOF(y, 1), y2 = BITOF(y, 2);
    uint z0 = BITOF(z, 0), z1 = BITOF(z, 1), z2 = BITOF(z, 2);

    if (MICRO_TILE_MODE == ADDR_SURF_DISPLAY_MICRO_TILING) {
#if BITS_PER_PIXEL == 8
        p0 = x0; p1 = x1; p2 = x2; p3 = y1; p4 = y0; p5 = y2;
#elif BITS_PER_PIXEL == 16
        p0 = x0; p1 = x1; p2 = x2; p3 = y0; p4 = y1; p5 = y2;
#elif BITS_PER_PIXEL == 32
        p0 = x0; p1 = x1; p2 = y0; p3 = x2; p4 = y1; p5 = y2;
#elif BITS_PER_PIXEL == 64
        p0 = x0; p1 = y0; p2 = x1; p3 = x2; p4 = y1; p5 = y2;
#elif BITS_PER_PIXEL == 128
        p0 = x0; p1 = y0; p2 = x1; p3 = y1; p4 = x2; p5 = y2;
#endif
    } else if (MICRO_TILE_MODE == ADDR_SURF_THIN_MICRO_TILING ||
               MICRO_TILE_MODE == ADDR_SURF_DEPTH_MICRO_TILING) {
        p0 = x0; p1 = y0; p2 = x1; p3 = y1; p4 = x2; p5 = y2;
    } else {
#if BITS_PER_PIXEL == 8 || BITS_PER_PIXEL == 16
        p0 = x0; p1 = y0; p2 = x1; p3 = y1; p4 = z0; p5 = z1;
#elif BITS_PER_PIXEL == 32
        p0 = x0; p1 = y0; p2 = x1; p3 = z0; p4 = y1; p5 = z1;
#elif BITS_PER_PIXEL == 64 || BITS_PER_PIXEL == 128
        p0 = x0; p1 = y0; p2 = z0; p3 = x1; p4 = y1; p5 = z1;
#endif
        p6 = x2;
        p7 = y2;
        if (thickness == 8) {
            p8 = z2;
        }
    }
    return p0 | (p1 << 1u) | (p2 << 2u) | (p3 << 3u) | (p4 << 4u) | (p5 << 5u) | (p6 << 6u) |
           (p7 << 7u) | (p8 << 8u);
}

uint ComputeSurfaceAddrFromCoordMicroTiled(uint x, uint y, uint slice, uint pitch, uint height,
                                           uint thickness, uint sample_index) {
    uint micro_tile_bytes = MICRO_TILE_PIXELS * thickness * BITS_PER_PIXEL * NUM_SAMPLES / 8u;
    uint slice_bytes = BITS_TO_BYTES(pitch * height * thickness * BITS_PER_PIXEL * NUM_SAMPLES);
    uint micro_tiles_per_row = pitch / MICRO_TILE_WIDTH;
    uint micro_tile_index_x = x / MICRO_TILE_WIDTH;
    uint micro_tile_index_y = y / MICRO_TILE_HEIGHT;
    uint micro_tile_index_z = slice / thickness;
    uint slice_offset = micro_tile_index_z * slice_bytes;
    uint micro_tile_offset =
        (micro_tile_index_y * micro_tiles_per_row + micro_tile_index_x) * micro_tile_bytes;
    uint pixel_index = ComputePixelIndexWithinMicroTile(x, y, slice, thickness);
    uint sample_offset, pixel_offset;
    if (MICRO_TILE_MODE == ADDR_SURF_DEPTH_MICRO_TILING) {
        sample_offset = sample_index * BITS_PER_PIXEL;
        pixel_offset = pixel_index * BITS_PER_PIXEL * NUM_SAMPLES;
    } else {
        sample_offset = sample_index * (micro_tile_bytes * 8u / NUM_SAMPLES);
        pixel_offset = pixel_index * BITS_PER_PIXEL;
    }
    return slice_offset + micro_tile_offset + (sample_offset + pixel_offset) / 8u;
}

#if IS_MACRO_TILED
uint ComputePipeFromCoord(uint x, uint y, uint slice) {
    uint p0 = 0, p1 = 0, p2 = 0;
    uint tx = x / MICRO_TILE_WIDTH;
    uint ty = y / MICRO_TILE_HEIGHT;
    uint x3 = BITOF(tx, 0), x4 = BITOF(tx, 1), x5 = BITOF(tx, 2);
    uint y3 = BITOF(ty, 0), y4 = BITOF(ty, 1), y5 = BITOF(ty, 2);

    if (PIPE_CONFIG == ADDR_SURF_P2) {
        p0 = x3 ^ y3;
    } else if (PIPE_CONFIG == ADDR_SURF_P8_32x32_8x16) {
        p0 = x4 ^ y3 ^ x5;
        p1 = x3 ^ y4;
        p2 = x5 ^ y5;
    } else if (PIPE_CONFIG == ADDR_SURF_P8_32x32_16x16) {
        p0 = x3 ^ y3 ^ x4;
        p1 = x4 ^ y4;
        p2 = x5 ^ y5;
    }
    uint p3 = 0;
    if (PIPE_CONFIG == 16) {
        p0 = x4 ^ y3;
        p1 = x3 ^ y4;
        p2 = x5 ^ BITOF(ty, 3);
        p3 = BITOF(tx, 3) ^ y5;
    }
    uint pipe = p0 | (p1 << 1u) | (p2 << 2u) | (p3 << 3u);
    uint pipe_swizzle = 0;
    if (ARRAY_MODE == ARRAY_3D_TILED_THIN1 || ARRAY_MODE == ARRAY_3D_TILED_THICK ||
        ARRAY_MODE == ARRAY_3D_TILED_XTHICK) {
        pipe_swizzle += max(1u, NUM_PIPES / 2u - 1u) * (slice / MICRO_TILE_THICKNESS);
    }
    pipe_swizzle &= (NUM_PIPES - 1u);
    return pipe ^ pipe_swizzle;
}

uint ComputeBankFromCoord(uint x, uint y, uint slice, uint tile_split_slice) {
    uint b0 = 0, b1 = 0, b2 = 0, b3 = 0, slice_rotation = 0, tile_split_rotation = 0;
    uint tx = x / MICRO_TILE_WIDTH / (BANK_WIDTH * NUM_PIPES);
    uint ty = y / MICRO_TILE_HEIGHT / BANK_HEIGHT;
    uint x3 = BITOF(tx, 0), x4 = BITOF(tx, 1), x5 = BITOF(tx, 2), x6 = BITOF(tx, 3);
    uint y3 = BITOF(ty, 0), y4 = BITOF(ty, 1), y5 = BITOF(ty, 2), y6 = BITOF(ty, 3);

    if (NUM_BANKS == 16) {
        b0 = x3 ^ y6;
        b1 = x4 ^ y5 ^ y6;
        b2 = x5 ^ y4;
        b3 = x6 ^ y3;
    } else if (NUM_BANKS == 8) {
        b0 = x3 ^ y5;
        b1 = x4 ^ y4 ^ y5;
        b2 = x5 ^ y3;
    } else if (NUM_BANKS == 4) {
        b0 = x3 ^ y4;
        b1 = x4 ^ y3;
    } else if (NUM_BANKS == 2) {
        b0 = x3 ^ y3;
    }
    uint bank = b0 | (b1 << 1u) | (b2 << 2u) | (b3 << 3u);

    if (ARRAY_MODE == ARRAY_2D_TILED_THIN1 || ARRAY_MODE == ARRAY_2D_TILED_THICK ||
        ARRAY_MODE == ARRAY_2D_TILED_XTHICK) {
        slice_rotation = ((NUM_BANKS / 2u) - 1u) * (slice / MICRO_TILE_THICKNESS);
    } else if (ARRAY_MODE == ARRAY_3D_TILED_THIN1 || ARRAY_MODE == ARRAY_3D_TILED_THICK ||
               ARRAY_MODE == ARRAY_3D_TILED_XTHICK) {
        slice_rotation =
            max(1u, (NUM_PIPES / 2u) - 1u) * (slice / MICRO_TILE_THICKNESS) / NUM_PIPES;
    }
    if (ARRAY_MODE == ARRAY_2D_TILED_THIN1 || ARRAY_MODE == ARRAY_3D_TILED_THIN1 ||
        ARRAY_MODE == ARRAY_PRT_2D_TILED_THIN1 || ARRAY_MODE == ARRAY_PRT_3D_TILED_THIN1) {
        tile_split_rotation = ((NUM_BANKS / 2u) + 1u) * tile_split_slice;
    }
    bank ^= info.bank_swizzle + slice_rotation;
    bank ^= tile_split_rotation;
    return bank & (NUM_BANKS - 1u);
}

uint ComputeSurfaceAddrFromCoordMacroTiled(uint x, uint y, uint slice, uint pitch, uint height,
                                           uint sample_index) {
    uint pixel_index = ComputePixelIndexWithinMicroTile(x, y, slice, MICRO_TILE_THICKNESS);
    uint sample_offset, pixel_offset;
    if (MICRO_TILE_MODE == ADDR_SURF_DEPTH_MICRO_TILING) {
        sample_offset = sample_index * BITS_PER_PIXEL;
        pixel_offset = pixel_index * BITS_PER_PIXEL * NUM_SAMPLES;
    } else {
        sample_offset = sample_index * (MICRO_TILE_BITS / NUM_SAMPLES);
        pixel_offset = pixel_index * BITS_PER_PIXEL;
    }
    uint element_offset = (pixel_offset + sample_offset) / 8u;

    uint micro_tile_bytes = MICRO_TILE_BYTES;
    uint slices_per_tile = 1;
    uint tile_split_slice = 0;
    if (MICRO_TILE_BYTES > TILE_SPLIT_BYTES && MICRO_TILE_THICKNESS == 1) {
        slices_per_tile = MICRO_TILE_BYTES / TILE_SPLIT_BYTES;
        tile_split_slice = element_offset / TILE_SPLIT_BYTES;
        element_offset %= TILE_SPLIT_BYTES;
        micro_tile_bytes = TILE_SPLIT_BYTES;
    }
    uint macro_tile_pitch = (MICRO_TILE_WIDTH * BANK_WIDTH * NUM_PIPES) * MACRO_TILE_ASPECT;
    uint macro_tile_height = (MICRO_TILE_HEIGHT * BANK_HEIGHT * NUM_BANKS) / MACRO_TILE_ASPECT;
    uint macro_tile_bytes = micro_tile_bytes * (macro_tile_pitch / MICRO_TILE_WIDTH) *
                            (macro_tile_height / MICRO_TILE_HEIGHT) / (NUM_PIPES * NUM_BANKS);
    uint macro_tiles_per_row = pitch / macro_tile_pitch;
    uint macro_tile_index_x = x / macro_tile_pitch;
    uint macro_tile_index_y = y / macro_tile_height;
    uint macro_tile_offset =
        ((macro_tile_index_y * macro_tiles_per_row) + macro_tile_index_x) * macro_tile_bytes;
    uint macro_tiles_per_slice = macro_tiles_per_row * (height / macro_tile_height);
    uint slice_bytes = macro_tiles_per_slice * macro_tile_bytes;
    uint slice_offset =
        slice_bytes * (tile_split_slice + slices_per_tile * (slice / MICRO_TILE_THICKNESS));
    uint tile_row_index = (y / MICRO_TILE_HEIGHT) % BANK_HEIGHT;
    uint tile_column_index = ((x / MICRO_TILE_WIDTH) / NUM_PIPES) % BANK_WIDTH;
    uint tile_index = (tile_row_index * BANK_WIDTH) + tile_column_index;
    uint tile_offset = tile_index * micro_tile_bytes;
    uint total_offset = slice_offset + macro_tile_offset + element_offset + tile_offset;

    if (ARRAY_MODE == ARRAY_PRT_TILED_THIN1 || ARRAY_MODE == ARRAY_PRT_TILED_THICK) {
        x %= macro_tile_pitch;
        y %= macro_tile_height;
    }
    uint pipe = ComputePipeFromCoord(x, y, slice);
    uint bank = ComputeBankFromCoord(x, y, slice, tile_split_slice);
    uint pipe_interleave_mask = (1u << NUM_PIPE_INTERLEAVE_BITS) - 1u;
    uint pipe_interleave_offset = total_offset & pipe_interleave_mask;
    uint offset = total_offset >> NUM_PIPE_INTERLEAVE_BITS;
    uint addr = pipe_interleave_offset;
    addr |= pipe << NUM_PIPE_INTERLEAVE_BITS;
    addr |= bank << (NUM_PIPE_INTERLEAVE_BITS + NUM_PIPE_BITS);
    addr |= offset << (NUM_PIPE_INTERLEAVE_BITS + NUM_PIPE_BITS + NUM_BANK_BITS);
    return addr;
}
#endif

uint LinearAddress(uint entry, uint x, uint y) {
    return info.entries_b[entry].x + y * info.entries_b[entry].y + x * BYTES_PER_PIXEL;
}

void Transfer(uint src_address, uint dst_address) {
#if BITS_PER_PIXEL < 32
    const uint src_word = src_address & ~3u;
    const uint src_shift = (src_address & 3u) * 8u;
    const uint value = (source_data.Load(src_word) >> src_shift) &
                       ((1u << BITS_PER_PIXEL) - 1u);
    const uint dst_word = dst_address & ~3u;
    const uint dst_shift = (dst_address & 3u) * 8u;
    const uint mask = ((1u << BITS_PER_PIXEL) - 1u) << dst_shift;
    dest_data.InterlockedAnd(dst_word, ~mask);
    dest_data.InterlockedOr(dst_word, value << dst_shift);
#elif BITS_PER_PIXEL == 32
    dest_data.Store(dst_address, source_data.Load(src_address));
#elif BITS_PER_PIXEL == 64
    dest_data.Store(dst_address, source_data.Load(src_address));
    dest_data.Store(dst_address + 4u, source_data.Load(src_address + 4u));
#elif BITS_PER_PIXEL == 96
    dest_data.Store(dst_address, source_data.Load(src_address));
    dest_data.Store(dst_address + 4u, source_data.Load(src_address + 4u));
    dest_data.Store(dst_address + 8u, source_data.Load(src_address + 8u));
#else
    dest_data.Store(dst_address, source_data.Load(src_address));
    dest_data.Store(dst_address + 4u, source_data.Load(src_address + 4u));
    dest_data.Store(dst_address + 8u, source_data.Load(src_address + 8u));
    dest_data.Store(dst_address + 12u, source_data.Load(src_address + 12u));
#endif
}

[numthreads(64, 1, 1)]
void main(uint dispatch : SV_DispatchThreadID) {
    if (dispatch >= info.pad) {
        return;
    }
    uint texel = dispatch;
    uint entry = 0;
    while (entry + 1u < info.num_entries && texel >= info.entries_b[entry].z) {
        texel -= info.entries_b[entry].z;
        ++entry;
    }
    const uint mip = info.entries_a[entry].x;
    const uint pitch = info.entries_a[entry].y;
    const uint height = info.entries_a[entry].z;
    const uint x = texel % pitch;
    const uint y = (texel / pitch) % height;
    const uint extent = info.entries_b[entry].w;
    if (x >= (extent & 0xffffu) || y >= (extent >> 16u)) {
        return;
    }

#if IS_MACRO_TILED
    uint tiled_offset = info.entries_a[entry].w;
    if ((info.micro_tiled_mips & (1u << mip)) != 0) {
        tiled_offset += ComputeSurfaceAddrFromCoordMicroTiled(x, y, 0u, pitch, height,
                                                              min(MICRO_TILE_THICKNESS, 4u), 0u);
    } else {
        tiled_offset += ComputeSurfaceAddrFromCoordMacroTiled(x, y, 0u, pitch, height, 0u);
    }
#else
    const uint tiled_offset = info.entries_a[entry].w +
                              ComputeSurfaceAddrFromCoordMicroTiled(x, y, 0u, pitch, height,
                                                                    MICRO_TILE_THICKNESS, 0u);
#endif
    const uint linear_offset = LinearAddress(entry, x, y);

#if IS_TILER != 0
    Transfer(linear_offset, tiled_offset);
#else
    Transfer(tiled_offset, linear_offset);
#endif
}
)";

Microsoft::WRL::ComPtr<ID3D12RootSignature> CreateRootSignature(ID3D12Device* device) {
    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[0].Descriptor.ShaderRegister = 0;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[1].Descriptor.ShaderRegister = 0;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[2].Descriptor.ShaderRegister = 0;
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC description{
        3, parameters, 0, nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS};
    Microsoft::WRL::ComPtr<ID3DBlob> signature, errors;
    winrt::check_hresult(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1,
                                                     &signature, &errors));
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    winrt::check_hresult(device->CreateRootSignature(
        0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&root)));
    return root;
}

std::array<std::wstring, 14> SpecDefines(std::span<const u32, 14> specs) {
    static constexpr std::array<const wchar_t*, 14> names{
        L"BITS_PER_PIXEL",    L"NUM_SAMPLES",   L"MICRO_TILE_MODE", L"MICRO_TILE_THICKNESS",
        L"IS_TILER",          L"ARRAY_MODE",    L"PIPE_CONFIG",     L"BANK_WIDTH",
        L"BANK_HEIGHT",       L"NUM_BANKS",     L"NUM_BANK_BITS",   L"TILE_SPLIT_BYTES",
        L"MACRO_TILE_ASPECT", L"IS_MACRO_TILED"};
    std::array<std::wstring, 14> defines{};
    for (size_t index = 0; index < names.size(); ++index) {
        defines[index] = std::wstring{L"-D"} + names[index] + L"=" + std::to_wstring(specs[index]);
    }
    return defines;
}

u64 KeyHash(std::span<const u32, 14> specs) {
    u64 hash = 1469598103934665603ull;
    for (const u32 value : specs) {
        hash ^= value;
        hash *= 1099511628211ull;
    }
    return hash;
}

u32 EntryPitch(const VideoCore::ImageInfo& info, u32 mip) {
    const auto& layout = info.mips_layout[mip];
    return info.props.is_block ? std::max<u32>((layout.pitch + 3) / 4, 1u) : layout.pitch;
}

u32 EntryHeight(const VideoCore::ImageInfo& info, u32 mip) {
    const auto& layout = info.mips_layout[mip];
    return info.props.is_block ? std::max<u32>((layout.height + 3) / 4, 1u) : layout.height;
}

std::array<u32, 14> SpecValues(const VideoCore::ImageInfo& info, bool is_tiler) {
    const auto micro_tile_mode = AmdGpu::GetMicroTileMode(info.tile_mode);
    const bool macro = AmdGpu::IsMacroTiled(info.array_mode);
    const u32 thickness = AmdGpu::GetMicroTileThickness(info.array_mode);
    std::array<u32, 14> specs{info.num_bits,
                              info.num_samples,
                              u32(micro_tile_mode),
                              thickness,
                              u32(is_tiler),
                              u32(info.array_mode),
                              0u,
                              1u,
                              1u,
                              1u,
                              1u,
                              1u,
                              1u,
                              u32(macro)};
    if (macro) {
        const auto macro_tile_mode =
            AmdGpu::CalculateMacrotileMode(info.tile_mode, info.num_bits, info.num_samples);
        specs[6] = u32(info.alt_tile ? AmdGpu::GetAltPipeConfig(info.tile_mode)
                                     : AmdGpu::GetPipeConfig(info.tile_mode));
        specs[7] = AmdGpu::GetBankWidth(macro_tile_mode);
        specs[8] = info.alt_tile ? AmdGpu::GetAltBankHeight(macro_tile_mode)
                                 : AmdGpu::GetBankHeight(macro_tile_mode);
        specs[9] = info.alt_tile ? AmdGpu::GetAltNumBanks(macro_tile_mode)
                                 : AmdGpu::GetNumBanks(macro_tile_mode);
        specs[10] = u32(std::bit_width(specs[9]) - 1);
        specs[11] = AmdGpu::CalculateTileSplit(info.tile_mode, info.array_mode, micro_tile_mode,
                                               info.num_bits);
        specs[12] = info.alt_tile ? AmdGpu::GetAltMacrotileAspect(macro_tile_mode)
                                  : AmdGpu::GetMacrotileAspect(macro_tile_mode);
    }
    return specs;
}

struct TilingInfoCB {
    u32 bank_swizzle;
    u32 micro_tiled_mips;
    u32 num_entries;
    u32 pad;
    alignas(16) u32 entries_a[16][4];
    alignas(16) u32 entries_b[16][4];
};
static_assert(sizeof(TilingInfoCB) == 528);

u64 FootprintBytes(std::span<const TilingEntry> entries) {
    u64 bytes = 0;
    for (const auto& entry : entries) {
        bytes =
            std::max<u64>(bytes, u64(entry.linear_offset) + u64(entry.row_pitch) * entry.height);
    }
    return bytes;
}

} // namespace

bool TilingRuntime::Supported(const VideoCore::ImageInfo& info, size_t entries) {
    if (!info.props.is_tiled || info.num_samples != 1 || info.resources.layers != 1 ||
        entries == 0 || entries > 16 || info.guest_size == 0) {
        return false;
    }
    switch (info.num_bits) {
    case 8:
    case 16:
    case 32:
    case 64:
    case 128:
        break;
    default:
        return false;
    }
    for (u32 mip = 0; mip < info.resources.levels; ++mip) {
        const auto& layout = info.mips_layout[mip];
        const u32 texels = u32(layout.size / (info.num_bits / 8));
        if (u64(EntryPitch(info, mip)) * EntryHeight(info, mip) != texels) {
            return false;
        }
    }
    return true;
}

std::vector<TilingEntry>
TilingRuntime::Entries(const VideoCore::ImageInfo& info,
                       std::span<const D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints) {
    std::vector<TilingEntry> entries;
    for (u32 mip = 0; mip < info.resources.levels; ++mip) {
        const auto& layout = info.mips_layout[mip];
        TilingEntry entry{};
        entry.mip = mip;
        entry.pitch = EntryPitch(info, mip);
        entry.height = EntryHeight(info, mip);
        entry.texels = u32(layout.size / (info.num_bits / 8));
        entry.tiled_offset = u32(layout.offset);
        entry.linear_offset = u32(footprints[mip].Offset);
        entry.row_pitch = u32(footprints[mip].Footprint.RowPitch);
        entry.width = footprints[mip].Footprint.Width;
        entry.visible_height = footprints[mip].Footprint.Height;
        entries.push_back(entry);
    }
    return entries;
}

D3D12Resource& TilingRuntime::RequestBuffer(D3D12Resource& buffer, u64 size, D3D12_HEAP_TYPE heap,
                                            D3D12_RESOURCE_STATES initial_state,
                                            D3D12_RESOURCE_STATES& state,
                                            D3D12_RESOURCE_FLAGS flags) {
    if (buffer.Get() && buffer.AllocationBytes() >= size)
        return buffer;
    buffer.Reset();
    winrt::check_hresult(
        allocator.CreateBuffer((size + 65535) & ~u64{65535}, heap, initial_state, flags, buffer));
    state = initial_state;
    return buffer;
}

void TilingRuntime::Transition(ID3D12GraphicsCommandList* commands, D3D12Resource& buffer,
                               D3D12_RESOURCE_STATES& state, D3D12_RESOURCE_STATES next) {
    if (!buffer.Get() || state == next)
        return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {buffer.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, state, next};
    commands->ResourceBarrier(1, &barrier);
    state = next;
}

void TilingRuntime::Initialize(D3D12DeviceContext& device_context, ShaderCache& cache) {
    device = &device_context;
    shaders = &cache;
    allocator.Initialize(device->Device(), 64ULL * 1024 * 1024);
    root = CreateRootSignature(device->Device());
}

void TilingRuntime::BeginBatch() {
    operations.clear();
    active = nullptr;
}

bool TilingRuntime::CanRecord(const VideoCore::ImageInfo& info,
                              std::span<const TilingEntry> entries, bool tile) const {
    const auto aligned = [](u64 size) { return (size + 65535) & ~u64{65535}; };
    const u64 bytes = aligned(info.guest_size);
    const u64 scratch = aligned(std::max(FootprintBytes(entries), u64(info.guest_size)));
    const u64 required = 65536 + bytes + scratch * 2 + ((tile || info.num_bits < 32) ? bytes : 0);
    const auto stats = allocator.Stats();
    return stats.live_bytes <= stats.budget_bytes &&
           required <= stats.budget_bytes - stats.live_bytes;
}

void TilingRuntime::BeginOperation() {
    auto operation = std::make_unique<Operation>();
    active = operation.get();
    operations.push_back(std::move(operation));
    RequestBuffer(active->params, 4096, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                  active->heap_state);
}

ID3D12PipelineState* TilingRuntime::Pipeline(const VideoCore::ImageInfo& info, bool is_tiler) {
    const auto specs = SpecValues(info, is_tiler);
    const u64 hash = KeyHash(specs);
    if (const auto it = pipelines.find(hash); it != pipelines.end())
        return it->second.Get();
    const auto defines = SpecDefines(specs);
    auto binary = shaders->CompileHlslRaw(std::string(TilingShader), L"cs_6_6", defines);
    D3D12_COMPUTE_PIPELINE_STATE_DESC description{};
    description.pRootSignature = root.Get();
    description.CS.pShaderBytecode = binary->dxil.data();
    description.CS.BytecodeLength = binary->dxil.size();
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    winrt::check_hresult(
        device->Device()->CreateComputePipelineState(&description, IID_PPV_ARGS(&pipeline)));
    ID3D12PipelineState* handle = pipeline.Get();
    pipelines.emplace(hash, std::move(pipeline));
    return handle;
}

void TilingRuntime::Dispatch(ID3D12GraphicsCommandList* commands, const VideoCore::ImageInfo& info,
                             std::span<const TilingEntry> entries, u32 texels, bool is_tiler,
                             u64 zero_size) {
    Transition(commands, active->input, active->input_state,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (zero_size) {
        Transition(commands, active->output, active->output_state, D3D12_RESOURCE_STATE_COPY_DEST);
        commands->CopyBufferRegion(active->output.Get(), 0, active->zeros.Get(), 0, zero_size);
        Transition(commands, active->output, active->output_state,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    } else {
        Transition(commands, active->output, active->output_state,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    void* mapped;
    constexpr D3D12_RANGE empty{0, 0};
    winrt::check_hresult(active->params->Map(0, &empty, &mapped));
    TilingInfoCB translation{};
    translation.bank_swizzle = u32(info.bank_swizzle);
    translation.micro_tiled_mips = info.micro_tiled_mips;
    translation.num_entries = u32(entries.size());
    u32 total_texels = 0;
    for (const auto& entry : entries)
        total_texels += entry.texels;
    translation.pad = total_texels;
    for (u32 index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        u32* a = translation.entries_a[index];
        u32* b = translation.entries_b[index];
        a[0] = entry.mip;
        a[1] = entry.pitch;
        a[2] = entry.height;
        a[3] = entry.tiled_offset;
        b[0] = entry.linear_offset;
        b[1] = entry.row_pitch;
        b[2] = entry.texels;
        b[3] = entry.width | (entry.visible_height << 16);
    }
    std::memcpy(mapped, &translation, sizeof(translation));
    active->params->Unmap(0, nullptr);
    commands->SetComputeRootSignature(root.Get());
    commands->SetPipelineState(Pipeline(info, is_tiler));
    commands->SetComputeRootShaderResourceView(0, active->input->GetGPUVirtualAddress());
    commands->SetComputeRootUnorderedAccessView(1, active->output->GetGPUVirtualAddress());
    commands->SetComputeRootConstantBufferView(2, active->params->GetGPUVirtualAddress());
    commands->Dispatch((texels + 63) / 64, 1, 1);
}

void TilingRuntime::CopyFromGuest(ID3D12GraphicsCommandList* commands, std::span<const u8> bytes) {
    RequestBuffer(active->staging, bytes.size_bytes(), D3D12_HEAP_TYPE_UPLOAD,
                  D3D12_RESOURCE_STATE_GENERIC_READ, active->heap_state);
    RequestBuffer(active->input, bytes.size_bytes(), D3D12_HEAP_TYPE_DEFAULT,
                  D3D12_RESOURCE_STATE_COMMON, active->input_state,
                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    void* mapped;
    constexpr D3D12_RANGE empty{0, 0};
    winrt::check_hresult(active->staging->Map(0, &empty, &mapped));
    std::memcpy(mapped, bytes.data(), bytes.size_bytes());
    active->staging->Unmap(0, &empty);
    Transition(commands, active->input, active->input_state, D3D12_RESOURCE_STATE_COPY_DEST);
    commands->CopyBufferRegion(active->input.Get(), 0, active->staging.Get(), 0,
                               bytes.size_bytes());
}

void TilingRuntime::EnsureZeroed() {
    if (!active->zeros.Get() || active->zeros_zeroed == active->zeros.AllocationBytes())
        return;
    void* mapped;
    constexpr D3D12_RANGE empty{0, 0};
    winrt::check_hresult(active->zeros->Map(0, &empty, &mapped));
    std::memset(mapped, 0, active->zeros.AllocationBytes());
    active->zeros->Unmap(0, nullptr);
    active->zeros_zeroed = active->zeros.AllocationBytes();
}

void TilingRuntime::OutputToImage(ID3D12GraphicsCommandList* commands,
                                  std::span<const TilingEntry> entries, ID3D12Resource* image,
                                  std::span<const D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints) {
    Transition(commands, active->output, active->output_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (u32 subresource = 0; subresource < entries.size(); ++subresource) {
        D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
        source.pResource = active->output.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = footprints[subresource];
        destination.pResource = image;
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = subresource;
        commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    }
}

void TilingRuntime::OutputToBuffer(ID3D12GraphicsCommandList* commands, u64 bytes) {
    Transition(commands, active->output, active->output_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commands->CopyBufferRegion(active->tile_readback.Get(), 0, active->output.Get(), 0, bytes);
}

void TilingRuntime::Detile(ID3D12GraphicsCommandList* commands, const VideoCore::ImageInfo& info,
                           std::span<const TilingEntry> entries, std::span<const u8> tiled_bytes,
                           ID3D12Resource* image,
                           std::span<const D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints) {
    BeginOperation();
    u32 texels = 0;
    for (const auto& entry : entries)
        texels += entry.texels;
    RequestBuffer(active->input, std::max(FootprintBytes(entries), u64(info.guest_size)),
                  D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, active->input_state,
                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    CopyFromGuest(commands, tiled_bytes);
    RequestBuffer(active->output, std::max(FootprintBytes(entries), u64(info.guest_size)),
                  D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, active->output_state,
                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    RequestBuffer(active->input, std::max(FootprintBytes(entries), u64(info.guest_size)),
                  D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, active->input_state);
    if (info.num_bits < 32) {
        RequestBuffer(active->zeros, info.guest_size, D3D12_HEAP_TYPE_UPLOAD,
                      D3D12_RESOURCE_STATE_GENERIC_READ, active->heap_state);
        EnsureZeroed();
    }
    Dispatch(commands, info, entries, texels, false, info.num_bits < 32 ? info.guest_size : 0);
    OutputToImage(commands, entries, image, footprints);
    Transition(commands, active->input, active->input_state, D3D12_RESOURCE_STATE_COMMON);
    Transition(commands, active->output, active->output_state, D3D12_RESOURCE_STATE_COMMON);
}

ID3D12Resource* TilingRuntime::Tile(ID3D12GraphicsCommandList* commands,
                                    const VideoCore::ImageInfo& info,
                                    std::span<const TilingEntry> entries, ID3D12Resource* image,
                                    std::span<const D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints,
                                    std::span<const u8> guest) {
    if (guest.size() != info.guest_size)
        throw std::runtime_error("Invalid D3D12 tiled image backing snapshot");
    BeginOperation();
    u32 texels = 0;
    for (const auto& entry : entries)
        texels += entry.texels;
    RequestBuffer(active->input, std::max(FootprintBytes(entries), u64(info.guest_size)),
                  D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, active->input_state,
                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    RequestBuffer(active->output, std::max(FootprintBytes(entries), u64(info.guest_size)),
                  D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, active->output_state,
                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    RequestBuffer(active->staging, guest.size(), D3D12_HEAP_TYPE_UPLOAD,
                  D3D12_RESOURCE_STATE_GENERIC_READ, active->heap_state);
    void* mapped;
    const D3D12_RANGE empty{0, 0};
    winrt::check_hresult(active->staging->Map(0, &empty, &mapped));
    std::memcpy(mapped, guest.data(), guest.size());
    active->staging->Unmap(0, nullptr);
    Transition(commands, active->output, active->output_state, D3D12_RESOURCE_STATE_COPY_DEST);
    commands->CopyBufferRegion(active->output.Get(), 0, active->staging.Get(), 0, guest.size());
    Transition(commands, active->input, active->input_state, D3D12_RESOURCE_STATE_COPY_DEST);
    for (u32 subresource = 0; subresource < entries.size(); ++subresource) {
        D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
        source.pResource = image;
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.SubresourceIndex = subresource;
        destination.pResource = active->input.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = footprints[subresource];
        commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    }
    Dispatch(commands, info, entries, texels, true, 0);
    RequestBuffer(active->tile_readback, info.guest_size, D3D12_HEAP_TYPE_READBACK,
                  D3D12_RESOURCE_STATE_COPY_DEST, active->heap_state);
    OutputToBuffer(commands, info.guest_size);
    Transition(commands, active->input, active->input_state, D3D12_RESOURCE_STATE_COMMON);
    Transition(commands, active->output, active->output_state, D3D12_RESOURCE_STATE_COMMON);
    return active->tile_readback.Get();
}
} // namespace D3D12
