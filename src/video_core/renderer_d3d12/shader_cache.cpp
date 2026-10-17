#include "video_core/renderer_d3d12/shader_cache.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <appmodel.h>
#include <spirv_hlsl.hpp>
#include <winrt/base.h>
#include "externals/mesa-uwp/src/microsoft/spirv_to_dxil/spirv_to_dxil.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"

namespace D3D12 {
namespace {
struct CompilerModule {
    HMODULE handle{};
    static void Anchor() {}
    explicit CompilerModule(const wchar_t* name) {
        handle = LoadPackagedLibrary(name, 0);
        UINT32 size{};
        if (!handle && GetCurrentPackageFullName(&size, nullptr) == APPMODEL_ERROR_NO_PACKAGE) {
            HMODULE self{};
            winrt::check_bool(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                                     GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                                 reinterpret_cast<LPCWSTR>(&Anchor), &self));
            std::wstring path(32768, L'\0');
            const auto length = GetModuleFileNameW(self, path.data(), path.size());
            if (!length || length == path.size())
                throw std::runtime_error("Cannot locate native shader compiler directory");
            path.resize(length);
            const auto slash = path.find_last_of(L"\\/");
            path.resize(slash + 1);
            path += name;
            handle =
                LoadLibraryExW(path.c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        }
        if (!handle)
            throw std::runtime_error("Cannot load native shader compiler module");
    }
    ~CompilerModule() {
        FreeLibrary(handle);
    }
    template <typename T>
    T Function(const char* name) const {
        const auto function = reinterpret_cast<T>(GetProcAddress(handle, name));
        if (!function)
            throw std::runtime_error("Missing native shader compiler entry point");
        return function;
    }
};
std::string SemanticMember(const std::string& source, std::string_view structure,
                           std::string_view semantic) {
    const auto begin = source.find("struct " + std::string(structure));
    if (begin == std::string::npos)
        throw std::runtime_error("Missing HLSL stage interface structure");
    const auto end = source.find("};", begin);
    const auto colon = source.find(": " + std::string(semantic), begin);
    if (colon == std::string::npos || colon > end)
        return {};
    size_t last = colon;
    while (last > begin && std::isspace(static_cast<unsigned char>(source[last - 1])))
        --last;
    size_t first = last;
    while (first > begin && (std::isalnum(static_cast<unsigned char>(source[first - 1])) ||
                             source[first - 1] == '_'))
        --first;
    if (first == last)
        throw std::runtime_error("Invalid HLSL stage semantic member");
    return source.substr(first, last - first);
}
void AddSemanticMember(std::string& source, std::string_view structure,
                       std::string_view declaration) {
    const auto begin = source.find("struct " + std::string(structure));
    const auto end = source.find("};", begin);
    if (begin == std::string::npos || end == std::string::npos)
        throw std::runtime_error("Missing HLSL stage interface structure");
    source.insert(end, std::string(declaration) + "\n");
}
void AlignSemanticRows(std::string& source, std::string_view structure) {
    const auto begin = source.find("struct " + std::string(structure));
    if (begin == std::string::npos)
        return;
    const auto brace = source.find('{', begin);
    const auto end = source.find("};", brace);
    if (brace == std::string::npos || end == std::string::npos)
        throw std::runtime_error("Invalid HLSL stage interface structure");
    std::map<u32, std::string> slots;
    std::vector<std::string> others;
    u32 span{};
    for (size_t start = brace + 1; start < end;) {
        const auto stop = source.find(";\n", start);
        if (stop == std::string::npos || stop >= end)
            break;
        const auto member = source.substr(start, stop - start);
        const auto colon = member.rfind(" : ");
        if (colon != std::string::npos &&
            member.compare(colon + 3, std::char_traits<char>::length("TEXCOORD"), "TEXCOORD") ==
                0) {
            const auto digits =
                member.substr(colon + 3 + std::char_traits<char>::length("TEXCOORD"));
            u32 index = 0;
            bool valid = !digits.empty();
            for (const char character : digits) {
                if (!std::isdigit(static_cast<unsigned char>(character))) {
                    valid = false;
                    break;
                }
                index = index * 10 + u32(character - '0');
            }
            if (!valid || index >= 32)
                throw std::runtime_error("Invalid HLSL stage texcoord semantic index");
            if (const auto [iter, inserted] = slots.try_emplace(index, member); !inserted)
                throw std::runtime_error("Duplicate HLSL stage texcoord semantic index");
            span = std::max(span, index + 1);
        } else {
            others.push_back(member);
        }
        start = stop + 2;
    }
    if (!span)
        return;
    std::string body;
    for (u32 index = 0; index < span; ++index) {
        if (const auto found = slots.find(index); found != slots.end()) {
            body += found->second + ";\n";
        } else {
            body += "    float4 shadRow" + std::to_string(index) + " : TEXCOORD" +
                    std::to_string(index) + ";\n";
        }
    }
    for (const auto& member : others) {
        body += member + ";\n";
    }
    source.replace(brace + 2, end - brace - 2, body);
}
void ApplySampleMasks(std::string& source, bool sample_shading) {
    auto entry = source.find(" main(");
    if (entry == std::string::npos)
        throw std::runtime_error("Missing HLSL fragment entry point");
    const auto close = source.find(')', entry);
    const bool has_input = close > entry + 6;
    const auto line = source.rfind('\n', entry);
    const bool has_output = source
                                .substr(line == std::string::npos ? 0 : line + 1,
                                        entry - (line == std::string::npos ? 0 : line + 1))
                                .find("void") == std::string::npos;
    std::string position = "shadMsaaPosition";
    std::string sample = "shadSampleIndex";
    if (has_input) {
        const auto member = SemanticMember(source, "SPIRV_Cross_Input", "SV_Position");
        if (member.empty())
            AddSemanticMember(source, "SPIRV_Cross_Input",
                              "float4 shadMsaaPosition : SV_Position;");
        else
            position = member;
        position = "input." + position;
        if (sample_shading) {
            const auto sample_member =
                SemanticMember(source, "SPIRV_Cross_Input", "SV_SampleIndex");
            if (sample_member.empty())
                AddSemanticMember(source, "SPIRV_Cross_Input",
                                  "uint shadSampleIndex : SV_SampleIndex;");
            else
                sample = sample_member;
            sample = "input." + sample;
        }
    }
    std::string coverage = "shadMsaaCoverage";
    if (has_output) {
        const auto member = SemanticMember(source, "SPIRV_Cross_Output", "SV_Coverage");
        if (member.empty())
            AddSemanticMember(source, "SPIRV_Cross_Output", "uint shadMsaaCoverage : SV_Coverage;");
        else
            coverage = member;
        const bool exported = !member.empty();
        coverage += exported ? " &= mask;" : " = mask;";
    } else {
        source += "\nstruct ShadMsaaOutput { uint shadMsaaCoverage : SV_Coverage; };\n";
        coverage += " = mask;";
    }
    entry = source.find(" main(");
    source.replace(entry + 1, 4, "shad_guest_main");
    source += "\ncbuffer ShadMsaaState : register(b3, space0) { uint4 shadMsaaMasks; };\n";
    if (source.find("[earlydepthstencil]") != std::string::npos)
        source += "[earlydepthstencil]\n";
    source += has_output ? "SPIRV_Cross_Output" : "ShadMsaaOutput";
    if (has_input)
        source += " main(SPIRV_Cross_Input input) {\n";
    else
        source += sample_shading ? " main(float4 shadMsaaPosition : SV_Position, uint "
                                   "shadSampleIndex : SV_SampleIndex) {\n"
                                 : " main(float4 shadMsaaPosition : SV_Position) {\n";
    source += "uint2 pixel = uint2(" + position +
              ".xy);\nuint mask = shadMsaaMasks[(pixel.x & 1) | ((pixel.y & 1) << 1)];\n";
    if (sample_shading)
        source += "mask &= (1u << " + sample + ");\n";
    if (has_output)
        source += "SPIRV_Cross_Output output = shad_guest_main(";
    else
        source += "shad_guest_main(";
    source += has_input ? "input);\n" : ");\n";
    if (!has_output)
        source += "ShadMsaaOutput output;\n";
    source += "output." + coverage + "\nreturn output;\n}\n";
}
void ApplyDepthBounds(std::string& source) {
    auto entry = source.find(" main(");
    if (entry == std::string::npos)
        throw std::runtime_error("Missing HLSL fragment entry point");
    const auto close = source.find(')', entry);
    if (close == std::string::npos)
        throw std::runtime_error("Invalid HLSL fragment entry point");
    std::string position;
    if (source.find("struct SPIRV_Cross_Input") != std::string::npos) {
        const auto member = SemanticMember(source, "SPIRV_Cross_Input", "SV_Position");
        if (!member.empty())
            position = "input." + member;
    }
    if (position.empty()) {
        const auto semantic = source.find(": SV_Position", entry);
        if (semantic != std::string::npos && semantic < close) {
            size_t last = semantic;
            while (last > entry && std::isspace(static_cast<unsigned char>(source[last - 1])))
                --last;
            size_t first = last;
            while (first > entry && (std::isalnum(static_cast<unsigned char>(source[first - 1])) ||
                                     source[first - 1] == '_'))
                --first;
            if (first != last)
                position = source.substr(first, last - first);
        }
    }
    if (position.empty()) {
        position = "shadDepthPosition";
        source.insert(close, "float4 shadDepthPosition : SV_Position");
    }
    const auto bol = source.rfind('\n', entry);
    source.insert(bol == std::string::npos ? 0 : bol + 1,
                  "cbuffer ShadDepthBounds : register(b4, space0) { float2 shadDepthBounds; };\n");
    const auto reopened = source.find(" main(");
    const auto body = source.find('{', source.find(')', reopened));
    if (body == std::string::npos)
        throw std::runtime_error("Invalid HLSL fragment entry point");
    source.insert(body + 1, "\nif (" + position + ".z < shadDepthBounds.x || " + position +
                                ".z > shadDepthBounds.y) discard;\n");
}
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
    fixed_graphics_wave64 = options.WaveLaneCountMin == 64 && options.WaveLaneCountMax == 64;
    winrt::check_hresult(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils)));
    winrt::check_hresult(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)));
    profile.supported_spirv = 0x00010300;
    profile.subgroup_size = 64;
    profile.max_viewport_width = D3D12_VIEWPORT_BOUNDS_MAX;
    profile.max_viewport_height = D3D12_VIEWPORT_BOUNDS_MAX;
    profile.max_shared_memory_size = D3D12_CS_TGSM_REGISTER_COUNT * sizeof(u32);
    profile.support_int64 = options.Int64ShaderOps;
    profile.supports_shader_stencil_export = true;
    profile.needs_buffer_offsets = true;
}
CompiledShader ShaderCache::Compile(Shader::ShaderParams params, Shader::RuntimeInfo runtime,
                                    Shader::Backend::Bindings bindings, bool flip_y,
                                    bool sample_mask, bool sample_shading,
                                    const TessellationLink* tessellation, bool depth_bounds) {
    auto prepared = Prepare(params, runtime, bindings);
    return Finalize(prepared, flip_y, sample_mask, sample_shading, tessellation, {}, depth_bounds);
}
PreparedShader ShaderCache::Prepare(Shader::ShaderParams params, Shader::RuntimeInfo runtime,
                                    Shader::Backend::Bindings bindings) {
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
    result.info.RefreshFlatBuf();
    return {std::move(result), words};
}
CompiledShader ShaderCache::Finalize(PreparedShader& prepared, bool flip_y, bool sample_mask,
                                     bool sample_shading, const TessellationLink* tessellation,
                                     std::span<const u32> peer, bool depth_bounds) {
    auto& result = prepared.shader;
    const auto& words = prepared.words;
    const auto& runtime = result.runtime;
    std::optional<TessellationLink> link;
    if (tessellation) {
        link = *tessellation;
        if (runtime.sw_stage == Shader::SwStage::TessellationControl)
            link->output_control_points = result.runtime.sw.tcs.NumOutputControlPoints();
    }
    result.binary = CompileSpirv(words, runtime.sw_stage, flip_y, sample_mask, sample_shading,
                                 link ? &*link : nullptr, peer, depth_bounds);
    return std::move(result);
}
std::shared_ptr<const ShaderBinary> ShaderCache::CompileSpirv(
    std::span<const u32> words, Shader::SwStage stage, bool flip_y, bool sample_mask,
    bool sample_shading, const TessellationLink* tessellation_link, std::span<const u32> peer,
    bool depth_bounds) {
    if (words.size() < 5 || words.size() > 4 * 1024 * 1024 || words[0] != spv::MagicNumber)
        throw std::runtime_error("Invalid guest SPIR-V module");
    bool subgroup{};
    bool quad_only = stage == Shader::SwStage::Fragment;
    for (size_t offset = 5; offset < words.size();) {
        const u32 length = words[offset] >> 16;
        if (!length || length > words.size() - offset)
            throw std::runtime_error("Invalid guest SPIR-V instruction length");
        const auto opcode = words[offset] & 0xffff;
        if (opcode == spv::OpCapability && length == 2 &&
            words[offset + 1] >= spv::CapabilityGroupNonUniform &&
            words[offset + 1] <= spv::CapabilityGroupNonUniformQuad) {
            subgroup = true;
            quad_only &= words[offset + 1] == spv::CapabilityGroupNonUniform ||
                         words[offset + 1] == spv::CapabilityGroupNonUniformQuad;
        }
        if (opcode >= spv::OpGroupNonUniformElect && opcode <= spv::OpGroupNonUniformQuadSwap) {
            subgroup = true;
            quad_only &= opcode == spv::OpGroupNonUniformQuadBroadcast ||
                         opcode == spv::OpGroupNonUniformQuadSwap;
        }
        if (opcode == spv::OpDecorate && length == 4 &&
            words[offset + 2] == spv::DecorationBuiltIn) {
            const auto builtin = words[offset + 3];
            if (builtin == spv::BuiltInSubgroupSize ||
                builtin == spv::BuiltInSubgroupLocalInvocationId ||
                (builtin >= spv::BuiltInSubgroupEqMask && builtin <= spv::BuiltInSubgroupLtMask)) {
                subgroup = true;
                quad_only = false;
            }
        }
        if (opcode == spv::OpCapability && length == 2 &&
            (words[offset + 1] == spv::CapabilitySubgroupBallotKHR ||
             words[offset + 1] == spv::CapabilitySubgroupVoteKHR)) {
            subgroup = true;
            quad_only = false;
        }
        offset += length;
    }
    if (stage != Shader::SwStage::Compute && !fixed_graphics_wave64 && subgroup && !quad_only)
        throw std::runtime_error(
            "D3D12 cross-lane graphics operations require a fixed wave64 device; "
            "variable-width graphics waves require compute emulation");
    std::vector<u32> key{static_cast<u32>(stage), static_cast<u32>(flip_y),
                         static_cast<u32>(sample_mask), static_cast<u32>(sample_shading),
                         static_cast<u32>(depth_bounds)};
    if (tessellation_link) {
        key.insert(key.end(), {static_cast<u32>(tessellation_link->type),
                               static_cast<u32>(tessellation_link->partitioning),
                               static_cast<u32>(tessellation_link->topology),
                               tessellation_link->input_control_points,
                               tessellation_link->output_control_points});
    }
    key.push_back(static_cast<u32>(words.size()));
    key.insert(key.end(), words.begin(), words.end());
    key.insert(key.end(), peer.begin(), peer.end());
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
    const bool tessellation =
        stage == Shader::SwStage::TessellationControl || stage == Shader::SwStage::TessellationEval;
    const bool native_spirv = tessellation || stage == Shader::SwStage::Geometry;
    if (!tessellation && tessellation_link)
        throw std::runtime_error("Tessellation metadata supplied for a different shader stage");
    if (tessellation && !tessellation_link)
        throw std::runtime_error("Native tessellation compilation requires linked stage metadata");
    if (native_spirv)
        CompileNativeSpirv(words, *result, stage, flip_y, tessellation_link, peer);
    else
        result->hlsl = cross.compile();
    if (!native_spirv) {
        AlignSemanticRows(result->hlsl, "SPIRV_Cross_Input");
        AlignSemanticRows(result->hlsl, "SPIRV_Cross_Output");
    }
    if (sample_mask || sample_shading) {
        if (stage != Shader::SwStage::Fragment)
            throw std::runtime_error("Sample masks require a fragment shader");
        ApplySampleMasks(result->hlsl, sample_shading);
    }
    if (depth_bounds) {
        if (stage != Shader::SwStage::Fragment)
            throw std::runtime_error("Depth bounds require a fragment shader");
        ApplyDepthBounds(result->hlsl);
    }
    if (stage == Shader::SwStage::Compute) {
        const auto entry = result->hlsl.find("[numthreads(");
        if (entry == std::string::npos)
            throw std::runtime_error("Missing guest compute workgroup declaration");
        result->hlsl.insert(entry, "[WaveSize(64)]\n");
    }
    if (!native_spirv)
        CompileHlsl(*result, stage);
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
std::shared_ptr<const ShaderBinary> ShaderCache::CompileRectangle(
    const ShaderBinary& vertex, const Shader::HwFragmentRuntimeInfo& fragment) {
    const auto first = vertex.hlsl.find("struct SPIRV_Cross_Output");
    const auto last = vertex.hlsl.find("};", first);
    if (first == std::string::npos || last == std::string::npos || last - first > 16384)
        throw std::runtime_error("Invalid rectangle vertex interface");
    const auto position = SemanticMember(vertex.hlsl, "SPIRV_Cross_Output", "SV_Position");
    if (position.empty())
        throw std::runtime_error("Rectangle expansion requires a position export");
    auto result = std::make_shared<ShaderBinary>();
    result->hlsl = vertex.hlsl.substr(first, last + 2 - first);
    result->hlsl += "\n[maxvertexcount(4)]\nvoid main(triangle SPIRV_Cross_Output input[3], "
                    "inout TriangleStream<SPIRV_Cross_Output> stream) {\n";
    result->hlsl += "float3 weight; uint corner = 0;\n";
    for (u32 i = 0; i < 3; ++i) {
        const auto a = "input[" + std::to_string(i) + "]." + position;
        const auto b = "input[" + std::to_string((i + 1) % 3) + "]." + position;
        const auto c = "input[" + std::to_string((i + 2) % 3) + "]." + position;
        result->hlsl += "bool edge" + std::to_string(i) + " = (" + a + ".x == " + b + ".x && " + a +
                        ".y == " + c + ".y) || (" + a + ".y == " + b + ".y && " + a + ".x == " + c +
                        ".x);\nweight[" + std::to_string(i) + "] = edge" + std::to_string(i) +
                        " ? -1.0 : 1.0;\n";
    }
    result->hlsl += "corner = edge1 ? 1 : (edge2 ? 2 : 0);\n"
                    "SPIRV_Cross_Output v[4];\n"
                    "v[0] = input[corner]; v[1] = input[(corner+1)%3]; "
                    "v[2] = input[(corner+2)%3]; v[3] = input[0];\n";
    const auto body = vertex.hlsl.find('{', first);
    std::istringstream members(vertex.hlsl.substr(body + 1, last - body - 1));
    for (std::string member; std::getline(members, member, ';');) {
        std::istringstream declaration(member);
        std::string type, name, colon, semantic;
        if (!(declaration >> type))
            continue;
        if (!(declaration >> name >> colon >> semantic) || colon != ":" ||
            name.find('[') != std::string::npos ||
            (type != "float" && type != "float2" && type != "float3" && type != "float4"))
            throw std::runtime_error("Unsupported rectangle stage interface member");
        bool flat{};
        for (u32 i = 0; i < fragment.num_inputs; ++i) {
            const auto& input = fragment.inputs[i];
            flat |= input.is_flat && semantic == "TEXCOORD" + std::to_string(input.param_index);
        }
        if (flat) {
            result->hlsl += "for(uint i=0;i<4;++i) v[i]." + name + " = input[0]." + name + ";\n";
        } else {
            result->hlsl += "v[3]." + name + " = input[0]." + name + " * weight.x + input[1]." +
                            name + " * weight.y + input[2]." + name + " * weight.z;\n";
        }
    }
    result->hlsl += "for(uint i=0;i<4;++i) stream.Append(v[i]); stream.RestartStrip();\n}\n";
    std::vector<u32> key{UINT32_MAX};
    for (const unsigned char character : result->hlsl)
        key.push_back(character);
    if (const auto found = cache.find(key); found != cache.end())
        return found->second;
    CompileHlsl(*result, Shader::SwStage::Geometry);
    const auto bytes = key.size() * sizeof(u32) + result->hlsl.size() + result->dxil.size();
    while (!cache.empty() && (cache.size() >= 512 || cache_bytes + bytes > 64 * 1024 * 1024)) {
        const auto& oldest = *cache.begin();
        cache_bytes -= oldest.first.size() * sizeof(u32) + oldest.second->dxil.size() +
                       oldest.second->hlsl.size();
        cache.erase(cache.begin());
    }
    cache_bytes += bytes;
    cache.emplace(std::move(key), result);
    return result;
}
void ShaderCache::CompileNativeSpirv(std::span<const u32> words, ShaderBinary& binary,
                                     Shader::SwStage stage, bool flip_y,
                                     const TessellationLink* link, std::span<const u32> peer) {
    static const CompilerModule module{L"spirv_to_dxil.dll"};
    static const CompilerModule validator_module{L"dxil.dll"};
    const auto config_size = module.Function<decltype(&spirv_to_dxil_get_runtime_conf_size)>(
        "spirv_to_dxil_get_runtime_conf_size");
    if (config_size() != sizeof(dxil_spirv_runtime_conf))
        throw std::runtime_error("Native shader compiler ABI mismatch");
    const auto compile = module.Function<decltype(&spirv_to_dxil)>("spirv_to_dxil");
    const auto release = module.Function<decltype(&spirv_to_dxil_free)>("spirv_to_dxil_free");
    const auto remap_module = [](std::span<const u32> input, Shader::SwStage model) {
        if (input.size() < 5 || input[0] != spv::MagicNumber || input.size() > 4 * 1024 * 1024)
            throw std::runtime_error("Invalid tessellation peer module");
        std::vector<u32> remapped(input.begin(), input.end());
        const spirv_cross::Compiler reflection{input.data(), input.size()};
        const auto buffers = reflection.get_shader_resources().storage_buffers;
        for (size_t offset = 5; offset < remapped.size();) {
            const u32 length = remapped[offset] >> 16;
            if (!length || length > remapped.size() - offset)
                throw std::runtime_error("Invalid tessellation peer instruction");
            if ((remapped[offset] & 0xffff) == spv::OpDecorate && length == 4 &&
                remapped[offset + 2] == spv::DecorationDescriptorSet)
                remapped[offset + 3] = static_cast<u32>(model) + 1;
            if ((remapped[offset] & 0xffff) == spv::OpDecorate && length == 3 &&
                remapped[offset + 2] == spv::DecorationNonWritable &&
                std::ranges::any_of(buffers, [&](const auto& resource) {
                    return resource.id == remapped[offset + 1];
                }))
                std::fill_n(remapped.begin() + offset, length, (1u << 16) | spv::OpNop);
            offset += length;
        }
        return remapped;
    };
    auto remapped = remap_module(words, stage);
    std::vector<u32> remapped_peer;
    if (link)
        remapped_peer = remap_module(peer, stage == Shader::SwStage::TessellationControl
                                               ? Shader::SwStage::TessellationEval
                                               : Shader::SwStage::TessellationControl);
    dxil_spirv_runtime_conf config{};
    config.fixed_point_size = true;
    config.push_constant_cbv = {0, 0};
    config.runtime_data_cbv = {0, 4};
    config.shader_model_max = SHADER_MODEL_6_6;
    config.declared_read_only_images_as_srvs = false;
    config.preserve_generic_io_location_count = 32;
    if (link) {
        config.link_tessellation = true;
        config.tessellation_peer_words = remapped_peer.data();
        config.tessellation_peer_word_count = remapped_peer.size();
        switch (link->type) {
        case AmdGpu::TessellationType::Triangle:
            config.tessellation_domain = 1;
            break;
        case AmdGpu::TessellationType::Quad:
            config.tessellation_domain = 2;
            break;
        case AmdGpu::TessellationType::Isoline:
            config.tessellation_domain = 3;
            break;
        default:
            throw std::runtime_error("Invalid native tessellation domain");
        }
        switch (link->partitioning) {
        case AmdGpu::TessellationPartitioning::Integer:
            config.tessellation_spacing = 1;
            break;
        case AmdGpu::TessellationPartitioning::FracOdd:
            config.tessellation_spacing = 2;
            break;
        case AmdGpu::TessellationPartitioning::FracEven:
            config.tessellation_spacing = 3;
            break;
        default:
            throw std::runtime_error("Native tessellation partitioning requires factor emulation");
        }
        config.tessellation_input_control_points = link->input_control_points;
        config.tessellation_output_control_points = link->output_control_points;
        config.tessellation_ccw = link->topology == AmdGpu::TessellationTopology::TriangleCcw;
        config.tessellation_point_mode = link->topology == AmdGpu::TessellationTopology::Point;
        if ((link->type == AmdGpu::TessellationType::Isoline &&
             link->topology != AmdGpu::TessellationTopology::Line &&
             !config.tessellation_point_mode) ||
            (link->type != AmdGpu::TessellationType::Isoline &&
             link->topology == AmdGpu::TessellationTopology::Line))
            throw std::runtime_error("Native tessellation topology requires geometry expansion");
    }
    config.yz_flip.mode = flip_y ? DXIL_SPIRV_Y_FLIP_UNCONDITIONAL : DXIL_SPIRV_YZ_FLIP_NONE;
    dxil_spirv_debug_options debug{};
    std::string messages;
    dxil_spirv_logger logger{};
    logger.priv = &messages;
    logger.log = [](void* context, const char* message) {
        static_cast<std::string*>(context)->append(message);
    };
    dxil_spirv_object output{};
    const auto free_output = [&](dxil_spirv_object* object) { release(object); };
    std::unique_ptr<dxil_spirv_object, decltype(free_output)> owner{&output, free_output};
    const auto model = stage == Shader::SwStage::Geometry ? DXIL_SPIRV_SHADER_GEOMETRY
                       : stage == Shader::SwStage::TessellationControl
                           ? DXIL_SPIRV_SHADER_TESS_CTRL
                           : DXIL_SPIRV_SHADER_TESS_EVAL;
    if (!compile(remapped.data(), remapped.size(), nullptr, 0, model, "main", DXIL_VALIDATOR_1_6,
                 &debug, &config, &logger, &output))
        throw std::runtime_error("Native SPIR-V compilation failed: " + messages);
    if (output.metadata.requires_runtime_data || output.metadata.lowered_large_points ||
        !output.binary.buffer || !output.binary.size)
        throw std::runtime_error("Native SPIR-V shader requires unsupported runtime data");
    const auto create = validator_module.Function<DxcCreateInstanceProc>("DxcCreateInstance");
    Microsoft::WRL::ComPtr<IDxcValidator> validator;
    winrt::check_hresult(create(CLSID_DxcValidator, IID_PPV_ARGS(&validator)));
    Microsoft::WRL::ComPtr<IDxcBlobEncoding> blob;
    winrt::check_hresult(utils->CreateBlob(output.binary.buffer, output.binary.size, 0, &blob));
    Microsoft::WRL::ComPtr<IDxcOperationResult> validation;
    winrt::check_hresult(
        validator->Validate(blob.Get(), DxcValidatorFlags_InPlaceEdit, &validation));
    HRESULT status;
    winrt::check_hresult(validation->GetStatus(&status));
    if (FAILED(status)) {
        Microsoft::WRL::ComPtr<IDxcBlobEncoding> errors;
        validation->GetErrorBuffer(&errors);
        throw std::runtime_error(
            errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()),
                                 errors->GetBufferSize())
                   : "Native SPIR-V DXIL validation failed");
    }
    const auto* first = static_cast<const u8*>(blob->GetBufferPointer());
    binary.dxil.assign(first, first + blob->GetBufferSize());
}
void ShaderCache::CompileHlsl(ShaderBinary& result, Shader::SwStage stage) {
    const wchar_t* arguments[]{L"-E",  L"main", L"-T",   Target(stage),
                               L"-HV", L"2021", L"-Gis", L"-O3"};
    const DxcBuffer source{result.hlsl.data(), result.hlsl.size(), DXC_CP_UTF8};
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
    result.dxil.assign(bytes, bytes + binary->GetBufferSize());
}

