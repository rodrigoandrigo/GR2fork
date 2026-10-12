#include "video_core/renderer_d3d12/command_runtime.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <winrt/base.h>
#include "d3d12_pipeline_cache.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"

namespace D3D12 {
void CommandRuntime::Draw(const DrawRequest& request) {
    std::scoped_lock lock{mutex};
    if (!request.count || !request.instances)
        return;
    if (request.colors.size() > 8 || request.depth.StencilEnable ||
        (request.depth.DepthEnable && !request.depth_image) ||
        request.vertex_runtime.sw_stage != Shader::SwStage::Vertex ||
        request.fragment_runtime.sw_stage != Shader::SwStage::Fragment)
        throw std::runtime_error("Invalid or unsupported D3D12 guest graphics state");
    auto vertex = shaders.Compile(request.vertex, request.vertex_runtime);
    auto fragment =
        shaders.Compile(request.fragment, request.fragment_runtime, vertex.end_bindings);
    struct Binding {
        CompiledShader* shader;
        ShaderBinding descriptor;
        u32 index;
    };
    std::vector<Binding> bindings;
    std::vector<D3D12_DESCRIPTOR_RANGE> ranges;
    std::vector<ResourceCache::Range> guest_ranges;
    for (auto* shader : {&vertex, &fragment})
        for (const auto& binding : shader->binary->bindings) {
            const u32 index = binding.binding - shader->start_bindings.unified;
            if (binding.kind != ResourceKind::Buffer || binding.count != 1 ||
                index >= shader->info.buffers.size())
                throw std::runtime_error("D3D12 draw image/sampler binding is not implemented");
            bindings.push_back({shader, binding, index});
            ranges.push_back({binding.type, 1, binding.binding, binding.space,
                              static_cast<UINT>(ranges.size())});
            ResourceCache::Range range{};
            const auto& resource = shader->info.buffers[index];
            if (resource.buffer_type == Shader::BufferType::Guest) {
                const auto sharp = resource.GetSharp(shader->info);
                range = {sharp.base_address,
                         u64(sharp.num_records) * std::max<u32>(1, sharp.stride)};
            } else if (resource.buffer_type != Shader::BufferType::Flatbuf &&
                       resource.buffer_type != Shader::BufferType::ClipPlanes)
                throw std::runtime_error(fmt::format("D3D12 draw special buffer {} is not implemented",
                                                     u32(resource.buffer_type)));
            guest_ranges.push_back(range);
        }
    resources.BeginBatch();
    images.BeginBatch();
    const auto views = resources.GetViews(guest_ranges);
    std::vector<ImageCache::Image*> colors;
    for (const auto& info : request.colors)
        colors.push_back(&images.GetImage(info, ConvertFormat(info.pixel_format),
                                          D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET));
    ImageCache::Image* depth{};
    if (request.depth_image)
        depth =
            &images.GetImage(*request.depth_image, ConvertFormat(request.depth_image->pixel_format),
                             D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    D3D12ResourceAllocator transient;
    transient.Initialize(device.Device(), 64ULL * 1024 * 1024);
    std::vector<D3D12Resource> input_resources;
    const auto upload_input = [&](VAddr address, u64 size) -> ID3D12Resource* {
        if (!size || size > 16ULL * 1024 * 1024 || address > UINT64_MAX - size)
            throw std::runtime_error("Invalid D3D12 guest input buffer");
        for (u32 i = 0; i < bindings.size(); ++i)
            if (bindings[i].shader->info.buffers[bindings[i].index].is_written &&
                guest_ranges[i].address && guest_ranges[i].address < address + size &&
                address < guest_ranges[i].address + guest_ranges[i].size)
                throw std::runtime_error(
                    "D3D12 writable shader/input buffer alias requires coherent fetching");
        std::vector<u8> contents(size);
        read(address, contents);
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
    if (request.index_address) {
        if (request.index_format != DXGI_FORMAT_R16_UINT &&
            request.index_format != DXGI_FORMAT_R32_UINT)
            throw std::runtime_error("Invalid guest index format");
        const u64 size =
            u64(request.count) * (request.index_format == DXGI_FORMAT_R16_UINT ? 2 : 4);
        auto* buffer = upload_input(request.index_address, size);
        index_view = {buffer->GetGPUVirtualAddress(), static_cast<UINT>(size),
                      request.index_format};
    }
    std::array<D3D12_ROOT_PARAMETER, 3> parameters{};
    parameters[0].ParameterType = parameters[1].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 20};
    parameters[1].Constants = {1, 0, 4};
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[2].DescriptorTable = {static_cast<UINT>(ranges.size()), ranges.data()};
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = ranges.empty() ? 2 : 3;
    root_desc.pParameters = parameters.data();
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    Microsoft::WRL::ComPtr<ID3DBlob> blob, errors;
    winrt::check_hresult(
        D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors));
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    winrt::check_hresult(device.Device()->CreateRootSignature(
        0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root.Get();
    desc.VS = {vertex.binary->dxil.data(), vertex.binary->dxil.size()};
    desc.PS = {fragment.binary->dxil.data(), fragment.binary->dxil.size()};
    desc.BlendState = request.blend;
    desc.RasterizerState = request.rasterizer;
    desc.DepthStencilState = request.depth;
    desc.SampleMask = UINT_MAX;
    desc.SampleDesc.Count = 1;
    desc.InputLayout = {elements.data(), static_cast<UINT>(elements.size())};
    desc.PrimitiveTopologyType = request.topology_type;
    desc.IBStripCutValue = request.strip_cut;
    desc.NumRenderTargets = colors.size();
    for (u32 i = 0; i < colors.size(); ++i)
        desc.RTVFormats[i] = colors[i]->native->GetDesc().Format;
    desc.DSVFormat = depth ? depth->native->GetDesc().Format : DXGI_FORMAT_UNKNOWN;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    winrt::check_hresult(
        device.Device()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)));
    D3D12DescriptorArena rtv, dsv, descriptors;
    rtv.Initialize(device.Device(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 8, false);
    dsv.Initialize(device.Device(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, false);
    descriptors.Initialize(device.Device(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                           std::max<u32>(1, bindings.size()), true);
    static_cast<void>(rtv.Allocate(8));
    static_cast<void>(dsv.Allocate(1));
    static_cast<void>(descriptors.Allocate(std::max<u32>(1, bindings.size())));
    winrt::check_hresult(allocator->Reset());
    winrt::check_hresult(commands->Reset(allocator.Get(), pipeline.Get()));
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 8> color_views{};
    for (u32 i = 0; i < colors.size(); ++i) {
        images.Upload(*colors[i], commands.Get());
        images.Barrier(*colors[i], commands.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        colors[i]->written = true;
        color_views[i] = rtv.Cpu(i);
        device.Device()->CreateRenderTargetView(colors[i]->native.Get(), nullptr, color_views[i]);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE depth_view{};
    if (depth) {
        images.Upload(*depth, commands.Get());
        images.Barrier(*depth, commands.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
        depth->written = request.depth.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL;
        depth_view = dsv.Cpu(0);
        device.Device()->CreateDepthStencilView(depth->native.Get(), nullptr, depth_view);
    }
    Shader::PushData push = request.push;
    std::vector<ResourceCache::Buffer*> uploaded;
    for (u32 i = 0; i < bindings.size(); ++i) {
        const auto& binding = bindings[i];
        const auto& resource = binding.shader->info.buffers[binding.index];
        auto view = views[i];
        if (resource.buffer_type == Shader::BufferType::Flatbuf) {
            const auto& info = binding.shader->info;
            const u64 bytes = (info.flattened_ud_buf.empty() ? Shader::NUM_USER_DATA_REGS
                                                             : info.flattened_ud_buf.size()) *
                              sizeof(u32);
            view = {&resources.GetHostBuffer(
                        0xffff000000000000ULL + (u64(info.sw_stage) << 32),
                        {reinterpret_cast<const u8*>(info.flat_ud), static_cast<size_t>(bytes)}),
                    0, bytes};
        } else if (resource.buffer_type == Shader::BufferType::ClipPlanes) {
            view = {&resources.GetHostBuffer(0xfffd000000000000ULL,
                {reinterpret_cast<const u8*>(request.clip_planes.data()), sizeof(request.clip_planes)}),
                0, sizeof(request.clip_planes)};
        } else
            push.AddOffset(binding.shader->start_bindings.buffer + binding.index, view.offset & 3);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R32_TYPELESS;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.FirstElement = view.offset / 4;
        uav.Buffer.NumElements = view.buffer ? (view.size + (view.offset & 3) + 3) / 4 : 1;
        uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device.Device()->CreateUnorderedAccessView(
            view.buffer ? view.buffer->native.Get() : nullptr, nullptr, &uav, descriptors.Cpu(i));
        if (view.buffer) {
            if (std::ranges::find(uploaded, view.buffer) == uploaded.end()) {
                resources.Upload(*view.buffer, commands.Get());
                uploaded.push_back(view.buffer);
            }
            view.buffer->written |= resource.is_written;
        }
    }
    commands->SetGraphicsRootSignature(root.Get());
    std::array<u32, 20> constants{};
    std::memcpy(constants.data(), &push, sizeof(push));
    commands->SetGraphicsRoot32BitConstants(0, constants.size(), constants.data(), 0);
    const std::array<u32, 4> base{
        static_cast<u32>(request.index_address ? request.base_vertex : request.first_vertex),
        request.first_instance, 0, 0};
    commands->SetGraphicsRoot32BitConstants(1, base.size(), base.data(), 0);
    ID3D12DescriptorHeap* heaps[]{descriptors.Heap()};
    commands->SetDescriptorHeaps(1, heaps);
    if (!bindings.empty())
        commands->SetGraphicsRootDescriptorTable(2, descriptors.Gpu(0));
    commands->IASetPrimitiveTopology(request.topology);
    commands->IASetVertexBuffers(0, vertices.size(), vertices.data());
    commands->RSSetViewports(1, &request.viewport);
    commands->RSSetScissorRects(1, &request.scissor);
    commands->OMSetRenderTargets(colors.size(), color_views.data(), FALSE,
                                 depth ? &depth_view : nullptr);
    commands->OMSetBlendFactor(request.blend_factor.data());
    if (request.index_address) {
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
    images.CommitDownloads();
}
} // namespace D3D12
