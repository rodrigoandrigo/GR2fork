#include "video_core/renderer_d3d12/command_runtime.h"

#include <bit>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <winrt/base.h>
#include "d3d12_pipeline_cache.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/frontend/tessellation.h"
#include "video_core/renderer_d3d12/resource_bindings.h"
#include "video_core/renderer_d3d12/temporal_jitter.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"

namespace D3D12 {
namespace {
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
void CommandRuntime::Draw(const DrawRequest& request) {
    std::scoped_lock lock{mutex};
    if (failed)
        throw std::runtime_error(
            "D3D12 command runtime requires recreation after recording failure");
    if (!request.count || !request.instances)
        return;
    if (!std::has_single_bit(request.samples) || request.samples > 16 ||
        request.colors.size() > 8 || request.depth.StencilEnable ||
        (request.depth.DepthEnable && !request.depth_image) ||
        request.vertex_runtime.sw_stage != Shader::SwStage::Vertex ||
        request.fragment_runtime.sw_stage != Shader::SwStage::Fragment ||
        (request.geometry && request.geometry_runtime.sw_stage != Shader::SwStage::Geometry) ||
        (!request.converted_indices.empty() && request.count != request.converted_indices.size()) ||
        request.hull.has_value() != request.domain.has_value())
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
                                        false, &link, prepared_hull.words));
        next_bindings = domain->end_bindings;
        stages.push_back(&*hull);
        stages.push_back(&*domain);
    }
    auto vertex = shaders.Compile(request.vertex, vertex_runtime, next_bindings,
                                  !request.geometry && !request.domain && request.flip_y);
    std::optional<CompiledShader> fragment;
    std::optional<CompiledShader> geometry;
    std::shared_ptr<const ShaderBinary> rectangle;
    stages.push_back(&vertex);
    auto end_bindings = vertex.end_bindings;
    if (request.rectangles) {
        if (request.geometry)
            throw std::runtime_error("Guest geometry cannot precede rectangle expansion");
        rectangle = shaders.CompileRectangle(*vertex.binary, request.fragment_runtime.hw.fs);
    }
    if (request.geometry) {
        geometry.emplace(shaders.Compile(*request.geometry, request.geometry_runtime, end_bindings,
                                         request.flip_y));
        end_bindings = geometry->end_bindings;
        stages.push_back(&*geometry);
    }
    if (!request.fragment.code.empty()) {
        fragment.emplace(shaders.Compile(request.fragment, request.fragment_runtime, end_bindings,
                                         false, request.samples > 1, request.sample_shading));
        stages.push_back(&*fragment);
    } else if (!request.colors.empty())
        throw std::runtime_error("D3D12 color rendering requires a fragment shader");
    ResourceBindings bindings{device, resources, images, stages};
    resources.BeginBatch();
    images.BeginBatch();

    std::vector<ImageCache::Image*> colors;
    for (const auto& info : request.colors) {
        if (info.num_samples != request.samples)
            throw std::runtime_error(
                "Guest raster and color sample counts require unsupported EQAA");
        bindings.ValidateTargetRange(info.guest_address, info.guest_size);
        colors.push_back(&images.GetImage(info, ConvertFormat(info.pixel_format),
                                          D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET));
    }
    ImageCache::Image* depth{};
    if (request.depth_image) {
        if (request.depth_image->num_samples != request.samples)
            throw std::runtime_error("Guest raster and depth sample counts do not match");
        bindings.ValidateTargetRange(request.depth_image->guest_address,
                                     request.depth_image->guest_size);
        if (request.depth_read_address)
            bindings.ValidateTargetRange(request.depth_read_address,
                                         request.depth_image->guest_size);
        depth =
            &images.GetImage(*request.depth_image, ConvertFormat(request.depth_image->pixel_format),
                             D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, request.depth_base_layer);
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
    Shader::Gcn::FetchShaderData fetch;
    Shader::Gcn::ParseFetchShader(vertex.info, fetch);
    if (fetch.attributes.size() > D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT)
        throw std::runtime_error("Guest fetch shader exceeds D3D12 input slots");
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    std::vector<D3D12_VERTEX_BUFFER_VIEW> vertices;
    for (u32 index = 0; index < fetch.attributes.size(); ++index) {
        const auto& attribute = fetch.attributes[index];
        const auto sharp = attribute.GetSharp(vertex.info);
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
        auto* buffer = upload_contents(
            {reinterpret_cast<const u8*>(request.converted_indices.data()), size});
        index_view = {buffer->GetGPUVirtualAddress(), static_cast<UINT>(size), DXGI_FORMAT_R32_UINT};
    } else if (request.index_address) {
        if (request.index_format != DXGI_FORMAT_R16_UINT &&
            request.index_format != DXGI_FORMAT_R32_UINT)
            throw std::runtime_error("Invalid guest index format");
        const u64 size =
            u64(request.count) * (request.index_format == DXGI_FORMAT_R16_UINT ? 2 : 4);
        auto* buffer = upload_input(request.index_address, size);
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
    if (fragment)
        desc.PS = {fragment->binary->dxil.data(), fragment->binary->dxil.size()};
    else if (request.samples > 1)
        desc.PS = {images.MaskShader().data(), images.MaskShader().size()};
    desc.BlendState = request.blend;
    desc.RasterizerState = request.rasterizer;
    desc.DepthStencilState = request.depth;
    desc.SampleMask = UINT_MAX;
    desc.SampleDesc.Count = request.samples;
    desc.RasterizerState.MultisampleEnable = request.samples > 1;
    desc.InputLayout = {elements.data(), static_cast<UINT>(elements.size())};
    desc.PrimitiveTopologyType = request.topology_type;
    desc.IBStripCutValue = request.strip_cut;
    desc.NumRenderTargets = colors.size();
    for (u32 i = 0; i < colors.size(); ++i)
        desc.RTVFormats[i] = ConvertFormat(colors[i]->info.pixel_format);
    desc.DSVFormat = depth ? ConvertFormat(depth->info.pixel_format) : DXGI_FORMAT_UNKNOWN;
    auto& cached =
        GetPipeline(PipelineKey(desc, blob.Get()), blob.Get(), [&](auto* root, auto** state) {
            desc.pRootSignature = root;
            return device.Device()->CreateGraphicsPipelineState(
                &desc, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(state));
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
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 8> color_views{};
    for (u32 i = 0; i < colors.size(); ++i) {
        colors[i]->sample_positions = request.sample_positions;
        images.Upload(*colors[i], commands.Get());
        images.Barrier(*colors[i], commands.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        colors[i]->written = true;
        color_views[i] = rtv.Cpu(i);
        D3D12_RENDER_TARGET_VIEW_DESC view{};
        view.Format = desc.RTVFormats[i];
        if (request.samples > 1) {
            view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
            view.Texture2DMSArray = {0, colors[i]->native_layers};
        } else {
            view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            view.Texture2DArray = {0, 0, colors[i]->native_layers, 0};
        }
        device.Device()->CreateRenderTargetView(colors[i]->Native(), &view, color_views[i]);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE depth_view{};
    if (depth) {
        depth->sample_positions = request.sample_positions;
        images.Upload(*depth, commands.Get(), request.depth_read_address);
        images.Barrier(*depth, commands.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
        depth->written = request.depth.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL;
        depth_view = dsv.Cpu(0);
        D3D12_DEPTH_STENCIL_VIEW_DESC view{};
        view.Format = desc.DSVFormat;
        if (request.samples > 1) {
            view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
            view.Texture2DMSArray = {0, depth->native_layers};
        } else {
            view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            view.Texture2DArray = {0, 0, depth->native_layers};
        }
        device.Device()->CreateDepthStencilView(depth->Native(), &view, depth_view);
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
    bindings.Bind(commands.Get(), true, resource_slot, sampler_slot);
    commands->IASetPrimitiveTopology(request.topology);
    commands->IASetVertexBuffers(0, vertices.size(), vertices.data());
    const auto viewport = NativeTemporalJitter().Apply(
        request.colors.empty() ? 0 : request.colors.front().guest_address, request.viewport);
    commands->RSSetViewports(1, &viewport);
    commands->RSSetScissorRects(1, &request.scissor);
    commands->OMSetRenderTargets(colors.size(), color_views.data(), FALSE,
                                 depth ? &depth_view : nullptr);
    commands->OMSetBlendFactor(request.blend_factor.data());
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
    device.Wait(device.Submit(commands.Get()));
    resources.CommitDownloads();
    bindings.CommitGds(gds);
    images.CommitDownloads();
    recording.completed = true;
}
} // namespace D3D12
