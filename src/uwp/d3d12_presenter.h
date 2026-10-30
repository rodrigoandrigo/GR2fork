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
#include "video_core/renderer_d3d12/profile.h"
#include "video_core/renderer_d3d12/fsr_pass.h"
#include "video_core/renderer_d3d12/shader_cache.h"
#include "video_core/resident_frame.h"

namespace Core::Uwp {
class D3D12Presenter final : public VideoCore::PresentationSink {
public:
    ~D3D12Presenter() override;
    static void CheckEnvironment();
    void Initialize(const ShadPS4Config& config);
    void Render();
    void Drain();
    void TrimMemory();
    u64 MemoryBytes() const;
    void Resize(uint32_t width, uint32_t height, float scale_x = 1.0f, float scale_y = 1.0f);
    void RegisterVideoOutSurface(const Libraries::VideoOut::BufferAttributeGroup& group,
                                 VAddr address) override;
    VideoCore::FrameHandle PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& group,
                                        VAddr address) override;
    VideoCore::FrameHandle PrepareBlankFrame(bool present_thread) override;
    VideoCore::FrameHandle PrepareLastFrame() override;
    void Present(const VideoCore::FrameHandle& frame, bool reuse = false,
                 bool game = true) override;
    bool IsHDRSupported() const override;
    void SetHDROutput(bool enabled, float sdr_white_nits = 80.0f);
    void SetHDR(bool enabled) override;
    void SetGamma(float value) override;

private:
    struct Frame;
    struct CommandFrame {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commands;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> conversion_heap;
        D3D12::FsrPass fsr;
        std::shared_ptr<Frame> submitted_frame;
        u64 fence{};
    };
    void ConvertResident(Frame& frame, CommandFrame& command_frame,
                         ID3D12GraphicsCommandList* commands);
    void WaitForFrameUse(const Frame& frame);
    void GetBuffers();
    void RenderFrame();
    void ResizeBuffers(uint32_t width, uint32_t height, DXGI_FORMAT format);
    std::array<float, 1024> ColorComponents(const Frame& frame, bool floating_point) const;
    std::vector<u8> ConvertPixels(const Frame& frame, bool floating_point) const;
    std::shared_ptr<Frame> last_frame;
    std::shared_ptr<std::atomic<u64>> frame_bytes{std::make_shared<std::atomic<u64>>(0)};
    bool float_supported{};
    bool hdr_enabled{};
    float sdr_white_nits{80.0f};
    std::atomic_bool hdr_display{};
    DXGI_FORMAT format{DXGI_FORMAT_R8G8B8A8_UNORM};
    std::atomic<float> gamma{1.0f};
    std::atomic_bool prepared_frame{};
    bool presented_frame{};
    mutable std::mutex mutex;
    D3D12DeviceContext context;
    D3D12::ShaderCache conversion_shaders;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> conversion_root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> conversion_pipeline;
    D3D12ResourceAllocator resources;
    D3D12DescriptorArena descriptors;
    D3D12TransferStats transfers;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain;
    std::array<CommandFrame, 2> command_frames;
    mutable D3D12::Profile presenter_profile;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2> buffers;
    uint32_t width{};
    uint32_t height{};
    uint32_t display_width{}, display_height{};
    float composition_scale_x{1.0f}, composition_scale_y{1.0f};
};
} // namespace Core::Uwp
