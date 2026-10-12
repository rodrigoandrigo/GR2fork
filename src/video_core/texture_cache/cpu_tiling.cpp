// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>
#include "video_core/texture_cache/cpu_tiling.h"
#include "video_core/texture_cache/tile.h"
namespace VideoCore {
namespace {
constexpr u32 _BIT(u32 value, u32 bit) {
    return (value >> bit) & 1;
}
constexpr u32 BITS_TO_BYTES(u32 value) {
    return (value + 7) / 8;
}
static constexpr u32 ADDR_SURF_DISPLAY_MICRO_TILING = 0;
static constexpr u32 ADDR_SURF_THIN_MICRO_TILING = 1;
static constexpr u32 ADDR_SURF_DEPTH_MICRO_TILING = 2;
static constexpr u32 ADDR_SURF_ROTATED_MICRO_TILING = 3;

static constexpr u32 ARRAY_LINEAR_GENERAL = 0;
static constexpr u32 ARRAY_LINEAR_ALIGNED = 1;
static constexpr u32 ARRAY_1D_TILED_THIN1 = 2;
static constexpr u32 ARRAY_1D_TILED_THICK = 3;
static constexpr u32 ARRAY_2D_TILED_THIN1 = 4;
static constexpr u32 ARRAY_PRT_TILED_THIN1 = 5;
static constexpr u32 ARRAY_PRT_2D_TILED_THIN1 = 6;
static constexpr u32 ARRAY_2D_TILED_THICK = 7;
static constexpr u32 ARRAY_2D_TILED_XTHICK = 8;
static constexpr u32 ARRAY_PRT_TILED_THICK = 9;
static constexpr u32 ARRAY_PRT_2D_TILED_THICK = 10;
static constexpr u32 ARRAY_PRT_3D_TILED_THIN1 = 11;
static constexpr u32 ARRAY_3D_TILED_THIN1 = 12;
static constexpr u32 ARRAY_3D_TILED_THICK = 13;
static constexpr u32 ARRAY_3D_TILED_XTHICK = 14;
static constexpr u32 ARRAY_PRT_3D_TILED_THICK = 15;

static constexpr u32 ADDR_SURF_P2 = 0;
static constexpr u32 ADDR_SURF_P8_32x32_8x16 = 10;
static constexpr u32 ADDR_SURF_P8_32x32_16x16 = 12;

template <u32 BITS_PER_PIXEL>
struct TileAddress {
    u32 NUM_SAMPLES, MICRO_TILE_MODE, MICRO_TILE_THICKNESS, ARRAY_MODE, PIPE_CONFIG;
    u32 BANK_WIDTH{1}, BANK_HEIGHT{1}, NUM_BANKS{1}, NUM_BANK_BITS{}, TILE_SPLIT_BYTES{1},
        MACRO_TILE_ASPECT{1};
    u32 NUM_PIPES, NUM_PIPE_BITS, MICRO_TILE_BITS, MICRO_TILE_BYTES, bank_swizzle;
    static constexpr u32 MICRO_TILE_WIDTH = 8, MICRO_TILE_HEIGHT = 8, MICRO_TILE_PIXELS = 64,
                         NUM_PIPE_INTERLEAVE_BITS = 8;
    explicit TileAddress(const ImageInfo& info) {
        NUM_SAMPLES = info.num_samples;
        MICRO_TILE_MODE = u32(AmdGpu::GetMicroTileMode(info.tile_mode));
        MICRO_TILE_THICKNESS = AmdGpu::GetMicroTileThickness(info.array_mode);
        ARRAY_MODE = u32(info.array_mode);
        PIPE_CONFIG = u32(AmdGpu::GetPipeConfig(info.tile_mode));
        NUM_PIPES = PIPE_CONFIG == 0 ? 2 : 8;
        NUM_PIPE_BITS = PIPE_CONFIG == 0 ? 1 : 3;
        MICRO_TILE_BITS = 64 * MICRO_TILE_THICKNESS * BITS_PER_PIXEL * NUM_SAMPLES;
        MICRO_TILE_BYTES = MICRO_TILE_BITS / 8;
        bank_swizzle = info.bank_swizzle;
        if (AmdGpu::IsMacroTiled(info.array_mode)) {
            const auto macro =
                AmdGpu::CalculateMacrotileMode(info.tile_mode, BITS_PER_PIXEL, NUM_SAMPLES);
            BANK_WIDTH = AmdGpu::GetBankWidth(macro);
            BANK_HEIGHT = AmdGpu::GetBankHeight(macro);
            NUM_BANKS = AmdGpu::GetNumBanks(macro);
            NUM_BANK_BITS = std::bit_width(NUM_BANKS) - 1;
            TILE_SPLIT_BYTES =
                AmdGpu::CalculateTileSplit(info.tile_mode, info.array_mode,
                                           AmdGpu::MicroTileMode(MICRO_TILE_MODE), BITS_PER_PIXEL);
            MACRO_TILE_ASPECT = AmdGpu::GetMacrotileAspect(macro);
        }
    }
    u32 ComputePixelIndexWithinMicroTile(u32 x, u32 y, u32 z, u32 thickness) const {
        u32 p0 = 0;
        u32 p1 = 0;
        u32 p2 = 0;
        u32 p3 = 0;
        u32 p4 = 0;
        u32 p5 = 0;
        u32 p6 = 0;
        u32 p7 = 0;
        u32 p8 = 0;

        u32 x0 = _BIT(x, 0);
        u32 x1 = _BIT(x, 1);
        u32 x2 = _BIT(x, 2);
        u32 y0 = _BIT(y, 0);
        u32 y1 = _BIT(y, 1);
        u32 y2 = _BIT(y, 2);
        u32 z0 = _BIT(z, 0);
        u32 z1 = _BIT(z, 1);
        u32 z2 = _BIT(z, 2);

        if (MICRO_TILE_MODE == ADDR_SURF_DISPLAY_MICRO_TILING) {
            if constexpr (BITS_PER_PIXEL == 8) {
                p0 = x0;
                p1 = x1;
                p2 = x2;
                p3 = y1;
                p4 = y0;
                p5 = y2;
            } else if constexpr (BITS_PER_PIXEL == 16) {
                p0 = x0;
                p1 = x1;
                p2 = x2;
                p3 = y0;
                p4 = y1;
                p5 = y2;
            } else if constexpr (BITS_PER_PIXEL == 32) {
                p0 = x0;
                p1 = x1;
                p2 = y0;
                p3 = x2;
                p4 = y1;
                p5 = y2;
            } else if constexpr (BITS_PER_PIXEL == 64) {
                p0 = x0;
                p1 = y0;
                p2 = x1;
                p3 = x2;
                p4 = y1;
                p5 = y2;
            } else if constexpr (BITS_PER_PIXEL == 128) {
                p0 = x0;
                p1 = y0;
                p2 = x1;
                p3 = y1;
                p4 = x2;
                p5 = y2;
            }
        } else if (MICRO_TILE_MODE == ADDR_SURF_THIN_MICRO_TILING ||
                   MICRO_TILE_MODE == ADDR_SURF_DEPTH_MICRO_TILING) {
            p0 = x0;
            p1 = y0;
            p2 = x1;
            p3 = y1;
            p4 = x2;
            p5 = y2;
        } else {
            if constexpr (BITS_PER_PIXEL == 8 || BITS_PER_PIXEL == 16) {
                p0 = x0;
                p1 = y0;
                p2 = x1;
                p3 = y1;
                p4 = z0;
                p5 = z1;
            } else if constexpr (BITS_PER_PIXEL == 32) {
                p0 = x0;
                p1 = y0;
                p2 = x1;
                p3 = z0;
                p4 = y1;
                p5 = z1;
            } else if constexpr (BITS_PER_PIXEL == 64 || BITS_PER_PIXEL == 128) {
                p0 = x0;
                p1 = y0;
                p2 = z0;
                p3 = x1;
                p4 = y1;
                p5 = z1;
            }
            p6 = x2;
            p7 = y2;

            if (thickness == 8) {
                p8 = z2;
            }
        }

        u32 pixel_number = ((p0) | (p1 << 1) | (p2 << 2) | (p3 << 3) | (p4 << 4) | (p5 << 5) |
                            (p6 << 6) | (p7 << 7) | (p8 << 8));

        return pixel_number;
    }

