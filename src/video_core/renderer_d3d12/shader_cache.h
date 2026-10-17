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
struct TessellationLink {
    AmdGpu::TessellationType type;
    AmdGpu::TessellationPartitioning partitioning;
    AmdGpu::TessellationTopology topology;
    u32 input_control_points, output_control_points;
};
struct PreparedShader {
    CompiledShader shader;
    std::vector<u32> words;
};
class ShaderCache {
public:
    void Initialize(ID3D12Device* device);
    PreparedShader Prepare(Shader::ShaderParams params, Shader::RuntimeInfo runtime,
                           Shader::Backend::Bindings bindings);
    CompiledShader Finalize(PreparedShader& shader, bool flip_y, bool sample_mask,
                            bool sample_shading, const TessellationLink* tessellation,
                            std::span<const u32> peer = {}, bool depth_bounds = false);
    CompiledShader Compile(Shader::ShaderParams params, Shader::RuntimeInfo runtime,
                           Shader::Backend::Bindings bindings = {}, bool flip_y = false,
                           bool sample_mask = false, bool sample_shading = false,
                           const TessellationLink* tessellation = nullptr,
                           bool depth_bounds = false);
    std::shared_ptr<const ShaderBinary> CompileSpirv(std::span<const u32> words,
                                                     Shader::SwStage stage, bool flip_y = false,
                                                     bool sample_mask = false,
                                                     bool sample_shading = false,
                                                     const TessellationLink* tessellation = nullptr,
                                                     std::span<const u32> peer = {},
                                                     bool depth_bounds = false);
    std::shared_ptr<const ShaderBinary> CompileRectangle(
        const ShaderBinary& vertex, const Shader::HwFragmentRuntimeInfo& fragment);
    std::shared_ptr<const ShaderBinary> CompileHlslRaw(const std::string& hlsl,
                                                       std::wstring_view target,
                                                       std::span<const std::wstring> defines);
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
    bool fixed_graphics_wave64{};
    void CompileHlsl(ShaderBinary& binary, Shader::SwStage stage);
    void CompileNativeSpirv(std::span<const u32> words, ShaderBinary& binary, Shader::SwStage stage,
                            bool flip_y, const TessellationLink* link, std::span<const u32> peer);
};
} // namespace D3D12
