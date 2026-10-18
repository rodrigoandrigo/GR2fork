#include "pch.h"

#include "ShadPS4Host.h"

#include <thread>
#include <array>
#include <vector>
#include <roapi.h>
#include <windows.ui.xaml.media.dxinterop.h>

using namespace Microsoft::WRL;
using namespace Windows::UI::Core;

ShadPS4Host::ShadPS4Host(Windows::UI::Xaml::Controls::SwapChainPanel ^ target) : panel{target} {}

ShadPS4Host::~ShadPS4Host() {
    Stop();
    Wait();
    {
        std::lock_guard<std::mutex> lock{work_mutex};
        closing = true;
    }
    work_ready.notify_one();
    if (presentation_thread.joinable()) {
        presentation_thread.join();
    }
    const bool released = !shutdown || shutdown() == 0;
    if (set_log_callback) {
        set_log_callback(nullptr, nullptr);
    }
    if (set_error_callback) {
        set_error_callback(nullptr, nullptr);
    }
    if (module && released) {
        FreeLibrary(module);
    }
}

void ShadPS4Host::SetLogConsumer(std::function<void(std::string)> consumer) {
    std::lock_guard<std::mutex> lock{log_mutex};
    log_consumer = std::move(consumer);
}

void ShadPS4Host::Log(void* context, uint32_t, const char* message) {
    if (context) {
        auto* host = static_cast<ShadPS4Host*>(context);
        std::lock_guard<std::mutex> lock{host->log_mutex};
        if (host->log_consumer)
            host->log_consumer(message);
    }
    OutputDebugStringA(message);
    OutputDebugStringA("\n");
}

void ShadPS4Host::ReportError(void*, const char* message) {
    OutputDebugStringA(message);
    OutputDebugStringA("\n");
}

void ShadPS4Host::Initialize(const std::wstring& user_directory, uint32_t width, uint32_t height,
                             bool frontend, bool game_surface) {
    module = LoadPackagedLibrary(L"shadPS4.dll", 0);
    if (!module) {
        throw ref new Platform::FailureException(ref new Platform::String(
            (L"Could not load shadPS4.dll: " + std::to_wstring(GetLastError())).c_str()));
    }
    const auto version = Resolve<decltype(&shadps4_get_api_version)>("shadps4_get_api_version");
    if (version() != 1) {
        throw ref new Platform::FailureException(L"Unsupported shadPS4 DLL interface.");
    }
    initialize = Resolve<decltype(initialize)>("shadps4_initialize");
    state = Resolve<decltype(state)>("shadps4_get_state");
    run = Resolve<decltype(run)>("shadps4_run");
    stop = Resolve<decltype(stop)>("shadps4_request_stop");
    pause = Resolve<decltype(pause)>("shadps4_set_paused");
    resize = Resolve<decltype(resize)>("shadps4_resize_surface");
    render = Resolve<decltype(render)>("shadps4_render_host_frame");
    error = Resolve<decltype(error)>("shadps4_get_error");
    shutdown = Resolve<decltype(shutdown)>("shadps4_shutdown");
    set_log_callback = Resolve<decltype(set_log_callback)>("shadps4_set_log_callback");
    set_error_callback = Resolve<decltype(set_error_callback)>("shadps4_set_error_callback");
    mount_folder = Resolve<decltype(mount_folder)>("shadps4_mount_storage_folder");
    open_file = Resolve<decltype(open_file)>("shadps4_open_mounted_file");
    file_size = Resolve<decltype(file_size)>("shadps4_file_get_size");
    read_file = Resolve<decltype(read_file)>("shadps4_file_read");
    close_file = Resolve<decltype(close_file)>("shadps4_file_close");
    set_log_callback(Log, this);
    set_error_callback(ReportError, nullptr);
    const auto values = Windows::Storage::ApplicationData::Current->LocalSettings->Values;
    graphics_backend = SHADPS4_GRAPHICS_D3D12_NATIVE;
    if (values->HasKey(L"shadps4.graphics.backend")) {
        const auto selected = dynamic_cast<Platform::String^>(values->Lookup(L"shadps4.graphics.backend"));
        if (selected && selected == L"dozen")
            graphics_backend = SHADPS4_GRAPHICS_VULKAN_DOZEN;
    }
    const auto set_backend = Resolve<decltype(&shadps4_set_graphics_backend)>("shadps4_set_graphics_backend");
    if (set_backend(graphics_backend))
        throw ref new Platform::FailureException(ref new Platform::String(Error().c_str()));
    ShadPS4Config config{};
    config.size = sizeof(config);
    config.version = 1;
    config.user_directory = user_directory.c_str();
    config.panel =
        !frontend || game_surface ? reinterpret_cast<IInspectable*>(panel.Get()) : nullptr;
    config.width = width;
    config.height = height;
    config.attach_swapchain = !frontend || game_surface ? AttachSwapChain : nullptr;
    config.attach_context = this;
    if (initialize(&config)) {
        throw ref new Platform::FailureException(ref new Platform::String(Error().c_str()));
    }
    if (!frontend)
        presentation_thread = std::thread([this] { PresentationLoop(); });
}

