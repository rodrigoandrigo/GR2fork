#include "video_core/renderer_d3d12/shader_cache.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <appmodel.h>
#include <d3d12shader.h>
#include <spirv_hlsl.hpp>
#include <winrt/base.h>
#include "core/emulator_settings.h"
#include "externals/mesa-uwp/src/microsoft/spirv_to_dxil/spirv_to_dxil.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "video_core/renderer_d3d12/image_buffer_alias.h"
#include "video_core/renderer_d3d12/logic_op_format.h"
#include "video_core/renderer_d3d12/profile.h"
#include "video_core/renderer_d3d12/resource_cache.h"

namespace D3D12 {
namespace {
Profile shader_profile;
}
struct ShaderVariant {
    struct Prepared {
        Shader::Backend::Bindings bindings;
        std::vector<ShaderResourceRange> stage_resources;
        u64 mapping_generation;
        IndexFetch indices;
        PreparedShader shader;
    };
    std::vector<u32> code, copy_code, dependencies;
    Shader::RuntimeInfo input_runtime{};
    CompiledShader translated{};
    std::vector<Prepared> prepared;
    size_t bytes{};
    u64 used{};
    bool direct_memory_access{};
};
namespace {
bool SameRuntime(const Shader::RuntimeInfo& first, const Shader::RuntimeInfo& second) {
    if (!(first == second))
        return false;
    if (first.hw_stage == Shader::HwStage::Compute &&
        first.hw.cs.shared_memory_size != second.hw.cs.shared_memory_size)
        return false;
    if (first.hw_stage == Shader::HwStage::Geometry)
        return first.hw.gs.in_vertex_data_size == second.hw.gs.in_vertex_data_size &&
               first.hw.gs.out_vertex_data_size == second.hw.gs.out_vertex_data_size &&
               first.hw.gs.mode == second.hw.gs.mode;
    return true;
}
void BindUserData(Shader::Info& info, std::span<const u32> user_data) {
    info.user_data = user_data;
    info.RefreshFlatBuf();
}
void DetachUserData(Shader::Info& info) {
    info.user_data = {};
    info.flat_ud = nullptr;
}
std::vector<u32> TranslationDependencies(const Shader::Info& info) {
    std::vector<u32> result(info.flat_ud, info.flat_ud + info.srt_info.flattened_bufsize_dw);
    const auto append = [&](const auto& value) {
        static_assert(sizeof(value) % sizeof(u32) == 0);
        const size_t offset = result.size();
        result.resize(offset + sizeof(value) / sizeof(u32));
        std::memcpy(result.data() + offset, &value, sizeof(value));
    };
    for (const auto& sampler : info.samplers)
        append(sampler.GetSharp(info));
    if (info.has_fetch_shader) {
        Shader::Gcn::FetchShaderData fetch;
        if (!Shader::Gcn::ParseFetchShader(info, fetch))
            throw std::runtime_error("Invalid cached D3D12 fetch shader");
        const auto* code = Shader::Gcn::GetFetchShaderCode(info, info.fetch_shader_sgpr_base);
        result.push_back(fetch.size);
        result.insert(result.end(), code, code + fetch.size / sizeof(u32));
        for (const auto& attribute : fetch.attributes)
            append(attribute.GetSharp(info));
    }
    if (info.tess_consts_dword_offset >= 0) {
        Shader::TessellationDataConstantBuffer constants{};
        info.ReadTessConstantBuffer(constants);
        append(constants);
    }
    return result;
}
size_t InfoBytes(const Shader::Info& info) {
    return sizeof(info) + info.flattened_ud_buf.capacity() * sizeof(u32) +
           info.buffers.size() * sizeof(Shader::BufferResource) +
           info.images.size() * sizeof(Shader::ImageResource) +
           info.samplers.size() * sizeof(Shader::SamplerResource) +
           info.fmasks.size() * sizeof(Shader::FMaskResource) +
           info.gs_copy_data.attr_map.size() * (sizeof(u32) * 8);
}
size_t PreparedBytes(const PreparedShader& prepared) {
    size_t bytes = sizeof(prepared) + InfoBytes(prepared.shader.info) +
                   prepared.words.capacity() * sizeof(u32);
    for (const auto& [binding, table] : prepared.shader.image_address_tables)
        bytes += sizeof(binding) + sizeof(table) + table.capacity() * sizeof(u32) + 32;
    return bytes;
}
bool SameRanges(std::span<const ShaderResourceRange> first,
                std::span<const ShaderResourceRange> second) {
    return std::ranges::equal(first, second, [](const auto& a, const auto& b) {
        return a.address == b.address && a.size == b.size && a.stage == b.stage &&
               a.written == b.written;
    });
}
std::vector<u8> ShaderDiskKey(std::span<const u32> key) {
    std::vector<u8> result{1};
    const auto* bytes = reinterpret_cast<const u8*>(key.data());
    result.insert(result.end(), bytes, bytes + key.size_bytes());
    return result;
}
std::vector<u8> EncodeBinary(const ShaderBinary& binary) {
    const u32 sizes[]{u32(binary.dxil.size()), u32(binary.hlsl.size()),
                      u32(binary.bindings.size())};
    const auto* header = reinterpret_cast<const u8*>(sizes);
    std::vector<u8> bytes(header, header + sizeof(sizes));
    bytes.insert(bytes.end(), binary.dxil.begin(), binary.dxil.end());
    bytes.insert(bytes.end(), binary.hlsl.begin(), binary.hlsl.end());
    for (const auto& binding : binary.bindings) {
        const u32 fields[]{u32(binding.kind), u32(binding.type), binding.binding, binding.space,
                           binding.count};
        const auto* first = reinterpret_cast<const u8*>(fields);
        bytes.insert(bytes.end(), first, first + sizeof(fields));
    }
    return bytes;
}
bool ValidDxil(std::span<const u8> bytes) {
    return bytes.size() >= 32 && std::memcmp(bytes.data(), "DXBC", 4) == 0;
}
std::shared_ptr<ShaderBinary> DecodeBinary(std::span<const u8> bytes) {
    u32 sizes[3]{};
    if (bytes.size() < sizeof(sizes))
        return {};
    std::memcpy(sizes, bytes.data(), sizeof(sizes));
    if (sizes[2] > 1024 ||
        u64(sizes[0]) + sizes[1] + u64(sizes[2]) * 20 + sizeof(sizes) != bytes.size())
        return {};
    bytes = bytes.subspan(sizeof(sizes));
    if (!ValidDxil(bytes.first(sizes[0])))
        return {};
    auto binary = std::make_shared<ShaderBinary>();
    binary->dxil.assign(bytes.begin(), bytes.begin() + sizes[0]);
    bytes = bytes.subspan(sizes[0]);
    binary->hlsl.assign(reinterpret_cast<const char*>(bytes.data()), sizes[1]);
    bytes = bytes.subspan(sizes[1]);
    for (u32 i = 0; i < sizes[2]; ++i) {
        u32 fields[5];
        std::memcpy(fields, bytes.data(), sizeof(fields));
        bytes = bytes.subspan(sizeof(fields));
        if (fields[0] > u32(ResourceKind::Constants) ||
            fields[1] > D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER || !fields[4] || fields[4] > 1024 ||
            fields[3] > Shader::MaxStageTypes)
            return {};
        binary->bindings.push_back({ResourceKind(fields[0]), D3D12_DESCRIPTOR_RANGE_TYPE(fields[1]),
                                    fields[2], fields[3], fields[4]});
    }
    return binary;
}
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
void RemapGeometryPrimitiveInput(std::string& source) {
    const auto first = source.find("struct SPIRV_Cross_Output");
    if (first == std::string::npos)
        return;
    const auto last = source.find("};", first);
    auto input_declaration = source.substr(first, last + 2 - first);
    const auto semantic = input_declaration.find("SV_PrimitiveID");
    const std::regex signature{R"(SPIRV_Cross_Output input\[(\d+)\])"};
    std::smatch match;
    const auto main = source.find("void main(");
    if (semantic == std::string::npos || main == std::string::npos)
        return;
    auto entry = source.substr(main);
    if (!std::regex_search(entry, match, signature))
        return;
    input_declaration.replace(semantic, 14, "TEXCOORD32");
    input_declaration.replace(input_declaration.find("SPIRV_Cross_Output"), 18, "Capture_Output");
    const u32 count = std::stoul(match[1].str());
    entry.replace(match.position(), match.length(),
                  "Capture_Output captured[" + std::to_string(count) + "]");
    std::string unpack = "\nSPIRV_Cross_Output input[" + std::to_string(count) + "];\n";
    const auto output = source.substr(first, last + 2 - first);
    const std::regex member{R"((\w+)\s*:\s*\w+\s*;)"};
    for (auto it = std::sregex_iterator(output.begin(), output.end(), member);
         it != std::sregex_iterator(); ++it)
        for (u32 i = 0; i < count; ++i)
            unpack += "input[" + std::to_string(i) + "]." + (*it)[1].str() + " = captured[" +
                      std::to_string(i) + "]." + (*it)[1].str() + ";\n";
    entry.insert(entry.find('{') + 1, unpack);
    source.replace(main, source.size() - main, entry);
    source.insert(last + 2, "\n" + input_declaration);
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
const wchar_t* Target(Shader::SwStage stage, D3D_SHADER_MODEL model) {
    switch (stage) {
    case Shader::SwStage::Vertex:
        return model >= D3D_SHADER_MODEL_6_6 ? L"vs_6_6" : L"vs_6_4";
    case Shader::SwStage::Fragment:
        return model >= D3D_SHADER_MODEL_6_6 ? L"ps_6_6" : L"ps_6_4";
    case Shader::SwStage::Compute:
        return model >= D3D_SHADER_MODEL_6_6 ? L"cs_6_6" : L"cs_6_4";
    case Shader::SwStage::Geometry:
        return model >= D3D_SHADER_MODEL_6_6 ? L"gs_6_6" : L"gs_6_4";
    case Shader::SwStage::TessellationControl:
        return model >= D3D_SHADER_MODEL_6_6 ? L"hs_6_6" : L"hs_6_4";
    case Shader::SwStage::TessellationEval:
        return model >= D3D_SHADER_MODEL_6_6 ? L"ds_6_6" : L"ds_6_4";
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
void ShaderCache::Initialize(ID3D12Device* device, const ResourceCache* resources,
                             D3D_SHADER_MODEL maximum_model) {
    variants.clear();
    variant_bytes = 0;
    variant_age = 0;
    statistics = {};
    cache.clear();
    cache_bytes = 0;
    this->resources = resources;
    native_device = device;
    persistent.Initialize("shaders");
    if (maximum_model < D3D_SHADER_MODEL_6_4 || maximum_model > D3D_SHADER_MODEL_6_6)
        throw std::runtime_error("Invalid D3D12 shader model ceiling");
    D3D12_FEATURE_DATA_SHADER_MODEL model{maximum_model};
    auto status = device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model));
    if (status == E_INVALIDARG && maximum_model > D3D_SHADER_MODEL_6_4) {
        model.HighestShaderModel = D3D_SHADER_MODEL_6_4;
        status = device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model));
    }
    winrt::check_hresult(status);
    if (model.HighestShaderModel < D3D_SHADER_MODEL_6_4)
        throw std::runtime_error("D3D12 guest shaders require Shader Model 6.4");
    shader_model = model.HighestShaderModel >= D3D_SHADER_MODEL_6_6 ? D3D_SHADER_MODEL_6_6
                                                                    : D3D_SHADER_MODEL_6_4;
    LOG_INFO(Core, "[UWP stage] native shader model {}.{} (reported {}.{})", u32(shader_model) >> 4,
             u32(shader_model) & 15, u32(model.HighestShaderModel) >> 4,
             u32(model.HighestShaderModel) & 15);
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 options{};
    winrt::check_hresult(
        device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options, sizeof(options)));
    LOG_INFO(Core, "[UWP stage] native wave capabilities: ops={} lanes={}..{}",
             bool(options.WaveOps), options.WaveLaneCountMin, options.WaveLaneCountMax);
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
    D3D12_FEATURE_DATA_D3D12_OPTIONS numeric_options{};
    winrt::check_hresult(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &numeric_options,
                                                     sizeof(numeric_options)));
    native_float64 = numeric_options.DoublePrecisionFloatShaderOps;
    profile.support_float64 = true;
    profile.support_fp32_denorm_flush = true;
    profile.support_fp32_denorm_preserve = true;
    LOG_INFO(Core, "[UWP stage] native FP64 support={}", native_float64);
    profile.supports_shader_stencil_export = true;
    profile.fast_shader_math = EmulatorSettings.IsFastShaderMath();
    profile.needs_image_load_feedback = true;
    profile.needs_subword_buffer_emulation = true;
    profile.needs_signed_atomic32_emulation = true;
    profile.needs_inline_vertex_fetch = true;
    profile.supports_image_sample_min_lod = true;
    profile.needs_unified_buffer_types = true;
    profile.needs_buffer_offsets = true;
}
void TessellationLink::ValidateNativeTopology() const {
    if (u32(type) > u32(AmdGpu::TessellationType::Quad))
        throw std::runtime_error("Invalid native tessellation domain");
    if (u32(topology) > u32(AmdGpu::TessellationTopology::TriangleCcw))
        throw std::runtime_error("Invalid native tessellation output topology");
    if ((type == AmdGpu::TessellationType::Isoline &&
         topology != AmdGpu::TessellationTopology::Line &&
         topology != AmdGpu::TessellationTopology::Point) ||
        (type != AmdGpu::TessellationType::Isoline &&
         topology == AmdGpu::TessellationTopology::Line))
        throw std::runtime_error(
            "Guest tessellation domain/topology pair has no defined D3D12 connectivity lowering");
}
CompiledShader ShaderCache::Compile(Shader::ShaderParams params, Shader::RuntimeInfo runtime,
                                    Shader::Backend::Bindings bindings, bool flip_y,
                                    bool sample_mask, bool sample_shading,
                                    const TessellationLink* tessellation, bool depth_bounds,
                                    bool flip_x) {
    auto prepared = Prepare(params, runtime, bindings);
    return Finalize(prepared, flip_y, sample_mask, sample_shading, tessellation, {}, depth_bounds,
                    flip_x);
}
std::unique_ptr<TranslatedShader> ShaderCache::Translate(Shader::ShaderParams params,
                                                         Shader::RuntimeInfo runtime) {
    if (params.code.empty() || params.code.size() > 1024 * 1024 ||
        runtime.props.num_user_data > Shader::NUM_USER_DATA_REGS ||
        (runtime.sw_stage == Shader::SwStage::Fragment && runtime.hw.fs.num_inputs > 32))
        throw std::runtime_error("Invalid guest shader code or register bounds");
    const auto stage = static_cast<u32>(runtime.sw_stage);
    if (stage >= pools.size())
        throw std::runtime_error("Invalid D3D12 shader stage");
    if (!EmulatorSettings.IsInlineFetchShader()) {
        for (const auto& variant : variants) {
            if (variant->direct_memory_access != EmulatorSettings.IsDirectMemoryAccessEnabled() ||
                !SameRuntime(variant->input_runtime, runtime) ||
                variant->translated.info.pgm_hash != params.hash ||
                variant->translated.info.pgm_base != params.Base() ||
                !std::ranges::equal(variant->code, params.code))
                continue;
            if (runtime.hw_stage == Shader::HwStage::Geometry &&
                !std::ranges::equal(variant->copy_code, runtime.hw.gs.vs_copy))
                continue;
            auto translated = std::make_unique<TranslatedShader>(
                TranslatedShader{variant->translated, {}, {}, {}});
            if (runtime.hw_stage == Shader::HwStage::Geometry)
                translated->shader.runtime.hw.gs.vs_copy = runtime.hw.gs.vs_copy;
            BindUserData(translated->shader.info, params.user_data);
            if (TranslationDependencies(translated->shader.info) != variant->dependencies)
                continue;
            translated->variant = variant;
            translated->params = params;
            if (shader_profile.enabled)
                shader_profile.Record("gcn_variant_hit", 128, 0);
            variant->used = ++variant_age;
            return translated;
        }
    }
    return TranslateUncached(params, runtime);
}
std::unique_ptr<TranslatedShader> ShaderCache::TranslateUncached(Shader::ShaderParams params,
                                                                 Shader::RuntimeInfo runtime) {
    ProfileScope timing{shader_profile, "gcn_translate", 16};
    const auto stage = static_cast<u32>(runtime.sw_stage);
    const auto input_runtime = runtime;
    if (!pools[stage])
        pools[stage] = std::make_unique<Shader::Pools>();
    auto translated = std::make_unique<TranslatedShader>(TranslatedShader{
        {Shader::Info{runtime.hw_stage, runtime.sw_stage, params}, runtime, {}, {}, {}}, {}});
    auto& shader = translated->shader;
    translated->program.emplace(
        Shader::TranslateProgram(params.code, *pools[stage], shader.info, shader.runtime, profile));
    if (shader.info.uses_fp64 && !native_float64)
        throw std::runtime_error(
            "Guest FP64 requires native double precision or a precise software implementation");
    shader.info.RefreshFlatBuf();
    ++statistics.translations;
    translated->params = params;
    if (!EmulatorSettings.IsInlineFetchShader()) {
        auto variant = std::make_shared<ShaderVariant>();
        variant->code.assign(params.code.begin(), params.code.end());
        variant->input_runtime = input_runtime;
        variant->direct_memory_access = EmulatorSettings.IsDirectMemoryAccessEnabled();
        if (runtime.hw_stage == Shader::HwStage::Geometry) {
            variant->copy_code.assign(input_runtime.hw.gs.vs_copy.begin(),
                                      input_runtime.hw.gs.vs_copy.end());
            variant->input_runtime.hw.gs.vs_copy = variant->copy_code;
        }
        variant->dependencies = TranslationDependencies(shader.info);
        variant->translated = shader;
        if (runtime.hw_stage == Shader::HwStage::Geometry)
            variant->translated.runtime.hw.gs.vs_copy = variant->copy_code;
        DetachUserData(variant->translated.info);
        variant->bytes = sizeof(ShaderVariant) +
                         (variant->code.capacity() + variant->copy_code.capacity() +
                          variant->dependencies.capacity()) *
                             sizeof(u32) +
                         InfoBytes(shader.info);
        translated->variant = std::move(variant);
    }
    return translated;
}
PreparedShader ShaderCache::Prepare(Shader::ShaderParams params, Shader::RuntimeInfo runtime,
                                    Shader::Backend::Bindings bindings) {
    return Prepare(Translate(params, runtime), bindings, {});
}
PreparedShader ShaderCache::Prepare(std::unique_ptr<TranslatedShader> translated,
                                    Shader::Backend::Bindings bindings,
                                    std::span<const ShaderResourceRange> stage_resources,
                                    const IndexFetch& indices) {
    const u64 mapping_generation = resources ? resources->MappingGeneration() : 0;
    auto variant = translated->variant;
    if (!translated->program) {
        for (const auto& prepared : variant->prepared) {
            if (prepared.bindings != bindings ||
                prepared.mapping_generation != mapping_generation || prepared.indices != indices ||
                !SameRanges(prepared.stage_resources, stage_resources))
                continue;
            auto result = prepared.shader;
            if (result.shader.runtime.hw_stage == Shader::HwStage::Geometry)
                result.shader.runtime.hw.gs.vs_copy = translated->shader.runtime.hw.gs.vs_copy;
            BindUserData(result.shader.info, translated->params->user_data);
            ++statistics.variant_hits;
            return result;
        }
        auto runtime = variant->input_runtime;
        if (runtime.hw_stage == Shader::HwStage::Geometry)
            runtime.hw.gs.vs_copy = translated->shader.runtime.hw.gs.vs_copy;
        translated = TranslateUncached(*translated->params, runtime);
        translated->variant = variant;
    }
    auto& result = translated->shader;
    auto& program = *translated->program;
    result.start_bindings = bindings;
    result.end_bindings = bindings;
    const bool index_fetch = LowerIndexFetch(result, program, indices);
    if (result.info.has_inline_fetch_shader)
        for (auto& buffer : result.info.buffers)
            if (buffer.buffer_type == Shader::BufferType::Guest)
                buffer.is_coherent = true;
    const bool image_aliases =
        LowerImageBufferAliases(result, program, resources, stage_resources, native_device);
    if (LowerBufferAliases(result, program, resources) || image_aliases || index_fetch) {
        Shader::Optimization::ConstantPropagationPass(program.post_order_blocks);
        Shader::Optimization::DeadCodeEliminationPass(program);
        Shader::Optimization::CollectShaderInfoPass(program, profile);
    }
    result.info.ResolveDirectReads();
    if (result.info.buffers.size() >
        Shader::NUM_BUFFERS - std::min(bindings.buffer, Shader::NUM_BUFFERS))
        throw std::runtime_error("D3D12 shader stages exceed push buffer offset capacity");
    const auto words =
        Shader::Backend::SPIRV::EmitSPIRV(profile, result.runtime, program, result.end_bindings);
    result.info.RefreshFlatBuf();
    PreparedShader prepared{std::move(result), words};
    if (variant && (!resources || mapping_generation == resources->MappingGeneration())) {
        constexpr size_t budget = 64 * 1024 * 1024;
        const size_t bytes = PreparedBytes(prepared) +
                             stage_resources.size() * sizeof(ShaderResourceRange) +
                             sizeof(ShaderVariant::Prepared);
        const size_t required = bytes + variant->bytes;
        if (required <= budget) {
            if (const auto found = std::ranges::find(variants, variant); found != variants.end()) {
                variant_bytes -= variant->bytes;
                variants.erase(found);
            }
            while (!variants.empty() &&
                   (variants.size() >= 512 || variant_bytes + required > budget)) {
                const auto oldest = std::ranges::min_element(
                    variants, {}, [](const auto& entry) { return entry->used; });
                variant_bytes -= (*oldest)->bytes;
                variants.erase(oldest);
            }
            if (std::ranges::find(variants, variant) == variants.end()) {
                variant_bytes += variant->bytes;
                variants.push_back(variant);
            }
            auto snapshot = prepared;
            DetachUserData(snapshot.shader.info);
            if (snapshot.shader.runtime.hw_stage == Shader::HwStage::Geometry)
                snapshot.shader.runtime.hw.gs.vs_copy = variant->copy_code;
            variant->prepared.push_back({bindings,
                                         {stage_resources.begin(), stage_resources.end()},
                                         mapping_generation,
                                         indices,
                                         std::move(snapshot)});
            variant->bytes += bytes;
            variant_bytes += bytes;
            variant->used = ++variant_age;
        }
    }
    return prepared;
}
CompiledShader ShaderCache::Finalize(PreparedShader& prepared, bool flip_y, bool sample_mask,
                                     bool sample_shading, const TessellationLink* tessellation,
                                     std::span<const u32> peer, bool depth_bounds, bool flip_x) {
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
                                 link ? &*link : nullptr, peer, depth_bounds, flip_x);
    return std::move(result);
}
std::shared_ptr<const ShaderBinary> ShaderCache::CompileSpirv(
    std::span<const u32> words, Shader::SwStage stage, bool flip_y, bool sample_mask,
    bool sample_shading, const TessellationLink* tessellation_link, std::span<const u32> peer,
    bool depth_bounds, bool flip_x) {
    if (words.size() < 5 || words.size() > 4 * 1024 * 1024 || words[0] != spv::MagicNumber)
        throw std::runtime_error("Invalid guest SPIR-V module");
    if (depth_bounds)
        throw std::runtime_error("Depth bounds requires output-merger state");
    if (tessellation_link)
        tessellation_link->ValidateNativeTopology();
    bool subgroup{};
    bool quad_only = true;
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
            "D3D12 graphics stages cannot force 64-lane waves (WaveSize is compute-only); "
            "guest cross-lane operations require a fixed wave64 device");
    if (stage == Shader::SwStage::Compute && shader_model < D3D_SHADER_MODEL_6_6 &&
        !fixed_graphics_wave64 && subgroup && !quad_only)
        throw std::runtime_error(
            "D3D12 Shader Model 6.4 cross-lane compute requires a fixed wave64 device");
    std::vector<u32> key{static_cast<u32>(stage),        static_cast<u32>(flip_y),
                         static_cast<u32>(sample_mask),  static_cast<u32>(sample_shading),
                         static_cast<u32>(depth_bounds), static_cast<u32>(flip_x)};
    if (tessellation_link) {
        key.insert(key.end(), {static_cast<u32>(tessellation_link->type),
                               static_cast<u32>(tessellation_link->partitioning),
                               static_cast<u32>(tessellation_link->topology),
                               tessellation_link->input_control_points,
                               tessellation_link->output_control_points});
    }
    key.push_back(static_cast<u32>(shader_model));
    key.push_back(static_cast<u32>(words.size()));
    key.insert(key.end(), words.begin(), words.end());
    key.insert(key.end(), peer.begin(), peer.end());
    if (stage == Shader::SwStage::Geometry || stage == Shader::SwStage::TessellationEval)
        key.push_back(0xf0900002);
    if (const auto found = cache.find(key); found != cache.end()) {
        if (shader_profile.enabled)
            shader_profile.Record("spirv_ram_hit", 128, 0);
        return found->second;
    }
    const auto disk_key = ShaderDiskKey(key);
    if (const auto bytes = persistent.Load(disk_key); !bytes.empty()) {
        if (auto binary = DecodeBinary(bytes)) {
            if (shader_profile.enabled)
                shader_profile.Record("spirv_disk_hit", 32, 0);
            Remember(std::move(key), binary);
            return binary;
        }
        persistent.Remove(disk_key);
    }
    ProfileScope timing{shader_profile, "spirv_compile_miss", 16};
    spirv_cross::CompilerHLSL cross{words.data(), words.size()};
    const auto entries = cross.get_entry_points_and_stages();
    if (entries.size() != 1 || entries.front().execution_model != Model(stage))
        throw std::runtime_error("Guest shader entry point mismatch");
    cross.set_entry_point(entries.front().name, Model(stage));
    auto common_options = cross.get_common_options();
    common_options.vertex.flip_vert_y = flip_y;
    common_options.vertex.flip_pos_x = flip_x;
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
    options.shader_model = shader_model >= D3D_SHADER_MODEL_6_6 ? 66 : 64;
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
        CompileNativeSpirv(words, *result, stage, flip_y, tessellation_link, peer, flip_x);
    else {
        result->hlsl = cross.compile();
        for (size_t offset = 5; offset < words.size();) {
            const u32 length = words[offset] >> 16;
            if (!length || length > words.size() - offset)
                throw std::runtime_error("Invalid numeric SPIR-V instruction");
            if ((words[offset] & 0xffff) == spv::OpExecutionMode && length == 4 &&
                words[offset + 3] == 32) {
                if (words[offset + 2] == spv::ExecutionModeDenormPreserve)
                    result->hlsl.insert(0, "#define SHAD_FP32_DENORM_PRESERVE 1\n");
                else if (words[offset + 2] == spv::ExecutionModeDenormFlushToZero)
                    result->hlsl.insert(0, "#define SHAD_FP32_DENORM_FLUSH 1\n");
            }
            offset += length;
        }
    }
    if (!native_spirv) {
        AlignSemanticRows(result->hlsl, "SPIRV_Cross_Input");
        AlignSemanticRows(result->hlsl, "SPIRV_Cross_Output");
    }
    if (sample_mask || sample_shading) {
        if (stage != Shader::SwStage::Fragment)
            throw std::runtime_error("Sample masks require a fragment shader");
        ApplySampleMasks(result->hlsl, sample_shading);
    }
    if (stage == Shader::SwStage::Compute && shader_model >= D3D_SHADER_MODEL_6_6) {
        const auto entry = result->hlsl.find("[numthreads(");
        if (entry == std::string::npos)
            throw std::runtime_error("Missing guest compute workgroup declaration");
        result->hlsl.insert(entry, "[WaveSize(64)]\n");
    }
    if (!native_spirv)
        CompileHlsl(*result, stage);
    else if (stage == Shader::SwStage::Geometry || stage == Shader::SwStage::TessellationEval)
        result->hlsl = OutputInterface(*result);
    persistent.Store(disk_key, EncodeBinary(*result));
    Remember(std::move(key), result);
    return result;
}
void ShaderCache::TrimMemory(u64 target) {
    for (auto& pool : pools)
        pool.reset();
    while (MemoryBytes() > target && !variants.empty()) {
        const auto oldest = std::ranges::min_element(
            variants, {}, [](const auto& variant) { return variant->used; });
        variant_bytes -= (*oldest)->bytes;
        variants.erase(oldest);
    }
    while (MemoryBytes() > target && !cache.empty()) {
        const auto& oldest = *cache.begin();
        cache_bytes -= oldest.first.size() * sizeof(u32) + oldest.second->dxil.size() +
                       oldest.second->hlsl.size();
        cache.erase(cache.begin());
    }
}
void ShaderCache::Remember(std::vector<u32> key, std::shared_ptr<const ShaderBinary> result) {
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
}
std::shared_ptr<const ShaderBinary> ShaderCache::CompileConstantAlphaBlend(
    const ShaderBinary& fragment, float alpha) {
    auto source = fragment.hlsl;
    const auto entry = source.find(" main(");
    if (entry == std::string::npos ||
        SemanticMember(source, "SPIRV_Cross_Output", "SV_Target0").empty())
        throw std::runtime_error("Constant alpha blending requires color output zero");
    const auto close = source.find(')', entry);
    if (close == std::string::npos)
        throw std::runtime_error("Invalid constant alpha blend fragment entry");
    const auto arguments = source.substr(entry + 6, close - entry - 6);
    std::string forwarded;
    std::istringstream argument_stream(arguments);
    for (std::string argument; std::getline(argument_stream, argument, ',');) {
        std::istringstream declaration(argument);
        std::string type, name;
        if (!(declaration >> type >> name))
            throw std::runtime_error("Invalid constant alpha blend fragment argument");
        if (!forwarded.empty())
            forwarded += ", ";
        forwarded += name;
    }
    const auto output_start = source.find("struct SPIRV_Cross_Output");
    const auto output_end = source.find("};", output_start);
    if (output_start == std::string::npos || output_end == std::string::npos)
        throw std::runtime_error("Missing constant alpha blend output interface");
    const std::regex members{R"(((?:float|uint|int)[1-4]?)\s+(\w+)\s*:\s*(\w+)\s*;)"};
    const auto output = source.substr(output_start, output_end - output_start);
    std::string declaration = "\nstruct ShadBlendOutput {\n", copies;
    for (std::sregex_iterator field(output.begin(), output.end(), members), end; field != end;
         ++field) {
        const auto semantic = (*field)[3].str();
        if (semantic.starts_with("SV_Target") && semantic != "SV_Target0")
            continue;
        declaration += field->str() + "\n";
        const auto name = (*field)[2].str();
        copies += "value." + name + " = guest." + name + ";\n";
    }
    declaration += "float4 shadBlendAlpha : SV_Target1; };\n";
    const auto renamed = source.find(" main(");
    source.replace(renamed + 1, 4, "shad_blend_guest_main");
    source += declaration;
    if (source.find("[earlydepthstencil]") != std::string::npos)
        source += "\n[earlydepthstencil]\n";
    source += "\nShadBlendOutput main(" + arguments + ") {\n";
    source += "SPIRV_Cross_Output guest = shad_blend_guest_main(" + forwarded + ");\n";
    source += "ShadBlendOutput value;\n" + copies;
    source += fmt::format("value.shadBlendAlpha = asfloat({}u).xxxx;\n", std::bit_cast<u32>(alpha));
    source += "return value; }\n";
    std::vector<u32> key{UINT32_MAX - 6, u32(shader_model)};
    for (const unsigned char character : source)
        key.push_back(character);
    for (const auto& binding : fragment.bindings)
        key.insert(key.end(), {u32(binding.kind), u32(binding.type), binding.binding, binding.space,
                               binding.count});
    if (const auto found = cache.find(key); found != cache.end())
        return found->second;
    auto result = std::make_shared<ShaderBinary>(fragment);
    result->hlsl = std::move(source);
    CompileHlsl(*result, Shader::SwStage::Fragment);
    Remember(std::move(key), result);
    return result;
}
std::shared_ptr<const ShaderBinary> ShaderCache::CompileFaceFragment(const ShaderBinary& fragment,
                                                                     bool front) {
    auto source = fragment.hlsl;
    const auto field = SemanticMember(source, "SPIRV_Cross_Input", "SV_IsFrontFace");
    const std::regex signature{R"(\bmain\s*\(\s*SPIRV_Cross_Input\s+(\w+))"};
    std::smatch match;
    if (field.empty() || !std::regex_search(source, match, signature))
        throw std::runtime_error("Missing fragment front-face interface");
    const auto body = source.find('{', size_t(match.position() + match.length()));
    if (body == std::string::npos)
        throw std::runtime_error("Missing fragment entry body");
    source.insert(body + 1,
                  "\n" + match[1].str() + "." + field + " = " + (front ? "true" : "false") + ";\n");
    return CompileHelperHlsl(source, Shader::SwStage::Fragment);
}
std::shared_ptr<const ShaderBinary> ShaderCache::CompilePointFill(
    const ShaderBinary& vertex, bool keep_front, bool front_ccw, bool cull,
    const Shader::HwFragmentRuntimeInfo* fragment, bool masks) {
    const auto first = vertex.hlsl.find("struct SPIRV_Cross_Output");
    const auto last = vertex.hlsl.find("};", first);
    if (first == std::string::npos || last == std::string::npos || last - first > 16384)
        throw std::runtime_error("Invalid point fill vertex interface");
    const auto position = SemanticMember(vertex.hlsl, "SPIRV_Cross_Output", "SV_Position");
    if (position.empty())
        throw std::runtime_error("Point fill requires a position export");
    auto result = std::make_shared<ShaderBinary>();
    result->hlsl = vertex.hlsl.substr(first, last + 2 - first);
    if (masks)
        result->hlsl +=
            "cbuffer FaceMask : register(b0,space100) { uint face_batch; uint polygon_mask; };\n";
    result->hlsl += "\n[maxvertexcount(3)]\nvoid main(triangle SPIRV_Cross_Output input[3], "
                    "inout PointStream<SPIRV_Cross_Output> stream) {\n";
    for (u32 i = 0; i < 3; ++i)
        result->hlsl += "float4 h" + std::to_string(i) + " = input[" + std::to_string(i) + "]." +
                        position + ";\n";
    result->hlsl += "float det = h0.x * (h1.y * h2.w - h1.w * h2.y) - h0.y * (h1.x * h2.w - h1.w * "
                    "h2.x) + h0.w * (h1.x * h2.y - h1.y * h2.x);\n";
    result->hlsl +=
        std::string("bool frontfacing = ") + (front_ccw ? "det > 0.0" : "det < 0.0") + ";\n";
    if (cull)
        result->hlsl +=
            std::string("if (!(") + (keep_front ? "frontfacing" : "!frontfacing") + ")) return;\n";
    for (u32 i = 0; i < 3; ++i) {
        if (masks)
            result->hlsl += "if ((polygon_mask & " + std::to_string(1u << (i + 3)) + ") != 0) ";
        result->hlsl += "{ SPIRV_Cross_Output value = input[" + std::to_string(i) + "];\n";
        if (fragment)
            for (u32 input = 0; input < fragment->num_inputs; ++input) {
                const auto& item = fragment->inputs[input];
                if (!item.is_flat)
                    continue;
                const auto field = SemanticMember(vertex.hlsl, "SPIRV_Cross_Output",
                                                  "TEXCOORD" + std::to_string(item.param_index));
                if (!field.empty())
                    result->hlsl += "value." + field + " = input[0]." + field + ";\n";
            }
        result->hlsl += "stream.Append(value); }\n";
    }
    result->hlsl += "}\n";
    std::vector<u32> key{UINT32_MAX - 1};
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
std::shared_ptr<const ShaderBinary> ShaderCache::CompileLineExpand(
    const ShaderBinary& vertex, const Shader::HwFragmentRuntimeInfo& fragment, bool keep_front,
    bool front_ccw, D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_type, bool adjacency, float width_px,
    u16 pattern, u16 repeat, bool pattern_msb_first, float pattern_scale,
    const std::array<f32, 32>& viewport_dims, bool cull, bool batches, bool masks, u32 linear_mask,
    u32 flat_mask, bool depth_clip) {
    const auto first = vertex.hlsl.find("struct SPIRV_Cross_Output");
    const auto last = vertex.hlsl.find("};", first);
    if (first == std::string::npos || last == std::string::npos || last - first > 16384)
        throw std::runtime_error("Invalid line expansion vertex interface");
    const auto position = SemanticMember(vertex.hlsl, "SPIRV_Cross_Output", "SV_Position");
    if (position.empty())
        throw std::runtime_error("Line expansion requires a position export");
    const auto viewport_index =
        SemanticMember(vertex.hlsl, "SPIRV_Cross_Output", "SV_ViewportArrayIndex");
    const bool stipple = pattern_scale > 0.f && pattern != u16(0xFFFF);
    const bool wire = topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    if (topology_type != D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE && !wire)
        throw std::runtime_error("Unsupported topology for line expansion");
    auto result = std::make_shared<ShaderBinary>();
    result->hlsl = vertex.hlsl.substr(first, last + 2 - first);
    std::string input_type;
    u32 input_count{}, pair_count{};
    std::array<u32, 3> bases{};
    if (!wire && !adjacency) {
        input_type = "line";
        input_count = 2;
        pair_count = 1;
        bases = {0u, 1u, 0u};
    } else if (!wire) {
        input_type = "lineadj";
        input_count = 4;
        pair_count = 1;
        bases = {1u, 2u, 0u};
    } else if (!adjacency) {
        input_type = "triangle";
        input_count = 3;
        pair_count = 3;
        bases = {0u, 1u, 2u};
    } else {
        input_type = "triangleadj";
        input_count = 6;
        pair_count = 3;
        bases = {0u, 2u, 4u};
    }
    u32 components{};
    for (const auto& output : StageOutputs(vertex))
        components += output.components;
    const u32 windows_per_instance =
        stipple ? std::max(1u, std::min(128u, 1024u / (4 * components))) : 1u;
    const u32 instance_groups = 32 / pair_count;
    if (batches)
        result->hlsl +=
            std::string("cbuffer LineBatch : register(b0,space100) { uint line_batch; ") +
            (masks ? "uint polygon_mask; " : "") + "};\n";
    result->hlsl += "\n[maxvertexcount(" + std::to_string(4u * windows_per_instance) +
                    ")]\n[instance(" + std::to_string(stipple ? 32u : pair_count) +
                    ")]\nvoid main(" + input_type + " SPIRV_Cross_Output input[" +
                    std::to_string(input_count) +
                    "], uint inst : SV_GSInstanceID, inout TriangleStream<SPIRV_Cross_Output> "
                    "stream) {\n";
    if (wire && cull) {
        const auto at = [&](u32 offset) {
            return "input[" + std::to_string(bases[offset]) + "]." + position;
        };
        result->hlsl += "float4 h0 = " + at(0) + "; float4 h1 = " + at(1) +
                        "; float4 h2 = " + at(2) +
                        ";\nfloat det = h0.x * (h1.y * h2.w - h1.w * "
                        "h2.y) - h0.y * (h1.x * h2.w - h1.w * h2.x) + h0.w * (h1.x * h2.y - h1.y * "
                        "h2.x);\nbool frontfacing = " +
                        (front_ccw ? "det > 0.0" : "det < 0.0") + ";\nif (!(" +
                        (keep_front ? "frontfacing" : "!frontfacing") + ")) return;\n";
    }
    result->hlsl += "if (inst >= " + std::to_string(instance_groups * pair_count) +
                    ") return;\nSPIRV_Cross_Output vset[2];\nfor (uint seg = 0; seg < " +
                    std::to_string(pair_count) + "; ++seg) {\nif (seg != inst % " +
                    std::to_string(pair_count) + ") continue;\n";
    if (masks)
        result->hlsl += "if ((polygon_mask & (1u << seg)) == 0) continue;\n";
    result->hlsl += "if (seg == 0) { vset[0] = input[" + std::to_string(bases[0]) +
                    "]; vset[1] = input[" + std::to_string(bases[1]) + "]; }\n";
    if (pair_count == 3) {
        result->hlsl += "else if (seg == 1) { vset[0] = input[" + std::to_string(bases[1]) +
                        "]; vset[1] = input[" + std::to_string(bases[2]) + "]; }\n";
        result->hlsl += "else { vset[0] = input[" + std::to_string(bases[2]) +
                        "]; vset[1] = input[" + std::to_string(bases[0]) + "]; }\n";
    }
    result->hlsl += "float4 ha = vset[0]." + position + "; float4 hb = vset[1]." + position + ";\n";
    result->hlsl += R"(
float4 original_a = ha, original_b = hb;
float clip_lo = 0.0, clip_hi = 1.0;
float aplanes[6] = {ha.w+ha.x,ha.w-ha.x,ha.w+ha.y,ha.w-ha.y,ha.z,ha.w-ha.z};
float bplanes[6] = {hb.w+hb.x,hb.w-hb.x,hb.w+hb.y,hb.w-hb.y,hb.z,hb.w-hb.z};
bool outside = false;
)";
    result->hlsl +=
        "for(uint plane=0;plane<" + std::to_string(depth_clip ? 6 : 4) +
        ";++plane) { float a=aplanes[plane], b=bplanes[plane]; if(a<0 && b<0) outside=true; else "
        "if(a<0) clip_lo=max(clip_lo,a/(a-b)); else if(b<0) clip_hi=min(clip_hi,a/(a-b)); }\n";
    result->hlsl += R"(
if(outside || clip_lo>clip_hi) continue;
ha = clip_lo == 0 ? original_a : lerp(original_a,original_b,clip_lo);
hb = clip_hi == 1 ? original_b : lerp(original_a,original_b,clip_hi);
if (!(ha.w > 0.0 && hb.w > 0.0)) continue;
float inv_w0 = 1.0 / ha.w, inv_w1 = 1.0 / hb.w;
float3 ndc0 = ha.xyz * inv_w0, ndc1 = hb.xyz * inv_w1;
)";
    std::string vp_table;
    for (u32 k = 0; k < 32; ++k) {
        if (k) {
            vp_table += ", ";
        }
        vp_table += std::to_string(viewport_dims[k]);
    }
    result->hlsl += "float vp_dim[32] = {" + vp_table + "};\n";
    if (viewport_index.empty()) {
        result->hlsl += "uint vp_idx = 0;\n";
    } else {
        result->hlsl += "uint vp_idx = uint(vset[0]." + viewport_index + ");\n";
    }
    result->hlsl += "if(vp_idx>=16) continue; float2 vp = float2(vp_dim[vp_idx * 2], vp_dim[vp_idx "
                    "* 2 + 1]); if(!(vp.x>0 && vp.y>0)) continue;\n";
    result->hlsl += "float2 dp = (ndc1.xy - ndc0.xy) * 0.5 * vp;\nfloat seg_len = length(dp);\n"
                    "if (!(seg_len > 0.0001)) continue;\n"
                    "float2 dir = dp / seg_len;\nfloat2 perp = float2(dir.y, -dir.x);\n"
                    "float2 off = perp / (0.5 * vp) * " +
                    std::to_string(width_px * 0.5f) + ";\n";
    std::string emit_body =
        "SPIRV_Cross_Output dash[4];\nfor (uint corner = 0; corner < 4; ++corner) {\n"
        "float tsel = (corner == 0 || corner == 1) ? t0 : t1;\n"
        "float2 pdir = (corner == 0 || corner == 3) ? off : -off;\n"
        "float2 pos_ndc = ndc0.xy + tsel * (ndc1.xy - ndc0.xy);\n"
        "float w = tsel==0 ? ha.w : tsel==1 ? hb.w : 1.0 / (inv_w0 + tsel * (inv_w1 - inv_w0));\n"
        "dash[corner]." +
        position +
        " = tsel==0 ? ha : tsel==1 ? hb : float4(pos_ndc * w, lerp(ndc0.z,ndc1.z,tsel) * w,w);\n"
        "dash[corner]." +
        position + ".xy += pdir * w;\n";
    const auto body = vertex.hlsl.find('{', first);
    std::istringstream members(vertex.hlsl.substr(body + 1, last - body - 1));
    for (std::string member; std::getline(members, member, ';');) {
        std::istringstream declaration(member);
        std::string type, name, colon, semantic;
        if (!(declaration >> type))
            continue;
        if (!(declaration >> name >> colon >> semantic) || colon != ":" ||
            name.find('[') != std::string::npos)
            throw std::runtime_error("Unsupported line expansion stage interface member");
        bool flat{}, linear{};
        for (u32 i = 0; i < fragment.num_inputs; ++i) {
            const auto& input = fragment.inputs[i];
            if (semantic == "TEXCOORD" + std::to_string(input.param_index) &&
                input.param_index < 32) {
                flat |= input.is_flat || (flat_mask & (1u << input.param_index));
                linear |= (linear_mask & (1u << input.param_index)) != 0;
            }
        }
        if (semantic == "SV_Position") {
            continue;
        }
        if (flat || (type != "float" && type != "float2" && type != "float3" && type != "float4")) {
            emit_body +=
                "dash[corner]." + name + " = " + (wire ? "input[0]." : "vset[0].") + name + ";\n";
        } else {
            const auto a = "vset[0]." + name;
            const auto b = "vset[1]." + name;
            const auto left = "(clip_lo==0 ? " + a + " : lerp(" + a + "," + b + "," +
                              (linear ? "clip_lo*original_b.w*inv_w0" : "clip_lo") + "))";
            const auto right = "(clip_hi==1 ? " + b + " : lerp(" + a + "," + b + "," +
                               (linear ? "clip_hi*original_b.w*inv_w1" : "clip_hi") + "))";
            emit_body += "dash[corner]." + name + " = tsel==0 ? " + left + " : tsel==1 ? " + right +
                         " : " +
                         (linear ? "lerp(" + left + "," + right + ",tsel)"
                                 : "lerp(" + left + "*inv_w0," + right + "*inv_w1,tsel)*w") +
                         ";\n";
        }
    }
    emit_body += "}\nstream.Append(dash[0]); stream.Append(dash[1]); stream.Append(dash[3]); "
                 "stream.Append(dash[2]); stream.RestartStrip();\n";
    if (stipple) {
        result->hlsl +=
            "const float bit_len = " +
            fmt::format("{:.9e}", double(pattern_scale) * (u32(repeat) + 1)) +
            ";\n"
            "const uint pattern_u = " +
            std::to_string(u32(pattern)) +
            ";\n"

            "float clipped_distance = 0;\n"
            "if(original_a.w>0 && clip_lo>0) clipped_distance = length((ndc0.xy - "
            "original_a.xy/original_a.w)*0.5*vp);\n"
            "float phase = clipped_distance / bit_len; uint phase_bit = "
            "uint(fmod(floor(phase),16.0));\n"
            "float phase_fraction = frac(phase);\n"
            "for (uint bit = (inst / " +
            std::to_string(pair_count) + ") * " + std::to_string(windows_per_instance) +
            "; bit < (inst / " + std::to_string(pair_count) + " + 1) * " +
            std::to_string(windows_per_instance) + "; ++bit) {\n" +
            (batches ? "uint window_bit = bit + line_batch * " +
                           std::to_string(instance_groups * windows_per_instance) + ";\n"
                     : "uint window_bit = bit;\n") +
            "float win_start = (float(window_bit) - phase_fraction) * bit_len;\n"
            "if (win_start >= seg_len) break;\n"
            "uint sel = (phase_bit + window_bit) % 16;\nsel = " +
            std::string(pattern_msb_first ? "15 - sel" : "sel") +
            ";\nif (!((pattern_u >> sel) & 1)) continue;\n"
            "float t0 = max(win_start,0.0) / seg_len;\n"
            "float t1 = min(win_start + bit_len, seg_len) / seg_len;\n" +
            emit_body + "}\n";
    } else {
        result->hlsl += "{ float t0 = 0.0; float t1 = 1.0;\n" + emit_body + "}\n";
    }
    result->hlsl += "}\n}\n";
    std::vector<u32> key{UINT32_MAX - 2};
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
std::shared_ptr<const ShaderBinary> ShaderCache::CompileLogicOp(
    const ShaderBinary& fragment, const std::array<DXGI_FORMAT, 8>& formats,
    const std::array<u32, 8>& masks) {
    std::vector<u32> key{0x4c4f4749};
    for (const auto format : formats)
        key.push_back(u32(format));
    key.insert(key.end(), masks.begin(), masks.end());
    key.push_back(u32(fragment.dxil.size()));
    for (u32 offset = 0; offset < fragment.dxil.size(); offset += 4) {
        u32 word{};
        std::memcpy(&word, fragment.dxil.data() + offset,
                    std::min<size_t>(4, fragment.dxil.size() - offset));
        key.push_back(word);
    }
    if (const auto found = cache.find(key); found != cache.end())
        return found->second;
    auto result = std::make_shared<ShaderBinary>(fragment);
    std::string helpers = R"(
uint LogicUnsignedFloat(float v, uint mantissa) {
    if (isnan(v)) return (31u << mantissa) | 1u;
    v = clamp(v, 0.0, (2.0 - exp2(-float(mantissa))) * 32768.0);
    uint word = asuint(v);
    if ((word >> 23u) < 113u) return uint(round(v * exp2(14.0 + float(mantissa))));
    uint shift = 23u - mantissa;
    return ((word + ((1u << (shift - 1u)) - 1u) + ((word >> shift) & 1u)) >> shift) - (112u << mantissa);
}
)";
    const std::regex declaration(
        R"(((?:float|uint|int)[1-4]?)\s+(\w+)\s*:\s*SV_Target([0-7])\s*;)");
    const auto start = result->hlsl.find("struct SPIRV_Cross_Output");
    const auto end = result->hlsl.find("};", start);
    if (start == std::string::npos || end == std::string::npos)
        throw std::runtime_error("Logic operation requires fragment output structure");
    std::string structure = result->hlsl.substr(start, end + 2 - start);
    const std::string original = structure;
    for (std::sregex_iterator it(original.begin(), original.end(), declaration), last; it != last;
         ++it) {
        const auto& match = *it;
        const u32 slot = u32(match[3].str()[0] - '0');
        if (formats[slot] == DXGI_FORMAT_UNKNOWN)
            continue;
        const auto format = GetLogicOpFormat(formats[slot]);
        const auto name = match[2].str();
        const auto type = match[1].str();
        const u32 count = std::isdigit(type.back()) ? u32(type.back() - '0') : 1;
        const auto base_type = type.substr(0, type.size() - (std::isdigit(type.back()) ? 1 : 0));
        const std::string function = "LogicPack" + std::to_string(slot);
        helpers += "uint4 " + function + "(" + base_type + "4 v) { uint4 r = 0;\n";
        u32 packed_component = 0;
        for (u32 c = 0; c < 4; ++c) {
            const u32 bits = format.bits[c];
            if (!bits || !(masks[slot] & (1u << c)))
                continue;
            const auto component = "v[" + std::to_string(c) + "]";
            std::string value = component;
            if (format.srgb && c < 3)
                value = "(" + component + " <= 0.0031308 ? " + component +
                        " * 12.92 : 1.055 * pow(max(" + component + ", 0.0), 1.0 / 2.4) - 0.055)";
            using Encoding = LogicOpFormat::Encoding;
            if (format.encoding == Encoding::Uint) {
                value = bits == 32
                            ? "uint(" + value + ")"
                            : "min(uint(" + value + "), " + std::to_string((1u << bits) - 1) + "u)";
            } else if (format.encoding == Encoding::Sint) {
                if (bits < 32)
                    value = "clamp(int(" + value + "), -" + std::to_string(1u << (bits - 1)) +
                            ", " + std::to_string((1u << (bits - 1)) - 1) + ")";
                value = "asuint(" + value + ")";
            } else if (format.encoding == Encoding::Float)
                value = bits == 32 ? "asuint(" + value + ")"
                                   : "f32tof16(clamp(" + value + ", -65504.0, 65504.0))";
            else if (format.encoding == Encoding::UnsignedFloat)
                value = "LogicUnsignedFloat(" + value + ", " + std::to_string(bits - 5) + "u)";
            else if (format.encoding == Encoding::Snorm) {
                const auto maximum = std::to_string((1u << (bits - 1)) - 1) + ".0";
                value = "(asuint(int(sign(" + value + ") * floor(abs(clamp(" + value +
                        ", -1.0, 1.0)) * " + maximum + " + 0.5))) & " +
                        std::to_string((1u << bits) - 1) + "u)";
            } else
                value = "uint(floor(saturate(" + value + ") * " + std::to_string((1u << bits) - 1) +
                        ".0 + 0.5))";
            helpers += "r[" + std::to_string(packed_component++) + "] = " + value + ";\n";
        }
        helpers += "return r; }\n";
        const auto declaration_at = structure.find(match.str());
        structure.replace(declaration_at, match.length(),
                          "uint4 " + name + " : SV_Target" + std::to_string(slot) + ";");
        const std::regex assignment("(\\b\\w+\\." + name + "\\s*=\\s*)([^;]+);");
        std::string rewritten;
        size_t previous = 0;
        bool assigned = false;
        for (std::sregex_iterator item(result->hlsl.begin(), result->hlsl.end(), assignment),
             finish;
             item != finish; ++item) {
            rewritten += result->hlsl.substr(previous, item->position() - previous);
            std::string expression = (*item)[2].str();
            if (count < 4) {
                expression = base_type + "4(" + expression;
                for (u32 c = count; c < 4; ++c)
                    expression += c == 3 ? ", 1.0" : ", 0.0";
                expression += ")";
            }
            rewritten += (*item)[1].str() + function + "(" + expression + ");";
            previous = item->position() + item->length();
            assigned = true;
        }
        if (!assigned)
            throw std::runtime_error("Missing logic operation color export assignment");
        rewritten += result->hlsl.substr(previous);
        result->hlsl = std::move(rewritten);
    }
    const auto structure_end = result->hlsl.find("};", start);
    result->hlsl.replace(start, structure_end + 2 - start, structure);
    result->hlsl.insert(0, helpers);
    CompileHlsl(*result, Shader::SwStage::Fragment);
    const auto bytes = key.size() * sizeof(u32) + result->hlsl.size() + result->dxil.size();
    if (bytes > 64 * 1024 * 1024)
        throw std::runtime_error("Logic operation shader exceeds cache budget");
    while (!cache.empty() && (cache.size() >= 512 || cache_bytes + bytes > 64 * 1024 * 1024)) {
        const auto& oldest = *cache.begin();
        cache_bytes -= oldest.first.size() * sizeof(u32) + oldest.second->dxil.size() +
                       oldest.second->hlsl.size();
        cache.erase(cache.begin());
    }
    cache.emplace(std::move(key), result);
    cache_bytes += bytes;
    return result;
}
std::shared_ptr<const ShaderBinary> ShaderCache::CompileRectangle(
    const ShaderBinary& vertex, const Shader::HwFragmentRuntimeInfo& fragment, bool flat_last) {
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
            result->hlsl += "for(uint i=0;i<4;++i) v[i]." + name + " = input[" +
                            std::string(flat_last ? "2" : "0") + "]." + name + ";\n";
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
                                     const TessellationLink* link, std::span<const u32> peer,
                                     bool flip_x) {
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
    config.shader_model_max =
        shader_model >= D3D_SHADER_MODEL_6_6 ? SHADER_MODEL_6_6 : SHADER_MODEL_6_4;
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
        case AmdGpu::TessellationPartitioning::Pow2:
            config.tessellation_spacing = 1;
            break;
        case AmdGpu::TessellationPartitioning::FracOdd:
            config.tessellation_spacing = 2;
            break;
        case AmdGpu::TessellationPartitioning::FracEven:
            config.tessellation_spacing = 3;
            break;
        default:
            throw std::runtime_error("Invalid native tessellation partitioning");
        }
        config.tessellation_input_control_points = link->input_control_points;
        config.tessellation_output_control_points = link->output_control_points;
        config.tessellation_ccw = link->topology == AmdGpu::TessellationTopology::TriangleCcw;
        config.tessellation_point_mode = link->topology == AmdGpu::TessellationTopology::Point;
    }
    config.yz_flip.mode = flip_y ? DXIL_SPIRV_Y_FLIP_UNCONDITIONAL : DXIL_SPIRV_YZ_FLIP_NONE;
    if (flip_x)
        config.yz_flip.mode = static_cast<enum dxil_spirv_yz_flip_mode>(
            config.yz_flip.mode | DXIL_SPIRV_X_FLIP_UNCONDITIONAL);
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
    const auto validator_version =
        shader_model >= D3D_SHADER_MODEL_6_6 ? DXIL_VALIDATOR_1_6 : DXIL_VALIDATOR_1_4;
    if (!compile(remapped.data(), remapped.size(), nullptr, 0, model, "main", validator_version,
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
std::vector<ShaderCache::StageOutput> ShaderCache::StageOutputs(const ShaderBinary& binary) {
    DxcBuffer buffer{binary.dxil.data(), binary.dxil.size(), 0};
    Microsoft::WRL::ComPtr<ID3D12ShaderReflection> reflection;
    winrt::check_hresult(utils->CreateReflection(&buffer, IID_PPV_ARGS(&reflection)));
    D3D12_SHADER_DESC description{};
    winrt::check_hresult(reflection->GetDesc(&description));
    std::vector<StageOutput> outputs;
    for (u32 i = 0; i < description.OutputParameters; ++i) {
        D3D12_SIGNATURE_PARAMETER_DESC parameter{};
        winrt::check_hresult(reflection->GetOutputParameterDesc(i, &parameter));
        std::string semantic = parameter.SemanticName;
        switch (parameter.SystemValueType) {
        case D3D_NAME_POSITION:
            semantic = "SV_Position";
            break;
        case D3D_NAME_CLIP_DISTANCE:
            semantic = "SV_ClipDistance";
            break;
        case D3D_NAME_CULL_DISTANCE:
            semantic = "SV_CullDistance";
            break;
        case D3D_NAME_RENDER_TARGET_ARRAY_INDEX:
            semantic = "SV_RenderTargetArrayIndex";
            break;
        case D3D_NAME_VIEWPORT_ARRAY_INDEX:
            semantic = "SV_ViewportArrayIndex";
            break;
        case D3D_NAME_PRIMITIVE_ID:
            semantic = "SV_PrimitiveID";
            break;
        default:
            break;
        }
        outputs.push_back({semantic, parameter.SemanticIndex,
                           u32(std::popcount(u32(parameter.Mask))),
                           parameter.ComponentType != D3D_REGISTER_COMPONENT_FLOAT32,
                           parameter.ComponentType == D3D_REGISTER_COMPONENT_SINT32});
    }
    return outputs;
}
std::string ShaderCache::OutputInterface(const ShaderBinary& binary) {
    std::string result = "struct SPIRV_Cross_Output {\n";
    const auto outputs = StageOutputs(binary);
    for (u32 i = 0; i < outputs.size(); ++i) {
        const auto& output = outputs[i];
        result += std::string(output.integer ? output.signed_integer ? "int" : "uint" : "float");
        if (output.components > 1)
            result += std::to_string(output.components);
        result += " p" + std::to_string(i) + " : " +
                  (output.semantic.starts_with("SV_") && !output.index
                       ? output.semantic
                       : output.semantic + std::to_string(output.index)) +
                  ";\n";
    }
    return result + "};\n";
}
std::shared_ptr<const ShaderBinary> ShaderCache::CompileHelperHlsl(const std::string& hlsl,
                                                                   Shader::SwStage stage) {
    std::vector<u32> key{UINT32_MAX - 5, u32(stage), u32(shader_model)};
    for (const unsigned char character : hlsl)
        key.push_back(character);
    if (const auto found = cache.find(key); found != cache.end())
        return found->second;
    auto result = std::make_shared<ShaderBinary>();
    result->hlsl = hlsl;
    CompileHlsl(*result, stage);
    Remember(std::move(key), result);
    return result;
}
void ShaderCache::CompileHlsl(ShaderBinary& result, Shader::SwStage stage) {
    if (stage == Shader::SwStage::Geometry)
        RemapGeometryPrimitiveInput(result.hlsl);
    std::vector<std::wstring> arguments;
    if (result.hlsl.find("#define SHAD_FP32_DENORM_PRESERVE 1") != std::string::npos)
        arguments = {L"-denorm", L"preserve"};
    else if (result.hlsl.find("#define SHAD_FP32_DENORM_FLUSH 1") != std::string::npos)
        arguments = {L"-denorm", L"ftz"};
    result.dxil = CompileHlslRaw(result.hlsl, Target(stage, shader_model), arguments)->dxil;
}

std::shared_ptr<const ShaderBinary> ShaderCache::CompileHlslRaw(
    const std::string& hlsl, std::wstring_view target, std::span<const std::wstring> defines) {
    std::wstring selected_target{target};
    if (selected_target.size() == 6 && selected_target[2] == L'_' && selected_target[3] == L'6' &&
        selected_target[4] == L'_' &&
        selected_target[5] > (shader_model >= D3D_SHADER_MODEL_6_6 ? L'6' : L'4'))
        selected_target[5] = shader_model >= D3D_SHADER_MODEL_6_6 ? L'6' : L'4';
    target = selected_target;
    std::vector<const wchar_t*> arguments{L"-E",
                                          L"main",
                                          L"-T",
                                          target.data(),
                                          L"-HV",
                                          L"2021",
                                          L"-Gis",
                                          L"-O3",
                                          L"-validator-version",
                                          shader_model >= D3D_SHADER_MODEL_6_6 ? L"1.6" : L"1.4"};
    for (const auto& define : defines)
        arguments.push_back(define.c_str());
    std::vector<u8> key{1};
    const auto append = [&](const void* data, size_t size) {
        const u64 length = size;
        const auto* header = reinterpret_cast<const u8*>(&length);
        key.insert(key.end(), header, header + sizeof(length));
        const auto* first = static_cast<const u8*>(data);
        key.insert(key.end(), first, first + size);
    };
    append(hlsl.data(), hlsl.size());
    for (const auto* argument : arguments)
        append(argument, std::wcslen(argument) * sizeof(wchar_t));
    if (auto bytes = persistent.Load(key); !bytes.empty()) {
        if (ValidDxil(bytes)) {
            auto result = std::make_shared<ShaderBinary>();
            result->dxil = std::move(bytes);
            return result;
        }
        persistent.Remove(key);
    }
    const DxcBuffer source{hlsl.data(), hlsl.size(), DXC_CP_UTF8};
    Microsoft::WRL::ComPtr<IDxcResult> compiled;
    ProfileSample(shader_profile, "dxc_compile", 16, [&] {
        winrt::check_hresult(compiler->Compile(&source, arguments.data(),
                                               static_cast<u32>(arguments.size()), nullptr,
                                               IID_PPV_ARGS(&compiled)));
    });
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
    persistent.Store(key, result->dxil);
    return result;
}
} // namespace D3D12
