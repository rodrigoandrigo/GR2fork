#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include "d3d12_command_encoder.h"
#include "d3d12_device_context.h"
#include "d3d12_pipeline_cache.h"
#include "d3d12_resource_allocator.h"
#include "uwp/shadps4_api.h"
#include "video_core/presentation_sink.h"

namespace Core::Uwp {
class D3D12Presenter final : public VideoCore::PresentationSink {
public:
    static void CheckEnvironment();
    void Initialize(const ShadPS4Config& config);
    void Render();
    void Resize(uint32_t width, uint32_t height);
    void RegisterVideoOutSurface(const Libraries::VideoOut::BufferAttributeGroup& group,
                                 VAddr address) override;
    VideoCore::FrameHandle PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& group,
                                        VAddr address) override;
    VideoCore::FrameHandle PrepareBlankFrame(bool present_thread) override;
    VideoCore::FrameHandle PrepareLastFrame() override;
    void Present(const VideoCore::FrameHandle& frame, bool reuse = false,
                 bool game = true) override;
    bool IsHDRSupported() const override {
        return false;
    }
    void SetHDR(bool enabled) override;
    void SetGamma(float value) override;

private:
    void GetBuffers();
    void RenderFrame();
    void ResizeBuffers(uint32_t width, uint32_t height);
    struct Frame;
    std::shared_ptr<Frame> last_frame;
    std::shared_ptr<std::atomic<u64>> frame_bytes{std::make_shared<std::atomic<u64>>(0)};
    std::atomic<float> gamma{1.0f};
    std::mutex mutex;
    D3D12DeviceContext context;
    D3D12ResourceAllocator resources;
    D3D12DescriptorArena descriptors;
    D3D12TransferStats transfers;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commands_allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commands;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2> buffers;
    uint32_t width{};
    uint32_t height{};
};
} // namespace Core::Uwp
