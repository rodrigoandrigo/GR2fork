#include "uwp/native_host.h"

#include <stdexcept>
#include "core/memory.h"
#include "common/logging/log.h"
#include "core/libraries/videoout/video_out.h"
#include "sdl_window.h"
#include "uwp/d3d12_presenter.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_d3d12/d3d12_rasterizer.h"

extern Frontend::WindowSDL* g_window;
extern std::unique_ptr<AmdGpu::Liverpool> liverpool;

namespace Core::Uwp {
namespace {
NativeHost* current_host;
}
void CheckNativeGraphicsEnvironment() {
    D3D12Presenter::CheckEnvironment();
}
NativeHost::NativeHost() = default;
NativeHost::~NativeHost() {
    if (current_host == this)
        current_host = nullptr;
    LOG_INFO(Core, "Stopping native D3D12 GPU processor");
    if (liverpool)
        liverpool->StopProcessing();
    Libraries::VideoOut::ShutdownVideoOut();
    VideoCore::SetPresentationSink({});
    LOG_INFO(Core, "Releasing native D3D12 rasterizer");
    liverpool.reset();
    LOG_INFO(Core, "Releasing native D3D12 presentation");
    presentation.reset();
    if (g_window == window.get())
        g_window = nullptr;
    window.reset();
    LOG_INFO(Core, "Native D3D12 host released");
}
void NativeHost::Initialize(const ShadPS4Config& config) {
    window = std::make_unique<Frontend::WindowSDL>(config.width, config.height, config.panel);
    presentation = std::make_shared<D3D12Presenter>();
    attach = config.attach_swapchain;
    attach_context = config.attach_context;
    presentation->Initialize(config);
    g_window = window.get();
    current_host = this;
    Render();
}
void NativeHost::DetachSurface() {
    if (!attach)
        return;
    presentation->Drain();
    const auto result = attach(attach_context, nullptr);
    if (result < 0)
        throw std::runtime_error("Could not detach the native D3D12 presentation surface: " +
                                 std::to_string(static_cast<uint32_t>(result)));
    attach = nullptr;
    attach_context = nullptr;
    presentation->Drain();
}
void NativeHost::InitializeGpu() {
    if (liverpool)
        throw std::runtime_error("The guest GPU processor is already initialized");
    LOG_INFO(Core, "[UWP stage] creating native GPU processor");
    auto gpu = std::make_unique<AmdGpu::Liverpool>();
    LOG_INFO(Core, "[UWP stage] creating native D3D12 rasterizer");
    gpu->OwnRasterizer(std::make_unique<D3D12::Rasterizer>(*gpu, *Core::Memory::Instance()));
    liverpool = std::move(gpu);
    VideoCore::SetPresentationSink(presentation);
    LOG_INFO(Core, "[UWP stage] native D3D12 GPU ready");
}
void NativeHost::Resize(uint32_t width, uint32_t height, float scale_x, float scale_y) {
    window->ResizeEmbedded(width, height);
    presentation->Resize(width, height, scale_x, scale_y);
}
void NativeHost::SetHDROutput(bool enabled, float sdr_white_nits) {
    presentation->SetHDROutput(enabled, sdr_white_nits);
}
void NativeHost::Render() {
    if (shadps4_get_state() != SHADPS4_RUNNING)
        presentation->Render();
}
void InitializeNativeGpu() {
    if (!current_host)
        throw std::runtime_error("Native D3D12 host is not initialized");
    current_host->InitializeGpu();
}
}
