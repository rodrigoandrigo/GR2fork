#include "video_core/renderer_d3d12/tiling_runtime.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <utility>
#include <winrt/base.h>
#include "video_core/amdgpu/tiling.h"
#include "video_core/renderer_d3d12/shader_cache.h"
#include "video_core/renderer_d3d12/logic_op_format.h"
#include "video_core/renderer_d3d12/image_cache.h"

namespace D3D12 {
namespace {

constexpr std::string_view TilingShader = R"(
struct TilingInfo {
    uint bank_swizzle, micro_tiled_mips, mip, pitch;
    uint height, tiled_offset, linear_offset, row_pitch;
    uint width, visible_height, depth, slice;
    uint sample_index, depth_pitch, num_texels, linear_tiling;
    uint4 bits;
    uint4 offsets;
    uint logic_mask;
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

uint LoadBits(uint address, uint bits) {
    uint shift = (address & 3u) * 8u;
    uint value = source_data.Load(address & ~3u) >> shift;
    if (shift != 0u && bits > 32u - shift)
        value |= source_data.Load((address & ~3u) + 4u) << (32u - shift);
    return bits == 32u ? value : value & ((1u << bits) - 1u);
}
void StoreBits(uint address, uint bits, uint value) {
    uint shift = (address & 3u) * 8u;
    if (bits == 32u && shift == 0u) {
        dest_data.Store(address, value);
    } else {
        uint mask = (bits == 32u ? 0xffffffffu : (1u << bits) - 1u) << shift;
        dest_data.InterlockedAnd(address & ~3u, ~mask);
        dest_data.InterlockedOr(address & ~3u, (value << shift) & mask);
        if (shift != 0u && bits > 32u - shift) {
            uint remaining = bits - (32u - shift);
            mask = (1u << remaining) - 1u;
            dest_data.InterlockedAnd((address & ~3u) + 4u, ~mask);
            dest_data.InterlockedOr((address & ~3u) + 4u, (value >> (32u - shift)) & mask);
        }
    }
}
void Transfer(uint tiled_address, uint linear_address) {
#if LOGIC_OP
    uint component = 0u;
#if !IS_TILER
    dest_data.Store4(linear_address, 0u);
#endif
    for (uint c = 0u; c < 4u; ++c) {
        uint bits = info.bits[c];
        if ((info.logic_mask & (1u << c)) == 0u || bits == 0u)
            continue;
        uint address = tiled_address + info.offsets[c] / 8u;
        uint shift = info.offsets[c] % 8u;
        uint mask = bits == 32u ? 0xffffffffu : (1u << bits) - 1u;
#if IS_TILER
        uint value = source_data.Load(linear_address + component * 4u) & mask;
        uint word = address & ~3u;
        uint bit_shift = (address & 3u) * 8u + shift;
        dest_data.InterlockedAnd(word, ~(mask << bit_shift));
        dest_data.InterlockedOr(word, value << bit_shift);
#else
        uint value = (LoadBits(address, bits + shift) >> shift) & mask;
        dest_data.Store(linear_address + component * 4u, value);
#endif
        ++component;
    }
#else
#if IS_TILER
    uint source = linear_address, destination = tiled_address;
#else
    uint source = tiled_address, destination = linear_address;
#endif
#if SWAP_4444
    uint value = LoadBits(source, 16u);
    value = ((value & 0xfu) << 12u) | ((value & 0xf0u) << 4u) |
            ((value & 0xf00u) >> 4u) | ((value & 0xf000u) >> 12u);
    StoreBits(destination, 16u, value);
#elif BITS_PER_PIXEL < 32
    StoreBits(destination, BITS_PER_PIXEL, LoadBits(source, BITS_PER_PIXEL));
#else
    for (uint offset = 0u; offset < BYTES_PER_PIXEL; offset += 4u)
        dest_data.Store(destination + offset, source_data.Load(source + offset));
#endif
#endif
}
[numthreads(64, 1, 1)]
void main(uint3 dispatch : SV_DispatchThreadID) {
    uint texel = dispatch.x + dispatch.y * (65535u * 64u);
    if (texel >= info.num_texels)
        return;
    uint x = texel % info.width;
    uint y = (texel / info.width) % info.visible_height;
    uint z = texel / (info.width * info.visible_height);
    uint slice = info.slice + z;
    uint tiled_offset = info.tiled_offset;
    if (info.linear_tiling != 0u) {
        tiled_offset += ((slice * NUM_SAMPLES + info.sample_index) * info.pitch * info.height +
                         y * info.pitch + x) * BYTES_PER_PIXEL;
    } else {
#if IS_MACRO_TILED
        if ((info.micro_tiled_mips & (1u << info.mip)) == 0u)
            tiled_offset += ComputeSurfaceAddrFromCoordMacroTiled(
                x, y, slice, info.pitch, info.height, info.sample_index);
        else
#endif
            tiled_offset += ComputeSurfaceAddrFromCoordMicroTiled(
                x, y, slice, info.pitch, info.height, min(MICRO_TILE_THICKNESS, 4u),
                info.sample_index);
    }
    uint native_bytes = LOGIC_OP ? 16u : BYTES_PER_PIXEL;
    uint linear_offset = info.linear_offset + z * info.depth_pitch + y * info.row_pitch +
                         x * native_bytes;
    Transfer(tiled_offset, linear_offset);
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
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[2].Constants = {0, 0, 25};
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

std::array<std::wstring, 16> SpecDefines(std::span<const u32, 16> specs) {
    static constexpr std::array<const wchar_t*, 16> names{
        L"BITS_PER_PIXEL",    L"NUM_SAMPLES",    L"MICRO_TILE_MODE", L"MICRO_TILE_THICKNESS",
        L"IS_TILER",          L"ARRAY_MODE",     L"PIPE_CONFIG",     L"BANK_WIDTH",
        L"BANK_HEIGHT",       L"NUM_BANKS",      L"NUM_BANK_BITS",   L"TILE_SPLIT_BYTES",
        L"MACRO_TILE_ASPECT", L"IS_MACRO_TILED", L"LOGIC_OP",        L"SWAP_4444"};
    std::array<std::wstring, 16> defines{};
    for (size_t index = 0; index < names.size(); ++index) {
        defines[index] = std::wstring{L"-D"} + names[index] + L"=" + std::to_wstring(specs[index]);
    }
    return defines;
}

u64 KeyHash(std::span<const u32, 16> specs) {
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

std::array<u32, 16> SpecValues(const VideoCore::ImageInfo& info, bool is_tiler, bool logic_op) {
    const auto micro_tile_mode = AmdGpu::GetMicroTileMode(info.tile_mode);
    const bool macro = AmdGpu::IsMacroTiled(info.array_mode);
    const u32 thickness = AmdGpu::GetMicroTileThickness(info.array_mode);
    std::array<u32, 16> specs{info.num_bits,
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
                              u32(macro),
                              u32(logic_op),
                              u32(info.pixel_format == vk::Format::eB4G4R4A4UnormPack16)};
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
    u32 bank_swizzle, micro_tiled_mips, mip, pitch;
    u32 height, tiled_offset, linear_offset, row_pitch;
    u32 width, visible_height, depth, slice;
    u32 sample_index, depth_pitch, num_texels, linear_tiling;
    std::array<u32, 4> bits{}, offsets{};
    u32 logic_mask;
};
static_assert(sizeof(TilingInfoCB) == 100);

u64 FootprintBytes(std::span<const TilingEntry> entries) {
    u64 bytes = 0;
    for (const auto& entry : entries) {
        bytes =
            std::max<u64>(bytes, u64(entry.linear_offset) + u64(entry.depth_pitch) * entry.depth);
    }
    return bytes;
}

} // namespace

bool TilingRuntime::Supported(const VideoCore::ImageInfo& info, size_t entries) {
    if (!info.resources.levels || info.resources.levels > 16 || !info.resources.layers ||
        !std::has_single_bit(info.num_samples) || info.num_samples > 16 ||
        (info.props.is_block && info.num_samples != 1) ||
        (info.num_bits == 96 && info.props.is_tiled) || !entries || entries > 32768 ||
        entries % info.resources.levels || !info.guest_size || info.guest_size > 64 * 1024 * 1024)
        return false;
    switch (info.num_bits) {
    case 8:
    case 16:
    case 32:
    case 64:
    case 96:
    case 128:
        break;
    default:
        return false;
    }
    for (u32 mip = 0; mip < info.resources.levels; ++mip) {
        const auto& layout = info.mips_layout[mip];
        const u64 plane = u64(EntryPitch(info, mip)) * EntryHeight(info, mip) * (info.num_bits / 8);
        if (!plane || !layout.size || layout.offset > info.guest_size ||
            layout.size > info.guest_size - layout.offset)
            return false;
    }
    return true;
}
std::vector<TilingEntry>
TilingRuntime::Entries(const VideoCore::ImageInfo& info,
                       std::span<const D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints,
                       u32 base_layer) {
    std::vector<TilingEntry> entries;
    if (!info.resources.levels || !info.num_samples)
        return entries;
    entries.reserve(footprints.size());
    for (u32 subresource = 0; subresource < footprints.size(); ++subresource) {
        const u32 mip = subresource % info.resources.levels;
        const auto& layout = info.mips_layout[mip];
        const auto& footprint = footprints[subresource];
        TilingEntry entry{};
        entry.mip = mip;
        entry.pitch = EntryPitch(info, mip);
        entry.height = EntryHeight(info, mip);
        entry.tiled_offset = u32(layout.offset);
        entry.linear_offset = u32(footprint.Offset);
        entry.row_pitch = footprint.Footprint.RowPitch;
        entry.width =
            info.props.is_block ? (footprint.Footprint.Width + 3) / 4 : footprint.Footprint.Width;
        entry.visible_height =
            info.props.is_block ? (footprint.Footprint.Height + 3) / 4 : footprint.Footprint.Height;
        entry.depth = footprint.Footprint.Depth;
        entry.depth_pitch = entry.row_pitch * entry.visible_height;
        const u32 plane = subresource / info.resources.levels;
        entry.slice = base_layer + plane / info.num_samples;
        entry.sample = plane % info.num_samples;
        entry.texels = entry.width * entry.visible_height * entry.depth;
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
    if (heap != D3D12_HEAP_TYPE_DEFAULT) {
        transfer_pool->Retire(std::move(buffer), device->LastSignaledValue());
        buffer = transfer_pool->Acquire(size, heap);
        state = initial_state;
        return buffer;
    }
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

void TilingRuntime::Initialize(D3D12DeviceContext& device_context, ShaderCache& cache,
                               std::shared_ptr<TransferPool> pool) {
    device = &device_context;
    transfer_pool = std::move(pool);
    if (!transfer_pool) {
        transfer_pool = std::make_shared<TransferPool>();
        transfer_pool->Initialize(device_context);
    }
    shaders = &cache;
    allocator.Initialize(device->Device(), 64ULL * 1024 * 1024);
    root = CreateRootSignature(device->Device());
}

void TilingRuntime::RetireUploads(u64 fence) {
    transfer_fence = fence;
    for (auto& operation : operations) {
        transfer_pool->Retire(std::move(operation->staging), fence);
    }
}
void TilingRuntime::ReleaseReadbacks(u64 fence) {
    if (!operations.empty() && device->CompletedValue() < fence)
        throw std::runtime_error("D3D12 tiling resources released before transfer fence");
    for (auto& operation : operations)
        transfer_pool->Retire(std::move(operation->tile_readback), fence);
    operations.clear();
    active = nullptr;
}
void TilingRuntime::TrimMemory() {
    RetireUploads(transfer_fence);
    ReleaseReadbacks(transfer_fence);
    pipelines.clear();
}
void TilingRuntime::BeginBatch() {
    const auto fence = device->LastSignaledValue();
    RetireUploads(fence);
    ReleaseReadbacks(fence);
    if (pipelines.size() > 128) {
        device->Wait(fence);
        while (pipelines.size() > 128)
            pipelines.erase(pipelines.begin());
    }
}

bool TilingRuntime::CanRecord(const VideoCore::ImageInfo& info,
                              std::span<const TilingEntry> entries, bool tile) const {
    if (!Supported(info, entries.size()))
        return false;
    for (const auto& entry : entries) {
        if (entry.mip >= info.resources.levels || !entry.pitch || !entry.height ||
            u64(entry.width) * entry.visible_height * entry.depth != entry.texels ||
            u64(entry.row_pitch) * entry.visible_height > entry.depth_pitch)
            return false;
        const auto& layout = info.mips_layout[entry.mip];
        if (!entry.texels || !entry.width || !entry.visible_height || !entry.depth ||
            entry.width > entry.pitch || entry.visible_height > entry.height ||
            entry.sample >= info.num_samples)
            return false;
        const u64 plane_bytes = u64(entry.pitch) * entry.height * (info.num_bits / 8);
        if (info.props.is_tiled) {
            if ((u64(entry.slice) + entry.depth) * info.num_samples > layout.size / plane_bytes)
                return false;
        } else {
            const u64 end =
                ((u64(entry.slice) + entry.depth - 1) * info.num_samples + entry.sample) *
                    plane_bytes +
                (u64(entry.visible_height - 1) * entry.pitch + entry.width) * (info.num_bits / 8);
            if (end > layout.size)
                return false;
        }
    }
    const auto aligned = [](u64 size) { return (size + 65535) & ~u64{65535}; };
    const u64 required = aligned(FootprintBytes(entries)) + (tile ? aligned(info.guest_size) : 0);
    const auto stats = allocator.Stats();
    return stats.live_bytes <= stats.budget_bytes &&
           required <= stats.budget_bytes - stats.live_bytes;
}
void TilingRuntime::BeginOperation() {
    auto operation = std::make_unique<Operation>();
    active = operation.get();
    operations.push_back(std::move(operation));
}

ID3D12PipelineState* TilingRuntime::Pipeline(const VideoCore::ImageInfo& info, bool is_tiler,
                                             bool logic_op) {
    const auto specs = SpecValues(info, is_tiler, logic_op);
    const u64 hash = KeyHash(specs);
    if (const auto it = pipelines.find(hash); it != pipelines.end())
        return it->second.Get();
    const auto defines = SpecDefines(specs);
    auto binary = shaders->CompileHlslRaw(std::string(TilingShader), L"cs_6_4", defines);
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
                             std::span<const TilingEntry> entries, bool is_tiler, bool logic_op,
                             u32 logic_mask) {
    if (is_tiler)
        Transition(commands, active->input, active->input_state,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(commands, active->output, active->output_state,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    commands->SetComputeRootSignature(root.Get());
    commands->SetPipelineState(Pipeline(info, is_tiler, logic_op));
    commands->SetComputeRootShaderResourceView(
        0, (is_tiler ? active->input.Get() : active->staging.Get())->GetGPUVirtualAddress());
    commands->SetComputeRootUnorderedAccessView(1, active->output->GetGPUVirtualAddress());
    const auto format =
        logic_op ? GetLogicOpFormat(ConvertFormat(info.pixel_format)) : LogicOpFormat{};
    for (const auto& entry : entries) {
        TilingInfoCB translation{u32(info.bank_swizzle),
                                 info.micro_tiled_mips,
                                 entry.mip,
                                 entry.pitch,
                                 entry.height,
                                 entry.tiled_offset,
                                 entry.linear_offset,
                                 entry.row_pitch,
                                 entry.width,
                                 entry.visible_height,
                                 entry.depth,
                                 entry.slice,
                                 entry.sample,
                                 entry.depth_pitch,
                                 entry.texels,
                                 u32(!info.props.is_tiled),
                                 format.bits,
                                 format.offsets,
                                 logic_mask};
        commands->SetComputeRoot32BitConstants(2, 25, &translation, 0);
        const u32 groups = (entry.texels + 63) / 64;
        commands->Dispatch(std::min(groups, 65535u), (groups + 65534) / 65535, 1);
    }
}
void TilingRuntime::CopyFromGuest(ID3D12GraphicsCommandList*, std::span<const u8> bytes,
                                  const Reader& reader, u64 size) {
    if (!reader)
        size = bytes.size_bytes();
    RequestBuffer(active->staging, size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                  active->heap_state);
    void* mapped;
    constexpr D3D12_RANGE empty{0, 0};
    winrt::check_hresult(active->staging->Map(0, &empty, &mapped));
    try {
        if (reader)
            reader({static_cast<u8*>(mapped), size_t(size)});
        else
            std::memcpy(mapped, bytes.data(), bytes.size_bytes());
    } catch (...) {
        active->staging->Unmap(0, &empty);
        throw;
    }
    const D3D12_RANGE written{0, size_t(size)};
    active->staging->Unmap(0, &written);
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
                           std::span<const D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints,
                           bool logic_op, u32 logic_mask, const Reader& reader) {
    if (entries.size() != footprints.size() || (!reader && tiled_bytes.size() != info.guest_size))
        throw std::runtime_error("Invalid D3D12 detile backing or footprints");
    BeginOperation();
    CopyFromGuest(commands, tiled_bytes, reader, info.guest_size);
    RequestBuffer(active->output, FootprintBytes(entries), D3D12_HEAP_TYPE_DEFAULT,
                  D3D12_RESOURCE_STATE_COMMON, active->output_state,
                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    Dispatch(commands, info, entries, false, logic_op, logic_mask);
    OutputToImage(commands, entries, image, footprints);
}
ID3D12Resource* TilingRuntime::Tile(ID3D12GraphicsCommandList* commands,
                                    const VideoCore::ImageInfo& info,
                                    std::span<const TilingEntry> entries, ID3D12Resource* image,
                                    std::span<const D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints,
                                    std::span<const u8> guest, bool logic_op, u32 logic_mask,
                                    const Reader& reader) {
    if ((!reader && guest.size() != info.guest_size) || entries.size() != footprints.size())
        throw std::runtime_error("Invalid D3D12 tiled image backing or footprints");
    BeginOperation();
    RequestBuffer(active->input, FootprintBytes(entries), D3D12_HEAP_TYPE_DEFAULT,
                  D3D12_RESOURCE_STATE_COPY_DEST, active->input_state);
    RequestBuffer(active->output, info.guest_size, D3D12_HEAP_TYPE_DEFAULT,
                  D3D12_RESOURCE_STATE_COPY_DEST, active->output_state,
                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    CopyFromGuest(commands, guest, reader, info.guest_size);
    commands->CopyBufferRegion(active->output.Get(), 0, active->staging.Get(), 0, info.guest_size);
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
    Dispatch(commands, info, entries, true, logic_op, logic_mask);
    RequestBuffer(active->tile_readback, info.guest_size, D3D12_HEAP_TYPE_READBACK,
                  D3D12_RESOURCE_STATE_COPY_DEST, active->heap_state);
    OutputToBuffer(commands, info.guest_size);
    return active->tile_readback.Get();
}
} // namespace D3D12
