#include "uwp/session.h"
#include "uwp/shadps4_api.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <SDL3/SDL.h>
#include <windows.h>
#include <winrt/base.h>
#include "common/key_manager.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/user_settings.h"
#include "emulator.h"
#include "uwp/d3d12_presenter.h"

namespace {
std::atomic<uint32_t> state{SHADPS4_UNINITIALIZED};
std::atomic_bool stop_requested{};
std::atomic_bool pause_requested{};
std::atomic_bool guest_ready{};
std::mutex api_mutex;
std::string last_error;
std::unique_ptr<Core::Uwp::D3D12Presenter> presentation;
std::mutex presentation_mutex;
void* panel{};
ShadPS4ErrorCallback error_callback{};
void* error_context{};

int32_t Fail(std::string message) {
    ShadPS4ErrorCallback callback;
    void* context;
    {
        std::scoped_lock lock{api_mutex};
        last_error = message;
        callback = error_callback;
        context = error_context;
    }
    LOG_ERROR(Core, "{}", message);
    if (callback) {
        try {
            callback(context, message.c_str());
        } catch (...) {
        }
    }
    return -1;
}

int ExecuteProbe(void* allocation, DWORD* exception) {
    __try {
        return reinterpret_cast<int(__cdecl*)()>(allocation)();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *exception = GetExceptionCode();
        return -1;
    }
}

bool CanExecuteGuestCode() {
    void* allocation = VirtualAlloc(nullptr, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!allocation) {
        Fail("VirtualAlloc: " + std::to_string(GetLastError()));
        return false;
    }
    constexpr unsigned char code[] = {0xb8, 0x2a, 0, 0, 0, 0xc3};
    std::memcpy(allocation, code, sizeof(code));
    DWORD protection{};
    bool usable = VirtualProtect(allocation, 65536, PAGE_EXECUTE_READ, &protection) != 0;
    if (usable) {
        FlushInstructionCache(GetCurrentProcess(), allocation, sizeof(code));
        DWORD exception{};
        usable = ExecuteProbe(allocation, &exception) == 42;
        if (!usable) {
            Fail("Guest code execution failed: " + std::to_string(exception));
        }
        if (usable && !VirtualProtect(allocation, 65536, PAGE_EXECUTE_READWRITE, &protection)) {
            Fail("Guest code patching denied: " + std::to_string(GetLastError()));
            usable = false;
        }
        if (usable) {
            static_cast<unsigned char*>(allocation)[1] = 43;
            FlushInstructionCache(GetCurrentProcess(), allocation, sizeof(code));
            usable = ExecuteProbe(allocation, &exception) == 43;
            if (!usable) {
                Fail("Patched guest code execution failed: " + std::to_string(exception));
            }
        }
    } else {
        Fail("Executable guest memory denied: " + std::to_string(GetLastError()));
    }
    VirtualFree(allocation, 0, MEM_RELEASE);
    return usable;
}
} // namespace

namespace Core::Uwp {
void ReportError(std::string message) {
    Fail(std::move(message));
}
bool StopRequested() {
    return stop_requested.load();
}

bool PauseRequested() {
    return pause_requested.load();
}

void SetGuestReady(bool ready) {
    guest_ready.store(ready);
}
} // namespace Core::Uwp

uint32_t shadps4_get_api_version() {
    return 1;
}

void shadps4_set_log_callback(ShadPS4LogCallback callback, void* context) {
    Common::Log::SetCallback(callback, context);
}

void shadps4_set_error_callback(ShadPS4ErrorCallback callback, void* context) {
    std::scoped_lock lock{api_mutex};
    error_callback = callback;
    error_context = context;
}

void shadps4_flush_logs() {
    try {
        Common::Log::Flush();
    } catch (const std::exception& exception) {
        Fail(exception.what());
    }
}

int32_t shadps4_shutdown() {
    shadps4_request_stop();
    if (state.load() == SHADPS4_RUNNING) {
        return Fail("Wait for the guest session to return before shutdown");
    }
    try {
        std::scoped_lock lock{presentation_mutex};
        presentation.reset();
        Core::Uwp::ClearStorage();
        panel = nullptr;
        Common::Log::Flush();
        Common::Log::Shutdown();
        guest_ready.store(false);
        state.store(SHADPS4_UNINITIALIZED);
        return 0;
    } catch (const std::exception& exception) {
        return Fail(exception.what());
    }
}

int32_t shadps4_check_environment() {
    if (!CanExecuteGuestCode()) {
        return -1;
    }
    try {
        Core::Uwp::D3D12Presenter::CheckEnvironment();
    } catch (const winrt::hresult_error& exception) {
        return Fail("D3D12 HRESULT " +
                    std::to_string(static_cast<uint32_t>(exception.code().value)) + ": " +
                    winrt::to_string(exception.message()));
    } catch (const std::exception& exception) {
        return Fail(exception.what());
    }
    return 0;
}

