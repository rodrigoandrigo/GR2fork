#include "pch.h"
#include "UwpDiagnostics.h"

#include "ImGuiFrontend.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <numeric>
#include <ppltasks.h>
#include <roapi.h>
#include <wincodec.h>
#include <windows.ui.xaml.media.dxinterop.h>
#include <winrt/base.h>

using Microsoft::WRL::ComPtr;
using namespace Windows::Storage;
using namespace Windows::UI::Core;

namespace {
void Check(HRESULT result) {
    if (FAILED(result))
        throw std::runtime_error("HRESULT " + std::to_string(static_cast<uint32_t>(result)));
}
std::string Utf8(const std::wstring& text) {
    if (text.empty())
        return {};
    const auto count =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), text.size(), nullptr, 0, nullptr, nullptr);
    std::string output(count, 0);
    WideCharToMultiByte(CP_UTF8, 0, text.data(), text.size(), output.data(), count, nullptr,
                        nullptr);
    return output;
}
std::wstring Wide(const std::string& text) {
    if (text.empty())
        return {};
    const auto count = MultiByteToWideChar(CP_UTF8, 0, text.data(), text.size(), nullptr, 0);
    std::wstring output(count, 0);
    MultiByteToWideChar(CP_UTF8, 0, text.data(), text.size(), output.data(), count);
    return output;
}
std::string Lower(std::string text) {
    for (auto& c : text)
        if (static_cast<unsigned char>(c) < 128)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}
std::string Label(std::string text) {
    static const std::unordered_map<std::string, std::string> labels{
        {"big_picture_scale", "Escala da interface (1000 = 100%)"},
        {"big_picture_folder_depth", "Profundidade de busca de jogos"},
        {"memory_swap_enabled", "Swap de memória (padrão no Xbox)"},
        {"show_fps_counter", "Exibir contador de FPS"},
        {"console_language", "Idioma do console"},
        {"volume_slider", "Volume (%)"},
        {"audio_backend", "Backend de áudio"},
        {"neo_mode", "Emular PS4 Pro"},
        {"dev_kit_mode", "Modo de desenvolvimento"},
        {"extra_dmem_in_mbytes", "Memória direta adicional (MB)"},
        {"extra_fmem_in_mbytes", "Memória flexível adicional (MB)"},
        {"full_screen", "Iniciar em tela cheia"},
        {"full_screen_mode", "Modo de tela cheia"},
        {"window_width", "Largura da janela"},
        {"window_height", "Altura da janela"},
        {"internal_screen_width", "Largura da resolução interna"},
        {"internal_screen_height", "Altura da resolução interna"},
        {"hdr_allowed", "Habilitar HDR"},
        {"fsr_enabled", "Habilitar FSR"},
        {"rcas_enabled", "Habilitar RCAS (nitidez)"},
        {"rcas_attenuation", "Atenuação RCAS"},
        {"vblank_frequency", "Frequência de VBlank"},
        {"null_gpu", "Habilitar NULL GPU"},
        {"gpu_id", "Dispositivo gráfico"},
        {"pipeline_cache_enabled", "Habilitar cache de pipelines"},
        {"pipeline_cache_archived", "Compactar cache de pipelines"},
        {"use_keyboard_as_keyboard", "Usar teclado como teclado do PS4"},
        {"use_mice_as_mice", "Usar mouse como mouse do PS4"},
        {"is_circle_enter", "Usar círculo para confirmar"},
        {"use_unified_input_config", "Compartilhar configuração de entrada"},
        {"special_pad_class", "Tipo de controle especial"},
        {"default_controller_id", "Controle padrão"},
        {"vblank_div", "Divisor de VBlank"},
        {"is_pro", "Emular PS4 Pro"},
        {"fullscreen", "Iniciar jogos em tela cheia"},
        {"log_filter", "Filtro de log"},
        {"flush_level", "Nível de gravação do log"}};
    if (const auto found = labels.find(text); found != labels.end())
        return found->second;
    std::replace(text.begin(), text.end(), '_', ' ');
    if (!text.empty())
        text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
    return text;
}
void LoadLauncherFont() {
    static const ImWchar ranges[]{0x0020, 0x00ff, 0x2000, 0x206f, 0};
    try {
        ComPtr<IDWriteFactory> factory;
        Check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                  reinterpret_cast<IUnknown**>(factory.GetAddressOf())));
        ComPtr<IDWriteFontCollection> collection;
        Check(factory->GetSystemFontCollection(&collection));
        UINT32 index{};
        BOOL exists{};
        Check(collection->FindFamilyName(L"Segoe UI", &index, &exists));
        if (!exists)
            throw std::runtime_error("Segoe UI");
        ComPtr<IDWriteFontFamily> family;
        Check(collection->GetFontFamily(index, &family));
        ComPtr<IDWriteFont> font;
        Check(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                           DWRITE_FONT_STYLE_NORMAL, &font));
        ComPtr<IDWriteFontFace> face;
        Check(font->CreateFontFace(&face));
        UINT32 count{};
        Check(face->GetFiles(&count, nullptr));
        if (count != 1)
            throw std::runtime_error("Font files");
        ComPtr<IDWriteFontFile> file;
        Check(face->GetFiles(&count, file.GetAddressOf()));
        const void* key{};
        UINT32 key_size{};
        Check(file->GetReferenceKey(&key, &key_size));
        ComPtr<IDWriteFontFileLoader> loader;
        Check(file->GetLoader(&loader));
        ComPtr<IDWriteFontFileStream> stream;
        Check(loader->CreateStreamFromKey(key, key_size, &stream));
        UINT64 size{};
        Check(stream->GetFileSize(&size));
        if (!size || size > 32 * 1024 * 1024)
            throw std::runtime_error("Font size");
        auto* memory = IM_ALLOC(static_cast<size_t>(size));
        if (!memory)
            throw std::bad_alloc();
        const void* fragment{};
        void* context{};
        const auto result = stream->ReadFileFragment(&fragment, 0, size, &context);
        if (FAILED(result)) {
            IM_FREE(memory);
            Check(result);
        }
        std::memcpy(memory, fragment, static_cast<size_t>(size));
        stream->ReleaseFileFragment(context);
        if (ImGui::GetIO().Fonts->AddFontFromMemoryTTF(memory, static_cast<int>(size), 15.0f,
                                                       nullptr, ranges))
            return;
    } catch (...) {
    }
    ImFontConfig font;
    font.SizePixels = 15.0f;
    ImGui::GetIO().Fonts->AddFontDefault(&font);
}
std::string SettingsSection(const std::string& group, const std::string& key) {
    if (group == "Vulkan")
        return key == "gpu_id" || key.compare(0, 15, "pipeline_cache_") == 0 ? "Dispositivo e cache"
                                                                             : "Avançado";
    if (group == "GPU") {
        if (key == "gpu_id" || key == "null_gpu")
            return "Dispositivo e cache";
        if (key == "fsr_enabled" || key == "rcas_enabled" || key == "rcas_attenuation")
            return "FSR";
        if (key == "full_screen" || key == "full_screen_mode" || key == "hdr_allowed" ||
            key == "window_width" || key == "window_height" || key == "internal_screen_width" ||
            key == "internal_screen_height" || key == "vblank_frequency")
            return "Vídeo";
        return "Avançado";
    }
    if (group == "Input")
        return key == "use_keyboard_as_keyboard" || key == "use_mice_as_mice" ||
                       key.compare(0, 4, "ime_") == 0
                   ? "Teclado e mouse"
                   : "Controle";
    if (group == "Audio" || key == "volume_slider")
        return "Saída de áudio";
    return "Opções";
}
std::string SettingsPage(const std::string& group, const std::string& key) {
    if (group == "GPU" || group == "Vulkan")
        return "Gráficos";
    if (group == "Input")
        return "Entrada";
    if (group == "Audio")
        return "Áudio";
    if (group == "Network")
        return "Rede";
    if (group == "General") {
        if (key == "volume_slider")
            return "Áudio";
        if (key == "big_picture_folder_depth")
            return "Pastas";
        if (key.compare(0, 12, "big_picture_") == 0 || key == "show_fps_counter")
            return "Interface";
        if (key == "console_language" || key.compare(0, 5, "user_") == 0)
            return "Usuário";
        return "Geral";
    }
    return group;
}
void LauncherStyle() {
    auto& style = ImGui::GetStyle();
    style.WindowRounding = 0;
    style.ChildRounding = 0;
    style.FrameRounding = 2;
    style.TabRounding = 2;
    style.WindowBorderSize = 1;
    style.FrameBorderSize = 1;
    style.WindowPadding = {10, 8};
    style.FramePadding = {8, 5};
    style.ItemSpacing = {8, 6};
    const ImVec4 window{50 / 255.0f, 50 / 255.0f, 50 / 255.0f, 1};
    const ImVec4 base{20 / 255.0f, 20 / 255.0f, 20 / 255.0f, 1};
    const ImVec4 button{53 / 255.0f, 53 / 255.0f, 53 / 255.0f, 1};
    const ImVec4 accent{42 / 255.0f, 130 / 255.0f, 218 / 255.0f, 1};
    style.Colors[ImGuiCol_TitleBg] = window;
    style.Colors[ImGuiCol_TitleBgActive] = window;
    style.Colors[ImGuiCol_WindowBg] = window;
    style.Colors[ImGuiCol_ChildBg] = base;
    style.Colors[ImGuiCol_PopupBg] = window;
    style.Colors[ImGuiCol_MenuBarBg] = window;
    style.Colors[ImGuiCol_FrameBg] = {0.12f, 0.12f, 0.12f, 1};
    style.Colors[ImGuiCol_Button] = button;
    style.Colors[ImGuiCol_ButtonHovered] = {0.28f, 0.28f, 0.28f, 1};
    style.Colors[ImGuiCol_ButtonActive] = accent;
    style.Colors[ImGuiCol_Header] = {0.2f, 0.2f, 0.2f, 1};
    style.Colors[ImGuiCol_HeaderHovered] = {0.22f, 0.36f, 0.49f, 1};
    style.Colors[ImGuiCol_HeaderActive] = accent;
    style.Colors[ImGuiCol_TableHeaderBg] = button;
    style.Colors[ImGuiCol_TableRowBg] = base;
    style.Colors[ImGuiCol_TableRowBgAlt] = {0.085f, 0.085f, 0.085f, 1};
    style.Colors[ImGuiCol_Tab] = button;
    style.Colors[ImGuiCol_TabActive] = {0.26f, 0.26f, 0.26f, 1};
    style.Colors[ImGuiCol_TabHovered] = accent;
    style.Colors[ImGuiCol_CheckMark] = accent;
    style.Colors[ImGuiCol_SliderGrab] = accent;
    style.Colors[ImGuiCol_Border] = {0.32f, 0.32f, 0.32f, 1};
    style.Colors[ImGuiCol_Separator] = {0.4f, 0.4f, 0.4f, 1};
}
std::unordered_map<std::string, std::string> Sfo(const std::vector<uint8_t>& bytes) {
    std::unordered_map<std::string, std::string> result;
    const auto u32 = [&](size_t offset) {
        uint32_t value{};
        if (offset <= bytes.size() && bytes.size() - offset >= 4)
            std::memcpy(&value, bytes.data() + offset, 4);
        return value;
    };
    if (bytes.size() < 20 || u32(0) != 0x46535000)
        return result;
    const auto keys = u32(8), data = u32(12), count = u32(16);
    if (count > 1024 || keys >= bytes.size() || data >= bytes.size() ||
        count > (bytes.size() - 20) / 16)
        return result;
    for (uint32_t index = 0; index < count; ++index) {
        const size_t record = 20 + index * 16;
        uint16_t key{}, format{};
        std::memcpy(&key, bytes.data() + record, 2);
        std::memcpy(&format, bytes.data() + record + 2, 2);
        const auto length = u32(record + 4), offset = u32(record + 12);
        if ((format != 0x0204 && format != 0x0404) || key >= bytes.size() - keys ||
            offset > bytes.size() - data || length > bytes.size() - data - offset)
            continue;
        const auto* name = reinterpret_cast<const char*>(bytes.data() + keys + key);
        const auto* end = static_cast<const char*>(std::memchr(name, 0, bytes.size() - keys - key));
        if (!end)
            continue;
        if (format == 0x0404) {
            if (length >= 4)
                result.emplace(std::string(name, end), std::to_string(u32(data + offset)));
            continue;
        }
        const auto* value = reinterpret_cast<const char*>(bytes.data() + data + offset);
        const auto* last = static_cast<const char*>(std::memchr(value, 0, length));
        result.emplace(std::string(name, end), std::string(value, last ? last : value + length));
    }
    return result;
}
void CopyFolder(StorageFolder ^ source, StorageFolder ^ target, const std::atomic_bool& stop) {
    const auto items = concurrency::create_task(source->GetItemsAsync()).get();
    for (auto item : items) {
        if (stop.load())
            return;
        if (item->IsOfType(StorageItemTypes::File)) {
            concurrency::create_task(safe_cast<StorageFile ^>(item)->CopyAsync(
                                         target, item->Name, NameCollisionOption::ReplaceExisting))
                .get();
        } else {
            auto destination =
                concurrency::create_task(
                    target->CreateFolderAsync(item->Name, CreationCollisionOption::OpenIfExists))
                    .get();
            CopyFolder(safe_cast<StorageFolder ^>(item), destination, stop);
        }
    }
}
} // namespace

