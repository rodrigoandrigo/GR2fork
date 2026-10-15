#include "uwp/session.h"
#include "uwp/shadps4_api.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
#include <SDL3/SDL.h>
#include <windows.h>
#include <winrt/Windows.System.h>
#include <winrt/base.h>
#include "common/key_manager.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/scope_exit.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/libraries/audio/audioout_backend.h"
#include "core/user_settings.h"
#include "emulator.h"
#include "uwp/dozen_host.h"
#include "uwp/gamepad_input.h"
#include "uwp/xaudio2_output.h"

namespace {
std::atomic<uint32_t> state{SHADPS4_UNINITIALIZED};
std::atomic_bool stop_requested{};
std::atomic_bool pause_requested{};
std::atomic_bool guest_ready{};
std::atomic_bool guest_started{};
std::atomic_bool guest_threads_stopped{true};
std::atomic_bool session_failed{};
std::mutex session_mutex;
std::mutex guest_control_mutex;
std::mutex worker_stop_mutex;
std::vector<std::stop_source> worker_stops;
std::mutex api_mutex;
std::string last_error;
std::shared_ptr<Core::Uwp::DozenHost> presentation;
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
void RegisterGuestWorker(std::stop_source source) {
    {
        std::scoped_lock lock{worker_stop_mutex};
        worker_stops.push_back(source);
    }
    if (StopRequested())
        source.request_stop();
}
void StopGuestWorkers() {
    std::vector<std::stop_source> sources;
    {
        std::scoped_lock lock{worker_stop_mutex};
        sources = worker_stops;
    }
    for (auto& source : sources)
        source.request_stop();
}
void ReportError(std::string message) {
    Fail(std::move(message));
}
void FailGuestSession(std::string message) {
    session_failed.store(true);
    stop_requested.store(true);
    StopGuestWorkers();
    guest_ready.store(false);
    Fail(std::move(message));
}
bool StopRequested() {
    return stop_requested.load();
}

bool PauseRequested() {
    return pause_requested.load();
}

void SetGuestReady(bool ready) {
    std::scoped_lock lock{guest_control_mutex};
    guest_ready.store(ready && !stop_requested.load());
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
    std::unique_lock session_lock{session_mutex, std::try_to_lock};
    if (!session_lock.owns_lock() || state.load() == SHADPS4_RUNNING) {
        return Fail("Wait for the guest session to return before shutdown");
    }
    if (!guest_threads_stopped.load()) {
        try {
            Common::Singleton<Core::Emulator>::Instance()->StopGuest();
            guest_threads_stopped.store(true);
        } catch (const std::exception& exception) {
            return Fail(exception.what());
        }
    }
    try {
        stop_requested.store(true);
        std::scoped_lock lock{presentation_mutex};
        presentation.reset();
        Core::Uwp::ShutdownDozenDriver();
        Core::Uwp::StopGamepadInput();
        Libraries::AudioOut::ShutdownAudioOut();
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
        Core::Uwp::DozenHost::CheckEnvironment();
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
    SCOPE_EXIT {
        if (state.load() == SHADPS4_UNINITIALIZED) {
            stop_requested.store(true);
            presentation.reset();
            panel = nullptr;
        }
    };
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
            presentation = std::make_shared<Core::Uwp::DozenHost>();
            presentation->Initialize(*config);
        }
        Core::Uwp::SuspendAudio(false);
        Core::Uwp::StartGamepadInput();
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

int32_t shadps4_set_host_state(int32_t active, int32_t input_captured) {
    if ((active != 0 && active != 1) || (input_captured != 0 && input_captured != 1) ||
        state.load() == SHADPS4_UNINITIALIZED)
        return Fail("Invalid host input/audio state");
    try {
        Core::Uwp::CaptureGamepadInput(!active || input_captured);
        Core::Uwp::SuspendAudio(!active);
        return 0;
    } catch (const winrt::hresult_error& exception) {
        return Fail("XAudio2 HRESULT " +
                    std::to_string(static_cast<uint32_t>(exception.code().value)) + ": " +
                    winrt::to_string(exception.message()));
    }
}

int32_t shadps4_run(const wchar_t* executable) {
    if (!executable || !*executable) {
        return Fail("No guest executable was selected");
    }
    std::unique_lock session_lock{session_mutex, std::try_to_lock};
    if (!session_lock.owns_lock())
        return Fail("A guest session is already running");
    if (guest_started.load())
        return Fail("Reload the core DLL before starting another guest session");
    uint32_t expected = SHADPS4_READY;
    if (!state.compare_exchange_strong(expected, SHADPS4_RUNNING)) {
        return Fail("The core is not ready for a guest session");
    }
    Core::Emulator* emulator{};
    try {
        APTTYPE apartment;
        APTTYPEQUALIFIER qualifier;
        winrt::check_hresult(CoGetApartmentType(&apartment, &qualifier));
        if (apartment != APTTYPE_MTA && apartment != APTTYPE_NA)
            throw std::runtime_error("Guest boot requires an MTA worker");
        if (!presentation)
            throw std::runtime_error("Guest boot requires a host game presentation surface");
        const auto path =
            Core::Uwp::PrepareBootStorage(winrt::to_string(std::wstring_view{executable}));
        {
            std::scoped_lock lock{api_mutex};
            last_error.clear();
        }
        session_failed.store(false);
        guest_started.store(true);
        guest_threads_stopped.store(false);
        try {
            const auto usage = winrt::Windows::System::MemoryManager::AppMemoryUsage();
            const auto limit = winrt::Windows::System::MemoryManager::AppMemoryUsageLimit();
            LOG_INFO(Core, "Host app memory: usage {} MiB, limit {} MiB", usage / (1024 * 1024),
                     limit / (1024 * 1024));
        } catch (const winrt::hresult_error& error) {
            LOG_INFO(Core, "Host app memory budget unavailable: {}",
                     winrt::to_string(error.message()));
        }
        emulator = Common::Singleton<Core::Emulator>::Instance();
        emulator->Run(std::filesystem::path{path});
        guest_threads_stopped.store(true);
        state.store(session_failed.load() ? SHADPS4_FAILED : SHADPS4_STOPPED);
        return session_failed.load() ? -1 : 0;
    } catch (const std::exception& exception) {
        Fail(exception.what());
    } catch (const winrt::hresult_error& exception) {
        Fail(winrt::to_string(exception.message()));
    } catch (...) {
        Fail("Unknown embedded boot error");
    }
    stop_requested.store(true);
    guest_ready.store(false);
    if (emulator) {
        try {
            emulator->StopGuest();
            guest_threads_stopped.store(true);
        } catch (const std::exception& exception) {
            Fail(exception.what());
        }
    } else {
        guest_threads_stopped.store(true);
    }
    state.store(guest_started.load() ? SHADPS4_FAILED : SHADPS4_READY);
    if (!guest_started.load())
        stop_requested.store(false);
    return -1;
}

void shadps4_request_stop() {
    stop_requested.store(true);
    Core::Uwp::StopGuestWorkers();
    if (state.load() == SHADPS4_RUNNING) {
        SDL_Event event{};
        event.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&event);
    }
}

void shadps4_set_paused(int32_t paused) {
    std::scoped_lock lock{guest_control_mutex};
    pause_requested.store(paused != 0);
    if (state.load() != SHADPS4_RUNNING || !guest_ready.load() || stop_requested.load()) {
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