int32_t shadps4_initialize(const ShadPS4Config* config) {
    if (!config || config->size != sizeof(ShadPS4Config) || config->version != 1 ||
        !config->user_directory || !*config->user_directory ||
        (config->panel && (!config->width || !config->height || !config->attach_swapchain)) ||
        (!config->panel && config->attach_swapchain)) {
        return Fail("Invalid embedded core configuration");
    }
    if (state.load() != SHADPS4_UNINITIALIZED) {
        return Fail("The core has already been initialized");
    }
    try {
        Common::FS::InitializeUserPaths(config->user_directory);
        Common::Log::Setup("shadps4.log");
        stop_requested.store(false);
        pause_requested.store(false);
        LOG_INFO(Core, "Embedded core initialized with host-owned storage");
        KeyManager::GetInstance()->LoadFromFile();
        EmulatorSettings.Load();
        UserSettings.Load();
        if (shadps4_check_environment() != 0) {
            return -1;
        }
        if (config->panel) {
            panel = config->panel;
            presentation = std::make_unique<Core::Uwp::D3D12Presenter>();
            presentation->Initialize(*config);
        }
        state.store(SHADPS4_READY);
        return 0;
    } catch (const winrt::hresult_error& exception) {
        return Fail("D3D12 HRESULT " +
                    std::to_string(static_cast<uint32_t>(exception.code().value)) + ": " +
                    winrt::to_string(exception.message()));
    } catch (const std::exception& exception) {
        return Fail(exception.what());
    }
}

int32_t shadps4_run(const wchar_t* executable) {
    if (!executable || !*executable) {
        return Fail("No guest executable was selected");
    }
    uint32_t expected = SHADPS4_READY;
    if (!state.compare_exchange_strong(expected, SHADPS4_RUNNING)) {
        return Fail("Only one guest session is supported per application process");
    }
    try {
        const std::filesystem::path path{executable};
        if (!std::filesystem::is_regular_file(path)) {
            state.store(SHADPS4_READY);
            return Fail("The guest executable is not accessible in AppContainer");
        }
        state.store(SHADPS4_READY);
        return Fail("The selected D3D12 source has no integrated PS4 draw/dispatch renderer yet");
    } catch (const std::exception& exception) {
        state.store(SHADPS4_FAILED);
        return Fail(exception.what());
    }
}

void shadps4_request_stop() {
    stop_requested.store(true);
    if (state.load() == SHADPS4_RUNNING) {
        SDL_Event event{};
        event.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&event);
    }
}

void shadps4_set_paused(int32_t paused) {
    pause_requested.store(paused != 0);
    if (state.load() != SHADPS4_RUNNING || !guest_ready.load()) {
        return;
    }
    try {
        if (paused) {
            DebugState.PauseGuestThreads();
        } else {
            DebugState.ResumeGuestThreads();
        }
    } catch (const std::exception& exception) {
        Fail(exception.what());
    }
}

void shadps4_resize(uint32_t width, uint32_t height) {
    try {
        std::scoped_lock lock{presentation_mutex};
        if (presentation) {
            presentation->Resize(width, height);
        }
    } catch (const winrt::hresult_error& exception) {
        Fail("D3D12 HRESULT " + std::to_string(static_cast<uint32_t>(exception.code().value)) +
             ": " + winrt::to_string(exception.message()));
    } catch (const std::exception& exception) {
        Fail(exception.what());
    }
}

int32_t shadps4_render_host_frame() {
    try {
        std::scoped_lock lock{presentation_mutex};
        if (!presentation || state.load() == SHADPS4_UNINITIALIZED) {
            return Fail("Host presentation is not initialized");
        }
        presentation->Render();
        return 0;
    } catch (const winrt::hresult_error& exception) {
        return Fail("D3D12 HRESULT " +
                    std::to_string(static_cast<uint32_t>(exception.code().value)) + ": " +
                    winrt::to_string(exception.message()));
    } catch (const std::exception& exception) {
        return Fail(exception.what());
    }
}

uint32_t shadps4_get_state() {
    return state.load();
}

uint32_t shadps4_get_error(char* destination, uint32_t capacity) {
    std::scoped_lock lock{api_mutex};
    if (destination && capacity) {
        const auto length = std::min<size_t>(capacity - 1, last_error.size());
        std::memcpy(destination, last_error.data(), length);
        destination[length] = 0;
    }
    return static_cast<uint32_t>(last_error.size() + 1);
}
