#pragma once

#include <initializer_list>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>
#include "video_core/amdgpu/regs_vertex.h"

namespace D3D12 {
inline void SwapIndexBytes(std::span<u8> bytes, AmdGpu::IndexSwapMode swap) {
    using S = AmdGpu::IndexSwapMode;
    if (swap == S::None) {
        return;
    }
    const u64 unit = swap == S::Swap16 ? 2 : 4;
    if (bytes.size() % unit) {
        throw std::runtime_error("Guest swapped index range is not a multiple of swap unit");
    }
    for (u64 base = 0; base < bytes.size(); base += unit) {
        if (swap == S::Swap16) {
            std::swap(bytes[base], bytes[base + 1]);
        } else if (swap == S::Swap32) {
            std::swap(bytes[base], bytes[base + 3]);
            std::swap(bytes[base + 1], bytes[base + 2]);
        } else {
            std::swap(bytes[base], bytes[base + 2]);
            std::swap(bytes[base + 1], bytes[base + 3]);
        }
    }
}
inline std::vector<u32> ConvertPrimitiveIndices(AmdGpu::PrimitiveType primitive,
                                                std::span<const u32> input,
                                                std::optional<u32> restart = {}) {
    std::vector<u32> output;
    const auto emit = [&](std::initializer_list<u32> vertices) {
        if (output.size() + vertices.size() > 4 * 1024 * 1024)
            throw std::runtime_error("Converted D3D12 indices exceed upload budget");
        output.insert(output.end(), vertices);
    };
    const auto segment = [&](std::span<const u32> indices) {
        using P = AmdGpu::PrimitiveType;
        switch (primitive) {
        case P::PointList:
            for (const auto index : indices)
                emit({index});
            break;
        case P::LineList:
            for (size_t i = 0; i + 1 < indices.size(); i += 2)
                emit({indices[i], indices[i + 1]});
            break;
        case P::LineStrip:
        case P::LineLoop:
            for (size_t i = 1; i < indices.size(); ++i)
                emit({indices[i - 1], indices[i]});
            if (primitive == P::LineLoop && indices.size() > 1)
                emit({indices.back(), indices.front()});
            break;
        case P::TriangleList:
        case P::RectList:
            for (size_t i = 0; i + 2 < indices.size(); i += 3)
                emit({indices[i], indices[i + 1], indices[i + 2]});
            break;
        case P::TriangleStrip:
            for (size_t i = 2; i < indices.size(); ++i) {
                if (i & 1)
                    emit({indices[i - 1], indices[i - 2], indices[i]});
                else
                    emit({indices[i - 2], indices[i - 1], indices[i]});
            }
            break;
        case P::TriangleFan:
        case P::Polygon:
            for (size_t i = 2; i < indices.size(); ++i)
                emit({indices[0], indices[i - 1], indices[i]});
            break;
        case P::QuadList:
            for (size_t i = 0; i + 3 < indices.size(); i += 4) {
                emit({indices[i], indices[i + 1], indices[i + 2]});
                emit({indices[i], indices[i + 2], indices[i + 3]});
            }
            break;
        case P::QuadStrip:
            for (size_t i = 0; i + 3 < indices.size(); i += 2) {
                emit({indices[i], indices[i + 1], indices[i + 3]});
                emit({indices[i], indices[i + 3], indices[i + 2]});
            }
            break;
        default:
            throw std::runtime_error("Unsupported D3D12 primitive index conversion");
        }
    };
    size_t first{};
    for (size_t i = 0; i < input.size(); ++i) {
        if (restart && input[i] == *restart) {
            segment(input.subspan(first, i - first));
            first = i + 1;
        }
    }
    segment(input.subspan(first));
    return output;
}
} // namespace D3D12
