#include "video_core/renderer_d3d12/command_runtime.h"

#include <stdexcept>
#include <winrt/base.h>

namespace D3D12 {
CommandRuntime::Pipeline& CommandRuntime::GetPipeline(
    std::vector<u8> key, ID3DBlob* blob,
    const std::function<HRESULT(ID3D12RootSignature*, ID3D12PipelineState**)>& create) {
    constexpr size_t limit = 32 * 1024 * 1024;
    if (key.size() > limit)
        throw std::runtime_error("D3D12 pipeline exceeds cache budget");
    if (const auto found = pipelines.find(key); found != pipelines.end())
        return found->second;
    device.WaitForGpu();
    while (!pipelines.empty() && (pipelines.size() >= 128 || pipeline_bytes + key.size() > limit)) {
        pipeline_bytes -= pipelines.begin()->first.size();
        pipelines.erase(pipelines.begin());
    }
    Pipeline created;
    winrt::check_hresult(device.Device()->CreateRootSignature(
        0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&created.root)));
    winrt::check_hresult(create(created.root.Get(), created.state.ReleaseAndGetAddressOf()));
    const size_t bytes = key.size();
    auto found = pipelines.emplace(std::move(key), std::move(created)).first;
    pipeline_bytes += bytes;
    return found->second;
}
} // namespace D3D12