void ShadPS4Host::PresentationLoop() {
    const HRESULT initialized = RoInitialize(RO_INIT_MULTITHREADED);
    for (;;) {
        std::unique_lock<std::mutex> lock{work_mutex};
        work_ready.wait(lock, [this] { return closing || frame_pending || pending_width; });
        if (closing) {
            break;
        }
        const auto width = pending_width;
        const auto height = pending_height;
        const auto scale_x = pending_scale_x, scale_y = pending_scale_y;
        const bool frame = frame_pending;
        pending_width = 0;
        frame_pending = false;
        lock.unlock();
        if (width) {
            resize(width, height, scale_x, scale_y);
        }
        if (frame && render() != 0) {
            presentation_failed.store(true);
            break;
        }
    }
    if (SUCCEEDED(initialized)) {
        RoUninitialize();
    }
}

void ShadPS4Host::Reload(const std::wstring &user_directory, uint32_t width,
                         uint32_t height, bool frontend, bool game_surface) {
  std::scoped_lock lock{module_mutex};
  stop_pending.store(false);
  Wait();
  if (shutdown && shutdown())
    throw ref new Platform::FailureException(
        ref new Platform::String(Error().c_str()));
  if (set_log_callback)
    set_log_callback(nullptr, nullptr);
  if (set_error_callback)
    set_error_callback(nullptr, nullptr);
  if (module) {
    if (!FreeLibrary(module))
      throw ref new Platform::FailureException(
          L"Não foi possível descarregar o núcleo.");
    module = nullptr;
    if (GetModuleHandleExW(0, L"shadPS4.dll", &module))
      throw ref new Platform::FailureException(
          L"O núcleo ainda possui tarefas ativas; não é seguro iniciar outra "
          L"sessão.");
  }
  initialize = nullptr;
  state = nullptr;
  run = nullptr;
  stop = nullptr;
  pause = nullptr;
  resize = nullptr;
  render = nullptr;
  error = nullptr;
  shutdown = nullptr;
  set_log_callback = nullptr;
  set_error_callback = nullptr;
  Initialize(user_directory, width, height, frontend, game_surface);
  if (stop_pending.load())
    stop();
}

std::wstring ShadPS4Host::Error() const {
  if (!error) {
    return L"The embedded core is unavailable.";
  }
  std::vector<char> text(error(nullptr, 0));
  error(text.data(), static_cast<uint32_t>(text.size()));
  const int length =
      MultiByteToWideChar(CP_UTF8, 0, text.data(), -1, nullptr, 0);
  std::vector<wchar_t> wide(length);
  MultiByteToWideChar(CP_UTF8, 0, text.data(), -1, wide.data(), length);
  return wide.data();
}

std::wstring ShadPS4Host::MountFolder(Platform::Agile<Windows::Storage::StorageFolder> folder) {
    struct Apartment {
        HRESULT result{RoInitialize(RO_INIT_MULTITHREADED)};
        ~Apartment() {
            if (SUCCEEDED(result)) {
                RoUninitialize();
            }
        }
    } apartment;
    if (FAILED(apartment.result)) {
        throw ref new Platform::FailureException(L"Brokered storage requires an MTA worker.");
    }
    if (mount_folder("/app0", reinterpret_cast<IInspectable*>(folder.Get()), 1)) {
        throw ref new Platform::FailureException(ref new Platform::String(Error().c_str()));
    }
    uint64_t token{};
    if (open_file("/app0/eboot.bin", SHADPS4_FILE_READ, &token)) {
        throw ref new Platform::FailureException(ref new Platform::String(Error().c_str()));
    }
    uint64_t length{};
    std::array<uint8_t, 16> header{};
    uint64_t transferred{};
    const auto result =
        file_size(token, &length) || read_file(token, header.data(), header.size(), &transferred);
    const auto message = result ? Error() : std::wstring{};
    close_file(token);
    if (result) {
        throw ref new Platform::FailureException(ref new Platform::String(message.c_str()));
    }
    return L"Pasta vinculada por acesso brokered. eboot.bin: " + std::to_wstring(length) +
           L" bytes. Pronto para iniciar o jogo.";
}

void ShadPS4Host::Run(std::wstring executable,
                      std::function<void(std::wstring)> completed) {
  if (session_thread.joinable())
    session_thread.join();
  session_thread = std::thread([this, executable, completed] {
    const HRESULT initialized = RoInitialize(RO_INIT_MULTITHREADED);
    const auto result = SUCCEEDED(initialized) ? run(executable.c_str()) : -1;
    const auto message = FAILED(initialized)
                             ? L"Não foi possível iniciar o worker MTA."
                         : result ? Error()
                                  : L"Sessão encerrada.";
    if (SUCCEEDED(initialized)) {
      RoUninitialize();
    }
    completed(message);
  });
}