ImGuiFrontend::ImGuiFrontend(Windows::UI::Xaml::Controls::SwapChainPanel ^ target,
                             Windows::UI::Xaml::Controls::SwapChainPanel ^ game_panel)
    : core{std::make_shared<ShadPS4Host>(game_panel ? game_panel : target)},
      game_surface{game_panel != nullptr}, panel{target}, dispatcher{target->Dispatcher} {}
ImGuiFrontend::~ImGuiFrontend() { Stop(); }
void ImGuiFrontend::Start(std::wstring root) {
    const auto self = shared_from_this();
    std::thread([self, root] { self->Run(root); }).detach();
}
void ImGuiFrontend::Stop() {
    stopping.store(true);
    core->Stop();
    work_ready.notify_all();
}
void ImGuiFrontend::UpdateInput(const std::function<void(FrontendInput&)>& update) {
    std::lock_guard<std::mutex> lock{input_mutex};
    update(input);
}
bool ImGuiFrontend::Queue(std::function<void()> job, bool thumbnail) {
    std::lock_guard<std::mutex> lock{work_mutex};
    auto& queue = thumbnail ? thumbnail_jobs : commands;
    if (stopping.load() || queue.size() >= 32)
        return false;
    queue.push_back(std::move(job));
    work_ready.notify_one();
    return true;
}
void ImGuiFrontend::Complete(std::function<void()> update) {
    std::unique_lock<std::mutex> lock{work_mutex};
    work_ready.wait(lock, [this] {
        return stopping.load() || worker_stopping.load() || completed.size() < 32;
    });
    if (!stopping.load() && !worker_stopping.load())
        completed.push_back(std::move(update));
}
void ImGuiFrontend::Work() {
    const auto initialized = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(initialized)) {
        Complete([this] { status = "Não foi possível criar o worker MTA."; });
        return;
    }
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock{work_mutex};
            work_ready.wait(lock, [this] {
                return stopping.load() || worker_stopping.load() || !commands.empty() ||
                       filter_job || !thumbnail_jobs.empty();
            });
            if (stopping.load() || worker_stopping.load())
                break;
            if (commands.empty() && filter_job) {
                job = std::move(filter_job);
                filter_job = {};
            } else {
                auto& queue = commands.empty() ? thumbnail_jobs : commands;
                job = std::move(queue.front());
                queue.pop_front();
            }
        }
        try {
            job();
        } catch (Platform::Exception ^ error) {
            const auto message = Utf8(error->Message->Data());
            Complete([this, message] {
                status = message;
                scanning.store(false);
            });
        } catch (const std::exception& error) {
            const std::string message = error.what();
            Complete([this, message] {
                status = message;
                scanning.store(false);
            });
        }
    }
    RoUninitialize();
}

HRESULT ImGuiFrontend::Attach(IDXGISwapChain* chain) {
    struct Binding {
        ComPtr<IDXGISwapChain> chain;
        HANDLE event{CreateEventExW(nullptr, nullptr, CREATE_EVENT_MANUAL_RESET, EVENT_ALL_ACCESS)};
        HRESULT result{E_FAIL};
        ~Binding() {
            if (event)
                CloseHandle(event);
        }
    };
    const auto binding = std::make_shared<Binding>();
    binding->chain = chain;
    if (!binding->event)
        return HRESULT_FROM_WIN32(GetLastError());
    const auto target = panel;
    dispatcher->RunAsync(CoreDispatcherPriority::Normal,
                         ref new DispatchedHandler([target, binding] {
                             ComPtr<ISwapChainPanelNative> native;
                             binding->result = reinterpret_cast<IInspectable*>(target.Get())
                                                   ->QueryInterface(IID_PPV_ARGS(&native));
                             if (SUCCEEDED(binding->result))
                                 binding->result = native->SetSwapChain(binding->chain.Get());
                             SetEvent(binding->event);
                         }));
    if (WaitForSingleObjectEx(binding->event, 10000, FALSE) != WAIT_OBJECT_0)
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    return binding->result;
}

void ImGuiFrontend::ResolveApi() {
#define RESOLVE(member, name) member = core->GetApi<decltype(member)>(name)
    RESOLVE(get_settings, "shadps4_get_settings");
    RESOLVE(get_game_frame_count, "shadps4_get_game_frame_count");
    RESOLVE(cheats_request, "shadps4_cheats_request");
    RESOLVE(set_host_state, "shadps4_set_host_state");
    RESOLVE(get_text_input, "shadps4_get_text_input");
    RESOLVE(complete_text_input, "shadps4_complete_text_input");
    RESOLVE(set_hdr_output, "shadps4_set_hdr_output");
    RESOLVE(set_setting, "shadps4_set_setting");
    RESOLVE(reset_settings, "shadps4_reset_settings");
    RESOLVE(mount, "shadps4_mount_storage_folder");
    RESOLVE(mount_pkg, "shadps4_mount_pkg");
    RESOLVE(mount_pkg_file, "shadps4_mount_pkg_file");
    RESOLVE(unmount, "shadps4_unmount_storage_folder");
    RESOLVE(enumerate, "shadps4_storage_enumerate");
    RESOLVE(stat, "shadps4_storage_stat");
    RESOLVE(open, "shadps4_open_mounted_file");
    RESOLVE(read, "shadps4_file_read");
    RESOLVE(size, "shadps4_file_get_size");
    RESOLVE(close, "shadps4_file_close");
    RESOLVE(get_game_settings, "shadps4_get_game_settings");
    RESOLVE(set_game_setting, "shadps4_set_game_setting");
    RESOLVE(delete_game_settings, "shadps4_delete_game_settings");
#undef RESOLVE
}

void ImGuiFrontend::ReportFailure(const std::wstring& message) noexcept {
    try {
        const auto directory = std::filesystem::path(user_root) / L"log";
        std::filesystem::create_directories(directory);
        std::ofstream output(directory / L"frontend-d3d12.log", std::ios::app);
        const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count();
        output << timestamp << " " << Utf8(message) << std::endl;
    } catch (...) {
    }
    try {
        if (failure)
            failure(message);
    } catch (...) {
    }
}

