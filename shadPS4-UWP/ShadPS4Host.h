#pragma once

#include <functional>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <memory>
#include <string>
#include <agile.h>
#include "../src/uwp/shadps4_api.h"

class ShadPS4Host : public std::enable_shared_from_this<ShadPS4Host> {
public:
    explicit ShadPS4Host(Windows::UI::Xaml::Controls::SwapChainPanel ^ panel);
    ~ShadPS4Host();
    void Initialize(const std::wstring& user_directory, uint32_t width, uint32_t height,
                    bool frontend = false, bool game_surface = false);
    void Reload(const std::wstring& user_directory, uint32_t width, uint32_t height,
                bool frontend, bool game_surface);
    uint32_t GraphicsBackend() const { return graphics_backend; }
    uint32_t State() const { return state ? state() : SHADPS4_UNINITIALIZED; }
    template <typename T>
    T GetApi(const char* name) {
        return Resolve<T>(name);
    }
    void SetLogConsumer(std::function<void(std::string)> consumer);
    void Run(std::wstring executable, std::function<void(std::wstring)> completed);
    void Pause(bool paused);
    void Stop();
    void Wait();
    void Resize(uint32_t width, uint32_t height);
    bool Render();
    std::wstring Error() const;
    std::wstring MountFolder(Platform::Agile<Windows::Storage::StorageFolder> folder);

private:
    void PresentationLoop();
    static void __cdecl Log(void* context, uint32_t level, const char* message);
    static void __cdecl ReportError(void* context, const char* message);
    static long __cdecl AttachSwapChain(void* context, void* swapchain);
    template <typename T>
    T Resolve(const char* name) {
        const auto address = GetProcAddress(module, name);
        if (!address) {
            throw ref new Platform::FailureException(L"The shadPS4 DLL interface is incomplete.");
        }
        return reinterpret_cast<T>(address);
    }

    uint32_t graphics_backend{SHADPS4_GRAPHICS_D3D12_NATIVE};
    HMODULE module{};
    Platform::Agile<Windows::UI::Xaml::Controls::SwapChainPanel> panel;
    decltype(&shadps4_initialize) initialize{};
    decltype(&shadps4_get_state) state{};
    decltype(&shadps4_run) run{};
    decltype(&shadps4_request_stop) stop{};
    decltype(&shadps4_set_paused) pause{};
    decltype(&shadps4_resize) resize{};
    decltype(&shadps4_render_host_frame) render{};
    decltype(&shadps4_get_error) error{};
    decltype(&shadps4_shutdown) shutdown{};
    decltype(&shadps4_set_log_callback) set_log_callback{};
    decltype(&shadps4_set_error_callback) set_error_callback{};
    decltype(&shadps4_mount_storage_folder) mount_folder{};
    decltype(&shadps4_open_mounted_file) open_file{};
    decltype(&shadps4_file_get_size) file_size{};
    decltype(&shadps4_file_read) read_file{};
    decltype(&shadps4_file_close) close_file{};
    std::thread presentation_thread;
    std::thread session_thread;
    std::mutex module_mutex;
    std::atomic_bool stop_pending{};
    std::mutex work_mutex;
    std::condition_variable work_ready;
    bool closing{};
    bool frame_pending{};
    uint32_t pending_width{};
    uint32_t pending_height{};
    std::atomic_bool presentation_failed{};
    std::mutex log_mutex;
    std::function<void(std::string)> log_consumer;
};
