#include "pch.h"
#include "UwpDiagnostics.h"

#include "ImGuiFrontend.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <numeric>
#include <fstream>
#include <filesystem>
#include <ppltasks.h>
#include <roapi.h>
#include <wincodec.h>
#include <windows.ui.xaml.media.dxinterop.h>
#include <imgui.h>
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
    std::replace(text.begin(), text.end(), '_', ' ');
    if (!text.empty())
        text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
    return text;
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
        if (format != 0x0204 || key >= bytes.size() - keys || offset > bytes.size() - data ||
            length > bytes.size() - data - offset)
            continue;
        const auto* name = reinterpret_cast<const char*>(bytes.data() + keys + key);
        const auto* end = static_cast<const char*>(std::memchr(name, 0, bytes.size() - keys - key));
        if (!end)
            continue;
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
ImGuiFrontend::~ImGuiFrontend() {
    Stop();
}
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
                return stopping.load() || worker_stopping.load() || !commands.empty() || filter_job ||
                       !thumbnail_jobs.empty();
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
  RESOLVE(set_host_state, "shadps4_set_host_state");
  RESOLVE(set_hdr_output, "shadps4_set_hdr_output");
  RESOLVE(set_setting, "shadps4_set_setting");
  RESOLVE(reset_settings, "shadps4_reset_settings");
  RESOLVE(mount, "shadps4_mount_storage_folder");
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
        std::chrono::system_clock::now().time_since_epoch()).count();
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
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |=
        ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui::StyleColorsDark();
    renderer = std::make_unique<FrontendRenderer>();
    renderer->Initialize(
        initial.width, initial.height,
        [this](IDXGISwapChain *chain) { return Attach(chain); });
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
      {
        std::lock_guard<std::mutex> lock{input_mutex};
        snapshot = input;
        input.wheel = 0;
        input.characters.clear();
      }
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
      for (auto &update : updates)
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
        core->Reload(user_root, snapshot.width, snapshot.height, true,
                     game_surface);
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
          LoadSettings();
        });
      }
      const bool running = core->State() == SHADPS4_RUNNING;
      if (running != game_running) {
        game_running = running;
        menu_open = !running;
      }
      const bool pressed = snapshot.keys[27];
      if (game_running && pressed && !menu_pressed)
        menu_open = !menu_open;
      menu_pressed = pressed;
      if (set_host_state(!snapshot.suspended && snapshot.active,
                         snapshot.captured ||
                             (game_running
                                  ? menu_open
                                  : io.NavActive || io.WantCaptureKeyboard)))
        throw std::runtime_error(Utf8(core->Error()));
      if (snapshot.suspended || !snapshot.active) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        continue;
      }
      set_hdr_output(snapshot.hdr_output, snapshot.sdr_white_nits);
      renderer->Resize(snapshot.width, snapshot.height, snapshot.scale_x, snapshot.scale_y);
      if (game_surface)
        core->Resize(snapshot.width, snapshot.height, snapshot.scale_x, snapshot.scale_y);
      renderer->SetTransparent(game_running);
      const auto now = std::chrono::steady_clock::now();
      const auto delta = std::chrono::duration<float>(now - last).count();
      last = now;
      SubmitInput(snapshot);
      io.FontGlobalScale = std::clamp(
          std::min(snapshot.width / 1280.0f, snapshot.height / 720.0f) *
              ui_scale,
          0.75f, 3.0f);
      renderer->BeginFrame(delta);
      unsigned uploads{};
      while (!decoded.empty() && uploads < 2) {
        if (!renderer->Upload(decoded.front()))
          break;
        pending_images.erase(decoded.front().key);
        decoded.pop_front();
        ++uploads;
      }
      Draw();
      renderer->EndFrame();
    }
  } catch (const winrt::hresult_error &error) {
    try {
      ReportFailure(L"D3D12 HRESULT " +
                std::to_wstring(static_cast<uint32_t>(error.code().value)) +
                L": " + std::wstring(error.message()));
    } catch (...) {
    }
  } catch (Platform::Exception ^ error) {
    try {
      ReportFailure(error->Message->Data());
    } catch (...) {
    }
  } catch (const std::exception &error) {
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
            if (root.folder.Get()->IsEqual(source.Get())) {
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
        ~Lease() {
            release(token);
        }
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
    std::unordered_map<std::string, std::string> names;
    for (size_t index = 0; index < roots.size(); ++index) {
        const auto& root = roots[index];
        const auto label = Utf8(root.folder.Get()->Name->Data());
        result->roots.push_back({label, root.token, root.enabled});
        if (!root.enabled)
            continue;
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
            if (stat((game.metadata_path + "/param.sfo").c_str(), &info) &&
                path.size() >= 7 && path.compare(path.size() - 7, 7, "/Image0") == 0) {
                const auto candidate = path.substr(0, path.find_last_of('/')) + "/Sc0";
                if (!stat((candidate + "/param.sfo").c_str(), &info))
                    game.metadata_path = candidate;
            }
            try {
                const auto metadata = Sfo(ReadFile(game.metadata_path + "/param.sfo", 256 * 1024));
                if (metadata.find("TITLE") != metadata.end())
                    game.title = metadata.at("TITLE");
                if (metadata.find("TITLE_ID") != metadata.end())
                    game.serial = metadata.at("TITLE_ID");
                if (metadata.find("APP_VER") != metadata.end())
                    game.version = metadata.at("APP_VER");
            } catch (...) {
            }
            result->games.push_back(std::move(game));
            continue;
        }
        if (level >= depth)
            continue;
        struct Context {
            std::deque<std::pair<std::string, uint32_t>>* folders;
            std::string parent;
            uint32_t depth;
        } context{&folders, path, level + 1};
        if (enumerate(
                path.c_str(),
                [](void* user, const char* name, const ShadPS4FileInfo* entry) {
                    auto* context = static_cast<Context*>(user);
                    if (entry->is_directory && name[0] != '.')
                        context->folders->emplace_back(context->parent + "/" + name,
                                                       context->depth);
                },
                &context))
            throw std::runtime_error(Utf8(core->Error()));
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
    std::lock_guard<std::mutex> lock{work_mutex};
    filter_job = [this, source, query, request] {
        auto indices = std::make_shared<std::vector<size_t>>();
        indices->reserve(source->games.size());
        for (size_t index = 0; index < source->games.size(); ++index) {
            const auto& game = source->games[index];
            if (query.empty() || Lower(game.title).find(query) != std::string::npos ||
                Lower(game.serial).find(query) != std::string::npos)
                indices->push_back(index);
        }
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
    Complete([this, values = std::move(values), hidden, scale, fps, request]() mutable {
        if (request != settings_request.load())
            return;
        settings = std::move(values);
        excluded = hidden;
        ui_scale = scale;
        show_fps = fps;
        status = "Configurações carregadas.";
    });
}
void ImGuiFrontend::SaveSetting(const Setting& setting, nlohmann::json value) {
    const auto group = setting.group, key = setting.key;
    const auto profile = setting.profile;
    const auto request = settings_request.load();
    if (!Queue([this, group, key, value, profile, request] {
            if (profile.empty() ? set_setting(group.c_str(), key.c_str(), value.dump().c_str())
                                : set_game_setting(profile.c_str(), group.c_str(), key.c_str(),
                                                   value.dump().c_str()))
                throw std::runtime_error(Utf8(core->Error()));
            LoadSettings(profile, request);
        }))
        status = "Aguarde as operações pendentes.";
}

ThumbnailPixels ImGuiFrontend::Decode(const std::string& key, const std::string& path) {
    auto bytes = ReadFile(path, 16 * 1024 * 1024);
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
        if (pads->Size && !capture) {
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

void ImGuiFrontend::Draw() {
    if (game_running && !menu_open)
        return;
    const auto display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(display);
    ImGui::Begin("shadPS4", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted("shadPS4  |  UWP / Direct3D 12");
    if (show_fps) {
        ImGui::SameLine();
        ImGui::Text("%.1f FPS", ImGui::GetIO().Framerate);
    }
    constexpr std::array labels{"Biblioteca", "Configurações", "Logs"};
    for (int index = 0; index < labels.size(); ++index) {
        if (index)
            ImGui::SameLine();
        if (ImGui::Button(labels[index]))
            tab = index;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(scanning.load() || session_pending || game_running);
    if (ImGui::Button("Adicionar pasta") && choose_folder)
        choose_folder(L"library");
    ImGui::SameLine();
    if (ImGui::Button("Atualizar biblioteca"))
        Queue([this] { Scan(); });
    ImGui::EndDisabled();
    if (ImGui::BeginMenu("Importar", !session_pending && !game_running)) {
        if (ImGui::MenuItem("Módulos de sistema") && choose_folder)
            choose_folder(L"sys_modules");
        if (ImGui::MenuItem("Fontes") && choose_folder)
            choose_folder(L"fonts");
        if (ImGui::MenuItem("DLCs") && choose_folder)
            choose_folder(L"addcont");
        ImGui::EndMenu();
    }
    ImGui::TextWrapped("%s", status.c_str());
    if (scanning.load())
        ImGui::Text("Examinando pastas: %u", scanned.load());
    ImGui::Separator();
    if (tab == 0)
        DrawLibrary();
    else if (tab == 1) {
        ImGui::BeginDisabled(session_pending || game_running);
        DrawSettings();
        ImGui::EndDisabled();
    }
    else
        DrawLogs();
    ImGui::End();
}

void ImGuiFrontend::DrawLibrary() {
    if (library && ImGui::TreeNode("Pastas da biblioteca")) {
        ImGui::BeginDisabled(scanning.load() || session_pending || game_running);
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
        ImGui::TreePop();
    }
    ImGui::SetNextItemWidth(std::min(500.0f, ImGui::GetContentRegionAvail().x));
    if (ImGui::InputText("Pesquisar", search, sizeof(search)))
        Filter();
    if (!library || !visible || visible->empty()) {
        ImGui::TextUnformatted("Selecione uma pasta com jogos PS4. Para Image0/Sc0, escolha a pasta que contém ambas.");
        return;
    }
    ImGui::Text("%zu jogos | %zu/64 capas residentes", visible->size(), renderer->ImageCount());
    const float row_height = 78 * ImGui::GetIO().FontGlobalScale;
    ImGui::BeginChild("catalogue", {0, -44 * ImGui::GetIO().FontGlobalScale}, true);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(visible->size()), row_height);
    while (clipper.Step())
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const auto index = (*visible)[row];
            const auto& game = library->games[index];
            ImGui::PushID(static_cast<int>(index));
            const auto start = ImGui::GetCursorPos();
            const auto key = std::to_string(library->generation) + ":" + game.path;
            const auto texture = renderer->Image(key);
            const float icon = 64 * ImGui::GetIO().FontGlobalScale;
            if (texture)
                ImGui::Image(reinterpret_cast<ImTextureID>(texture), {icon, icon});
            else {
                ImGui::Dummy({icon, icon});
                RequestImage(key, game.metadata_path + "/icon0.png");
            }
            ImGui::SameLine();
            ImGui::BeginGroup();
            if (ImGui::Selectable(game.title.c_str(), selection == index))
                selection = index;
            ImGui::Text("%s  %s", game.serial.c_str(), game.version.c_str());
            ImGui::EndGroup();
            ImGui::SetCursorPos({start.x, start.y + row_height});
            ImGui::PopID();
        }
    ImGui::EndChild();
    ImGui::BeginDisabled(!game_surface || scanning.load() || session_pending ||
                         core->State() != SHADPS4_READY || !library ||
                         selection >= library->games.size());
    if (ImGui::Button("Iniciar jogo")) {
      const auto executable = library->games[selection].path + "/eboot.bin";
      session_pending = true;
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
    ImGui::EndDisabled();
    if (game_running) {
      ImGui::SameLine();
      if (ImGui::Button(session_paused ? "Continuar" : "Pausar")) {
        session_paused = !session_paused;
        core->Pause(session_paused);
      }
      ImGui::SameLine();
      if (ImGui::Button("Parar")) {
        status = "Encerrando sessão…";
        core->Stop();
      }
    }
}

void ImGuiFrontend::DrawSettings() {
  if (next_graphics_backend < 0)
    next_graphics_backend = static_cast<int>(core->GraphicsBackend());
  const char* backends[] = {"Vulkan / Dozen", "D3D12 nativo (experimental)"};
  if (ImGui::Combo("Backend grafico", &next_graphics_backend, backends, 2)) {
    try {
      ApplicationData::Current->LocalSettings->Values->Insert(
          L"shadps4.graphics.backend",
          ref new Platform::String(next_graphics_backend ? L"d3d12" : L"dozen"));
      reload_core = next_graphics_backend != static_cast<int>(core->GraphicsBackend());
      session_pending = reload_core;
      status = reload_core ? "Aplicando backend grafico e recarregando a biblioteca…"
                           : "Backend grafico selecionado.";
    } catch (Platform::Exception^) {
      next_graphics_backend = static_cast<int>(core->GraphicsBackend());
      status = "Nao foi possivel salvar o backend grafico.";
    }
  }
  ImGui::TextWrapped("Backend ativo: %s. A selecao sera usada ao iniciar jogos.",
                    backends[core->GraphicsBackend()]);
  if (next_graphics_backend == SHADPS4_GRAPHICS_D3D12_NATIVE)
    ImGui::TextWrapped("Compatibilidade experimental: alguns jogos ainda nao iniciam.");

  if (ImGui::BeginCombo("Perfil", settings_profile.empty()
                                      ? "Global"
                                      : settings_profile.c_str())) {
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(library ? library->profiles.size() + 1 : 1));
    while (clipper.Step())
      for (int index = clipper.DisplayStart; index < clipper.DisplayEnd;
           ++index) {
        const auto serial =
            index ? library->profiles[index - 1].second : std::string{};
        const auto label =
            index ? library->profiles[index - 1].first + " (" + serial + ")"
                  : std::string{"Global"};
        if (ImGui::Selectable(label.c_str(), settings_profile == serial)) {
          const auto request = ++settings_request;
          if (Queue(
                  [this, serial, request] { LoadSettings(serial, request); })) {
            settings_profile = serial;
            settings.clear();
          }
        }
      }
    ImGui::EndCombo();
  }
  ImGui::Text(
      "%zu opções compatíveis; %zu opções incompatíveis preservadas e ocultas.",
      settings.size(), excluded);
  if (ImGui::Button(settings_profile.empty() ? "Restaurar padrões"
                                             : "Excluir perfil do jogo")) {
    const auto profile = settings_profile;
    const auto request = settings_request.load();
    Queue([this, profile, request] {
      if (profile.empty() ? reset_settings()
                          : delete_game_settings(profile.c_str()))
        throw std::runtime_error(Utf8(core->Error()));
      LoadSettings(profile, request);
    });
  }
  std::vector<std::string> groups;
  for (const auto &setting : settings)
    if (std::find(groups.begin(), groups.end(), setting.group) == groups.end())
      groups.push_back(setting.group);
  if (ImGui::BeginTabBar("settings-pages")) {
    for (const auto &group : groups)
      if (ImGui::BeginTabItem(group.c_str())) {
        ImGui::BeginChild(group.c_str(), {0, 0}, false);
        for (const auto &setting : settings) {
          if (setting.group != group)
            continue;
          ImGui::PushID(setting.key.c_str());
          ImGui::BeginDisabled(!setting.editable);
          if (!setting.choices.empty()) {
            std::string label = setting.value.dump();
            for (const auto &choice : setting.choices)
              if (choice.at("value") == setting.value)
                label = choice.at("label");
            if (ImGui::BeginCombo(setting.label.c_str(), label.c_str())) {
              for (const auto &choice : setting.choices)
                if (ImGui::Selectable(choice.at("label")
                                          .get_ref<const std::string &>()
                                          .c_str(),
                                      choice.at("value") == setting.value))
                  SaveSetting(setting, choice.at("value"));
              ImGui::EndCombo();
            }
          } else if (setting.value.is_boolean()) {
            bool value = setting.value;
            if (ImGui::Checkbox(setting.label.c_str(), &value))
              SaveSetting(setting, value);
          } else if (setting.value.is_number_integer()) {
            int64_t value = setting.value,
                    minimum = static_cast<int64_t>(setting.minimum),
                    maximum = static_cast<int64_t>(setting.maximum);
            if (ImGui::InputScalar(setting.label.c_str(), ImGuiDataType_S64,
                                   &value, nullptr, nullptr, "%lld",
                                   ImGuiInputTextFlags_EnterReturnsTrue))
              SaveSetting(setting, std::clamp(value, minimum, maximum));
          } else if (setting.value.is_number()) {
            double value = setting.value;
            if (ImGui::InputDouble(setting.label.c_str(), &value, 0, 0, "%.3f",
                                   ImGuiInputTextFlags_EnterReturnsTrue))
              SaveSetting(setting,
                          std::clamp(value, setting.minimum, setting.maximum));
          } else {
            ImGui::TextWrapped(
                "%s: %s", setting.label.c_str(),
                setting.value.get_ref<const std::string &>().c_str());
            if (ImGui::Button("Editar") && edit_text) {
              const auto copy = setting;
              const auto request = settings_request.load();
              edit_text(
                  setting.label, setting.value, false,
                  [self = shared_from_this(), copy,
                   request](std::string value) {
                    self->Queue([self, copy, value, request] {
                      const auto json = nlohmann::json(value).dump();
                      if (copy.profile.empty()
                              ? self->set_setting(copy.group.c_str(),
                                                  copy.key.c_str(),
                                                  json.c_str())
                              : self->set_game_setting(
                                    copy.profile.c_str(), copy.group.c_str(),
                                    copy.key.c_str(), json.c_str()))
                        throw std::runtime_error(Utf8(self->core->Error()));
                      self->LoadSettings(copy.profile, request);
                    });
                  });
            }
          }
          ImGui::EndDisabled();
          if (!setting.editable &&
              ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Esta opção é global e é herdada pelo perfil.");
          ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::EndTabItem();
      }
    ImGui::EndTabBar();
  }
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