void ImGuiFrontend::Run(std::wstring root) {
    const auto apartment = RoInitialize(RO_INIT_MULTITHREADED);
    try {
        Check(apartment);
        user_root = root;
        try {
            const auto values = ApplicationData::Current->LocalSettings->Values;
            if (values->HasKey(L"shadps4.launcher.layout")) {
                const auto stored =
                    safe_cast<Platform::String ^>(values->Lookup(L"shadps4.launcher.layout"));
                const auto layout = nlohmann::json::parse(Utf8(stored->Data()));
                show_game_list = layout.value("game_list", true);
                show_game_log = layout.value("game_log", true);
                grid_view = layout.value("grid", false);
                icon_size = std::clamp(layout.value("icon_size", 48.0f), 24.0f, 128.0f);
                log_height = std::clamp(layout.value("log_height", 140.0f), 60.0f, 600.0f);
            }
        } catch (...) {
            status = "Preferências de interface restauradas.";
        }

        FrontendInput initial;
        {
            std::lock_guard<std::mutex> lock{input_mutex};
            initial = input;
        }
        core->SetLogConsumer([this](std::string message) {
            std::lock_guard<std::mutex> lock{work_mutex};
            if (pending_logs.size() == 256)
                pending_logs.pop_front();
            pending_logs.push_back(message.substr(0, 4096));
        });
        D3D12DeviceContext::EnableDiagnostics();
        core->Initialize(root, initial.width, initial.height, true, game_surface);
        ResolveApi();
        set_hdr_output(initial.hdr_output, initial.sdr_white_nits);
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        ImGui::StyleColorsDark();
        LauncherStyle();
        LoadLauncherFont();
        renderer = std::make_unique<FrontendRenderer>();
        renderer->Initialize(initial.width, initial.height,
                             [this](IDXGISwapChain* chain) { return Attach(chain); });
        worker = std::thread([this] { Work(); });
        Queue([this] { LoadSettings(); });
        if (ready)
            ready();
        auto last = std::chrono::steady_clock::now();
        auto last_memory_sample = last;
        while (!stopping.load()) {
            const auto memory_time = std::chrono::steady_clock::now();
            if (memory_time - last_memory_sample >= std::chrono::seconds(1)) {
                UwpDiagnostics::Memory("memory_sample bytes/limit");
                last_memory_sample = memory_time;
            }
            FrontendInput snapshot;
            std::deque<std::function<void()>> text_completed;
            {
                std::lock_guard<std::mutex> lock{input_mutex};
                snapshot = input;
                text_completed.swap(text_input_results);
                input.wheel = 0;
                input.characters.clear();
            }
            for (auto& complete : text_completed)
                complete();
            std::deque<std::function<void()>> updates;
            {
                std::lock_guard<std::mutex> lock{work_mutex};
                updates.swap(completed);
                while (!pending_logs.empty()) {
                    logs.push_back(std::move(pending_logs.front()));
                    pending_logs.pop_front();
                    if (logs.size() > 512)
                        logs.pop_front();
                }
            }
            work_ready.notify_all();
            for (auto& update : updates)
                update();
            if (reload_core) {
                worker_stopping.store(true);
                work_ready.notify_all();
                if (worker.joinable())
                    worker.join();
                {
                    std::lock_guard<std::mutex> lock{work_mutex};
                    commands.clear();
                    thumbnail_jobs.clear();
                    filter_job = {};
                    completed.clear();
                }
                pending_images.clear();
                decoded.clear();
                core->Reload(user_root, snapshot.width, snapshot.height, true, game_surface);
                ResolveApi();
                root_mounts.clear();
                reload_core = false;
                session_pending = false;
                session_paused = false;
                worker_stopping.store(false);
                worker = std::thread([this] { Work(); });
                scanning.store(true);
                Queue([this] {
                    Scan();
                    LoadSettings({}, settings_request.load());
                });
            }
            const bool running = core->State() == SHADPS4_RUNNING;
            ShadPS4TextInput text_request{};
            text_request.size = sizeof(text_request);
            if (get_text_input(&text_request))
                throw std::runtime_error("Unable to query game text input");
            if (text_request.id != text_input_id && game_text_input) {
                text_input_id = text_request.id;
                const std::weak_ptr<ImGuiFrontend> weak = shared_from_this();
                game_text_input(text_request, [weak, id = text_request.id](bool confirmed,
                                                                            std::wstring text) {
                    if (auto owner = weak.lock()) {
                        std::lock_guard<std::mutex> lock{owner->input_mutex};
                        owner->text_input_results.emplace_back([target = owner.get(), id, confirmed,
                                                     text = std::move(text)] {
                            if (target->complete_text_input(id, confirmed, text.c_str())) {
                                ShadPS4TextInput current{};
                                current.size = sizeof(current);
                                if (!target->get_text_input(&current) && current.id == id)
                                    target->text_input_id = 0;
                            }
                        });
                    }
                });
            }
            if (running != game_running) {
                game_running = running;
                menu_open = !running;
            }
            if (restart_requested && !game_running && !session_pending && !scanning.load() &&
                core->State() == SHADPS4_READY && library) {
                restart_requested = false;
                const auto found = std::find_if(
                    library->games.begin(), library->games.end(),
                    [this](const auto& game) { return game.path == active_game_path; });
                if (found != library->games.end()) {
                    selection = static_cast<size_t>(found - library->games.begin());
                    StartGame();
                }
            }
            const bool pressed = snapshot.keys[27];
            if (game_running && pressed && !menu_pressed)
                menu_open = !menu_open;
            menu_pressed = pressed;
            const bool show_pointer = (!(game_running || session_pending) || menu_open || snapshot.captured);
            if (show_pointer != pointer_visible) {
                pointer_visible = show_pointer;
                if (set_pointer_mode)
                    set_pointer_mode(show_pointer);
            }
            if (set_host_state(
                    !snapshot.suspended,
                    !snapshot.active || !snapshot.visible || snapshot.captured || snapshot.system_pointer ||
                        (game_running ? menu_open : io.NavActive || io.WantCaptureKeyboard)))
                throw std::runtime_error(Utf8(core->Error()));
            if (snapshot.suspended || !snapshot.active || !snapshot.visible) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            set_hdr_output(snapshot.hdr_output, snapshot.sdr_white_nits);
            renderer->Resize(snapshot.width, snapshot.height, snapshot.scale_x, snapshot.scale_y);
            if (game_surface)
                core->Resize(snapshot.width, snapshot.height, snapshot.scale_x, snapshot.scale_y);
            renderer->SetTransparent(game_running);
            const auto now = std::chrono::steady_clock::now();
            static auto fps_time = now;
            static uint64_t fps_count = get_game_frame_count();
            if (now - fps_time >= std::chrono::milliseconds(500)) {
                const auto count = get_game_frame_count();
                const auto seconds = std::chrono::duration<float>(now - fps_time).count();
                if (set_game_fps)
                    set_game_fps(game_running && show_fps,
                                 (count >= fps_count ? count - fps_count : 0) / seconds);
                fps_count = count;
                fps_time = now;
            }
            const auto delta = std::chrono::duration<float>(now - last).count();
            last = now;
            SubmitInput(snapshot);
            io.FontGlobalScale =
                std::clamp(std::min(snapshot.width / 1280.0f, snapshot.height / 720.0f) * ui_scale,
                           0.75f, 3.0f);
            renderer->BeginFrame(delta);
            unsigned uploads{};
            while ((!game_running || menu_open) && !decoded.empty() && uploads < 2) {
                if (!renderer->Upload(decoded.front()))
                    break;
                pending_images.erase(decoded.front().key);
                decoded.pop_front();
                ++uploads;
            }
            Draw();
            if (!renderer->EndFrame())
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
    } catch (const winrt::hresult_error& error) {
        try {
            ReportFailure(L"D3D12 HRESULT " +
                          std::to_wstring(static_cast<uint32_t>(error.code().value)) + L": " +
                          std::wstring(error.message()));
        } catch (...) {
        }
    } catch (Platform::Exception ^ error) {
        try {
            ReportFailure(error->Message->Data());
        } catch (...) {
        }
    } catch (const std::exception& error) {
        try {
            ReportFailure(Wide(error.what()));
        } catch (...) {
        }
    } catch (...) {
        try {
            ReportFailure(L"Falha inesperada no frontend.");
        } catch (...) {
        }
    }
    if (set_game_fps)
        set_game_fps(false, 0);
    stopping.store(true);
    work_ready.notify_all();
    if (worker.joinable())
        worker.join();
    core->Stop();
    core->Wait();
    {
        std::lock_guard<std::mutex> lock{work_mutex};
        commands.clear();
        thumbnail_jobs.clear();
        completed.clear();
    }
    renderer.reset();
    if (ImGui::GetCurrentContext())
        ImGui::DestroyContext();
    try {
        Attach(nullptr);
    } catch (...) {
    }
    core->SetLogConsumer({});
    if (SUCCEEDED(apartment))
        RoUninitialize();
}

void ImGuiFrontend::SetFolder(Platform::Agile<StorageFolder> source, std::string token) {
    Queue([this, source, token] {
        if (roots.size() >= 64) {
            Windows::Storage::AccessCache::StorageApplicationPermissions::FutureAccessList->Remove(
                ref new Platform::String(Wide(token).c_str()));
            throw std::runtime_error("Limite de 64 pastas de biblioteca.");
        }
        for (const auto& root : roots)
            if (root.folder.Get() && root.folder.Get()->IsEqual(source.Get())) {
                Windows::Storage::AccessCache::StorageApplicationPermissions::FutureAccessList
                    ->Remove(ref new Platform::String(Wide(token).c_str()));
                Complete([this] { status = "Esta pasta já está na biblioteca."; });
                return;
            }
        roots.push_back({source, token, true});
        PersistRoots();
        Scan();
    });
}
void ImGuiFrontend::SetPackage(Platform::Agile<StorageFile> source, std::string token) {
    Queue([this, source, token] {
        const auto access =
            Windows::Storage::AccessCache::StorageApplicationPermissions::FutureAccessList;
        if (roots.size() >= 64) {
            access->Remove(ref new Platform::String(Wide(token).c_str()));
            throw std::runtime_error("Limite de 64 fontes de biblioteca.");
        }
        for (const auto& root : roots)
            if (root.file.Get() && root.file.Get()->IsEqual(source.Get())) {
                access->Remove(ref new Platform::String(Wide(token).c_str()));
                Complete([this] { status = "Este PKG já está na biblioteca."; });
                return;
            }
        roots.push_back({{}, token, true, source});
        PersistRoots();
        Scan();
    });
}
void ImGuiFrontend::SetFolders(std::vector<FrontendRoot> folders) {
    Queue([this, folders = std::move(folders)]() mutable {
        roots = std::move(folders);
        if (roots.size() > 64)
            roots.resize(64);
        Scan();
    });
}
void ImGuiFrontend::PersistRoots() {
    nlohmann::json document = nlohmann::json::array();
    for (const auto& root : roots)
        document.push_back({{"token", root.token}, {"enabled", root.enabled}});
    ApplicationData::Current->LocalSettings->Values->Insert(
        L"shadps4.library.roots", ref new Platform::String(Wide(document.dump()).c_str()));
}
void ImGuiFrontend::ChangeRoot(std::string token, bool enabled, bool remove) {
    Queue([this, token, enabled, remove] {
        const auto found = std::find_if(roots.begin(), roots.end(),
                                        [&](const auto& root) { return root.token == token; });
        if (found == roots.end())
            return;
        if (remove) {
            Windows::Storage::AccessCache::StorageApplicationPermissions::FutureAccessList->Remove(
                ref new Platform::String(Wide(token).c_str()));
            roots.erase(found);
        } else
            found->enabled = enabled;
        PersistRoots();
        Scan();
    });
}
void ImGuiFrontend::ImportFolder(Platform::Agile<StorageFolder> source, std::wstring target) {
    Queue([this, source, target] {
        if (target != L"sys_modules" && target != L"fonts" && target != L"addcont")
            throw std::runtime_error("Destino inválido.");
        auto root = concurrency::create_task(StorageFolder::GetFolderFromPathAsync(
                                                 ref new Platform::String(user_root.c_str())))
                        .get();
        auto destination = concurrency::create_task(
                               root->CreateFolderAsync(ref new Platform::String(target.c_str()),
                                                       CreationCollisionOption::OpenIfExists))
                               .get();
        CopyFolder(source.Get(), destination, stopping);
        Complete([this] { status = "Importação concluída no armazenamento privado."; });
    });
}

std::vector<uint8_t> ImGuiFrontend::ReadFile(const std::string& path, uint64_t limit) {
    uint64_t token{};
    if (open(path.c_str(), SHADPS4_FILE_READ, &token))
        throw std::runtime_error(Utf8(core->Error()));
    struct Lease {
        uint64_t token;
        decltype(close) release;
        ~Lease() { release(token); }
    } lease{token, close};
    uint64_t length{};
    if (size(token, &length) || length > limit)
        throw std::runtime_error("Arquivo excede o limite de leitura.");
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    uint64_t offset{};
    while (offset < length) {
        if (stopping.load())
            throw std::runtime_error("Operação cancelada.");
        uint64_t got{};
        if (read(token, bytes.data() + offset, std::min<uint64_t>(length - offset, 1024 * 1024),
                 &got) ||
            !got)
            throw std::runtime_error("Leitura incompleta.");
        offset += got;
    }
    return bytes;
}

void ImGuiFrontend::ReadMetadata(FrontendGame& game) {
    try {
        const auto metadata = Sfo(ReadFile(game.metadata_path + "/param.sfo", 256 * 1024));
        if (metadata.find("TITLE") != metadata.end())
            game.title = metadata.at("TITLE");
        if (metadata.find("TITLE_ID") != metadata.end())
            game.serial = metadata.at("TITLE_ID");
        if (metadata.find("APP_VER") != metadata.end())
            game.version = metadata.at("APP_VER");
        if (game.format == "PKG") {
            const auto category = metadata.find("CATEGORY");
            if (category != metadata.end() && (category->second == "gp" || category->second == "ac")) {
                game.format = category->second == "gp" ? "PKG (atualização)" : "PKG (DLC)";
                game.executable.clear();
                game.launch_error = "Este pacote é um complemento. Inicie o jogo base correspondente.";
            }
        }

        if (const auto found = metadata.find("CONTENT_ID");
            found != metadata.end() && !found->second.empty()) {
            switch (found->second.front()) {
            case 'U':
                game.region = "EUA";
                break;
            case 'E':
                game.region = "Europa";
                break;
            case 'J':
                game.region = "Japão";
                break;
            case 'H':
                game.region = "Ásia";
                break;
            case 'K':
                game.region = "Coreia";
                break;
            }
        }
        if (const auto found = metadata.find("PS4_SYSTEM_VER"); found != metadata.end()) {
            const auto version = std::stoul(found->second);
            char text[24]{};
            std::snprintf(text, sizeof(text), "%x.%02x", static_cast<unsigned>(version >> 24),
                          static_cast<unsigned>((version >> 16) & 255));
            game.firmware = text;
        }

    } catch (...) {
    }
}

void ImGuiFrontend::Scan() {
    scanning.store(true);
    scanned.store(0);
    const auto version = ++generation;
    auto result = std::make_shared<Library>();
    result->generation = version;
    for (const auto& previous : root_mounts)
        unmount(previous.c_str());
    root_mounts.clear();
    std::deque<std::pair<std::string, uint32_t>> folders;
    std::vector<std::string> package_paths;
    std::vector<std::pair<std::string, Platform::Agile<StorageFile>>> selected_packages;
    std::unordered_map<std::string, std::string> names;
    for (size_t index = 0; index < roots.size(); ++index) {
        const auto& root = roots[index];
        const auto label =
            Utf8(root.file.Get() ? root.file.Get()->Name->Data() : root.folder.Get()->Name->Data());
        result->roots.push_back({label, root.token, root.enabled});
        if (!root.enabled)
            continue;
        if (root.file.Get()) {
            selected_packages.emplace_back("pkgfile:" + root.token, root.file);
            continue;
        }
        const auto path = "/library" + std::to_string(index);
        if (mount(path.c_str(), reinterpret_cast<IInspectable*>(root.folder.Get()), 1))
            throw std::runtime_error(Utf8(core->Error()));
        root_mounts.push_back(path);
        folders.emplace_back(path, 0);
        names.emplace(path, label);
    }
    const auto depth = scan_depth.load();
    while (!folders.empty() && !stopping.load()) {
        auto [path, level] = std::move(folders.front());
        folders.pop_front();
        ++scanned;
        ShadPS4FileInfo info{sizeof(info)};
        if (!stat((path + "/eboot.bin").c_str(), &info) && !info.is_directory) {
            FrontendGame game{path, path.substr(path.find_last_of('/') + 1), {}, {}};
            if (!level && names.find(path) != names.end())
                game.title = names.at(path);
            game.metadata_path = path + "/sce_sys";
            if (stat((game.metadata_path + "/param.sfo").c_str(), &info) && path.size() >= 7 &&
                path.compare(path.size() - 7, 7, "/Image0") == 0) {
                const auto candidate = path.substr(0, path.find_last_of('/')) + "/Sc0";
                if (!stat((candidate + "/param.sfo").c_str(), &info))
                    game.metadata_path = candidate;
            }
            game.executable = path + "/eboot.bin";
            if (game.metadata_path.find("/Sc0") != std::string::npos)
                game.format = "Pasta (Image0/Sc0)";
            ReadMetadata(game);
            result->games.push_back(std::move(game));
            continue;
        }
        struct Context {
            std::deque<std::pair<std::string, uint32_t>>* folders;
            std::string parent;
            uint32_t depth;
            uint32_t maximum;
            std::vector<std::string>* packages;
        } context{&folders, path, level + 1, depth, &package_paths};
        if (enumerate(
                path.c_str(),
                [](void* user, const char* name, const ShadPS4FileInfo* entry) {
                    auto* context = static_cast<Context*>(user);
                    if (entry->is_directory && name[0] != '.' && context->depth <= context->maximum)
                        context->folders->emplace_back(context->parent + "/" + name,
                                                       context->depth);
                    else if (!entry->is_directory) {
                        const auto extension = Lower(name);
                        if (extension.size() >= 4 &&
                            extension.compare(extension.size() - 4, 4, ".pkg") == 0)
                            context->packages->push_back(context->parent + "/" + name);
                    }
                },
                &context))
            throw std::runtime_error(Utf8(core->Error()));
    }
    std::sort(package_paths.begin(), package_paths.end());
    for (const auto& path : package_paths)
        selected_packages.emplace_back(path, Platform::Agile<StorageFile>{});
    for (size_t index = 0; index < selected_packages.size() && !stopping.load(); ++index) {
        const auto& [source, file] = selected_packages[index];
        FrontendGame game{source, source.substr(source.find_last_of('/') + 1), {}, {}};
        game.format = "PKG";
        if (file.Get()) {
            game.title = Utf8(file.Get()->Name->Data());
            game.source_path = Utf8(file.Get()->Path->Data());
            if (game.source_path.empty())
                game.source_path = game.title;
        }
        const auto point = "/pkg" + std::to_string(index);
        int32_t bootable{};
        const auto result_code =
            file.Get() ? mount_pkg_file(point.c_str(), reinterpret_cast<IInspectable*>(file.Get()),
                                        &bootable)
                       : mount_pkg(point.c_str(), source.c_str(), &bootable);
        if (result_code) {
            game.launch_error = Utf8(core->Error());
        } else {
            root_mounts.push_back(point);
            game.metadata_path = point + "/sce_sys";
            if (bootable)
                game.executable = point + "/eboot.bin";
            else
                game.launch_error = "PKG sem executável acessível. Verifique se é um jogo base em "
                                    "formato FPKG compatível.";
            ReadMetadata(game);
        }
        result->games.push_back(std::move(game));
    }
    std::stable_sort(result->games.begin(), result->games.end(),
                     [](const auto& a, const auto& b) { return Lower(a.title) < Lower(b.title); });
    auto indices = std::make_shared<std::vector<size_t>>(result->games.size());
    std::unordered_set<std::string> serials;
    for (const auto& game : result->games)
        if (!game.serial.empty() && game.serial.size() <= 32 && serials.insert(game.serial).second)
            result->profiles.emplace_back(game.title, game.serial);
    std::iota(indices->begin(), indices->end(), 0);
    result->indices = indices;
    Complete([this, result] {
        library = result;
        visible = result->indices;
        selection = 0;
        pending_images.clear();
        failed_images.clear();
        decoded.clear();
        status = std::to_string(result->games.size()) +
                 " jogos encontrados. Selecione um jogo para iniciar.";
        scanning.store(false);
        Filter();
    });
}

void ImGuiFrontend::Filter() {
    if (!library)
        return;
    const auto source = library;
    const auto query = Lower(search);
    const auto request = ++filter_generation;
    const auto column = sort_column;
    const auto descending = sort_descending;
    std::lock_guard<std::mutex> lock{work_mutex};
    filter_job = [this, source, query, request, column, descending] {
        auto indices = std::make_shared<std::vector<size_t>>();
        indices->reserve(source->games.size());
        for (size_t index = 0; index < source->games.size(); ++index) {
            const auto& game = source->games[index];
            if (query.empty() || Lower(game.title).find(query) != std::string::npos ||
                Lower(game.serial).find(query) != std::string::npos ||
                Lower(game.format).find(query) != std::string::npos)
                indices->push_back(index);
        }
        const auto value = [column](const FrontendGame& game) -> const std::string& {
            switch (column) {
            case 2:
                return game.serial;
            case 3:
                return game.region;
            case 4:
                return game.firmware;
            case 5:
                return game.version;
            case 6:
                return game.path;
            case 7:
                return game.format;
            default:
                return game.title;
            }
        };
        std::stable_sort(indices->begin(), indices->end(), [&](size_t a, size_t b) {
            const auto left = Lower(value(source->games[a]));
            const auto right = Lower(value(source->games[b]));
            return descending ? left > right : left < right;
        });
        Complete([this, source, indices, request] {
            if (library == source && request == filter_generation)
                visible = indices;
        });
    };
    work_ready.notify_one();
}

void ImGuiFrontend::LoadSettings(std::string profile, uint64_t request) {
    uint32_t count{};
    if (profile.empty() ? get_settings(nullptr, 0, &count)
                        : get_game_settings(profile.c_str(), nullptr, 0, &count))
        throw std::runtime_error(Utf8(core->Error()));
    std::vector<char> data(count);
    if (profile.empty() ? get_settings(data.data(), static_cast<uint32_t>(data.size()), &count)
                        : get_game_settings(profile.c_str(), data.data(),
                                            static_cast<uint32_t>(data.size()), &count))
        throw std::runtime_error(Utf8(core->Error()));
    const auto document = nlohmann::json::parse(data.data());
    std::vector<Setting> values;
    size_t hidden{};
    float scale = 1;
    bool fps{};
    uint32_t depth = 2;
    for (const auto& entry : document.at("entries")) {
        if (!entry.at("compatible").get<bool>()) {
            ++hidden;
            continue;
        }
        Setting setting{entry.at("group"),  entry.at("key"),     Label(entry.at("key")),
                        entry.at("value"),  entry.at("choices"), entry.at("minimum"),
                        entry.at("maximum")};
        setting.editable = entry.value("editable", true);
        setting.profile = profile;
        if (setting.key == "big_picture_scale")
            scale = setting.value.get<float>() / 1000.0f;
        if (setting.key == "big_picture_folder_depth")
            depth = setting.value.get<uint32_t>();
        if (setting.key == "show_fps_counter")
            fps = setting.value.get<bool>();
        values.push_back(std::move(setting));
    }
    scan_depth.store(depth);
    Complete([this, values = std::move(values), hidden, scale, fps, profile, request]() mutable {
        if (request != settings_request.load())
            return;
        settings = std::move(values);
        settings_profile = profile;
        excluded = hidden;
        ui_scale = scale;
        show_fps = fps;
        status = "Configurações carregadas.";
    });
}
ThumbnailPixels ImGuiFrontend::Decode(const std::string& key, const std::string& path) {
    std::vector<uint8_t> bytes;
    if (key.compare(0, 9, "launcher:") == 0) {
        const auto installed = Windows::ApplicationModel::Package::Current->InstalledLocation;
        const auto filename =
            std::filesystem::path(installed->Path->Data()) / L"Assets" / L"Launcher" / Wide(path);
        std::ifstream stream(filename, std::ios::binary | std::ios::ate);
        const auto length = stream.tellg();
        if (!stream || length <= 0 || length > 1024 * 1024)
            throw std::runtime_error("Ícone do launcher inválido.");
        bytes.resize(static_cast<size_t>(length));
        stream.seekg(0);
        if (!stream.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
            throw std::runtime_error("Não foi possível ler o ícone do launcher.");
    } else {
        bytes = ReadFile(path, 16 * 1024 * 1024);
    }
    ComPtr<IWICImagingFactory> factory;
    Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                           IID_PPV_ARGS(&factory)));
    ComPtr<IWICStream> stream;
    Check(factory->CreateStream(&stream));
    Check(stream->InitializeFromMemory(bytes.data(), bytes.size()));
    ComPtr<IWICBitmapDecoder> decoder;
    Check(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand,
                                           &decoder));
    ComPtr<IWICBitmapFrameDecode> frame;
    Check(decoder->GetFrame(0, &frame));
    uint32_t width{}, height{};
    Check(frame->GetSize(&width, &height));
    if (!width || !height || width > 16384 || height > 16384)
        throw std::runtime_error("Imagem inválida.");
    const auto ratio = std::min(1.0, 256.0 / std::max(width, height));
    ThumbnailPixels output{key,
                           std::max(1U, static_cast<uint32_t>(width * ratio)),
                           std::max(1U, static_cast<uint32_t>(height * ratio)),
                           {}};
    ComPtr<IWICBitmapScaler> scaled;
    Check(factory->CreateBitmapScaler(&scaled));
    Check(scaled->Initialize(frame.Get(), output.width, output.height,
                             WICBitmapInterpolationModeFant));
    ComPtr<IWICFormatConverter> converter;
    Check(factory->CreateFormatConverter(&converter));
    Check(converter->Initialize(scaled.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                                nullptr, 0, WICBitmapPaletteTypeCustom));
    output.pixels.resize(static_cast<size_t>(output.width) * output.height * 4);
    Check(converter->CopyPixels(nullptr, output.width * 4, output.pixels.size(),
                                output.pixels.data()));
    if (key.compare(0, 9, "launcher:") == 0)
        for (size_t index = 0; index < output.pixels.size(); index += 4)
            for (size_t channel = 0; channel < 3; ++channel)
                output.pixels[index + channel] = 255 - output.pixels[index + channel];
    return output;
}
void ImGuiFrontend::RequestImage(const std::string& key, const std::string& path) {
    if (pending_images.find(key) != pending_images.end() || pending_images.size() >= 32 ||
        decoded.size() >= 16)
        return;
    const auto failure = failed_images.find(key);
    if (failure != failed_images.end() && std::chrono::steady_clock::now() < failure->second)
        return;
    const auto request = library ? library->generation : 0;
    if (Queue(
            [this, key, path, request] {
                try {
                    if (request != generation.load()) {
                        Complete([this, key] { pending_images.erase(key); });
                        return;
                    }
                    auto pixels = Decode(key, path);
                    Complete([this, pixels = std::move(pixels), request]() mutable {
                        if (library && library->generation == request && decoded.size() < 16)
                            decoded.push_back(std::move(pixels));
                        else
                            pending_images.erase(pixels.key);
                    });
                } catch (...) {
                    Complete([this, key] {
                        pending_images.erase(key);
                        if (failed_images.size() >= 128)
                            failed_images.erase(failed_images.begin());
                        failed_images[key] =
                            std::chrono::steady_clock::now() + std::chrono::seconds(30);
                    });
                }
            },
            true))
        pending_images.insert(key);
}

