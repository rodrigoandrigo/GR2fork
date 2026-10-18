#pragma once

#include <array>
#include <cstring>
#include <stdexcept>
#include <d3d12.h>
#include "common/types.h"

namespace D3D12 {
struct LogicOpFormat {
    enum class Encoding { Unorm, Snorm, Float, UnsignedFloat, Uint, Sint };
    std::array<u32, 4> bits{}, offsets{};
    Encoding encoding{};
    bool srgb{};
    u32 bytes{};
};
inline LogicOpFormat GetLogicOpFormat(DXGI_FORMAT format) {
    using Encoding = LogicOpFormat::Encoding;
    LogicOpFormat result{};
    switch (format) {
#define LOGIC_FORMAT(name, count, width, kind)                                                     \
    case DXGI_FORMAT_##name:                                                                       \
        result.encoding = Encoding::kind;                                                          \
        result.bytes = count * width / 8;                                                          \
        for (u32 c = 0; c < count; ++c) {                                                          \
            result.bits[c] = width;                                                                \
            result.offsets[c] = c * width;                                                         \
        }                                                                                          \
        return result
        LOGIC_FORMAT(R8_UNORM, 1, 8, Unorm);
        LOGIC_FORMAT(R8_SNORM, 1, 8, Snorm);
        LOGIC_FORMAT(R8G8_UNORM, 2, 8, Unorm);
        LOGIC_FORMAT(R8G8_SNORM, 2, 8, Snorm);
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        result.srgb = true;
        LOGIC_FORMAT(R8G8B8A8_UNORM, 4, 8, Unorm);
        LOGIC_FORMAT(R8G8B8A8_SNORM, 4, 8, Snorm);
        LOGIC_FORMAT(R16_UNORM, 1, 16, Unorm);
        LOGIC_FORMAT(R16_SNORM, 1, 16, Snorm);
        LOGIC_FORMAT(R16_FLOAT, 1, 16, Float);
        LOGIC_FORMAT(R16G16_UNORM, 2, 16, Unorm);
        LOGIC_FORMAT(R16G16_SNORM, 2, 16, Snorm);
        LOGIC_FORMAT(R16G16_FLOAT, 2, 16, Float);
        LOGIC_FORMAT(R16G16B16A16_UNORM, 4, 16, Unorm);
        LOGIC_FORMAT(R16G16B16A16_SNORM, 4, 16, Snorm);
        LOGIC_FORMAT(R16G16B16A16_FLOAT, 4, 16, Float);
        LOGIC_FORMAT(R32_FLOAT, 1, 32, Float);
        LOGIC_FORMAT(R32G32_FLOAT, 2, 32, Float);
        LOGIC_FORMAT(R32G32B32A32_FLOAT, 4, 32, Float);
        LOGIC_FORMAT(R8_UINT, 1, 8, Uint);
        LOGIC_FORMAT(R8_SINT, 1, 8, Sint);
        LOGIC_FORMAT(R8G8_UINT, 2, 8, Uint);
        LOGIC_FORMAT(R8G8_SINT, 2, 8, Sint);
        LOGIC_FORMAT(R8G8B8A8_UINT, 4, 8, Uint);
        LOGIC_FORMAT(R8G8B8A8_SINT, 4, 8, Sint);
        LOGIC_FORMAT(R16_UINT, 1, 16, Uint);
        LOGIC_FORMAT(R16_SINT, 1, 16, Sint);
        LOGIC_FORMAT(R16G16_UINT, 2, 16, Uint);
        LOGIC_FORMAT(R16G16_SINT, 2, 16, Sint);
        LOGIC_FORMAT(R16G16B16A16_UINT, 4, 16, Uint);
        LOGIC_FORMAT(R16G16B16A16_SINT, 4, 16, Sint);
        LOGIC_FORMAT(R32_UINT, 1, 32, Uint);
        LOGIC_FORMAT(R32_SINT, 1, 32, Sint);
        LOGIC_FORMAT(R32G32_UINT, 2, 32, Uint);
        LOGIC_FORMAT(R32G32_SINT, 2, 32, Sint);
        LOGIC_FORMAT(R32G32B32A32_UINT, 4, 32, Uint);
        LOGIC_FORMAT(R32G32B32A32_SINT, 4, 32, Sint);
#undef LOGIC_FORMAT
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return {{8, 8, 8, 8},
                {16, 8, 0, 24},
                Encoding::Unorm,
                format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                4};
    case DXGI_FORMAT_B4G4R4A4_UNORM:
        return {{4, 4, 4, 4}, {8, 4, 0, 12}, Encoding::Unorm, false, 2};
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return {{10, 10, 10, 2}, {0, 10, 20, 30}, Encoding::Unorm, false, 4};
    case DXGI_FORMAT_R10G10B10A2_UINT:
        return {{10, 10, 10, 2}, {0, 10, 20, 30}, Encoding::Uint, false, 4};
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return {{11, 11, 10, 0}, {0, 11, 22, 0}, Encoding::UnsignedFloat, false, 4};
    default:
        throw std::runtime_error("Unsupported non-integer logic operation format");
    }
}
inline void CopyLogicOpTexel(const LogicOpFormat& format, u8* guest, u8* native, bool upload,
                             u32 write_mask) {
    if (upload)
        std::memset(native, 0, 16);
    u32 component = 0;
    for (u32 c = 0; c < 4; ++c) {
        if (!(write_mask & (1u << c)))
            continue;
        u32 value{};
        if (!format.bits[c])
            continue;
        const u32 shift = format.offsets[c] % 8;
        const u32 bytes = (shift + format.bits[c] + 7) / 8;
        const u64 mask = ((u64{1} << format.bits[c]) - 1) << shift;
        u64 word{};
        std::memcpy(&word, guest + format.offsets[c] / 8, bytes);
        if (upload) {
            value = u32((word & mask) >> shift);
            std::memcpy(native + component * 4, &value, 4);
        } else {
            std::memcpy(&value, native + component * 4, 4);
            word = (word & ~mask) | ((u64(value) << shift) & mask);
            std::memcpy(guest + format.offsets[c] / 8, &word, bytes);
        }
        ++component;
    }
}
} // namespace D3D12
