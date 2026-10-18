#pragma once

namespace Shader::IR {
struct Program;
}
namespace D3D12 {
struct CompiledShader;
class ResourceCache;
bool LowerImageBufferAliases(CompiledShader& shader, Shader::IR::Program& program,
                             const ResourceCache* resources);
} // namespace D3D12