void ImGuiFrontend::SubmitInput(const FrontendInput& snapshot) {
    auto& io = ImGui::GetIO();
    const bool capture = snapshot.captured;
    io.AddMousePosEvent(capture ? -10000.0f : snapshot.x, capture ? -10000.0f : snapshot.y);
    io.AddMouseButtonEvent(0, !capture && snapshot.left);
    io.AddMouseButtonEvent(1, !capture && snapshot.right);
    io.AddMouseWheelEvent(0, capture ? 0 : snapshot.wheel);
    constexpr std::array<std::pair<int, ImGuiKey>, 12> keys{{{13, ImGuiKey_Enter},
                                                             {27, ImGuiKey_Escape},
                                                             {9, ImGuiKey_Tab},
                                                             {8, ImGuiKey_Backspace},
                                                             {37, ImGuiKey_LeftArrow},
                                                             {38, ImGuiKey_UpArrow},
                                                             {39, ImGuiKey_RightArrow},
                                                             {40, ImGuiKey_DownArrow},
                                                             {36, ImGuiKey_Home},
                                                             {35, ImGuiKey_End},
                                                             {46, ImGuiKey_Delete},
                                                             {32, ImGuiKey_Space}}};
    for (const auto& [code, key] : keys)
        io.AddKeyEvent(key, !capture && snapshot.keys[code]);
    io.AddKeyEvent(ImGuiMod_Ctrl, !capture && snapshot.keys[17]);
    io.AddKeyEvent(ImGuiMod_Shift, !capture && snapshot.keys[16]);
    for (const auto code : snapshot.characters)
        if (!capture)
            io.AddInputCharacter(code);
    io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    Windows::Gaming::Input::GamepadReading pad{};
    try {
        const auto pads = Windows::Gaming::Input::Gamepad::Gamepads;
        if (pads->Size && !capture && !snapshot.system_pointer) {
            pad = pads->GetAt(0)->GetCurrentReading();
            io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        }
    } catch (Platform::Exception ^) {
    }
    using Button = Windows::Gaming::Input::GamepadButtons;
    const auto pressed = [&](Button button) {
        return (static_cast<unsigned>(pad.Buttons) & static_cast<unsigned>(button)) != 0;
    };
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, !snapshot.left && pressed(Button::A));
    io.AddKeyEvent(ImGuiKey_GamepadFaceRight, pressed(Button::B));
    io.AddKeyEvent(ImGuiKey_GamepadFaceLeft, pressed(Button::X));
    io.AddKeyEvent(ImGuiKey_GamepadFaceUp, pressed(Button::Y));
    io.AddKeyEvent(ImGuiKey_GamepadBack, pressed(Button::View));
    io.AddKeyEvent(ImGuiKey_GamepadStart, pressed(Button::Menu));
    io.AddKeyEvent(ImGuiKey_GamepadL1, pressed(Button::LeftShoulder));
    io.AddKeyEvent(ImGuiKey_GamepadR1, pressed(Button::RightShoulder));
    io.AddKeyEvent(ImGuiKey_GamepadL3, pressed(Button::LeftThumbstick));
    io.AddKeyEvent(ImGuiKey_GamepadR3, pressed(Button::RightThumbstick));
    io.AddKeyEvent(ImGuiKey_GamepadDpadUp, pressed(Button::DPadUp));
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown, pressed(Button::DPadDown));
    io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, pressed(Button::DPadLeft));
    io.AddKeyEvent(ImGuiKey_GamepadDpadRight, pressed(Button::DPadRight));
    const auto analog = [&](ImGuiKey key, double value) {
        const float pressure = static_cast<float>(std::clamp((value - 0.25) / 0.75, 0.0, 1.0));
        io.AddKeyAnalogEvent(key, pressure > 0, pressure);
    };
    analog(ImGuiKey_GamepadLStickLeft, -pad.LeftThumbstickX);
    analog(ImGuiKey_GamepadLStickRight, pad.LeftThumbstickX);
    analog(ImGuiKey_GamepadLStickUp, pad.LeftThumbstickY);
    analog(ImGuiKey_GamepadLStickDown, -pad.LeftThumbstickY);
    analog(ImGuiKey_GamepadRStickLeft, -pad.RightThumbstickX);
    analog(ImGuiKey_GamepadRStickRight, pad.RightThumbstickX);
    analog(ImGuiKey_GamepadRStickUp, pad.RightThumbstickY);
    analog(ImGuiKey_GamepadRStickDown, -pad.RightThumbstickY);
    analog(ImGuiKey_GamepadL2, pad.LeftTrigger);
    analog(ImGuiKey_GamepadR2, pad.RightTrigger);
}

