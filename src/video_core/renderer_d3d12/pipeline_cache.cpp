#include "video_core/renderer_d3d12/command_runtime.h"

#include <stdexcept>
#include <winrt/base.h>
#include "video_core/renderer_d3d12/profile.h"

namespace D3D12 {
namespace {
Profile pipeline_profile;
}
CommandRuntime::Pipeline& CommandRuntime::GetPipeline(
    std::vector<u8> key, ID3DBlob* blob,
    const std::function<HRESULT(ID3D12RootSignature*, ID3D12PipelineState**, D3D12_CACHED_PIPELINE_STATE)>& create) {
    constexpr size_t limit = 32 * 1024 * 1024;
    if (key.size() > limit)
        throw std::runtime_error("D3D12 pipeline exceeds cache budget");
    if (const auto found = pipelines.find(key); found != pipelines.end()) {
        if (pipeline_profile.enabled)
            pipeline_profile.Record("pipeline_ram_hit", 128, 0);
        return found->second;
    }
    ProfileScope timing{pipeline_profile, "pipeline_miss", 16};
    if (!coordinator)
        device.WaitForGpu();
    while (!pipelines.empty() && (pipelines.size() >= 128 || pipeline_bytes + key.size() > limit)) {
        pipeline_bytes -= pipelines.begin()->first.size();
        pipelines.erase(pipelines.begin());
    }
    Pipeline created;
    winrt::check_hresult(device.Device()->CreateRootSignature(
        0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&created.root)));
    const auto cached = ProfileSample(pipeline_profile, "pipeline_disk_load", 16,
                                      [&] { return persistent_pipelines.Load(key); });
    HRESULT result = create(created.root.Get(), created.state.ReleaseAndGetAddressOf(),
                            {cached.data(), cached.size()});
    const bool reused = SUCCEEDED(result) && !cached.empty();
    if (pipeline_profile.enabled)
        pipeline_profile.Record(reused ? "pipeline_blob_hit" : "pipeline_blob_miss", 16, 0);
    if (FAILED(result) && !cached.empty()) {
        persistent_pipelines.Remove(key);
        created.state.Reset();
        result = create(created.root.Get(), created.state.ReleaseAndGetAddressOf(), {});
    }
    device.CheckResult(result, L"Native D3D12 pipeline creation");
    device.CheckResult(device.Device()->GetDeviceRemovedReason(),
                       L"Native D3D12 device after pipeline creation");
    Microsoft::WRL::ComPtr<ID3DBlob> compiled;
    if (persistent_pipelines.Enabled() && !reused &&
        SUCCEEDED(created.state->GetCachedBlob(&compiled)) && compiled)
        persistent_pipelines.Store(key, {static_cast<const u8*>(compiled->GetBufferPointer()),
                                        compiled->GetBufferSize()});
    device.CheckResult(device.Device()->GetDeviceRemovedReason(),
                       L"Native D3D12 device after pipeline cache extraction");
    const size_t bytes = key.size();
    auto found = pipelines.emplace(std::move(key), std::move(created)).first;
    pipeline_bytes += bytes;
    return found->second;
}
} // namespace D3D12
