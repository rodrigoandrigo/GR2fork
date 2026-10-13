#include "uwp/dozen_host.h"

#include <filesystem>
#include <stdexcept>
#include <string_view>
#include <windows.h>

#include <appmodel.h>
#include "common/logging/log.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "core/libraries/videoout/video_out.h"
#include "sdl_window.h"
#include "uwp/session.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/presentation_sink.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_presentation_sink.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"

extern Frontend::WindowSDL* g_window;
extern std::unique_ptr<Vulkan::Presenter> presenter;
extern std::unique_ptr<AmdGpu::Liverpool> liverpool;

namespace Core::Uwp {
namespace {
HMODULE driver;
using SetWindow = void (*)(void*, int, int);
using SetAttach = void (*)(ShadPS4AttachSwapChain, void*);
using SetLog = void (*)(void (*)(void*, const char*), void*);
SetWindow set_window;
SetAttach set_attach;
SetLog set_log;
DozenHost* current_host;

template <typename T>
T GetExport(const char* name) {
    auto function = reinterpret_cast<T>(GetProcAddress(driver, name));
    if (!function)
        throw std::runtime_error(std::string("Missing Dozen export: ") + name);
    return function;
}

void DriverLog(void*, const char* message) {
    const std::string_view text{message};
    if (text.find(": error:") != text.npos)
        LOG_ERROR(Render_Vulkan, "Dozen: {}", message);
    else if (text.find(": warning:") != text.npos)
        LOG_WARNING(Render_Vulkan, "Dozen: {}", message);
    else
        LOG_INFO(Render_Vulkan, "Dozen: {}", message);
}

void LoadDriver() {
    if (driver)
        return;
    driver = LoadPackagedLibrary(L"vulkan_dzn.dll", 0);
    UINT32 package_length = 0;
    if (!driver &&
        GetCurrentPackageFullName(&package_length, nullptr) == APPMODEL_ERROR_NO_PACKAGE) {
        if (!GetModuleHandleW(L"d3d12.dll") &&
            !LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
            throw std::runtime_error("Cannot load system D3D12 for the unpackaged host");
        HMODULE module;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&LoadDriver), &module))
            throw std::runtime_error("Cannot locate the embedded core module");
        wchar_t path[32768];
        const auto length = GetModuleFileNameW(module, path, std::size(path));
        if (!length || length == std::size(path))
            throw std::runtime_error("Cannot locate the embedded core directory");
        const auto library = std::filesystem::path{path}.parent_path() / L"vulkan_dzn.dll";
        const auto validator = std::filesystem::path{path}.parent_path() / L"dxil.dll";
        if (!GetModuleHandleW(L"dxil.dll") && !LoadLibraryExW(validator.c_str(), nullptr,
                            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32))
            throw std::runtime_error("Cannot load the adjacent DXIL validator");
        driver = LoadLibraryExW(library.c_str(), nullptr,
                                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    }
    if (!driver)
        throw std::runtime_error("Cannot load packaged vulkan_dzn.dll: " +
                                 std::to_string(GetLastError()));
    try {
        set_window = GetExport<SetWindow>("uwp_set_window_reference");
        set_attach = GetExport<SetAttach>("mesa_uwp_set_swapchain_attach_callback");
        set_log = GetExport<SetLog>("mesa_uwp_set_log_callback");
        set_log(&DriverLog, nullptr);
        VULKAN_HPP_DEFAULT_DISPATCHER.init(
            GetExport<PFN_vkGetInstanceProcAddr>("vk_icdGetInstanceProcAddr"));
    } catch (...) {
        FreeLibrary(driver);
        driver = nullptr;
        throw;
    }
}
} // namespace

struct DozenHost::Bootstrap {
    Vulkan::Instance instance;
    Vulkan::Scheduler scheduler;
    Vulkan::Swapchain swapchain;

    explicit Bootstrap(Frontend::WindowSDL& window)
        : instance{window, EmulatorSettings.GetGpuId(), false, false}, scheduler{instance},
          swapchain{instance, window} {}
    ~Bootstrap() {
        try {
            scheduler.Finish();
        } catch (const std::exception& error) {
            LOG_ERROR(Render_Vulkan, "Dozen shutdown: {}", error.what());
        }
    }
};

DozenHost::DozenHost() = default;
DozenHost::~DozenHost() {
    current_host = nullptr;
    Libraries::VideoOut::ShutdownVideoOut();
    VideoCore::SetPresentationSink({});
    liverpool.reset();
    if (presenter)
        Core::Memory::Instance()->SetRasterizer(nullptr);
    presenter.reset();
    if (g_window == window.get())
        g_window = nullptr;
    if (driver) {
        set_attach(nullptr, nullptr);
        set_window(nullptr, 0, 0);
    }
}

