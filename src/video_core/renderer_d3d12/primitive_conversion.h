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
                                                std::optional<u32> restart = {},
                                                bool provoking_last = false) {
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
                if ((i & 1) && provoking_last)
                    emit({indices[i - 1], indices[i - 2], indices[i]});
                else if (i & 1)
                    emit({indices[i - 2], indices[i], indices[i - 1]});
                else
                    emit({indices[i - 2], indices[i - 1], indices[i]});
            }
            break;
        case P::AdjLineList:
            for (size_t i = 0; i + 3 < indices.size(); i += 4)
                emit({indices[i + 1], indices[i + 2]});
            break;
        case P::AdjLineStrip:
            for (size_t i = 0; i + 3 < indices.size(); ++i)
                emit({indices[i + 1], indices[i + 2]});
            break;
        case P::AdjTriangleList:
            for (size_t i = 0; i + 5 < indices.size(); i += 6)
                emit({indices[i], indices[i + 2], indices[i + 4]});
            break;
        case P::AdjTriangleStrip:
            for (size_t i = 0; i + 5 < indices.size(); i += 2) {
                if ((i / 2) & 1) {
                    if (provoking_last)
                        emit({indices[i + 2], indices[i], indices[i + 4]});
                    else
                        emit({indices[i], indices[i + 4], indices[i + 2]});
                } else {
                    emit({indices[i], indices[i + 2], indices[i + 4]});
                }
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
inline std::vector<u32> ConvertPolygonFaceMasks(AmdGpu::PrimitiveType primitive,
                                                std::span<const u32> input,
                                                std::optional<u32> restart = {}) {
    std::vector<u32> output;
    const auto segment = [&](size_t count) {
        if (primitive == AmdGpu::PrimitiveType::Polygon) {
            for (size_t vertex = 2; vertex < count; ++vertex)
                output.push_back(2u | (vertex == 2 ? 1u : 0u) | (vertex + 1 == count ? 4u : 0u) |
                                 ((vertex == 2 ? 7u : 4u) << 3));
        } else if (primitive == AmdGpu::PrimitiveType::QuadList ||
                   primitive == AmdGpu::PrimitiveType::QuadStrip) {
            const size_t step = primitive == AmdGpu::PrimitiveType::QuadList ? 4 : 2;
            for (size_t vertex = 0; vertex + 3 < count; vertex += step) {
                output.push_back(59);
                output.push_back(38);
            }
        }
    };
    size_t first{};
    for (size_t index = 0; index < input.size(); ++index)
        if (restart && input[index] == *restart) {
            segment(index - first);
            first = index + 1;
        }
    segment(input.size() - first);
    return output;
}
inline std::vector<u32> ConvertPolygonFlatVertices(AmdGpu::PrimitiveType primitive,
                                                   std::span<const u32> input,
                                                   std::optional<u32> restart = {}) {
    std::vector<u32> output;
    const auto segment = [&](size_t count) {
        if (primitive == AmdGpu::PrimitiveType::Polygon && count >= 3) {
            const u32 triangles = u32(count - 2);
            output.insert(output.end(), triangles, (u32(output.size()) + triangles) * 3 - 1);
        } else if (primitive == AmdGpu::PrimitiveType::QuadList ||
                   primitive == AmdGpu::PrimitiveType::QuadStrip) {
            const size_t step = primitive == AmdGpu::PrimitiveType::QuadList ? 4 : 2;
            for (size_t i = 0; i + 3 < count; i += step) {
                const u32 index = u32(output.size()) * 3 + (step == 4 ? 5 : 2);
                output.insert(output.end(), {index, index});
            }
        }
    };
    size_t first{};
    for (size_t i = 0; i < input.size(); ++i)
        if (restart && input[i] == *restart) {
            segment(i - first);
            first = i + 1;
        }
    segment(input.size() - first);
    return output;
}
inline std::vector<u32> ConvertPolygonPoints(AmdGpu::PrimitiveType primitive,
                                             std::span<const u32> input,
                                             std::optional<u32> restart = {}) {
    const std::vector<u32> triangles = ConvertPrimitiveIndices(primitive, input, restart);
    std::vector<u32> points;
    points.reserve(triangles.size());
    for (size_t i = 0; i + 2 < triangles.size(); i += 3)
        points.insert(points.end(), triangles.begin() + static_cast<ptrdiff_t>(i),
                      triangles.begin() + static_cast<ptrdiff_t>(i) + 3);
    return points;
}
inline std::vector<u32> ConvertPolygonEdges(AmdGpu::PrimitiveType primitive,
                                            std::span<const u32> input,
                                            std::optional<u32> restart = {}) {
    std::vector<u32> edges;
    const auto emit_edge = [&](u32 a, u32 b) {
        if (edges.size() + 2 > 4 * 1024 * 1024)
            throw std::runtime_error("Converted D3D12 edge indices exceed upload budget");
        edges.push_back(a);
        edges.push_back(b);
    };
    const auto segment = [&](std::span<const u32> indices) {
        using P = AmdGpu::PrimitiveType;
        switch (primitive) {
        case P::RectList:
            throw std::runtime_error("D3D12 RectList wireframe requires a synthesized vertex");
        case P::QuadList:
            for (size_t i = 0; i + 3 < indices.size(); i += 4) {
                emit_edge(indices[i], indices[i + 1]);
                emit_edge(indices[i + 1], indices[i + 2]);
                emit_edge(indices[i + 2], indices[i + 3]);
                emit_edge(indices[i + 3], indices[i]);
            }
            break;
        case P::QuadStrip:
            for (size_t i = 0; i + 3 < indices.size(); i += 2) {
                emit_edge(indices[i], indices[i + 1]);
                emit_edge(indices[i + 1], indices[i + 3]);
                emit_edge(indices[i + 3], indices[i + 2]);
                emit_edge(indices[i + 2], indices[i]);
            }
            break;
        case P::Polygon:
            for (size_t i = 1; i < indices.size(); ++i)
                emit_edge(indices[i - 1], indices[i]);
            if (indices.size() > 2)
                emit_edge(indices.back(), indices.front());
            break;
        default:
            throw std::runtime_error("Unsupported D3D12 polygon edge conversion");
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
    return edges;
}
} // namespace D3D12
