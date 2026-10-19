#pragma once

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <vector>
#include <d3d12.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include "shader_recompiler/info.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "video_core/renderer_d3d12/image_buffer_alias.h"
#include "video_core/renderer_d3d12/persistent_cache.h"

namespace D3D12 {
class ResourceCache;
struct ShaderVariant;
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
    std::map<u32, std::vector<u32>> image_address_tables{};
};
struct TessellationLink {
    AmdGpu::TessellationType type;
    AmdGpu::TessellationPartitioning partitioning;
    AmdGpu::TessellationTopology topology;
    u32 input_control_points, output_control_points;
};
struct TranslatedShader {
    CompiledShader shader;
    std::optional<Shader::IR::Program> program;
    std::shared_ptr<ShaderVariant> variant;
    std::optional<Shader::ShaderParams> params;
};
struct PreparedShader {
    CompiledShader shader;
    std::vector<u32> words;
};
class ShaderCache {
public:
    void Initialize(ID3D12Device* device, const ResourceCache* resources = nullptr,
                    D3D_SHADER_MODEL maximum_model = D3D_SHADER_MODEL_6_6);
    D3D_SHADER_MODEL ShaderModel() const {
        return shader_model;
    }
    std::unique_ptr<TranslatedShader> Translate(Shader::ShaderParams params,
                                                Shader::RuntimeInfo runtime);
    PreparedShader Prepare(std::unique_ptr<TranslatedShader> translated,
                           Shader::Backend::Bindings bindings,
                           std::span<const ShaderResourceRange> stage_resources);
    PreparedShader Prepare(Shader::ShaderParams params, Shader::RuntimeInfo runtime,
                           Shader::Backend::Bindings bindings);
    CompiledShader Finalize(PreparedShader& shader, bool flip_y, bool sample_mask,
                            bool sample_shading, const TessellationLink* tessellation,
                            std::span<const u32> peer = {}, bool depth_bounds = false,
                            bool flip_x = false);
    CompiledShader Compile(Shader::ShaderParams params, Shader::RuntimeInfo runtime,
                           Shader::Backend::Bindings bindings = {}, bool flip_y = false,
                           bool sample_mask = false, bool sample_shading = false,
                           const TessellationLink* tessellation = nullptr,
                           bool depth_bounds = false, bool flip_x = false);
    std::shared_ptr<const ShaderBinary> CompileSpirv(
        std::span<const u32> words, Shader::SwStage stage, bool flip_y = false,
        bool sample_mask = false, bool sample_shading = false,
        const TessellationLink* tessellation = nullptr, std::span<const u32> peer = {},
        bool depth_bounds = false, bool flip_x = false);
    std::shared_ptr<const ShaderBinary> CompileLogicOp(const ShaderBinary& fragment,
                                                       const std::array<DXGI_FORMAT, 8>& formats,
                                                       const std::array<u32, 8>& masks);
    std::shared_ptr<const ShaderBinary> CompileRectangle(
        const ShaderBinary& vertex, const Shader::HwFragmentRuntimeInfo& fragment);
    std::shared_ptr<const ShaderBinary> CompilePointFill(const ShaderBinary& vertex,
                                                         bool keep_front, bool front_ccw);
    std::shared_ptr<const ShaderBinary> CompileLineExpand(
        const ShaderBinary& vertex, const Shader::HwFragmentRuntimeInfo& fragment, bool keep_front,
        bool front_ccw, D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_type, bool adjacency, float width_px,
        u16 pattern, u16 repeat, bool pattern_msb_first, float pattern_scale,
        const std::array<f32, 32>& viewport_dims);
    std::shared_ptr<const ShaderBinary> CompileHlslRaw(const std::string& hlsl,
                                                       std::wstring_view target,
                                                       std::span<const std::wstring> defines);
    struct Statistics {
        u64 translations{}, variant_hits{};
    };
    const Statistics& GetStatistics() const {
        return statistics;
    }
    const PersistentCache::Statistics& PersistentStatistics() const {
        return persistent.GetStatistics();
    }
    const Shader::Profile& Profile() const {
        return profile;
    }

private:
    std::unique_ptr<TranslatedShader> TranslateUncached(Shader::ShaderParams params,
                                                        Shader::RuntimeInfo runtime);
    std::vector<std::shared_ptr<ShaderVariant>> variants;
    size_t variant_bytes{};
    u64 variant_age{};
    Statistics statistics{};
    PersistentCache persistent;
    void Remember(std::vector<u32> key, std::shared_ptr<const ShaderBinary> binary);
    Shader::Profile profile{};
    std::array<std::unique_ptr<Shader::Pools>, Shader::MaxStageTypes> pools;
    Microsoft::WRL::ComPtr<IDxcUtils> utils;
    Microsoft::WRL::ComPtr<IDxcCompiler3> compiler;
    std::map<std::vector<u32>, std::shared_ptr<const ShaderBinary>> cache;
    size_t cache_bytes{};
    bool fixed_graphics_wave64{};
    D3D_SHADER_MODEL shader_model{D3D_SHADER_MODEL_6_4};
    const ResourceCache* resources{};
    void CompileHlsl(ShaderBinary& binary, Shader::SwStage stage);
    void CompileNativeSpirv(std::span<const u32> words, ShaderBinary& binary, Shader::SwStage stage,
                            bool flip_y, const TessellationLink* link, std::span<const u32> peer,
                            bool flip_x);
};
} // namespace D3D12
