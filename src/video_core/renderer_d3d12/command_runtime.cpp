#include "video_core/renderer_d3d12/command_runtime.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <winrt/base.h>
#include "shader_recompiler/frontend/fetch_shader.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"

namespace D3D12 {
void CommandRuntime::Initialize(ResourceCache::Read read, ResourceCache::Write write) {
    device.Initialize();
    shaders.Initialize(device.Device());
    this->read = read;
    images.Initialize(device, read, write);
    resources.Initialize(device, std::move(read), std::move(write));
    device.CreateDirectCommands(allocator, commands);
    winrt::check_hresult(commands->Close());
}
void CommandRuntime::Dispatch(Shader::ShaderParams params, Shader::RuntimeInfo runtime,
                              std::array<u32, 3> groups) {
    std::scoped_lock lock{mutex};
    if (runtime.sw_stage != Shader::SwStage::Compute)
        throw std::runtime_error("D3D12 dispatch requires a compute shader");
    for (const auto count : groups) {
        if (!count)
            return;
        if (count > D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION)
            throw std::runtime_error("Guest dispatch exceeds D3D12 workgroup limits");
    }
    auto shader = shaders.Compile(params, runtime);
    std::vector<D3D12_DESCRIPTOR_RANGE> ranges;
    for (const auto& binding : shader.binary->bindings) {
        if (binding.kind != ResourceKind::Buffer || binding.count != 1 ||
            binding.binding >= shader.info.buffers.size())
            throw std::runtime_error("D3D12 compute resource requires an image or special binding");
        ranges.push_back(
            {binding.type, 1, binding.binding, binding.space, static_cast<UINT>(ranges.size())});
    }
    std::array<D3D12_ROOT_PARAMETER, 2> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 20};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {static_cast<UINT>(ranges.size()), ranges.data()};
    D3D12_ROOT_SIGNATURE_DESC root_description{};
    root_description.NumParameters = ranges.empty() ? 1 : 2;
    root_description.pParameters = parameters.data();
    Microsoft::WRL::ComPtr<ID3DBlob> root_blob, errors;
    winrt::check_hresult(D3D12SerializeRootSignature(
        &root_description, D3D_ROOT_SIGNATURE_VERSION_1, &root_blob, &errors));
    std::vector<u8> pipeline_key = shader.binary->dxil;
    const auto* root_bytes = static_cast<const u8*>(root_blob->GetBufferPointer());
    pipeline_key.insert(pipeline_key.end(), root_bytes, root_bytes + root_blob->GetBufferSize());
    if (pipeline_key.size() > 32 * 1024 * 1024)
        throw std::runtime_error("D3D12 compute pipeline exceeds cache budget");
    auto found = pipelines.find(pipeline_key);
    if (found == pipelines.end()) {
        device.WaitForGpu();
        while (!pipelines.empty() && (pipelines.size() >= 128 ||
                                      pipeline_bytes + pipeline_key.size() > 32 * 1024 * 1024)) {
            pipeline_bytes -= pipelines.begin()->first.size();
            pipelines.erase(pipelines.begin());
        }
        Pipeline created;
        winrt::check_hresult(device.Device()->CreateRootSignature(0, root_blob->GetBufferPointer(),
                                                                  root_blob->GetBufferSize(),
                                                                  IID_PPV_ARGS(&created.root)));
        D3D12_COMPUTE_PIPELINE_STATE_DESC description{};
        description.pRootSignature = created.root.Get();
        description.CS = {shader.binary->dxil.data(), shader.binary->dxil.size()};
        winrt::check_hresult(device.Device()->CreateComputePipelineState(
            &description, IID_PPV_ARGS(&created.state)));
        pipeline_bytes += pipeline_key.size();
        found = pipelines.emplace(std::move(pipeline_key), std::move(created)).first;
    }
    auto& root = found->second.root;
    auto& pipeline = found->second.state;
    D3D12_DESCRIPTOR_HEAP_DESC heap_description{};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_description.NumDescriptors = std::max<UINT>(1, ranges.size());
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    winrt::check_hresult(
        device.Device()->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&heap)));
    resources.BeginBatch();
    std::vector<ResourceCache::Range> guest_ranges;
    for (const auto& binding : shader.binary->bindings) {
        const auto& resource = shader.info.buffers[binding.binding];
        ResourceCache::Range range{};
        if (resource.buffer_type == Shader::BufferType::Guest) {
            const auto sharp = resource.GetSharp(shader.info);
            range = {sharp.base_address,
                     static_cast<u64>(sharp.num_records) * std::max<u32>(1, sharp.stride)};
        }
        guest_ranges.push_back(range);
    }
    const auto views = resources.GetViews(guest_ranges);
    winrt::check_hresult(allocator->Reset());
    winrt::check_hresult(commands->Reset(allocator.Get(), pipeline.Get()));
    auto descriptor = heap->GetCPUDescriptorHandleForHeapStart();
    const UINT increment = device.Device()->GetDescriptorHandleIncrementSize(heap_description.Type);
    std::vector<ResourceCache::Buffer*> uploads;
    std::array<u32, 20> constants{};
    Shader::PushData push{};
    u32 binding_index{};
    for (const auto& binding : shader.binary->bindings) {
        const auto& resource = shader.info.buffers[binding.binding];
        ResourceCache::Buffer* buffer{};
        u64 offset{}, size{};
        switch (resource.buffer_type) {
        case Shader::BufferType::Guest: {
            const auto& view = views[binding_index];
            buffer = view.buffer;
            offset = view.offset;
            size = view.size;
            push.AddOffset(binding.binding, offset & 3);
            break;
        }
        case Shader::BufferType::Flatbuf: {
            const size_t words = shader.info.flattened_ud_buf.empty()
                                     ? Shader::NUM_USER_DATA_REGS
                                     : shader.info.flattened_ud_buf.size();
            buffer = &resources.GetHostBuffer(
                0xffff000000000000ULL,
                {reinterpret_cast<const u8*>(shader.info.flat_ud), words * sizeof(u32)});
            size = buffer->size;
            break;
        }
        default:
            throw std::runtime_error("D3D12 compute special buffer is not implemented");
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R32_TYPELESS;
        view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        view.Buffer.FirstElement = offset / 4;
        view.Buffer.NumElements = buffer ? static_cast<UINT>((size + (offset & 3) + 3) / 4) : 1;
        view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device.Device()->CreateUnorderedAccessView(buffer ? buffer->native.Get() : nullptr, nullptr,
                                                   &view, descriptor);
        descriptor.ptr += increment;
        ++binding_index;
        if (buffer) {
            if (std::ranges::find(uploads, buffer) == uploads.end()) {
                resources.Upload(*buffer, commands.Get());
                uploads.push_back(buffer);
            }
            buffer->written |= resource.is_written;
        }
    }
    commands->SetComputeRootSignature(root.Get());
    static_assert(sizeof(push) <= sizeof(constants));
    std::memcpy(constants.data(), &push, sizeof(push));
    commands->SetComputeRoot32BitConstants(0, constants.size(), constants.data(), 0);
    ID3D12DescriptorHeap* heaps[]{heap.Get()};
    commands->SetDescriptorHeaps(1, heaps);
    if (!ranges.empty())
        commands->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
    commands->Dispatch(groups[0], groups[1], groups[2]);
    resources.PrepareDownload(commands.Get());
    winrt::check_hresult(commands->Close());
    device.Wait(device.Submit(commands.Get()));
    resources.CommitDownloads();
}
} // namespace D3D12