bool ImGuiFrontend::ToolbarButton(const char* label, const char* icon, bool enabled) {
    const float scale = ImGui::GetIO().FontGlobalScale;
    ImGui::PushID(label);
    ImGui::BeginDisabled(!enabled);
    const auto position = ImGui::GetCursorScreenPos();
    const float width = std::max(72.0f * scale, ImGui::CalcTextSize(label).x + 16 * scale);
    const bool pressed = ImGui::Button("##tool", {width, 76 * scale});
    const auto key = std::string{"launcher:"} + icon;
    const auto texture = renderer->Image(key);
    auto* draw = ImGui::GetWindowDrawList();
    if (texture) {
        const float left = position.x + (width - 40 * scale) / 2;
        draw->AddImage(reinterpret_cast<ImTextureID>(texture), {left, position.y + 6 * scale},
                       {left + 40 * scale, position.y + 46 * scale}, {0, 0}, {1, 1},
                       ImGui::GetColorU32(ImGuiCol_Text));
    } else {
        RequestImage(key, icon);
    }
    const auto size = ImGui::CalcTextSize(label);
    draw->AddText({position.x + (width - size.x) / 2, position.y + 54 * scale},
                  ImGui::GetColorU32(ImGuiCol_Text), label);
    ImGui::EndDisabled();
    ImGui::PopID();
    return pressed;
}

