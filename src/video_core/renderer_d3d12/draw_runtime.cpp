#include "video_core/renderer_d3d12/command_runtime.h"

#include <bit>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <winrt/base.h>
#include "d3d12_pipeline_cache.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/frontend/tessellation.h"
#include "video_core/renderer_d3d12/primitive_conversion.h"
#include "video_core/renderer_d3d12/profile.h"
#include "video_core/renderer_d3d12/resource_bindings.h"
#include "video_core/renderer_d3d12/temporal_jitter.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"

namespace D3D12 {
namespace {
Profile profile;
std::vector<u8> PipelineKey(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, ID3DBlob* root) {
    std::vector<u8> key{1};
    const auto integer = [&](u64 value) {
        for (u32 i = 0; i < 8; ++i)
            key.push_back(static_cast<u8>(value >> (i * 8)));
    };
    const auto bytes = [&](const void* data, size_t size) {
        integer(size);
        if (!size)
            return;
        const auto* begin = static_cast<const u8*>(data);
        key.insert(key.end(), begin, begin + size);
    };
    const auto real = [&](float value) { integer(std::bit_cast<u32>(value)); };
    bytes(root->GetBufferPointer(), root->GetBufferSize());
    bytes(desc.VS.pShaderBytecode, desc.VS.BytecodeLength);
    bytes(desc.PS.pShaderBytecode, desc.PS.BytecodeLength);
    bytes(desc.GS.pShaderBytecode, desc.GS.BytecodeLength);
    bytes(desc.HS.pShaderBytecode, desc.HS.BytecodeLength);
    bytes(desc.DS.pShaderBytecode, desc.DS.BytecodeLength);
    integer(desc.BlendState.AlphaToCoverageEnable);
    integer(desc.BlendState.IndependentBlendEnable);
    for (const auto& blend : desc.BlendState.RenderTarget) {
        integer(blend.BlendEnable);
        integer(blend.LogicOpEnable);
        integer(blend.SrcBlend);
        integer(blend.DestBlend);
        integer(blend.BlendOp);
        integer(blend.SrcBlendAlpha);
        integer(blend.DestBlendAlpha);
        integer(blend.BlendOpAlpha);
        integer(blend.LogicOp);
        integer(blend.RenderTargetWriteMask);
    }
    const auto& raster = desc.RasterizerState;
    integer(raster.FillMode);
    integer(raster.CullMode);
    integer(raster.FrontCounterClockwise);
    integer(raster.DepthBias);
    real(raster.DepthBiasClamp);
    real(raster.SlopeScaledDepthBias);
    integer(raster.DepthClipEnable);
    integer(raster.MultisampleEnable);
    integer(raster.AntialiasedLineEnable);
    integer(raster.ForcedSampleCount);
    integer(raster.ConservativeRaster);
    const auto& depth = desc.DepthStencilState;
    integer(depth.DepthEnable);
    integer(depth.DepthWriteMask);
    integer(depth.DepthFunc);
    integer(depth.StencilEnable);
    integer(depth.StencilReadMask);
    integer(depth.StencilWriteMask);
    for (const auto& face : {depth.FrontFace, depth.BackFace}) {
        integer(face.StencilFailOp);
        integer(face.StencilDepthFailOp);
        integer(face.StencilPassOp);
        integer(face.StencilFunc);
    }
    integer(desc.InputLayout.NumElements);
    for (u32 i = 0; i < desc.InputLayout.NumElements; ++i) {
        const auto& element = desc.InputLayout.pInputElementDescs[i];
        bytes(element.SemanticName, std::strlen(element.SemanticName));
        integer(element.SemanticIndex);
        integer(element.Format);
        integer(element.InputSlot);
        integer(element.AlignedByteOffset);
        integer(element.InputSlotClass);
        integer(element.InstanceDataStepRate);
    }
    integer(desc.SampleMask);
    integer(desc.IBStripCutValue);
    integer(desc.PrimitiveTopologyType);
    integer(desc.NumRenderTargets);
    for (const auto format : desc.RTVFormats)
        integer(format);
    integer(desc.DSVFormat);
    integer(desc.SampleDesc.Count);
    integer(desc.SampleDesc.Quality);
    integer(desc.NodeMask);
    integer(desc.Flags);
    return key;
}
} // namespace
u64 CommandRuntime::GsInvocations(D3D_PRIMITIVE_TOPOLOGY topology, u64 count) {
    if (!count)
        return 0;
    switch (topology) {
    case D3D_PRIMITIVE_TOPOLOGY_POINTLIST:
        return count;
    case D3D_PRIMITIVE_TOPOLOGY_LINELIST:
        return count / 2;
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP:
        return count - 1;
    case D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ:
        return count / 4;
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ:
        return count > 3 ? count - 3 : 0;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST:
        return count / 3;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
        return count > 2 ? count - 2 : 0;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ:
        return count / 6;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ:
        return count > 3 ? (count - 2) / 2 : 0;
    default:
        throw std::runtime_error("Unsupported D3D12 topology for geometry invocation count");
    }
}
void CommandRuntime::Draw(const DrawRequest& request) {
    ProfileSample(profile, "runtime_draw", 128, [&] { DrawImpl(request); });
}
void CommandRuntime::DrawImpl(const DrawRequest& request) {
    std::scoped_lock lock{mutex};
    strmout_filled = 0;
    if (failed)
        throw std::runtime_error(
            "D3D12 command runtime requires recreation after recording failure");
    if (!request.count || !request.instances)
        return;
    if (!std::has_single_bit(request.samples) || request.samples > 16 ||
        request.colors.size() > 8 || request.color_slots.size() != request.colors.size() ||
        request.color_base_slice.size() != request.colors.size() ||
        ((request.depth.DepthEnable || request.depth.StencilEnable) && !request.depth_image) ||
        (!request.depth.DepthEnable && !request.depth.StencilEnable && request.depth_image) ||
        request.vertex_runtime.sw_stage != Shader::SwStage::Vertex ||
        request.fragment_runtime.sw_stage != Shader::SwStage::Fragment ||
        (request.geometry && request.geometry_runtime.sw_stage != Shader::SwStage::Geometry) ||
        (!request.converted_indices.empty() && request.count != request.converted_indices.size()) ||
        request.hull.has_value() != request.domain.has_value() ||
        (request.depth_bounds && request.fragment.code.empty()))
        throw std::runtime_error("Invalid or unsupported D3D12 guest graphics state");
    Shader::Backend::Bindings next_bindings{};
    auto vertex_runtime = request.vertex_runtime;
    auto domain_runtime = request.domain_runtime;
    std::optional<CompiledShader> hull, domain;
    std::vector<CompiledShader*> stages;
    if (request.hull) {
        if (request.hull_runtime.sw_stage != Shader::SwStage::TessellationControl ||
            domain_runtime.sw_stage != Shader::SwStage::TessellationEval ||
            vertex_runtime.hw_stage != Shader::HwStage::Local)
            throw std::runtime_error("Invalid native tessellation stage configuration");
        TessellationLink link{domain_runtime.sw.tes.tess_type,
                              domain_runtime.sw.tes.tess_partitioning,
                              domain_runtime.sw.tes.tess_topology,
                              request.hull_runtime.sw.tcs.num_input_control_points, 0};
        auto prepared_hull = shaders.Prepare(*request.hull, request.hull_runtime, next_bindings);
        next_bindings = prepared_hull.shader.end_bindings;
        Shader::TessellationDataConstantBuffer constants{};
        prepared_hull.shader.info.ReadTessConstantBuffer(constants);
        vertex_runtime.hw.ls.ls_stride = constants.ls_stride;
        domain_runtime.InitFromTessConstants(constants);
        link.output_control_points = prepared_hull.shader.runtime.sw.tcs.NumOutputControlPoints();
        auto prepared_domain = shaders.Prepare(*request.domain, domain_runtime, next_bindings);
        hull.emplace(
            shaders.Finalize(prepared_hull, false, false, false, &link, prepared_domain.words));
        domain.emplace(shaders.Finalize(prepared_domain, !request.geometry && request.flip_y, false,
                                        false, &link, prepared_hull.words, false,
                                        !request.geometry && request.flip_x));
        next_bindings = domain->end_bindings;
        stages.push_back(&*hull);
        stages.push_back(&*domain);
    }
    auto vertex = ProfileSample(profile, "pso_compile", 128, [&] {
        return shaders.Compile(request.vertex, vertex_runtime, next_bindings,
                               !request.geometry && !request.domain && request.flip_y, false, false,
                               nullptr, false,
                               !request.geometry && !request.domain && request.flip_x);
    });
    std::array<DXGI_FORMAT, 8> logic_formats{};
    std::array<u32, 8> logic_masks{};
    for (u32 bound = 0; bound < request.colors.size(); ++bound) {
        const auto slot = request.color_slots[bound];
        if (slot >= 8)
            throw std::runtime_error("Invalid logic operation target slot");
        const auto format = ConvertFormat(request.colors[bound].pixel_format);
        if (request.blend.RenderTarget[slot].LogicOpEnable) {
            logic_formats[slot] = format;
            logic_masks[slot] = request.blend.RenderTarget[slot].RenderTargetWriteMask;
        }
    }
    std::optional<CompiledShader> fragment;
    std::optional<CompiledShader> geometry;
    std::shared_ptr<const ShaderBinary> rectangle;
    std::shared_ptr<const ShaderBinary> point_fill;
    std::shared_ptr<const ShaderBinary> line_expand;
    stages.push_back(&vertex);
    auto end_bindings = vertex.end_bindings;
    if (request.rectangles) {
        if (request.geometry)
            throw std::runtime_error("Guest geometry cannot precede rectangle expansion");
        rectangle = shaders.CompileRectangle(*vertex.binary, request.fragment_runtime.hw.fs);
    }
    if (request.point_fill) {
        if (request.geometry)
            throw std::runtime_error("Guest geometry cannot precede point fill expansion");
        point_fill = shaders.CompilePointFill(*vertex.binary, request.point_keep_front,
                                              request.rasterizer.FrontCounterClockwise != 0);
    }
    if (request.line_expand) {
        if (request.geometry)
            throw std::runtime_error("Guest geometry cannot precede line expansion");
        const auto& source_binary = (domain ? domain->binary : vertex.binary);
        const bool adjacency = request.topology == D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ ||
                               request.topology == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ ||
                               request.topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ ||
                               request.topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ;
        const auto input_topology = [&]() {
            if (request.topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH)
                return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
            return request.topology_type;
        }();
        line_expand = shaders.CompileLineExpand(
            *source_binary, request.fragment_runtime.hw.fs, request.line_keep_front,
            request.rasterizer.FrontCounterClockwise != 0, input_topology, adjacency,
            request.line_width, request.line_pattern, request.line_pattern_repeat,
            request.line_pattern_msb, request.line_pattern_scale, request.line_viewport_dims);
    }
    if (request.geometry) {
        geometry.emplace(shaders.Compile(*request.geometry, request.geometry_runtime, end_bindings,
                                         request.flip_y, false, false, nullptr, false,
                                         request.flip_x));
        end_bindings = geometry->end_bindings;
        stages.push_back(&*geometry);
        const bool strmout_active = std::any_of(request.strmout.begin(), request.strmout.end(),
                                                [](const auto& slot) { return slot.address != 0; });
        if (strmout_active) {
            if (!geometry->info.gs_emit_vertices)
                throw std::runtime_error("D3D12 stream-out capture requires vertex emissions");
            const u64 partitions = GsInvocations(request.topology, request.count);
            if (!partitions)
                throw std::runtime_error("Guest stream-out draw has no geometry partitions");
            const auto& gs_info = request.geometry_runtime.hw.gs;
            const u64 written =
                partitions * u64(request.instances) * u64(gs_info.num_invocations) *
                std::min(u64(geometry->info.gs_emit_vertices), u64(gs_info.output_vertices));
            strmout_filled = u32(std::min<u64>(written, request.strmout[0].size /
                                                            (u64(gs_info.strmout_stride) * 4)));
        }
    }
    if (!request.fragment.code.empty()) {
        fragment.emplace(shaders.Compile(request.fragment, request.fragment_runtime, end_bindings,
                                         false, request.samples > 1, request.sample_shading,
                                         nullptr, request.depth_bounds));
        if (std::any_of(logic_formats.begin(), logic_formats.end(),
                        [](auto format) { return format != DXGI_FORMAT_UNKNOWN; }))
            fragment->binary = shaders.CompileLogicOp(*fragment->binary, logic_formats, logic_masks);
        stages.push_back(&*fragment);
    } else if (!request.colors.empty())
        throw std::runtime_error("D3D12 color rendering requires a fragment shader");
    if (request.depth_gated) {
        const auto writes_memory = [](const CompiledShader& shader) {
            const auto& info = shader.info;
            return info.uses_dma || info.has_storage_images ||
                   info.uses_buffer_atomic_float_min_max || info.uses_image_atomic_float_min_max ||
                   std::any_of(info.buffers.begin(), info.buffers.end(),
                               [](const auto& buffer) {
                                   // Stream-out capture only writes in the final pass; the
                                   // depth-fail pass binds a null UAV for it.
                                   return buffer.is_written &&
                                          buffer.buffer_type != Shader::BufferType::Strmout;
                               }) ||
                   std::any_of(info.images.begin(), info.images.end(), [](const auto& image) {
                       return image.is_written || image.is_atomic;
                   });
        };
        if (std::any_of(stages.begin(), stages.end(),
                        [&](const CompiledShader* shader) { return writes_memory(*shader); }))
            throw std::runtime_error(
                "Guest depth-result color write gating requires side-effect-free shaders");
    }
    ResourceBindings bindings{device, resources, images, stages, request.strmout};
    Shader::Gcn::FetchShaderData fetch;
    Shader::Gcn::ParseFetchShader(vertex.info, fetch);
    std::vector<AmdGpu::Buffer> vertex_sharps;
    auto access_ranges = bindings.AccessRanges(request.border_colors);
    for (const auto& attribute : fetch.attributes) {
        const auto sharp = attribute.GetSharp(vertex.info);
        vertex_sharps.push_back(sharp);
        access_ranges.push_back(
            {sharp.base_address, u64(sharp.num_records) * std::max<u32>(1, sharp.stride)});
    }
    if (request.index_address)
        access_ranges.push_back(
            {request.index_address,
             u64(request.count) * (request.index_format == DXGI_FORMAT_R16_UINT ? 2 : 4)});
    for (const auto& color : request.colors)
        access_ranges.push_back({color.guest_address, color.guest_size});
    if (request.depth_image) {
        const auto& depth = *request.depth_image;
        access_ranges.push_back({depth.guest_address, depth.guest_size});
        access_ranges.push_back({depth.stencil_addr, depth.stencil_size});
        access_ranges.push_back({request.depth_read_address, depth.guest_size});
        access_ranges.push_back({request.stencil_read_address, depth.stencil_size});
    }
    const auto memory_access = acquire_memory ? acquire_memory(access_ranges) : nullptr;
    resources.BeginBatch();
    images.BeginBatch();

    std::array<ImageCache::Image*, 8> colors{};
    u32 num_render_targets = 0;
    VAddr jitter_address{};
    for (u32 bound = 0; bound < request.colors.size(); ++bound) {
        const auto& info = request.colors[bound];
        const u32 slot = request.color_slots[bound];
        if (slot >= 8 || bound >= request.color_base_slice.size())
            throw std::runtime_error("Invalid D3D12 guest color slot mapping");
        if (info.num_samples != request.samples)
            throw std::runtime_error(
                "Guest raster and color sample counts require unsupported EQAA");
        bindings.ValidateTargetRange(info.guest_address, info.guest_size);
        colors[slot] = &images.GetImage(
            info, ConvertFormat(info.pixel_format), D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
            request.color_base_slice[bound], 0, logic_formats[slot] != DXGI_FORMAT_UNKNOWN,
            logic_masks[slot]);
        num_render_targets = std::max(num_render_targets, slot + 1);
        if (!jitter_address) {
            jitter_address = info.guest_address;
        }
    }
    ImageCache::Image* depth{};
    if (request.depth_image) {
        if (request.depth_image->num_samples != request.samples)
            throw std::runtime_error("Guest raster and depth sample counts do not match");
        if (request.depth_image->guest_address) {
            bindings.ValidateTargetRange(request.depth_image->guest_address,
                                         request.depth_image->guest_size);
        }
        if (request.depth_read_address) {
            bindings.ValidateTargetRange(request.depth_read_address,
                                         request.depth_image->guest_size);
        }
        if (request.depth_image->stencil_addr) {
            bindings.ValidateTargetRange(request.depth_image->stencil_addr,
                                         request.depth_image->stencil_size);
        }
        if (request.stencil_read_address) {
            bindings.ValidateTargetRange(request.stencil_read_address,
                                         request.depth_image->stencil_size);
        }
        depth =
            &images.GetImage(*request.depth_image, ConvertFormat(request.depth_image->pixel_format),
                             D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, request.depth_base_layer);
        if (request.depth.StencilEnable && !depth->has_stencil) {
            throw std::runtime_error("D3D12 stencil test requires stencil plane");
        }
    }
    D3D12ResourceAllocator transient;
    transient.Initialize(device.Device(), 64ULL * 1024 * 1024);
    std::vector<D3D12Resource> input_resources;
    const auto upload_contents = [&](std::span<const u8> contents) -> ID3D12Resource* {
        const u64 size = contents.size();
        if (!size || size > 16ULL * 1024 * 1024)
            throw std::runtime_error("D3D12 input buffer exceeds upload budget");
        D3D12Resource resource;
        winrt::check_hresult(transient.CreateBuffer(size, D3D12_HEAP_TYPE_UPLOAD,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, resource));
        void* mapped;
        const D3D12_RANGE empty{0, 0};
        winrt::check_hresult(resource->Map(0, &empty, &mapped));
        std::memcpy(mapped, contents.data(), contents.size());
        const D3D12_RANGE written{0, static_cast<SIZE_T>(size)};
        resource->Unmap(0, &written);
        auto* output = resource.Get();
        input_resources.push_back(std::move(resource));
        return output;
    };
    const auto upload_input = [&](VAddr address, u64 size) -> ID3D12Resource* {
        if (!size || size > 16ULL * 1024 * 1024 || address > UINT64_MAX - size)
            throw std::runtime_error("Invalid D3D12 guest input buffer");
        bindings.ValidateInputRange(address, size);
        std::vector<u8> contents(size);
        read(address, contents);
        return upload_contents(contents);
    };
    if (fetch.attributes.size() > D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT)
        throw std::runtime_error("Guest fetch shader exceeds D3D12 input slots");
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    std::vector<D3D12_VERTEX_BUFFER_VIEW> vertices;
    for (u32 index = 0; index < fetch.attributes.size(); ++index) {
        const auto& attribute = fetch.attributes[index];
        const auto sharp = vertex_sharps[index];
        const u64 size = u64(sharp.num_records) * std::max<u32>(1, sharp.stride);
        auto* buffer = upload_input(sharp.base_address, size);
        u32 step{};
        switch (attribute.GetStepRate()) {
        case Shader::Gcn::VertexAttribute::None:
            break;
        case Shader::Gcn::VertexAttribute::Plain:
            step = 1;
            break;
        case Shader::Gcn::VertexAttribute::OverStepRate0:
            step = request.vertex_runtime.sw.vs.step_rate_0;
            break;
        case Shader::Gcn::VertexAttribute::OverStepRate1:
            step = request.vertex_runtime.sw.vs.step_rate_1;
            break;
        }
        if (attribute.GetStepRate() != Shader::Gcn::VertexAttribute::None && !step)
            throw std::runtime_error(
                "Zero guest instance step rate requires shader input emulation");
        elements.push_back({"TEXCOORD", index,
                            ConvertFormat(Vulkan::LiverpoolToVK::SurfaceFormat(
                                sharp.GetDataFmt(), sharp.GetNumberFmt())),
                            index, 0,
                            step ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                                 : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
                            step});
        vertices.push_back({buffer->GetGPUVirtualAddress(), static_cast<UINT>(size), sharp.stride});
    }
    D3D12_INDEX_BUFFER_VIEW index_view{};
    const bool indexed = request.index_address || !request.converted_indices.empty();
    if (!request.converted_indices.empty()) {
        const auto size = request.converted_indices.size() * sizeof(u32);
        auto* buffer =
            upload_contents({reinterpret_cast<const u8*>(request.converted_indices.data()), size});
        index_view = {buffer->GetGPUVirtualAddress(), static_cast<UINT>(size),
                      DXGI_FORMAT_R32_UINT};
    } else if (request.index_address) {
        if (request.index_format != DXGI_FORMAT_R16_UINT &&
            request.index_format != DXGI_FORMAT_R32_UINT)
            throw std::runtime_error("Invalid guest index format");
        const u64 size =
            u64(request.count) * (request.index_format == DXGI_FORMAT_R16_UINT ? 2 : 4);
        if (!size || size > 16ULL * 1024 * 1024 || request.index_address > UINT64_MAX - size)
            throw std::runtime_error("Invalid D3D12 guest input buffer");
        bindings.ValidateInputRange(request.index_address, size);
        std::vector<u8> contents(size);
        read(request.index_address, contents);
        SwapIndexBytes(contents, request.index_swap);
        auto* buffer = upload_contents(contents);
        index_view = {buffer->GetGPUVirtualAddress(), static_cast<UINT>(size),
                      request.index_format};
    }
    std::vector<D3D12_ROOT_PARAMETER> parameters(2);
    parameters[0].ParameterType = parameters[1].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 20};
    parameters[1].Constants = {1, 0, 4};
    if (request.samples > 1) {
        D3D12_ROOT_PARAMETER masks{};
        masks.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        masks.Constants = {3, 0, 4};
        parameters.push_back(masks);
    }
    u32 bounds_slot = UINT32_MAX;
    if (request.depth_bounds) {
        D3D12_ROOT_PARAMETER bounds{};
        bounds.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        bounds.Constants = {4, 0, 2};
        bounds_slot = u32(parameters.size());
        parameters.push_back(bounds);
    }
    const auto add_table = [&](const auto& ranges) {
        const u32 slot = parameters.size();
        if (!ranges.empty()) {
            D3D12_ROOT_PARAMETER parameter{};
            parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameter.DescriptorTable = {static_cast<UINT>(ranges.size()), ranges.data()};
            parameters.push_back(parameter);
        }
        return slot;
    };
    const u32 resource_slot = add_table(bindings.ResourceRanges());
    const u32 sampler_slot = add_table(bindings.SamplerRanges());
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = parameters.size();
    root_desc.pParameters = parameters.data();
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    Microsoft::WRL::ComPtr<ID3DBlob> blob, errors;
    winrt::check_hresult(
        D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.VS = {vertex.binary->dxil.data(), vertex.binary->dxil.size()};
    if (hull) {
        desc.HS = {hull->binary->dxil.data(), hull->binary->dxil.size()};
        desc.DS = {domain->binary->dxil.data(), domain->binary->dxil.size()};
    }
    if (geometry)
        desc.GS = {geometry->binary->dxil.data(), geometry->binary->dxil.size()};
    else if (rectangle)
        desc.GS = {rectangle->dxil.data(), rectangle->dxil.size()};
    else if (point_fill)
        desc.GS = {point_fill->dxil.data(), point_fill->dxil.size()};
    else if (line_expand)
        desc.GS = {line_expand->dxil.data(), line_expand->dxil.size()};
    if (fragment)
        desc.PS = {fragment->binary->dxil.data(), fragment->binary->dxil.size()};
    else if (request.samples > 1)
        desc.PS = {images.MaskShader().data(), images.MaskShader().size()};
    desc.BlendState = request.blend;
    const auto logic_target = std::ranges::find_if(request.colors, [&](const auto& info) {
        const auto bound = &info - request.colors.data();
        return request.blend.RenderTarget[request.color_slots[bound]].LogicOpEnable != FALSE;
    });
    if (logic_target != request.colors.end()) {
        const auto bound = std::distance(request.colors.begin(), logic_target);
        const auto state = request.blend.RenderTarget[request.color_slots[bound]];
        for (const auto slot : request.color_slots)
            if (!request.blend.RenderTarget[slot].LogicOpEnable ||
                request.blend.RenderTarget[slot].LogicOp != state.LogicOp)
                throw std::runtime_error(
                    "Guest logic operation must be shared by all color targets");
        desc.BlendState.IndependentBlendEnable = FALSE;
        for (auto& target : desc.BlendState.RenderTarget) {
            target = state;
            target.BlendEnable = FALSE;
            target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        }
    }
    desc.BlendState.AlphaToCoverageEnable = request.alpha_to_coverage;
    desc.RasterizerState = request.rasterizer;
    desc.DepthStencilState = request.depth;
    desc.SampleMask = UINT_MAX;
    desc.SampleDesc.Count = request.samples;
    desc.RasterizerState.MultisampleEnable = request.samples > 1;
    desc.InputLayout = {elements.data(), static_cast<UINT>(elements.size())};
    desc.PrimitiveTopologyType = request.topology_type;
    desc.IBStripCutValue = request.strip_cut;
    desc.NumRenderTargets = num_render_targets;
    for (u32 i = 0; i < num_render_targets; ++i) {
        desc.RTVFormats[i] =
            colors[i] ? (colors[i]->logic_op ? DXGI_FORMAT_R32G32B32A32_UINT
                                             : ConvertFormat(colors[i]->info.pixel_format))
                      : DXGI_FORMAT_UNKNOWN;
    }
    desc.DSVFormat =
        depth ? (depth->has_stencil ? depth->dsv_format : ConvertFormat(depth->info.pixel_format))
              : DXGI_FORMAT_UNKNOWN;
    if (const char* dump = std::getenv("GAME_SHADER_DUMP"); dump) {
        std::ofstream vs{std::string(dump) + "\\draw-vs.hlsl", std::ios::binary};
        vs << vertex.binary->hlsl;
        if (fragment) {
            std::ofstream ps{std::string(dump) + "\\draw-ps.hlsl", std::ios::binary};
            ps << fragment->binary->hlsl;
        }
        if (geometry) {
            std::ofstream gs{std::string(dump) + "\\draw-gs.hlsl", std::ios::binary};
            gs << geometry->binary->hlsl;
        }
        if (hull) {
            std::ofstream hs{std::string(dump) + "\\draw-hull.hlsl", std::ios::binary};
            hs << hull->binary->hlsl;
            std::ofstream ds{std::string(dump) + "\\draw-domain.hlsl", std::ios::binary};
            ds << domain->binary->hlsl;
        }
    }
    auto& cached =
        GetPipeline(PipelineKey(desc, blob.Get()), blob.Get(), [&](auto* root, auto** state) {
            desc.pRootSignature = root;
            const HRESULT created = ProfileSample(profile, "pso_create", 128, [&] {
                return device.Device()->CreateGraphicsPipelineState(
                    &desc, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(state));
            });
            return created;
        });
    auto& pipeline = cached.state;
    auto& root = cached.root;
    D3D12DescriptorArena rtv, dsv;
    rtv.Initialize(device.Device(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 8, false);
    dsv.Initialize(device.Device(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, false);
    static_cast<void>(rtv.Allocate(8));
    static_cast<void>(dsv.Allocate(1));
    winrt::check_hresult(allocator->Reset());
    winrt::check_hresult(commands->Reset(allocator.Get(), pipeline.Get()));
    RecordingGuard recording{failed};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 8> color_handles{};
    for (u32 i = 0; i < num_render_targets; ++i) {
        if (!colors[i]) {
            D3D12_RENDER_TARGET_VIEW_DESC view{};
            view.Format = DXGI_FORMAT_R32G32B32A32_UINT;
            view.ViewDimension = request.samples > 1 ? D3D12_RTV_DIMENSION_TEXTURE2DMS
                                                    : D3D12_RTV_DIMENSION_TEXTURE2D;
            color_handles[i] = rtv.Cpu(i);
            device.Device()->CreateRenderTargetView(nullptr, &view, color_handles[i]);
            continue;
        }
        colors[i]->sample_positions = request.sample_positions;
        ProfileSample(profile, "color_upload", 128,
                      [&] { images.Upload(*colors[i], commands.Get()); });
        images.Barrier(*colors[i], commands.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        colors[i]->written = true;
        color_handles[i] = rtv.Cpu(i);
        D3D12_RENDER_TARGET_VIEW_DESC view{};
        view.Format = desc.RTVFormats[i];
        if (request.samples > 1) {
            view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
            view.Texture2DMSArray = {0, colors[i]->native_layers};
        } else {
            view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            view.Texture2DArray = {0, 0, colors[i]->native_layers, 0};
        }
        device.Device()->CreateRenderTargetView(colors[i]->Native(), &view, color_handles[i]);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE depth_view{};
    if (depth) {
        depth->sample_positions = request.sample_positions;
        images.Upload(*depth, commands.Get(), request.depth_read_address,
                      request.stencil_read_address);
        images.Barrier(*depth, commands.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
        const bool clear_depth = request.clear_depth && depth->has_depth;
        const bool clear_stencil = request.clear_stencil && depth->has_stencil;
        if (clear_depth && (request.dsv_flags & D3D12_DSV_FLAG_READ_ONLY_DEPTH)) {
            throw std::runtime_error("D3D12 guest depth clear with read-only view");
        }
        if (clear_stencil && (request.dsv_flags & D3D12_DSV_FLAG_READ_ONLY_STENCIL)) {
            throw std::runtime_error("D3D12 guest stencil clear with read-only view");
        }
        depth->written = request.depth.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL || clear_depth;
        depth->stencil_written =
            (request.depth.StencilEnable && request.depth.StencilWriteMask != 0) || clear_stencil;
        depth_view = dsv.Cpu(0);
        D3D12_DEPTH_STENCIL_VIEW_DESC view{};
        view.Format = desc.DSVFormat;
        view.Flags = request.dsv_flags;
        if (request.samples > 1) {
            view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
            view.Texture2DMSArray = {0, depth->native_layers};
        } else {
            view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            view.Texture2DArray = {0, 0, depth->native_layers};
        }
        device.Device()->CreateDepthStencilView(depth->Native(), &view, depth_view);
        if (clear_depth || clear_stencil) {
            const D3D12_CLEAR_FLAGS flags =
                (clear_depth ? D3D12_CLEAR_FLAG_DEPTH : D3D12_CLEAR_FLAGS(0)) |
                (clear_stencil ? D3D12_CLEAR_FLAG_STENCIL : D3D12_CLEAR_FLAGS(0));
            commands->ClearDepthStencilView(depth_view, flags, request.depth_clear,
                                            static_cast<UINT8>(request.stencil_clear), 0, nullptr);
        }
    }
    Shader::PushData push = request.push;
    bindings.Upload(commands.Get(), push, request.clip_planes, request.border_colors, read, gds);
    commands->SetGraphicsRootSignature(root.Get());
    commands->SetPipelineState(pipeline.Get());
    SetSamplePositions(commands.Get(), request.samples, request.sample_positions);
    std::array<u32, 20> constants{};
    std::memcpy(constants.data(), &push, sizeof(push));
    commands->SetGraphicsRoot32BitConstants(0, constants.size(), constants.data(), 0);
    const std::array<u32, 4> base{
        static_cast<u32>(indexed ? request.base_vertex : request.first_vertex),
        request.first_instance, 0, 0};
    commands->SetGraphicsRoot32BitConstants(1, base.size(), base.data(), 0);
    if (request.samples > 1)
        commands->SetGraphicsRoot32BitConstants(2, request.sample_masks.size(),
                                                request.sample_masks.data(), 0);
    if (request.depth_bounds) {
        const std::array<u32, 2> bounds{std::bit_cast<u32>(request.bounds_min),
                                        std::bit_cast<u32>(request.bounds_max)};
        commands->SetGraphicsRoot32BitConstants(bounds_slot, bounds.size(), bounds.data(), 0);
    }
    bindings.Bind(commands.Get(), true, resource_slot, sampler_slot);
    commands->IASetPrimitiveTopology(request.topology);
    commands->IASetVertexBuffers(0, vertices.size(), vertices.data());
    std::array<D3D12_VIEWPORT, AmdGpu::NUM_VIEWPORTS> viewports = request.viewports;
    for (u32 index = 0; index < request.viewport_count; ++index)
        viewports[index] = NativeTemporalJitter().Apply(jitter_address, viewports[index]);
    commands->RSSetViewports(request.viewport_count, viewports.data());
    commands->RSSetScissorRects(request.viewport_count, request.scissors.data());
    commands->OMSetRenderTargets(num_render_targets,
                                 num_render_targets ? color_handles.data() : nullptr, FALSE,
                                 depth ? &depth_view : nullptr);
    commands->OMSetBlendFactor(request.blend_factor.data());
    commands->OMSetStencilRef(request.stencil_ref);
    if (indexed) {
        commands->IASetIndexBuffer(&index_view);
        commands->DrawIndexedInstanced(request.count, request.instances, 0, request.base_vertex,
                                       request.first_instance);
    } else
        commands->DrawInstanced(request.count, request.instances, request.first_vertex,
                                request.first_instance);
    resources.PrepareDownload(commands.Get());
    images.PrepareDownloads(commands.Get());
    winrt::check_hresult(commands->Close());
    if (profile.enabled) {
        const auto wait_start = std::chrono::steady_clock::now();
        device.Wait(device.Submit(commands.Get()));
        const auto waited = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - wait_start)
                                .count();
        if (waited > 3000)
            std::fprintf(stderr, "WAIT_SLOW %llu us\n", static_cast<unsigned long long>(waited));
    } else {
        device.Wait(device.Submit(commands.Get()));
    }
    resources.CommitDownloads(bool(memory_access));
    bindings.CommitGds(gds);
    images.CommitDownloads();
    recording.completed = true;
}
} // namespace D3D12
