#include "uwp/native_host.h"

#include <windows.h>

#include <stdexcept>
#include <basetyps.h>
#include <gamingdeviceinformation.h>
#include <winrt/Windows.System.Profile.h>
#include <winrt/base.h>
#include "common/logging/log.h"
#include "core/libraries/videoout/video_out.h"
#include "core/memory.h"
#include "sdl_window.h"
#include "uwp/d3d12_presenter.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_d3d12/d3d12_rasterizer.h"

#pragma comment(lib, "OneCore.lib")

extern Frontend::WindowSDL* g_window;
extern std::unique_ptr<AmdGpu::Liverpool> liverpool;

namespace Core::Uwp {
namespace {
NativeHost* current_host;
}
void CheckNativeGraphicsProfile() {
    if (winrt::Windows::System::Profile::AnalyticsInfo::VersionInfo().DeviceFamily() !=
        L"Windows.Xbox")
        return;

    GAMING_DEVICE_MODEL_INFORMATION model{};
    const HRESULT model_result = GetGamingDeviceModelInformation(&model);
    if (FAILED(model_result))
        throw std::runtime_error("Could not query the Xbox model for native D3D12: HRESULT " +
                                 std::to_string(static_cast<uint32_t>(model_result)));
    if (model.vendorId != GAMING_DEVICE_VENDOR_ID_MICROSOFT)
        throw std::runtime_error("Native D3D12 is blocked because the Xbox vendor is unknown");

    const bool xbox_one = model.deviceId == GAMING_DEVICE_DEVICE_ID_XBOX_ONE ||
                          model.deviceId == GAMING_DEVICE_DEVICE_ID_XBOX_ONE_S ||
                          model.deviceId == GAMING_DEVICE_DEVICE_ID_XBOX_ONE_X ||
                          model.deviceId == GAMING_DEVICE_DEVICE_ID_XBOX_ONE_X_DEVKIT;
    const bool xbox_series = model.deviceId == GAMING_DEVICE_DEVICE_ID_XBOX_SERIES_S ||
                             model.deviceId == GAMING_DEVICE_DEVICE_ID_XBOX_SERIES_X ||
                             model.deviceId == GAMING_DEVICE_DEVICE_ID_XBOX_SERIES_X_DEVKIT;
    if (!xbox_one && !xbox_series)
        throw std::runtime_error("Native D3D12 is blocked because this Xbox model is unrecognized");

    if (xbox_one)
        throw std::runtime_error(
            "Native D3D12 on Xbox One is disabled because this UWP host cannot verify the "
            "package's actual App/Game Mode. HasExpandedResources reports resource state, not "
            "mode permission; use Vulkan / Dozen until a trustworthy mode signal is available");

    LOG_INFO(Core,
             "Native D3D12 Xbox Series profile accepted: device_id={:#x}; runtime mode, "
             "quotas and capabilities remain subject to validation",
             static_cast<uint32_t>(model.deviceId));
}
void CheckNativeGraphicsEnvironment() {
    CheckNativeGraphicsProfile();
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
    CheckNativeGraphicsProfile();
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
    auto renderer = std::make_unique<D3D12::Rasterizer>(*gpu, *Core::Memory::Instance());
    {
        std::scoped_lock lock{renderer_mutex};
        const std::weak_ptr<D3D12Presenter> presenter = presentation;
        renderer->SetExternalMemory(
            [presenter] {
                const auto value = presenter.lock();
                return value ? value->MemoryBytes() : 0;
            },
            [presenter] {
                if (const auto value = presenter.lock())
                    value->TrimMemory();
            });
        renderer->SetBackground(!active);
        auto* pointer = renderer.get();
        gpu->OwnRasterizer(std::move(renderer));
        rasterizer = pointer;
    }
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
void NativeHost::SetActive(bool value) {
    std::scoped_lock lock{renderer_mutex};
    const bool changed = active != value;
    active = value;
    if (rasterizer)
        rasterizer->SetBackground(!active);
    if (changed && !active)
        presentation->TrimMemory();
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