std::shared_ptr<const ShaderBinary> ShaderCache::CompileHlslRaw(
    const std::string& hlsl, std::wstring_view target, std::span<const std::wstring> defines) {
    std::vector<const wchar_t*> arguments{L"-E",  L"main", L"-T",   target.data(),
                                          L"-HV", L"2021", L"-Gis", L"-O3"};
    for (const auto& define : defines)
        arguments.push_back(define.c_str());
    const DxcBuffer source{hlsl.data(), hlsl.size(), DXC_CP_UTF8};
    Microsoft::WRL::ComPtr<IDxcResult> compiled;
    winrt::check_hresult(compiler->Compile(&source, arguments.data(),
                                           static_cast<u32>(arguments.size()), nullptr,
                                           IID_PPV_ARGS(&compiled)));
    HRESULT status;
    winrt::check_hresult(compiled->GetStatus(&status));
    if (FAILED(status)) {
        Microsoft::WRL::ComPtr<IDxcBlobUtf8> errors;
        compiled->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
        throw std::runtime_error(errors ? errors->GetStringPointer()
                                        : "Compute DXIL compilation failed");
    }
    Microsoft::WRL::ComPtr<IDxcBlob> binary;
    winrt::check_hresult(compiled->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&binary), nullptr));
    auto result = std::make_shared<ShaderBinary>();
    const auto* bytes = static_cast<const u8*>(binary->GetBufferPointer());
    result->dxil.assign(bytes, bytes + binary->GetBufferSize());
    return result;
}
} // namespace D3D12
