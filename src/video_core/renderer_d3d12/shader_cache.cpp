#include "video_core/renderer_d3d12/shader_cache.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <spirv_hlsl.hpp>
#include <winrt/base.h>
#include "shader_recompiler/backend/spirv/emit_spirv.h"

namespace D3D12 {
namespace {
const wchar_t* Target(Shader::SwStage stage) {
    switch (stage) {
    case Shader::SwStage::Vertex:
        return L"vs_6_6";
    case Shader::SwStage::Fragment:
        return L"ps_6_6";
    case Shader::SwStage::Compute:
        return L"cs_6_6";
    case Shader::SwStage::Geometry:
        return L"gs_6_6";
    case Shader::SwStage::TessellationControl:
        return L"hs_6_6";
    case Shader::SwStage::TessellationEval:
        return L"ds_6_6";
    default:
        throw std::runtime_error("Invalid D3D12 shader stage");
    }
}
spv::ExecutionModel Model(Shader::SwStage stage) {
    switch (stage) {
    case Shader::SwStage::Vertex:
        return spv::ExecutionModelVertex;
    case Shader::SwStage::Fragment:
        return spv::ExecutionModelFragment;
    case Shader::SwStage::Compute:
        return spv::ExecutionModelGLCompute;
    case Shader::SwStage::Geometry:
        return spv::ExecutionModelGeometry;
    case Shader::SwStage::TessellationControl:
        return spv::ExecutionModelTessellationControl;
    case Shader::SwStage::TessellationEval:
        return spv::ExecutionModelTessellationEvaluation;
    default:
        throw std::runtime_error("Invalid D3D12 shader stage");
    }
}
} // namespace
void ShaderCache::Initialize(ID3D12Device* device) {
    D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_6};
    winrt::check_hresult(
        device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model)));
    if (model.HighestShaderModel < D3D_SHADER_MODEL_6_6)
        throw std::runtime_error("D3D12 guest shaders require Shader Model 6.6");
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 options{};
    winrt::check_hresult(
        device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options, sizeof(options)));
    if (!options.WaveOps || options.WaveLaneCountMin > 64 || options.WaveLaneCountMax < 64)
        throw std::runtime_error("D3D12 guest shaders require wave64 support");
    winrt::check_hresult(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils)));
    winrt::check_hresult(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)));
    profile.supported_spirv = 0x00010300;
    profile.subgroup_size = 64;
    profile.max_viewport_width = D3D12_VIEWPORT_BOUNDS_MAX;
    profile.max_viewport_height = D3D12_VIEWPORT_BOUNDS_MAX;
    profile.max_shared_memory_size = D3D12_CS_TGSM_REGISTER_COUNT * sizeof(u32);
    profile.support_int64 = options.Int64ShaderOps;
    profile.needs_buffer_offsets = true;
}
CompiledShader ShaderCache::Compile(Shader::ShaderParams params, Shader::RuntimeInfo runtime,
                                    Shader::Backend::Bindings bindings, bool flip_y) {
    if (params.code.empty() || params.code.size() > 1024 * 1024 ||
        runtime.props.num_user_data > Shader::NUM_USER_DATA_REGS ||
        (runtime.sw_stage == Shader::SwStage::Fragment && runtime.hw.fs.num_inputs > 32))
        throw std::runtime_error("Invalid guest shader code or register bounds");
    CompiledShader result{
        Shader::Info{runtime.hw_stage, runtime.sw_stage, params}, runtime, bindings, bindings, {}};
    pools.ReleaseContents();
    const auto program =
        Shader::TranslateProgram(params.code, pools, result.info, result.runtime, profile);
    result.info.ResolveDirectReads();
    if (result.info.buffers.size() >
        Shader::NUM_BUFFERS - std::min(bindings.buffer, Shader::NUM_BUFFERS))
        throw std::runtime_error("D3D12 shader stages exceed push buffer offset capacity");
    const auto words =
        Shader::Backend::SPIRV::EmitSPIRV(profile, result.runtime, program, result.end_bindings);
    result.binary = CompileSpirv(words, runtime.sw_stage, flip_y);
    result.info.RefreshFlatBuf();
    return result;
}
std::shared_ptr<const ShaderBinary> ShaderCache::CompileSpirv(std::span<const u32> words,
                                                              Shader::SwStage stage, bool flip_y) {
    if (words.size() < 5 || words.size() > 4 * 1024 * 1024 || words[0] != spv::MagicNumber)
        throw std::runtime_error("Invalid guest SPIR-V module");
    for (size_t offset = 5; offset < words.size();) {
        const u32 length = words[offset] >> 16;
        if (!length || length > words.size() - offset)
            throw std::runtime_error("Invalid guest SPIR-V instruction length");
        if (stage != Shader::SwStage::Compute && (words[offset] & 0xffff) == spv::OpCapability &&
            length == 2 && words[offset + 1] >= spv::CapabilityGroupNonUniform &&
            words[offset + 1] <= spv::CapabilityGroupNonUniformQuad)
            throw std::runtime_error("D3D12 graphics wave64 lowering is not implemented");
        offset += length;
    }
    std::vector<u32> key{static_cast<u32>(stage), static_cast<u32>(flip_y)};
    key.insert(key.end(), words.begin(), words.end());
    if (const auto found = cache.find(key); found != cache.end())
        return found->second;
    spirv_cross::CompilerHLSL cross{words.data(), words.size()};
    const auto entries = cross.get_entry_points_and_stages();
    if (entries.size() != 1 || entries.front().execution_model != Model(stage))
        throw std::runtime_error("Guest shader entry point mismatch");
    cross.set_entry_point(entries.front().name, Model(stage));
    auto common_options = cross.get_common_options();
    common_options.vertex.flip_vert_y = flip_y;
    cross.set_common_options(common_options);
    auto result = std::make_shared<ShaderBinary>();
    const auto resources = cross.get_shader_resources();
    const u32 space = static_cast<u32>(stage) + 1;
    const auto bind = [&](const auto& list, ResourceKind kind, D3D12_DESCRIPTOR_RANGE_TYPE type) {
        for (const auto& resource : list) {
            const auto set = cross.get_decoration(resource.id, spv::DecorationDescriptorSet);
            const auto index = cross.get_decoration(resource.id, spv::DecorationBinding);
            const auto& resource_type = cross.get_type(resource.type_id);
            u32 count = 1;
            for (const auto dimension : resource_type.array) {
                if (dimension == 0 || dimension > 1024 || count > 1024 / dimension)
                    throw std::runtime_error("Unbounded guest shader descriptor array");
                count *= dimension;
            }
            spirv_cross::HLSLResourceBinding remap{};
            remap.stage = Model(stage);
            remap.desc_set = set;
            remap.binding = index;
            remap.cbv = remap.srv = remap.uav = remap.sampler = {space, index};
            cross.add_hlsl_resource_binding(remap);
            const auto existing = std::ranges::find_if(result->bindings, [&](const auto& binding) {
                return binding.binding == index && binding.space == space && binding.type == type;
            });
            if (existing == result->bindings.end())
                result->bindings.push_back({kind, type, index, space, count});
            else if (existing->count != count || existing->kind != kind)
                throw std::runtime_error("Incompatible guest descriptor aliases");
        }
    };
    bind(resources.storage_buffers, ResourceKind::Buffer, D3D12_DESCRIPTOR_RANGE_TYPE_UAV);
    bind(resources.uniform_buffers, ResourceKind::Constants, D3D12_DESCRIPTOR_RANGE_TYPE_CBV);
    bind(resources.storage_images, ResourceKind::Image, D3D12_DESCRIPTOR_RANGE_TYPE_UAV);
    bind(resources.separate_images, ResourceKind::Image, D3D12_DESCRIPTOR_RANGE_TYPE_SRV);
    bind(resources.separate_samplers, ResourceKind::Sampler, D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER);
    if (!resources.sampled_images.empty() || !resources.subpass_inputs.empty() ||
        !resources.atomic_counters.empty() || !resources.acceleration_structures.empty())
        throw std::runtime_error("Unsupported guest shader resource class");
    spirv_cross::HLSLResourceBinding push{};
    push.stage = Model(stage);
    push.desc_set = spirv_cross::ResourceBindingPushConstantDescriptorSet;
    push.binding = spirv_cross::ResourceBindingPushConstantBinding;
    push.cbv = {0, 0};
    cross.add_hlsl_resource_binding(push);
    if (stage == Shader::SwStage::Compute) {
        if (const auto workgroups = cross.remap_num_workgroups_builtin()) {
            constexpr u32 auxiliary_set = UINT32_MAX - 1;
            cross.set_decoration(workgroups, spv::DecorationDescriptorSet, auxiliary_set);
            cross.set_decoration(workgroups, spv::DecorationBinding, 0);
            spirv_cross::HLSLResourceBinding remap{};
            remap.stage = Model(stage);
            remap.desc_set = auxiliary_set;
            remap.binding = 0;
            remap.cbv = {0, 2};
            cross.add_hlsl_resource_binding(remap);
        }
    }
    auto options = cross.get_hlsl_options();
    options.shader_model = 66;
    options.force_storage_buffer_as_uav = true;
    options.support_nonzero_base_vertex_base_instance = true;
    cross.set_hlsl_aux_buffer_binding(spirv_cross::HLSL_AUX_BINDING_BASE_VERTEX_INSTANCE, 1, 0);
    cross.set_hlsl_options(options);
    result->hlsl = cross.compile();
    if (stage == Shader::SwStage::Compute) {
        const auto entry = result->hlsl.find("[numthreads(");
        if (entry == std::string::npos)
            throw std::runtime_error("Missing guest compute workgroup declaration");
        result->hlsl.insert(entry, "[WaveSize(64)]\n");
    }
    const wchar_t* arguments[]{L"-E",  L"main", L"-T",   Target(stage),
                               L"-HV", L"2021", L"-Gis", L"-O3"};
    const DxcBuffer source{result->hlsl.data(), result->hlsl.size(), DXC_CP_UTF8};
    Microsoft::WRL::ComPtr<IDxcResult> compiled;
    winrt::check_hresult(compiler->Compile(&source, arguments, std::size(arguments), nullptr,
                                           IID_PPV_ARGS(&compiled)));
    HRESULT status;
    winrt::check_hresult(compiled->GetStatus(&status));
    if (FAILED(status)) {
        Microsoft::WRL::ComPtr<IDxcBlobUtf8> errors;
        compiled->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
        throw std::runtime_error(errors ? errors->GetStringPointer()
                                        : "Guest DXIL compilation failed");
    }
    Microsoft::WRL::ComPtr<IDxcBlob> binary;
    winrt::check_hresult(compiled->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&binary), nullptr));
    const auto* bytes = static_cast<const u8*>(binary->GetBufferPointer());
    result->dxil.assign(bytes, bytes + binary->GetBufferSize());
    const size_t bytes_used = key.size() * sizeof(u32) + result->dxil.size() + result->hlsl.size();
    if (bytes_used > 64 * 1024 * 1024)
        throw std::runtime_error("Guest shader exceeds D3D12 compiler cache budget");
    while (!cache.empty() && (cache.size() >= 512 || cache_bytes + bytes_used > 64 * 1024 * 1024)) {
        const auto& oldest = *cache.begin();
        cache_bytes -= oldest.first.size() * sizeof(u32) + oldest.second->dxil.size() +
                       oldest.second->hlsl.size();
        cache.erase(cache.begin());
    }
    cache_bytes += bytes_used;
    cache.emplace(std::move(key), result);
    return result;
}
} // namespace D3D12