void ImGuiFrontend::Draw() {
    if (game_running && !menu_open)
        return;
    const auto display = ImGui::GetIO().DisplaySize;
    const float scale = ImGui::GetIO().FontGlobalScale;
    const bool idle = !session_pending && !game_running;
    const bool can_play =
        game_surface && idle && !scanning.load() && core->State() == SHADPS4_READY && library &&
        selection < library->games.size() && !library->games[selection].executable.empty();
    const auto open_settings = [&](const char* page = "") {
        settings_open = true;
        requested_settings_group = page;
    };
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(display);
    ImGui::Begin("shadPS4-UWP", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar);
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("Arquivo")) {
            if (ImGui::MenuItem("Adicionar pasta de jogos…", nullptr, false,
                                idle && !scanning.load()) &&
                choose_folder)
                choose_folder(L"library");
            if (ImGui::MenuItem("Adicionar PKG…", nullptr, false, idle && !scanning.load()) &&
                choose_folder)
                choose_folder(L"pkg");
            if (ImGui::MenuItem("Iniciar jogo", nullptr, false, can_play))
                StartGame();
            if (ImGui::BeginMenu("Importar", idle)) {
                if (ImGui::MenuItem("Módulos de sistema…") && choose_folder)
                    choose_folder(L"sys_modules");
                if (ImGui::MenuItem("Fontes…") && choose_folder)
                    choose_folder(L"fonts");
                if (ImGui::MenuItem("DLCs…") && choose_folder)
                    choose_folder(L"addcont");
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Exibir")) {
            ImGui::MenuItem("Lista de jogos", nullptr, &show_game_list);
            ImGui::MenuItem("Log do jogo", nullptr, &show_game_log);
            ImGui::Separator();
            if (ImGui::MenuItem("Modo lista", nullptr, !grid_view))
                grid_view = false;
            if (ImGui::MenuItem("Modo grade", nullptr, grid_view))
                grid_view = true;
            if (ImGui::MenuItem("Tela cheia") && toggle_fullscreen)
                toggle_fullscreen();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Configurações")) {
            if (ImGui::MenuItem("Configurar…", nullptr, false, idle))
                open_settings();
            if (ImGui::MenuItem("Controles e teclado…", nullptr, false, idle))
                open_settings("Entrada");
            if (ImGui::MenuItem("Pastas da biblioteca…", nullptr, false, idle))
                open_settings("Pastas");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Cheats / Patches")) {
            if (ImGui::MenuItem("Gerenciar jogo selecionado…", nullptr, false,
                                library && selection < library->games.size() && !session_pending))
                OpenCheats();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Ajuda")) {
            if (ImGui::MenuItem("Sobre o ShadPS4-UWP"))
                about_open = true;
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
    ImGui::BeginChild("toolbar", {0, 90 * scale}, false, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushStyleColor(ImGuiCol_Button, {0, 0, 0, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {2 * scale, 4 * scale});
    if (ToolbarButton(game_running ? (session_paused ? "Continuar" : "Pausar") : "Iniciar",
                      game_running && !session_paused ? "pause_icon.png" : "play_icon.png",
                      game_running || can_play)) {
        if (game_running) {
            session_paused = !session_paused;
            core->Pause(session_paused);
        } else {
            StartGame();
        }
    }
    ImGui::SameLine();
    if (ToolbarButton("Parar", "stop_icon.png", game_running)) {
        restart_requested = false;
        status = "Encerrando sessão…";
        core->Stop();
    }
    ImGui::SameLine();
    if (ToolbarButton("Reiniciar", "restart_game_icon.png", game_running)) {
        restart_requested = true;
        status = "Reiniciando jogo…";
        core->Stop();
    }
    ImGui::SameLine();
    if (ToolbarButton("Configurações", "settings_icon.png", idle))
        open_settings();
    ImGui::SameLine();
    if (ToolbarButton("Tela cheia", "fullscreen_icon.png") && toggle_fullscreen)
        toggle_fullscreen();
    ImGui::SameLine();
    if (ToolbarButton("Controles", "controller_icon.png", idle))
        open_settings("Entrada");
    ImGui::SameLine();
    if (ToolbarButton("Teclado", "keyboard_icon.png", idle))
        open_settings("Entrada");
    ImGui::SameLine();
    ImGui::Dummy({2 * scale, 76 * scale});
    ImGui::SameLine();
    if (ToolbarButton("Atualizar lista", "refreshlist_icon.png", idle && !scanning.load()))
        Queue([this] { Scan(); });
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::SetNextItemWidth(180 * scale);
    ImGui::SliderFloat("##icon-size", &icon_size, 24, 128, "Tamanho: %.0f");
    ImGui::SetNextItemWidth(220 * scale);
    if (ImGui::InputTextWithHint("##search", "Pesquisar…", search, sizeof(search)))
        Filter();
    ImGui::EndGroup();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    ImGui::EndChild();
    ImGui::Separator();
    const float footer = ImGui::GetFrameHeightWithSpacing();
    const float available = ImGui::GetContentRegionAvail().y - footer;
    const float minimum = std::min(60 * scale, std::max(0.0f, available / 3));
    const float log_size = show_game_log ? std::clamp(log_height * scale, minimum,
                                                      std::max(minimum, available - minimum))
                                         : 0;
    if (show_game_list) {
        ImGui::TextUnformatted("Lista de jogos");
        ImGui::BeginChild(
            "game-list",
            {0, std::max(1.0f, available - (show_game_log ? log_size + 12 * scale : 0) -
                                   ImGui::GetFrameHeight())},
            true);
        DrawLibrary();
        ImGui::EndChild();
    }
    if (show_game_log) {
        if (show_game_list) {
            ImGui::InvisibleButton("log-splitter", {-1, 5 * scale});
            if (ImGui::IsItemActive())
                log_height = std::clamp(log_height - ImGui::GetIO().MouseDelta.y / scale, 60.0f,
                                        std::max(60.0f, available / scale - 60));
            if (ImGui::IsItemHovered())
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        }
        ImGui::BeginChild("game-log", {0, -footer}, true);
        ImGui::TextUnformatted("Log do jogo");
        DrawLogs();
        ImGui::EndChild();
    }
    ImGui::Separator();
    ImGui::Text("Jogos: %zu", visible ? visible->size() : 0);
    ImGui::SameLine();
    if (scanning.load())
        ImGui::Text("Examinando pastas: %u", scanned.load());
    else
        ImGui::TextUnformatted(status.c_str());


    const auto layout = nlohmann::json{
        {"game_list", show_game_list},
        {"game_log", show_game_log},
        {"grid", grid_view},
        {"icon_size", icon_size},
        {"log_height",
         log_height}}.dump();
    if (layout != launcher_layout && !ImGui::IsMouseDown(0)) {
        launcher_layout = layout;
        try {
            ApplicationData::Current->LocalSettings->Values->Insert(
                L"shadps4.launcher.layout", ref new Platform::String(Wide(layout).c_str()));
        } catch (Platform::Exception ^) {
            status = "Não foi possível salvar a disposição da interface.";
        }
    }
    ImGui::End();
    DrawCheats();
    if (settings_open) {
        ImGui::SetNextWindowSize(
            {std::min(1000 * scale, display.x - 24), std::min(740 * scale, display.y - 24)},
            ImGuiCond_Appearing);
        ImGui::SetNextWindowPos({display.x / 2, display.y / 2}, ImGuiCond_Appearing, {0.5f, 0.5f});
        if (ImGui::Begin("Configurações", &settings_open, ImGuiWindowFlags_NoCollapse)) {
            ImGui::BeginDisabled(!idle || settings_saving);
            DrawSettings();
            ImGui::EndDisabled();
        }
        ImGui::End();
        if (!settings_open && !settings_saving) {
            settings_changes.clear();
            next_graphics_backend = static_cast<int>(core->GraphicsBackend());
            const auto profile = settings_profile;
            const auto request = settings_request.load();
            Queue([this, profile, request] { LoadSettings(profile, request); });
        }
    }
    if (about_open) {
        ImGui::SetNextWindowSize({440 * scale, 180 * scale}, ImGuiCond_Appearing);
        if (ImGui::Begin("Sobre o ShadPS4-UWP", &about_open, ImGuiWindowFlags_NoCollapse)) {
            ImGui::TextUnformatted("shadPS4-UWP");
            ImGui::TextWrapped("Emulador de PlayStation 4 para Windows e Xbox UWP.");
            ImGui::Separator();
            ImGui::TextWrapped("Interface adaptada do shadPS4 Qt Launcher.\nshadPS4 "
                               "Emulator Project — GPL-2.0-or-later.");
        }
        ImGui::End();
    }
}

void ImGuiFrontend::DrawLibrary() {
    if (!library || !visible || visible->empty()) {
        ImGui::TextWrapped("Nenhum jogo encontrado. Use Arquivo > Adicionar pasta de jogos. Para "
                           "Image0/Sc0, escolha a pasta que contém ambas.");
        return;
    }
    const float scale = ImGui::GetIO().FontGlobalScale;
    const auto draw_game = [&](size_t index, bool grid) {
        const auto& game = library->games[index];
        ImGui::PushID(static_cast<int>(index));
        const auto key = std::to_string(library->generation) + ":" + game.path;
        const auto texture = renderer->Image(key);
        const float size = (grid ? std::max(icon_size, 96.0f) : icon_size) * scale;
        if (texture)
            ImGui::Image(reinterpret_cast<ImTextureID>(texture), {size, size});
        else {
            ImGui::Dummy({size, size});
            if (!game.metadata_path.empty())
                RequestImage(key, game.metadata_path + "/icon0.png");
        }
        if (grid && ImGui::IsItemClicked())
            selection = index;
        if (!grid)
            ImGui::TableSetColumnIndex(1);
        if (ImGui::Selectable(game.title.c_str(), selection == index,
                              ImGuiSelectableFlags_AllowDoubleClick |
                                  (grid ? 0 : ImGuiSelectableFlags_SpanAllColumns),
                              {0, grid ? 0 : size})) {
            selection = index;
            if (!game.launch_error.empty())
                status = game.launch_error;
            if (ImGui::IsMouseDoubleClicked(0) || ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown))
                StartGame();
        }
        if (ImGui::BeginPopupContextItem("game-actions")) {
            selection = index;
            if (ImGui::MenuItem("Iniciar", nullptr, false, !game_running && !session_pending))
                StartGame();
            if (ImGui::MenuItem("Configurações do jogo…", nullptr, false,
                                !game_running && !session_pending && !game.serial.empty() &&
                                    settings_changes.empty())) {
                const auto serial = game.serial;
                const auto request = ++settings_request;
                if (Queue([this, serial, request] { LoadSettings(serial, request); })) {
                    settings_profile = serial;
                    settings.clear();
                    settings_open = true;
                }
            }
            ImGui::EndPopup();
        }
        if (grid) {
            ImGui::TextUnformatted(game.serial.c_str());
            ImGui::TextWrapped("%s", game.format.c_str());
        } else {
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(game.serial.c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(game.region.c_str());
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(game.firmware.c_str());
            ImGui::TableSetColumnIndex(5);
            ImGui::TextUnformatted(game.version.c_str());
            ImGui::TableSetColumnIndex(6);
            ImGui::TextUnformatted(game.source_path.empty() ? game.path.c_str()
                                                            : game.source_path.c_str());
            ImGui::TableSetColumnIndex(7);
            ImGui::TextUnformatted(game.format.c_str());
        }
        ImGui::PopID();
    };
    if (grid_view) {
        const float cell = (std::max(icon_size, 96.0f) + 32) * scale;
        const int columns =
            std::clamp(static_cast<int>(ImGui::GetContentRegionAvail().x / cell), 1, 64);
        const float row_height = std::max(icon_size, 96.0f) * scale +
            ImGui::GetTextLineHeightWithSpacing() * 4 + ImGui::GetStyle().CellPadding.y * 2;

        if (ImGui::BeginTable("game-grid", columns)) {
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>((visible->size() + columns - 1) / columns),
                          row_height);
            while (clipper.Step())
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                    ImGui::TableNextRow(0, row_height);
                    for (int column = 0; column < columns; ++column) {
                        const size_t position = static_cast<size_t>(row) * columns + column;
                        if (position >= visible->size())
                            break;
                        ImGui::TableSetColumnIndex(column);
                        draw_game((*visible)[position], true);
                    }
                }
            ImGui::EndTable();
        }
    } else if (ImGui::BeginTable("game-table", 8,
                                 ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV |
                                     ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                     ImGuiTableFlags_ScrollX | ImGuiTableFlags_Reorderable |
                                     ImGuiTableFlags_Hideable | ImGuiTableFlags_Sortable)) {
        ImGui::TableSetupScrollFreeze(2, 1);
        ImGui::TableSetupColumn("Ícone",
                                ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,
                                icon_size * scale + 8);
        ImGui::TableSetupColumn("Nome", ImGuiTableColumnFlags_WidthStretch, 3);
        ImGui::TableSetupColumn("Serial", ImGuiTableColumnFlags_WidthFixed, 120 * scale);
        ImGui::TableSetupColumn("Região", ImGuiTableColumnFlags_WidthFixed, 80 * scale);
        ImGui::TableSetupColumn("Firmware", ImGuiTableColumnFlags_WidthFixed, 80 * scale);
        ImGui::TableSetupColumn("Versão", ImGuiTableColumnFlags_WidthFixed, 80 * scale);
        ImGui::TableSetupColumn("Caminho", ImGuiTableColumnFlags_WidthStretch, 2);
        ImGui::TableSetupColumn("Formato", ImGuiTableColumnFlags_WidthFixed, 140 * scale);
        ImGui::TableHeadersRow();
        if (const auto specs = ImGui::TableGetSortSpecs();
            specs && specs->SpecsDirty && specs->SpecsCount) {
            sort_column = specs->Specs[0].ColumnIndex;
            sort_descending = specs->Specs[0].SortDirection == ImGuiSortDirection_Descending;
            Filter();
            specs->SpecsDirty = false;
        }
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(visible->size()), icon_size * scale + 4);
        while (clipper.Step())
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                ImGui::TableNextRow(0, icon_size * scale + 4);
                ImGui::TableSetColumnIndex(0);
                draw_game((*visible)[row], false);
            }
        ImGui::EndTable();
    }
}

