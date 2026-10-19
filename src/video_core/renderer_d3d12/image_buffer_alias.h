#pragma once

#include <span>
#include <vector>
#include "common/types.h"

namespace Shader {
struct Info;
enum class SwStage : u32;
} // namespace Shader

namespace Shader::IR {
struct Program;
}
namespace D3D12 {
struct CompiledShader;
class ResourceCache;
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
                             std::span<const ShaderResourceRange> stage_resources = {});
} // namespace D3D12