void DozenHost::CheckEnvironment() {
    LoadDriver();
    const vk::ApplicationInfo application{.pApplicationName = "shadPS4 UWP environment",
                                          .apiVersion = Vulkan::TargetVulkanApiVersion};
    const vk::InstanceCreateInfo create_info{.pApplicationInfo = &application};
    vk::detail::DispatchLoaderDynamic dispatch;
    dispatch.init(GetExport<PFN_vkGetInstanceProcAddr>("vk_icdGetInstanceProcAddr"));
    auto result = vk::createInstanceUnique(create_info, nullptr, dispatch);
    if (result.result != vk::Result::eSuccess)
        throw std::runtime_error("Dozen instance creation failed: " + vk::to_string(result.result));
    auto instance = std::move(result.value);
    dispatch.init(*instance);
    const auto devices = instance->enumeratePhysicalDevices(dispatch);
    if (devices.result != vk::Result::eSuccess || devices.value.empty())
        throw std::runtime_error("Dozen did not expose a D3D12 adapter");
}

void DozenHost::Initialize(const ShadPS4Config& config) {
    LoadDriver();
    set_window(config.panel, config.width, config.height);
    set_attach(config.attach_swapchain, config.attach_context);
    window = std::make_unique<Frontend::WindowSDL>(config.width, config.height, config.panel);
    g_window = window.get();
    current_host = this;
    bootstrap = std::make_unique<Bootstrap>(*window);
    Render();
}

void DozenHost::Resize(uint32_t width, uint32_t height) {
    window->ResizeEmbedded(width, height);
    set_window(window->GetWindowInfo().render_surface, width, height);
}

void DozenHost::Render() {
    if (!bootstrap || shadps4_get_state() == SHADPS4_RUNNING)
        return;
    auto& chain = bootstrap->swapchain;
    auto& scheduler = bootstrap->scheduler;
    if (!window->GetWidth() || !window->GetHeight())
        return;
    if (window->GetWidth() != chain.GetWidth() || window->GetHeight() != chain.GetHeight())
        chain.Recreate(window->GetWidth(), window->GetHeight());
    while (!chain.AcquireNextImage()) {
        if (StopRequested())
            throw std::runtime_error("Presentation cancelled during shutdown");
        chain.Recreate(window->GetWidth(), window->GetHeight());
    }
    auto cmdbuf = scheduler.CommandBuffer();
    const vk::ImageSubresourceRange range{
        .aspectMask = vk::ImageAspectFlagBits::eColor, .levelCount = 1, .layerCount = 1};
    const vk::ImageMemoryBarrier barrier{.dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                                         .oldLayout = vk::ImageLayout::eUndefined,
                                         .newLayout = vk::ImageLayout::eTransferDstOptimal,
                                         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                         .image = chain.Image(),
                                         .subresourceRange = range};
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                           vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, barrier);
    cmdbuf.clearColorImage(chain.Image(), vk::ImageLayout::eTransferDstOptimal,
                           vk::ClearColorValue{std::array<float, 4>{0, 0, 0, 1}}, range);
    auto present_barrier = barrier;
    present_barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
    present_barrier.dstAccessMask = {};
    present_barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
    present_barrier.newLayout = vk::ImageLayout::ePresentSrcKHR;
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                           vk::PipelineStageFlagBits::eBottomOfPipe, {}, {}, {}, present_barrier);
    Vulkan::SubmitInfo info{};
    info.AddWait(chain.GetImageAcquiredSemaphore());
    info.AddSignal(chain.GetPresentReadySemaphore());
    scheduler.Flush(info);
    std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
    if (!chain.Present())
        chain.Recreate(window->GetWidth(), window->GetHeight());
}

void DozenHost::ReleaseBootstrap() {
    bootstrap.reset();
}
void ReleaseDozenBootstrap() {
    if (!current_host)
        throw std::runtime_error("The Dozen host is not initialized");
    current_host->ReleaseBootstrap();
}

void ShutdownDozenDriver() {
    if (!driver)
        return;
    set_log(nullptr, nullptr);
    VULKAN_HPP_DEFAULT_DISPATCHER = vk::detail::DispatchLoaderDynamic{};
    FreeLibrary(driver);
    driver = nullptr;
    set_attach = nullptr;
    set_window = nullptr;
    set_log = nullptr;
}
} // namespace Core::Uwp
