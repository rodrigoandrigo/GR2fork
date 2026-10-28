#pragma once

#include <span>
#include <vector>
#include "common/types.h"

struct ID3D12Device;

namespace Shader {
struct Info;
struct ImageResource;
enum class SwStage : u32;
} // namespace Shader

namespace Shader::IR {
struct Program;
}
namespace AmdGpu {
struct Image;
}
namespace VideoCore {
struct ImageInfo;
}
namespace D3D12 {
inline constexpr u32 SharedSamplerWords = 16;
VideoCore::ImageInfo ShaderImageLayout(const AmdGpu::Image& sharp,
                                       const Shader::ImageResource& resource);
struct CompiledShader;
class ResourceCache;
struct IndexFetch {
    VAddr address{};
    u32 size{}, element_size{}, byte_xor{};
    u32 count{UINT32_MAX};
    bool operator==(const IndexFetch&) const = default;
};
bool LowerIndexFetch(CompiledShader& shader, Shader::IR::Program& program,
                     const IndexFetch& indices);
struct ShaderResourceRange {
    VAddr address;
    u64 size;
    Shader::SwStage stage;
    bool written;
};
std::vector<ShaderResourceRange> GetShaderResourceRanges(const Shader::Info& info);
bool LowerBufferAliases(CompiledShader& shader, Shader::IR::Program& program,
                        const ResourceCache* resources);
bool LowerImageBufferAliases(CompiledShader& shader, Shader::IR::Program& program,
                             const ResourceCache* resources,
                             std::span<const ShaderResourceRange> stage_resources = {},
                             ID3D12Device* device = nullptr);
} // namespace D3D12
