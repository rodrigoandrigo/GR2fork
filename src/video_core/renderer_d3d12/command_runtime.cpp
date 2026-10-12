#include "video_core/renderer_d3d12/command_runtime.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <winrt/base.h>
#include "video_core/renderer_d3d12/resource_bindings.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"

namespace D3D12 {
u64 CommandRuntime::Flush() {
    std::scoped_lock lock{mutex};
    if (failed)
        throw std::runtime_error(
            "D3D12 command runtime requires recreation after recording failure");
    device.WaitForGpu();
    return device.LastSignaledValue();
}
void CommandRuntime::Initialize(ResourceCache::Read read, ResourceCache::Write write,
                                ResourceCache::Resolve resolve) {
    device.Initialize();
    shaders.Initialize(device.Device());
    this->read = read;
    resources.Initialize(device, read, write, std::move(resolve));
    images.Initialize(device, std::move(read), std::move(write), [this](VAddr address, u64 size) {
        return resources.Canonicalize(address, size);
    });
    device.CreateDirectCommands(allocator, commands);
    winrt::check_hresult(commands->Close());
}
void CommandRuntime::Dispatch(Shader::ShaderParams params, Shader::RuntimeInfo runtime,
                              std::array<u32, 3> groups, VAddr border_colors) {
    std::scoped_lock lock{mutex};
    if (failed)
        throw std::runtime_error(
            "D3D12 command runtime requires recreation after recording failure");
    if (runtime.sw_stage != Shader::SwStage::Compute)
        throw std::runtime_error("D3D12 dispatch requires a compute shader");
    const auto& local = runtime.hw.cs.workgroup_size;
    if (!local[0] || !local[1] || !local[2] || local[0] > 1024 || local[1] > 1024 ||
        local[2] > 64 || u64(local[0]) * local[1] * local[2] > 1024)
        throw std::runtime_error("Guest compute local size exceeds D3D12 limits");
    for (const auto count : groups) {
        if (!count)
            return;
        if (count > D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION)
            throw std::runtime_error("Guest dispatch exceeds D3D12 workgroup limits");
    }
    auto shader = shaders.Compile(params, runtime);
    std::array<CompiledShader*, 1> stages{&shader};
    ResourceBindings bindings{device, resources, images, stages};
    std::vector<D3D12_ROOT_PARAMETER> parameters(2);
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 20};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants = {2, 0, 3};
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
    D3D12_ROOT_SIGNATURE_DESC root_description{};
    root_description.NumParameters = parameters.size();
    root_description.pParameters = parameters.data();
    Microsoft::WRL::ComPtr<ID3DBlob> root_blob, errors;
    winrt::check_hresult(D3D12SerializeRootSignature(
        &root_description, D3D_ROOT_SIGNATURE_VERSION_1, &root_blob, &errors));
    std::vector<u8> pipeline_key{0};
    pipeline_key.insert(pipeline_key.end(), shader.binary->dxil.begin(), shader.binary->dxil.end());
    const auto* root_bytes = static_cast<const u8*>(root_blob->GetBufferPointer());
    pipeline_key.insert(pipeline_key.end(), root_bytes, root_bytes + root_blob->GetBufferSize());
    auto& cached =
        GetPipeline(std::move(pipeline_key), root_blob.Get(), [&](auto* root, auto** state) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC description{};
            description.pRootSignature = root;
            description.CS = {shader.binary->dxil.data(), shader.binary->dxil.size()};
            return device.Device()->CreateComputePipelineState(
                &description, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(state));
        });
    auto& root = cached.root;
    auto& pipeline = cached.state;
    resources.BeginBatch();
    images.BeginBatch();
    winrt::check_hresult(allocator->Reset());
    winrt::check_hresult(commands->Reset(allocator.Get(), pipeline.Get()));
    RecordingGuard recording{failed};
    Shader::PushData push{};
    const std::array<float, 24> planes{};
    std::vector<u8> shared_memory;
    if (std::ranges::any_of(shader.info.buffers, [](const auto& buffer) {
            return buffer.buffer_type == Shader::BufferType::SharedMemory;
        })) {
        const u64 group_count = u64(groups[0]) * groups[1] * groups[2];
        if (!runtime.hw.cs.shared_memory_size ||
            group_count > (16 * 1024 * 1024) / runtime.hw.cs.shared_memory_size)
            throw std::runtime_error("Guest shared memory storage exceeds D3D12 budget");
        shared_memory.resize(group_count * runtime.hw.cs.shared_memory_size);
    }
    bindings.Upload(commands.Get(), push, planes, border_colors, read, gds, shared_memory);
    std::array<u32, 20> constants{};
    commands->SetComputeRootSignature(root.Get());
    commands->SetPipelineState(pipeline.Get());
    static_assert(sizeof(push) <= sizeof(constants));
    std::memcpy(constants.data(), &push, sizeof(push));
    commands->SetComputeRoot32BitConstants(0, constants.size(), constants.data(), 0);
    commands->SetComputeRoot32BitConstants(1, groups.size(), groups.data(), 0);
    bindings.Bind(commands.Get(), false, resource_slot, sampler_slot);
    commands->Dispatch(groups[0], groups[1], groups[2]);
    resources.PrepareDownload(commands.Get());
    images.PrepareDownloads(commands.Get());
    winrt::check_hresult(commands->Close());
    device.Wait(device.Submit(commands.Get()));
    resources.CommitDownloads();
    bindings.CommitGds(gds);
    images.CommitDownloads();
    recording.completed = true;
}
void CommandRuntime::Resolve(const VideoCore::ImageInfo& source,
                             const VideoCore::ImageInfo& destination, u32 source_layer,
                             u32 destination_layer, u32 layers) {
    std::scoped_lock lock{mutex};
    if (failed)
        throw std::runtime_error(
            "D3D12 command runtime requires recreation after recording failure");
    images.BeginBatch();
    auto& input = images.GetImage(source, ConvertFormat(source.pixel_format),
                                  D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, source_layer, layers);
    auto& output =
        images.GetImage(destination, ConvertFormat(destination.pixel_format),
                        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, destination_layer, layers);
    winrt::check_hresult(allocator->Reset());
    winrt::check_hresult(commands->Reset(allocator.Get(), nullptr));
    RecordingGuard recording{failed};
    images.Resolve(input, output, commands.Get());
    images.PrepareDownloads(commands.Get());
    winrt::check_hresult(commands->Close());
    device.Wait(device.Submit(commands.Get()));
    images.CommitDownloads();
    recording.completed = true;
}
} // namespace D3D12