    u32 ComputeSurfaceAddrFromCoordMicroTiled(u32 x, u32 y, u32 slice, u32 pitch, u32 height,
                                              u32 thickness, u32 sample_index) const {
        u32 micro_tile_bytes = MICRO_TILE_PIXELS * thickness * BITS_PER_PIXEL * NUM_SAMPLES / 8;
        u32 slice_bytes = BITS_TO_BYTES(pitch * height * thickness * BITS_PER_PIXEL * NUM_SAMPLES);

        u32 micro_tiles_per_row = pitch / MICRO_TILE_WIDTH;
        u32 micro_tile_index_x = x / MICRO_TILE_WIDTH;
        u32 micro_tile_index_y = y / MICRO_TILE_HEIGHT;
        u32 micro_tile_index_z = slice / thickness;

        u32 slice_offset = micro_tile_index_z * slice_bytes;
        u32 micro_tile_offset =
            (micro_tile_index_y * micro_tiles_per_row + micro_tile_index_x) * micro_tile_bytes;

        u32 pixel_index = ComputePixelIndexWithinMicroTile(x, y, slice, thickness);

        u32 sample_offset;
        u32 pixel_offset;
        if (MICRO_TILE_MODE == ADDR_SURF_DEPTH_MICRO_TILING) {
            sample_offset = sample_index * BITS_PER_PIXEL;
            pixel_offset = pixel_index * BITS_PER_PIXEL * NUM_SAMPLES;
        } else {
            sample_offset = sample_index * (micro_tile_bytes * 8 / NUM_SAMPLES);
            pixel_offset = pixel_index * BITS_PER_PIXEL;
        }

        u32 elem_offset = (sample_offset + pixel_offset) / 8;
        return slice_offset + micro_tile_offset + elem_offset;
    }