void ImGuiFrontend::StartGame() {
    if (!game_surface || scanning.load() || session_pending || game_running ||
        core->State() != SHADPS4_READY || !library || selection >= library->games.size())
        return;
    const auto& game = library->games[selection];
    if (game.executable.empty()) {
        status = game.launch_error.empty() ? "O jogo não possui executável acessível."
                                           : game.launch_error;
        return;
    }
    active_game_path = game.path;
    const auto executable = game.executable;
    session_pending = true;
    menu_open = false;
    status = "Iniciando jogo…";
    const auto weak = weak_from_this();
    if (!Queue([this, executable, weak] {
            core->Run(Wide(executable), [weak](std::wstring message) {
                if (const auto self = weak.lock())
                    self->Complete([self, message] {
                        self->reload_core = self->core->State() != SHADPS4_READY;
                        self->session_pending = self->reload_core;
                        self->session_paused = false;
                        self->status = Utf8(message);
                    });
            });
        })) {
        session_pending = false;
        status = "O worker está ocupado.";
    }
}

void ImGuiFrontend::ApplySettings(bool close_window) {
    if (settings_saving)
        return;
    const auto changes = settings_changes;
    const auto profile = settings_profile;
    const auto request = settings_request.load();
    const int backend = next_graphics_backend;
    if (Queue([this, changes, profile, request, backend, close_window] {
            try {
                for (const auto& [identity, value] : changes) {
                    const auto& [group, key] = identity;
                    const auto json = value.dump();
                    if (profile.empty() ? set_setting(group.c_str(), key.c_str(), json.c_str())
                                        : set_game_setting(profile.c_str(), group.c_str(),
                                                           key.c_str(), json.c_str()))
                        throw std::runtime_error(Utf8(core->Error()));
                }
                if (backend >= 0)
                    ApplicationData::Current->LocalSettings->Values->Insert(
                        L"shadps4.graphics.backend",
                        ref new Platform::String(backend ? L"d3d12" : L"dozen"));
                LoadSettings(profile, request);
                Complete([this, backend, close_window] {
                    settings_changes.clear();
                    settings_saving = false;
                    if (close_window)
                        settings_open = false;
                    reload_core =
                        backend >= 0 && backend != static_cast<int>(core->GraphicsBackend());
                    session_pending = reload_core;
                    status = "Configurações salvas.";
                });
            } catch (...) {
                Complete([this] { settings_saving = false; });
                throw;
            }
        }))
        settings_saving = true;
    else
        status = "Aguarde as operações pendentes.";
}

