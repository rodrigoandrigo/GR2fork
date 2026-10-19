// SPDX-FileCopyrightText: Copyright (C) 2007-2024 Advanced Micro Devices, Inc.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <stdexcept>
#include <utility>
#include "video_core/amdgpu/regs_color.h"

namespace D3D12 {
inline uint64_t CmaskBytes(const AmdGpu::ColorBuffer& buffer) {
    const uint64_t bytes = (uint64_t(buffer.cmask_slice.tile_max) + 1) * 128 * buffer.NumSlices();
    if (bytes > 64 * 1024 * 1024)
        throw std::runtime_error("D3D12 CMASK exceeds metadata budget");
    return bytes;
}

struct CmaskAddress {
    AmdGpu::PipeConfig pipe;
    uint32_t pipes, pitch, height, macro_width, macro_height, micro_shift, element_bits;
    uint64_t slice_bytes;
    bool linear;

    CmaskAddress(uint32_t width, uint32_t rows, AmdGpu::PipeConfig config, bool is_linear)
        : pipe(config), linear(is_linear) {
        using P = AmdGpu::PipeConfig;
        switch (pipe) {
        case P::P2:
            pipes = 2;
            micro_shift = 3;
            element_bits = 3;
            break;
        case P::P8_32x32_8x16:
        case P::P8_32x32_16x16:
            pipes = 8;
            micro_shift = 1;
            element_bits = 2;
            break;
        case P::P16_32x32_8x16:
            pipes = 16;
            micro_shift = 0;
            element_bits = 2;
            break;
        default:
            throw std::invalid_argument("Guest CMASK pipe is outside the PS4 tile tables");
        }
        if (linear) {
            macro_width = macro_height = pipe == P::P2 ? 32 : 64;
        } else {
            uint32_t cache_width = 256, cache_height = 1;
            while (cache_width > cache_height * 2 * pipes && !(cache_width & 1)) {
                cache_width /= 2;
                cache_height *= 2;
            }
            macro_width = 8 * cache_width;
            macro_height = 8 * cache_height * pipes;
        }
        if (!width || !rows || width > UINT32_MAX - macro_width + 1 ||
            rows > UINT32_MAX - macro_height + 1)
            throw std::out_of_range("Guest CMASK dimensions cannot be padded");
        pitch = (width + macro_width - 1) & ~(macro_width - 1);
        height = (rows + macro_height - 1) & ~(macro_height - 1);
        const uint32_t alignment = 256 * pipes;
        while ((uint64_t(pitch) * height / 128) % alignment) {
            if (height > UINT32_MAX - macro_height)
                throw std::out_of_range("Guest CMASK slice alignment overflow");
            height += macro_height;
        }
        slice_bytes = uint64_t(pitch) * height / 128;
    }

    uint32_t Pipe(uint32_t tx, uint32_t ty) const {
        const auto b = [](uint32_t value, uint32_t bit) { return (value >> bit) & 1u; };
        using P = AmdGpu::PipeConfig;
        switch (pipe) {
        case P::P2:
            return b(tx, 0) ^ b(ty, 0);
        case P::P8_32x32_8x16:
            return (b(tx, 1) ^ b(ty, 0) ^ b(tx, 2)) | ((b(tx, 0) ^ b(ty, 1)) << 1) |
                   ((b(tx, 2) ^ b(ty, 2)) << 2);
        case P::P8_32x32_16x16:
            return (b(tx, 0) ^ b(ty, 0) ^ b(tx, 1)) | ((b(tx, 1) ^ b(ty, 1)) << 1) |
                   ((b(tx, 2) ^ b(ty, 2)) << 2);
        case P::P16_32x32_8x16:
            return (b(tx, 1) ^ b(ty, 0)) | ((b(tx, 0) ^ b(ty, 1)) << 1) |
                   ((b(tx, 2) ^ b(ty, 3)) << 2) | ((b(tx, 3) ^ b(ty, 2)) << 3);
        default:
            throw std::invalid_argument("Invalid guest CMASK pipe");
        }
    }

    uint32_t Element(uint32_t tx, uint32_t ty) const {
        const uint32_t x1 = (tx >> 1) & 1, y0 = ty & 1, y1 = (ty >> 1) & 1;
        using P = AmdGpu::PipeConfig;
        switch (pipe) {
        case P::P2:
            return (x1 ^ y0) | ((x1 ^ y1) << 1) | (x1 << 2);
        case P::P8_32x32_8x16:
        case P::P16_32x32_8x16:
            return (x1 ^ y1) | (x1 << 1);
        case P::P8_32x32_16x16:
            return (x1 ^ y0) | (x1 << 1);
        default:
            throw std::invalid_argument("Invalid guest CMASK element configuration");
        }
    }

    std::pair<uint64_t, uint32_t> At(uint32_t layer, uint32_t x, uint32_t y) const {
        if (x >= pitch || y >= height)
            throw std::out_of_range("Guest CMASK coordinate exceeds padded dimensions");
        const uint32_t tx = x / 8, ty = y / 8;
        uint64_t micro_number, macro_offset;
        if (linear) {
            micro_number = (uint64_t(tx / 4) + uint64_t(ty / 4) * (pitch / 32)) << micro_shift;
            macro_offset = uint64_t(layer) * (pitch / 8) * (height / 8) / pipes * 4;
        } else {
            micro_number = (uint64_t((x % macro_width) / 32) +
                            uint64_t((y % macro_height) / 32) * (macro_width / 32))
                           << micro_shift;
            const uint64_t macro_number =
                uint64_t(x / macro_width) + uint64_t(y / macro_height) * (pitch / macro_width) +
                uint64_t(layer) * (pitch / macro_width) * (height / macro_height);
            macro_offset = macro_number * 1024;
        }
        if (element_bits != micro_shift)
            micro_number &= ~((uint64_t(1) << element_bits) - 1);
        micro_number += Element(tx, ty);
        const uint64_t bits = macro_offset + micro_number * 4;
        const uint64_t byte =
            (bits % 2048) / 8 + uint64_t(Pipe(tx, ty)) * 256 + (bits / 2048) * 256 * pipes;
        if (byte < uint64_t(layer) * slice_bytes || byte >= (uint64_t(layer) + 1) * slice_bytes)
            throw std::out_of_range("Guest CMASK tile exceeds metadata slice");
        return {byte, uint32_t(bits % 8)};
    }
};

} // namespace D3D12