    u32 ComputePipeFromCoord(u32 x, u32 y, u32 slice) const {
        u32 p0 = 0;
        u32 p1 = 0;
        u32 p2 = 0;

        u32 tx = x / MICRO_TILE_WIDTH;
        u32 ty = y / MICRO_TILE_HEIGHT;
        u32 x3 = _BIT(tx, 0);
        u32 x4 = _BIT(tx, 1);
        u32 x5 = _BIT(tx, 2);
        u32 y3 = _BIT(ty, 0);
        u32 y4 = _BIT(ty, 1);
        u32 y5 = _BIT(ty, 2);

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

        u32 pipe = p0 | (p1 << 1) | (p2 << 2);

        u32 pipe_swizzle = 0;
        if (ARRAY_MODE == ARRAY_3D_TILED_THIN1 || ARRAY_MODE == ARRAY_3D_TILED_THICK ||
            ARRAY_MODE == ARRAY_3D_TILED_XTHICK) {
            pipe_swizzle += std::max(1u, NUM_PIPES / 2 - 1) * (slice / MICRO_TILE_THICKNESS);
        }
        pipe_swizzle &= (NUM_PIPES - 1);
        pipe = pipe ^ pipe_swizzle;
        return pipe;
    }

    u32 ComputeBankFromCoord(u32 x, u32 y, u32 slice, u32 tile_split_slice) const {
        u32 b0 = 0;
        u32 b1 = 0;
        u32 b2 = 0;
        u32 b3 = 0;
        u32 slice_rotation = 0;
        u32 tile_split_rotation = 0;

        u32 tx = x / MICRO_TILE_WIDTH / (BANK_WIDTH * NUM_PIPES);
        u32 ty = y / MICRO_TILE_HEIGHT / BANK_HEIGHT;

        u32 x3 = _BIT(tx, 0);
        u32 x4 = _BIT(tx, 1);
        u32 x5 = _BIT(tx, 2);
        u32 x6 = _BIT(tx, 3);
        u32 y3 = _BIT(ty, 0);
        u32 y4 = _BIT(ty, 1);
        u32 y5 = _BIT(ty, 2);
        u32 y6 = _BIT(ty, 3);

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

        u32 bank = b0 | (b1 << 1) | (b2 << 2) | (b3 << 3);

        if (ARRAY_MODE == ARRAY_2D_TILED_THIN1 || ARRAY_MODE == ARRAY_2D_TILED_THICK ||
            ARRAY_MODE == ARRAY_2D_TILED_XTHICK) {
            slice_rotation = ((NUM_BANKS / 2) - 1) * (slice / MICRO_TILE_THICKNESS);
        } else if (ARRAY_MODE == ARRAY_3D_TILED_THIN1 || ARRAY_MODE == ARRAY_3D_TILED_THICK ||
                   ARRAY_MODE == ARRAY_3D_TILED_XTHICK) {
            slice_rotation =
                std::max(1u, (NUM_PIPES / 2) - 1) * (slice / MICRO_TILE_THICKNESS) / NUM_PIPES;
        }

        if (ARRAY_MODE == ARRAY_2D_TILED_THIN1 || ARRAY_MODE == ARRAY_3D_TILED_THIN1 ||
            ARRAY_MODE == ARRAY_PRT_2D_TILED_THIN1 || ARRAY_MODE == ARRAY_PRT_3D_TILED_THIN1) {
            tile_split_rotation = ((NUM_BANKS / 2) + 1) * tile_split_slice;
        }

        bank ^= bank_swizzle + slice_rotation;
        bank ^= tile_split_rotation;
        bank &= (NUM_BANKS - 1);

        return bank;
    }