void ImGuiFrontend::DrawSetting(Setting& setting) {
    ImGui::PushID(setting.group.c_str());
    ImGui::PushID(setting.key.c_str());
    ImGui::BeginDisabled(!setting.editable);
    if (!setting.value.is_boolean())
        ImGui::TextUnformatted(setting.label.c_str());
    ImGui::SetNextItemWidth(-1);
    const auto stage = [&](nlohmann::json value) {
        setting.value = value;
        settings_changes[{setting.group, setting.key}] = std::move(value);
    };
    if (!setting.choices.empty()) {
        std::string label = setting.value.dump();
        for (const auto& choice : setting.choices)
            if (choice.at("value") == setting.value)
                label = choice.at("label");
        if (ImGui::BeginCombo("##value", label.c_str())) {
            for (const auto& choice : setting.choices)
                if (ImGui::Selectable(choice.at("label").get_ref<const std::string&>().c_str(),
                                      choice.at("value") == setting.value))
                    stage(choice.at("value"));
            ImGui::EndCombo();
        }
    } else if (setting.value.is_boolean()) {
        bool value = setting.value;
        if (ImGui::Checkbox(setting.label.c_str(), &value))
            stage(value);
    } else if (setting.value.is_number_integer()) {
        int64_t value = setting.value;
        if (ImGui::InputScalar("##value", ImGuiDataType_S64, &value))
            stage(std::clamp(value, static_cast<int64_t>(setting.minimum),
                             static_cast<int64_t>(setting.maximum)));
    } else if (setting.value.is_number()) {
        double value = setting.value;
        if (ImGui::InputDouble("##value", &value, 0, 0, "%.3f"))
            stage(std::clamp(value, setting.minimum, setting.maximum));
    } else {
        ImGui::TextWrapped("%s", setting.value.get_ref<const std::string&>().c_str());
        if (ImGui::Button("Editar…") && edit_text) {
            const auto copy = setting;
            const auto request = settings_request.load();
            edit_text(setting.label, setting.value, false,
                      [self = shared_from_this(), copy, request](std::string value) {
                          self->Complete([self, copy, request, value] {
                              if (request != self->settings_request.load())
                                  return;
                              for (auto& entry : self->settings)
                                  if (entry.group == copy.group && entry.key == copy.key) {
                                      entry.value = value;
                                      self->settings_changes[{copy.group, copy.key}] = value;
                                      break;
                                  }
                          });
                      });
        }
    }
    ImGui::EndDisabled();
    if (!setting.editable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Opção global herdada pelo perfil do jogo.");
    ImGui::PopID();
    ImGui::PopID();
}

void ImGuiFrontend::DrawSettings() {
    if (next_graphics_backend < 0)
        next_graphics_backend = static_cast<int>(core->GraphicsBackend());
    const bool dirty = !settings_changes.empty() ||
                       next_graphics_backend != static_cast<int>(core->GraphicsBackend());
    ImGui::BeginDisabled(dirty);
    ImGui::SetNextItemWidth(360 * ImGui::GetIO().FontGlobalScale);
    if (ImGui::BeginCombo("Perfil",
                          settings_profile.empty() ? "Global" : settings_profile.c_str())) {
        const size_t count = library ? library->profiles.size() + 1 : 1;
        for (size_t index = 0; index < count; ++index) {
            const auto serial = index ? library->profiles[index - 1].second : std::string{};
            const auto label = index ? library->profiles[index - 1].first + " (" + serial + ")"
                                     : std::string{"Global"};
            if (ImGui::Selectable(label.c_str(), settings_profile == serial)) {
                const auto request = ++settings_request;
                if (Queue([this, serial, request] { LoadSettings(serial, request); })) {
                    settings_profile = serial;
                    settings.clear();
                }
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    const float scale = ImGui::GetIO().FontGlobalScale;
    const float description_height =
        std::clamp(ImGui::GetContentRegionAvail().y * 0.2f, 44 * scale, 130 * scale);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
    ImGui::BeginChild("settings-content",
                      {0, -description_height - ImGui::GetFrameHeightWithSpacing() * 2}, true);
    constexpr std::array pages{"Geral", "Interface", "Gráficos", "Usuário", "Entrada",
                               "Áudio", "Rede",      "Pastas",   "Log",     "Debug"};
    if (ImGui::BeginTabBar("settings-pages")) {
        for (const auto* page : pages) {
            const bool requested = requested_settings_group == page;
            if (!ImGui::BeginTabItem(page, nullptr, requested ? ImGuiTabItemFlags_SetSelected : 0))
                continue;
            if (requested)
                requested_settings_group.clear();
            ImGui::BeginChild("page-options", {0, 0}, false);
            if (std::string_view(page) == "Entrada") {
                ImGui::TextUnformatted("Touchpad no controle Xbox");
                ImGui::TextWrapped("Segure View e mova o analógico esquerdo para tocar e deslizar. Pressione o analógico esquerdo (L3) com View segurado para clicar. Solte View para levantar o dedo.");
                ImGui::Separator();
            }

            if (std::string_view(page) == "Gráficos") {
                ImGui::TextUnformatted("Renderização");
                ImGui::Separator();
                const char* backends[] = {"Vulkan / Dozen", "D3D12 nativo"};
                ImGui::BeginDisabled(!settings_profile.empty());
                ImGui::Combo("Backend gráfico (global)", &next_graphics_backend, backends, 2);
                ImGui::EndDisabled();
                ImGui::Spacing();
            }
            if (std::string_view(page) == "Pastas") {
                ImGui::TextUnformatted("Pastas da biblioteca");
                ImGui::Separator();
                ImGui::BeginDisabled(scanning.load() || !settings_profile.empty());
                if (ImGui::Button("Adicionar pasta…") && choose_folder)
                    choose_folder(L"library");
                ImGui::SameLine();
                if (ImGui::Button("Adicionar PKG…") && choose_folder)
                    choose_folder(L"pkg");
                if (library)
                    for (const auto& root : library->roots) {
                        ImGui::PushID(root.token.c_str());
                        bool enabled = root.enabled;
                        if (ImGui::Checkbox(root.name.c_str(), &enabled))
                            ChangeRoot(root.token, enabled, false);
                        ImGui::SameLine();
                        if (ImGui::Button("Remover"))
                            ChangeRoot(root.token, enabled, true);
                        ImGui::PopID();
                    }
                ImGui::EndDisabled();
                ImGui::TextWrapped("Módulos, fontes e DLCs são importados pelo menu "
                                   "Arquivo > Importar.");
            }
            bool any = std::string_view(page) == "Gráficos" || std::string_view(page) == "Pastas";
            std::vector<std::string> sections;
            for (const auto& setting : settings) {
                if (SettingsPage(setting.group, setting.key) != page)
                    continue;
                any = true;
                const auto section = SettingsSection(setting.group, setting.key);
                if (section != "Avançado" &&
                    std::find(sections.begin(), sections.end(), section) == sections.end())
                    sections.push_back(section);
            }
            if (std::string_view(page) == "Gráficos")
                std::stable_sort(sections.begin(), sections.end(),
                                 [](const auto& a, const auto& b) {
                                     const auto rank = [](const auto& section) {
                                         return section == "Dispositivo e cache" ? 0
                                                : section == "Vídeo"             ? 1
                                                                                 : 2;
                                     };
                                     return rank(a) < rank(b);
                                 });
            const int columns =
                std::max(1, std::min(3, static_cast<int>(ImGui::GetContentRegionAvail().x /
                                                         (280 * ImGui::GetIO().FontGlobalScale))));
            if (!sections.empty() &&
                ImGui::BeginTable("option-sections", columns, ImGuiTableFlags_SizingStretchSame)) {
                for (const auto& section : sections) {
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(section.c_str());
                    ImGui::Separator();
                    for (auto& setting : settings)
                        if (SettingsPage(setting.group, setting.key) == page &&
                            SettingsSection(setting.group, setting.key) == section)
                            DrawSetting(setting);
                }
                ImGui::EndTable();
            }
            if (std::string_view(page) == "Gráficos" &&
                ImGui::CollapsingHeader("Opções avançadas de GPU"))
                for (auto& setting : settings)
                    if (SettingsPage(setting.group, setting.key) == page &&
                        SettingsSection(setting.group, setting.key) == "Avançado")
                        DrawSetting(setting);

            if (!any)
                ImGui::TextWrapped("Nenhuma opção disponível nesta categoria para o "
                                   "perfil selecionado.");
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    if (ImGui::Button(settings_profile.empty() ? "Restaurar padrões" : "Excluir perfil do jogo")) {
        const auto profile = settings_profile;
        const auto request = settings_request.load();
        if (Queue([this, profile, request] {
                if (profile.empty() ? reset_settings() : delete_game_settings(profile.c_str()))
                    throw std::runtime_error(Utf8(core->Error()));
                LoadSettings(profile, request);
                Complete([this] {
                    settings_changes.clear();
                    next_graphics_backend = static_cast<int>(core->GraphicsBackend());
                });
            }))
            status = "Restaurando configurações…";
    }
    ImGui::SameLine();
    ImGui::SetCursorPosX(
        std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - 240 * scale));
    if (ImGui::Button("Salvar", {72 * scale, 0}))
        ApplySettings(true);
    ImGui::SameLine();
    if (ImGui::Button("Aplicar", {72 * scale, 0}))
        ApplySettings(false);
    ImGui::SameLine();
    if (ImGui::Button("Fechar", {72 * scale, 0}))
        settings_open = false;
    ImGui::BeginChild("settings-description", {0, 0}, true);
    ImGui::TextWrapped("%s", dirty
                                 ? "Há alterações não salvas. Salve ou aplique antes de trocar de "
                                   "perfil."
                                 : "As configurações são compartilhadas com o núcleo de emulação.");
    ImGui::EndChild();
}

void ImGuiFrontend::DrawLogs() {
    if (ImGui::Button("Limpar visualização"))
        logs.clear();
    ImGui::BeginChild("log-tail", {0, 0}, true, ImGuiWindowFlags_HorizontalScrollbar);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(logs.size()));
    while (clipper.Step())
        for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index)
            ImGui::TextUnformatted(logs[index].c_str());
    ImGui::EndChild();
}

void ImGuiFrontend::OpenCheats() {
    if (!library || selection >= library->games.size() || cheats_busy)
        return;
    const auto& game = library->games[selection];
    cheats_serial = game.serial;
    cheats_version = game.version;
    cheats_title = game.title;
    cheats_open = true;
    cheats_data = nlohmann::json::object();
    CheatsCommand({{"operation", "list"}});
}

void ImGuiFrontend::CheatsCommand(nlohmann::json request) {
    if (cheats_busy)
        return;
    request["serial"] = cheats_serial;
    request["version"] = cheats_version;
    if (!request.contains("repository"))
        request["repository"] = cheats_repository == 0 ? "shadPS4" : "GoldHEN";
    cheats_busy = true;
    cheats_error.clear();
    if (!Queue([this, request] {
            try {
                std::vector<char> buffer(2 * 1024 * 1024);
                const auto code = cheats_request(request.dump().c_str(), buffer.data(),
                                                 static_cast<uint32_t>(buffer.size()));
                auto data = nlohmann::json::parse(buffer.data());
                Complete([this, data = std::move(data), code] {
                    cheats_busy = false;
                    if (code)
                        cheats_error = data.value("error", "Falha em cheats/patches.");
                    else
                        cheats_data = data;
                });
            } catch (const std::exception& error) {
                const std::string message = error.what();
                Complete([this, message] {
                    cheats_busy = false;
                    cheats_error = message;
                });
            }
        })) {
        cheats_busy = false;
        cheats_error = "A fila de tarefas está ocupada.";
    }
}

void ImGuiFrontend::DrawCheats() {
    if (!cheats_open)
        return;
    const auto display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize({std::min(900.0f, display.x - 24), std::min(650.0f, display.y - 24)},
                             ImGuiCond_Appearing);
    if (ImGui::Begin("Cheats / Patches", &cheats_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextWrapped("%s | %s | %s", cheats_title.c_str(), cheats_serial.c_str(),
                           cheats_version.c_str());
        if (!cheats_error.empty())
            ImGui::TextWrapped("%s", cheats_error.c_str());
        if (cheats_busy)
            ImGui::TextUnformatted("Processando…");
        ImGui::BeginDisabled(cheats_busy);
        const char* repositories[]{"shadPS4", "GoldHEN"};
        ImGui::Combo("Repositório", &cheats_repository, repositories, 2);
        if (ImGui::Button("Atualizar lista local"))
            CheatsCommand({{"operation", "list"}});
        if (ImGui::BeginTabBar("cheats-patches-tabs")) {
            if (ImGui::BeginTabItem("Cheats")) {
                if (ImGui::Button("Baixar / atualizar cheats deste jogo"))
                    CheatsCommand({{"operation", "download_cheats"}});
                ImGui::TextWrapped("Cheats são aplicados durante a execução e podem "
                                   "ser desativados na mesma sessão.");
                const bool matching_game = game_running && active_game_path.size() && library &&
                                           std::any_of(library->games.begin(), library->games.end(),
                                                       [this](const auto& game) {
                                                           return game.path == active_game_path &&
                                                                  game.serial == cheats_serial;
                                                       });
                if (!matching_game)
                    ImGui::TextWrapped("Inicie este jogo para aplicar os cheats compatíveis.");
                if (!cheats_data.contains("cheats") || cheats_data["cheats"].empty())
                    ImGui::TextUnformatted("Nenhum arquivo de cheats local para este jogo.");
                for (const auto& file : cheats_data.value("cheats", nlohmann::json::array())) {
                    const auto filename = file.at("file").get<std::string>();
                    ImGui::PushID(filename.c_str());
                    if (ImGui::TreeNodeEx(filename.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                        if (file.contains("error")) {
                            ImGui::TextWrapped("%s", file.at("error").get<std::string>().c_str());
                        } else {
                            const bool compatible = file.at("compatible").get<bool>();
                            ImGui::Text("Versão: %s%s",
                                        file.at("version").get<std::string>().c_str(),
                                        compatible ? "" : " (incompatível)");
                            ImGui::TextWrapped("Créditos: %s", file.at("credits").dump().c_str());
                            ImGui::BeginDisabled(!matching_game || !compatible || cheats_busy);
                            for (const auto& mod : file.at("mods")) {
                                ImGui::PushID(mod.at("index").get<int>());
                                const auto name = mod.at("name").get<std::string>();
                                bool enabled = mod.at("enabled").get<bool>();
                                const bool button = mod.at("type") == "button";
                                const bool changed = button
                                                         ? ImGui::Button(name.c_str())
                                                         : ImGui::Checkbox(name.c_str(), &enabled);
                                if (changed)
                                    CheatsCommand({{"operation", "apply"},
                                                   {"file", filename},
                                                   {"index", mod.at("index")},
                                                   {"enabled", button || enabled}});
                                if (!mod.at("hint").is_null())
                                    ImGui::TextWrapped("%s", mod.at("hint").dump().c_str());
                                ImGui::PopID();
                            }
                            ImGui::EndDisabled();
                        }
                        ImGui::BeginDisabled(game_running || cheats_busy);
                        if (ImGui::Button("Excluir arquivo de cheats"))
                            CheatsCommand({{"operation", "delete_cheats"}, {"file", filename}});
                        ImGui::EndDisabled();
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Patches")) {
                if (ImGui::Button("Baixar / atualizar patches deste jogo"))
                    CheatsCommand({{"operation", "download_patches"}});
                ImGui::TextWrapped("As seleções são salvas automaticamente e aplicadas "
                                   "na próxima inicialização do jogo.");
                if (!cheats_data.contains("patches") || cheats_data["patches"].empty())
                    ImGui::TextUnformatted("Nenhum patch local para este jogo.");
                for (const auto& patch : cheats_data.value("patches", nlohmann::json::array())) {
                    const auto repo = patch.at("repository").get<std::string>();
                    const auto filename = patch.at("file").get<std::string>();
                    ImGui::PushID((repo + "/" + filename).c_str());
                    ImGui::PushID(patch.at("index").get<int>());
                    bool enabled = patch.at("enabled").get<bool>();
                    ImGui::BeginDisabled((!patch.at("compatible").get<bool>() && !enabled) ||
                                         cheats_busy);
                    if (ImGui::Checkbox(patch.at("name").get<std::string>().c_str(), &enabled))
                        CheatsCommand({{"operation", "patch"},
                                       {"repository", repo},
                                       {"file", filename},
                                       {"index", patch.at("index")},
                                       {"enabled", enabled}});
                    ImGui::EndDisabled();
                    ImGui::TextWrapped("%s | Versão %s | %s", repo.c_str(),
                                       patch.at("version").get<std::string>().c_str(),
                                       patch.at("author").get<std::string>().c_str());
                    if (ImGui::TreeNode("Notas e arquivo")) {
                        ImGui::TextWrapped("%s", patch.at("note").get<std::string>().c_str());
                        ImGui::TextWrapped("%s", filename.c_str());
                        if (ImGui::Button("Excluir arquivo de patches"))
                            CheatsCommand({{"operation", "delete_patches"},
                                           {"repository", repo},
                                           {"file", filename}});
                        ImGui::TreePop();
                    }
                    ImGui::Separator();
                    ImGui::PopID();
                    ImGui::PopID();
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::EndDisabled();
    }
    ImGui::End();
}