void ShadPS4Host::Wait() {
  if (session_thread.joinable())
    session_thread.join();
}

void ShadPS4Host::Pause(bool paused) {
    if (pause) {
        pause(paused);
    }
}

void ShadPS4Host::Stop() {
  stop_pending.store(true);
  std::unique_lock lock{module_mutex, std::try_to_lock};
  if (lock.owns_lock() && stop) {
    stop();
  }
}

void ShadPS4Host::Resize(uint32_t width, uint32_t height, float scale_x, float scale_y) {
  if (graphics_backend == SHADPS4_GRAPHICS_VULKAN_DOZEN && scale_x > 0 && scale_y > 0) {
    std::scoped_lock lock{chain_mutex};
    if (composition_chain && (chain_scale_x != scale_x || chain_scale_y != scale_y)) {
      const DXGI_MATRIX_3X2_F transform{1.0f / scale_x, 0, 0, 1.0f / scale_y, 0, 0};
      const auto result = composition_chain->SetMatrixTransform(&transform);
      if (FAILED(result))
        throw ref new Platform::Exception(result);
      chain_scale_x = scale_x;
      chain_scale_y = scale_y;
    }
  }
  if (!presentation_thread.joinable()) {
    if (resize && width && height)
      resize(width, height, scale_x, scale_y);
    return;
  }
  if (width && height) {
    std::lock_guard<std::mutex> lock{work_mutex};
    pending_width = width;
    pending_height = height;
    pending_scale_x = scale_x;
    pending_scale_y = scale_y;
  }
  work_ready.notify_one();
}

bool ShadPS4Host::Render() {
    if (presentation_failed.load()) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock{work_mutex};
        frame_pending = true;
    }
    work_ready.notify_one();
    return true;
}

long ShadPS4Host::AttachSwapChain(void* context, void* swapchain) {
    auto* self = static_cast<ShadPS4Host*>(context);
    struct Attachment {
        HANDLE event =
            CreateEventExW(nullptr, nullptr, CREATE_EVENT_MANUAL_RESET, EVENT_ALL_ACCESS);
        HRESULT result{E_FAIL};
        ComPtr<IDXGISwapChain> chain;
        ~Attachment() {
            if (event) {
                CloseHandle(event);
            }
        }
    };
    const auto attachment = std::make_shared<Attachment>();
    if (!attachment->event) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    attachment->chain = static_cast<IDXGISwapChain*>(swapchain);
    auto target = self->panel;
    const auto attach = [self, target, attachment] {
        ComPtr<ISwapChainPanelNative> native;
        attachment->result =
            reinterpret_cast<IInspectable*>(target.Get())->QueryInterface(IID_PPV_ARGS(&native));
        if (SUCCEEDED(attachment->result)) {
            attachment->result = native->SetSwapChain(attachment->chain.Get());
            if (SUCCEEDED(attachment->result)) {
                std::scoped_lock lock{self->chain_mutex};
                self->composition_chain.Reset();
                if (attachment->chain)
                    attachment->result = attachment->chain.As(&self->composition_chain);
                if (SUCCEEDED(attachment->result) && self->composition_chain) {
                    const DXGI_MATRIX_3X2_F transform{
                        1.0f / target->CompositionScaleX, 0, 0,
                        1.0f / target->CompositionScaleY, 0, 0};
                    attachment->result = self->composition_chain->SetMatrixTransform(&transform);
                    self->chain_scale_x = target->CompositionScaleX;
                    self->chain_scale_y = target->CompositionScaleY;
                }
            }
        }
        if (SUCCEEDED(attachment->result) && attachment->chain.Get()) {
            const auto sdl = LoadPackagedLibrary(L"SDL3.dll", 0);
            if (!sdl) {
                attachment->result = HRESULT_FROM_WIN32(GetLastError());
            } else {
                using AttachPanel = bool(__cdecl*)(void*);
                const auto attach_panel =
                    reinterpret_cast<AttachPanel>(GetProcAddress(sdl, "SDL_WinRTAttachXAMLPanel"));
                attachment->result = !attach_panel ? HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND)
                                     : attach_panel(reinterpret_cast<IInspectable*>(target.Get()))
                                         ? S_OK
                                         : E_FAIL;
                FreeLibrary(sdl);
            }
        }
        SetEvent(attachment->event);
    };
    if (target->Dispatcher->HasThreadAccess) {
        attach();
    } else {
        target->Dispatcher->RunAsync(CoreDispatcherPriority::Normal,
                                     ref new DispatchedHandler(attach));
        if (WaitForSingleObjectEx(attachment->event, 10000, FALSE) != WAIT_OBJECT_0) {
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
    }
    return attachment->result;
}
