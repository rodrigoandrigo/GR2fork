#pragma once

#include <map>
#include <memory>
#include <vector>
#include <d3d12.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include "shader_recompiler/info.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"

namespace D3D12 {
enum class ResourceKind { Buffer, Image, Sampler, Constants };
struct ShaderBinding {
    ResourceKind kind;
    D3D12_DESCRIPTOR_RANGE_TYPE type;
    u32 binding;
    u32 space;
    u32 count;
};
struct ShaderBinary {
    std::vector<u8> dxil;
    std::string hlsl;
    std::vector<ShaderBinding> bindings;
};
struct CompiledShader {
    Shader::Info info;
    Shader::RuntimeInfo runtime;
    Shader::Backend::Bindings start_bindings;
    Shader::Backend::Bindings end_bindings;
    std::shared_ptr<const ShaderBinary> binary;
};
class ShaderCache {
public:
    void Initialize(ID3D12Device* device);
    CompiledShader Compile(Shader::ShaderParams params, Shader::RuntimeInfo runtime,
                           Shader::Backend::Bindings bindings = {});
    std::shared_ptr<const ShaderBinary> CompileSpirv(std::span<const u32> words,
                                                     Shader::SwStage stage);
    const Shader::Profile& Profile() const {
        return profile;
    }

private:
    Shader::Profile profile{};
    Shader::Pools pools;
    Microsoft::WRL::ComPtr<IDxcUtils> utils;
    Microsoft::WRL::ComPtr<IDxcCompiler3> compiler;
    std::map<std::vector<u32>, std::shared_ptr<const ShaderBinary>> cache;
    size_t cache_bytes{};
};
} // namespace D3D12