    u32 ComputeSurfaceAddrFromCoordMacroTiled(u32 x, u32 y, u32 slice, u32 pitch, u32 height,
                                              u32 sample_index) const {
        u32 pixel_index = ComputePixelIndexWithinMicroTile(x, y, slice, MICRO_TILE_THICKNESS);

        u32 sample_offset;
        u32 pixel_offset;
        if (MICRO_TILE_MODE == ADDR_SURF_DEPTH_MICRO_TILING) {
            sample_offset = sample_index * BITS_PER_PIXEL;
            pixel_offset = pixel_index * BITS_PER_PIXEL * NUM_SAMPLES;
        } else {
            sample_offset = sample_index * (MICRO_TILE_BITS / NUM_SAMPLES);
            pixel_offset = pixel_index * BITS_PER_PIXEL;
        }

        u32 element_offset = (pixel_offset + sample_offset) / 8;

        u32 micro_tile_bytes = MICRO_TILE_BYTES;
        u32 slices_per_tile = 1;
        u32 tile_split_slice = 0;
        if (MICRO_TILE_BYTES > TILE_SPLIT_BYTES && MICRO_TILE_THICKNESS == 1) {
            slices_per_tile = MICRO_TILE_BYTES / TILE_SPLIT_BYTES;
            tile_split_slice = element_offset / TILE_SPLIT_BYTES;
            element_offset %= TILE_SPLIT_BYTES;
            micro_tile_bytes = TILE_SPLIT_BYTES;
        }

        u32 macro_tile_pitch = (MICRO_TILE_WIDTH * BANK_WIDTH * NUM_PIPES) * MACRO_TILE_ASPECT;
        u32 macro_tile_height = (MICRO_TILE_HEIGHT * BANK_HEIGHT * NUM_BANKS) / MACRO_TILE_ASPECT;

        u32 macro_tile_bytes = micro_tile_bytes * (macro_tile_pitch / MICRO_TILE_WIDTH) *
                               (macro_tile_height / MICRO_TILE_HEIGHT) / (NUM_PIPES * NUM_BANKS);

        u32 macro_tiles_per_row = pitch / macro_tile_pitch;

        u32 macro_tile_index_x = x / macro_tile_pitch;
        u32 macro_tile_index_y = y / macro_tile_height;
        u32 macro_tile_offset =
            ((macro_tile_index_y * macro_tiles_per_row) + macro_tile_index_x) * macro_tile_bytes;
        u32 macro_tiles_per_slice = macro_tiles_per_row * (height / macro_tile_height);

        u32 slice_bytes = macro_tiles_per_slice * macro_tile_bytes;
        u32 slice_offset =
            slice_bytes * (tile_split_slice + slices_per_tile * (slice / MICRO_TILE_THICKNESS));

        u32 tile_row_index = (y / MICRO_TILE_HEIGHT) % BANK_HEIGHT;
        u32 tile_column_index = ((x / MICRO_TILE_WIDTH) / NUM_PIPES) % BANK_WIDTH;
        u32 tile_index = (tile_row_index * BANK_WIDTH) + tile_column_index;
        u32 tile_offset = tile_index * micro_tile_bytes;

        u32 total_offset = slice_offset + macro_tile_offset + element_offset + tile_offset;

        if (ARRAY_MODE == ARRAY_PRT_TILED_THIN1 || ARRAY_MODE == ARRAY_PRT_TILED_THICK ||
            ARRAY_MODE == ARRAY_PRT_2D_TILED_THIN1 || ARRAY_MODE == ARRAY_PRT_2D_TILED_THICK ||
            ARRAY_MODE == ARRAY_PRT_3D_TILED_THIN1 || ARRAY_MODE == ARRAY_PRT_3D_TILED_THICK) {
            x %= macro_tile_pitch;
            y %= macro_tile_height;
        }

        u32 pipe = ComputePipeFromCoord(x, y, slice);
        u32 bank = ComputeBankFromCoord(x, y, slice, tile_split_slice);

        u32 pipe_interleave_mask = (1 << NUM_PIPE_INTERLEAVE_BITS) - 1;
        u32 pipe_interleave_offset = total_offset & pipe_interleave_mask;
        u32 offset = total_offset >> NUM_PIPE_INTERLEAVE_BITS;

        u32 addr = pipe_interleave_offset;
        u32 pipe_bits = pipe << NUM_PIPE_INTERLEAVE_BITS;
        u32 bank_bits = bank << (NUM_PIPE_INTERLEAVE_BITS + NUM_PIPE_BITS);
        u32 offset_bits = offset << (NUM_PIPE_INTERLEAVE_BITS + NUM_PIPE_BITS + NUM_BANK_BITS);

        addr |= pipe_bits;
        addr |= bank_bits;
        addr |= offset_bits;

        return addr;
    }
};
template <u32 Bits>
void Convert(std::span<const u8> source, std::span<u8> destination, const ImageInfo& info,
             bool encode) {
    TileAddress<Bits> address{info};
    const u32 block = Bits / 8;
    u64 linear = 0;
    for (u32 mip = 0; mip < info.resources.levels; ++mip) {
        const auto& layout = info.mips_layout[mip];
        const u32 pitch =
            info.props.is_block ? std::max<u32>((layout.pitch + 3) / 4, 1) : layout.pitch;
        const u32 height =
            info.props.is_block ? std::max<u32>((layout.height + 3) / 4, 1) : layout.height;
        if (!pitch || !height)
            throw std::runtime_error("Invalid guest image tiling dimensions");
        for (u32 texel = 0; texel < layout.size / block; ++texel) {
            const u32 x = texel % pitch, y = (texel / pitch) % height,
                      slice = texel / (pitch * height);
            const bool macro =
                AmdGpu::IsMacroTiled(info.array_mode) && !(info.micro_tiled_mips & (1u << mip));
            const u64 tiled =
                layout.offset +
                (macro
                     ? address.ComputeSurfaceAddrFromCoordMacroTiled(x, y, slice, pitch, height, 0)
                     : address.ComputeSurfaceAddrFromCoordMicroTiled(
                           x, y, slice, pitch, height, std::min(address.MICRO_TILE_THICKNESS, 4u),
                           0));
            const u64 from = encode ? linear : tiled, to = encode ? tiled : linear;
            if (from + block > source.size() || to + block > destination.size())
                throw std::runtime_error("Guest tiled image exceeds backing range");
            std::memcpy(destination.data() + to, source.data() + from, block);
            linear += block;
        }
    }
}
} // namespace
void ConvertTiling(std::span<const u8> source, std::span<u8> destination, const ImageInfo& info,
                   bool encode) {
    if (info.num_samples != 1 || info.resources.levels > MAX_MIPS ||
        info.guest_size > source.size() || info.guest_size > destination.size())
        throw std::runtime_error("Unsupported guest image tiling range or multisampling");
    if (!info.props.is_tiled) {
        std::memcpy(destination.data(), source.data(), info.guest_size);
        return;
    }
    switch (info.num_bits) {
    case 8:
        return Convert<8>(source, destination, info, encode);
    case 16:
        return Convert<16>(source, destination, info, encode);
    case 32:
        return Convert<32>(source, destination, info, encode);
    case 64:
        return Convert<64>(source, destination, info, encode);
    case 96:
        return Convert<96>(source, destination, info, encode);
    case 128:
        return Convert<128>(source, destination, info, encode);
    default:
        throw std::runtime_error("Unsupported guest image block size");
    }
}
} // namespace VideoCore
