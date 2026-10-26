#pragma once

#include "FrontendRenderer.h"
#include "ShadPS4Host.h"
#include <agile.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

struct FrontendInput {
    std::array<bool, 256> keys{};
    std::vector<uint32_t> characters;
    float x{-10000};
    float y{-10000};
    float wheel{};
    bool left{};
    bool right{};
    bool captured{};
    bool system_pointer{};
    bool suspended{};
    bool active{true};
    bool visible{true};
    bool hdr_output{};
    float sdr_white_nits{80.0f};
    uint32_t width{1280};
    uint32_t height{720};
    float scale_x{1.0f}, scale_y{1.0f};
};

struct FrontendGame {
    std::string path;
    std::string title;
    std::string serial;
    std::string version;
    std::string metadata_path;
    std::string region;
    std::string firmware;
    std::string format{"Pasta"};
    std::string executable;
    std::string launch_error;
    std::string source_path;
};
struct FrontendRoot {
    Platform::Agile<Windows::Storage::StorageFolder> folder;
    std::string token;
    bool enabled{true};
    Platform::Agile<Windows::Storage::StorageFile> file;
};

class ImGuiFrontend : public std::enable_shared_from_this<ImGuiFrontend> {
  public:
    explicit ImGuiFrontend(Windows::UI::Xaml::Controls::SwapChainPanel ^ panel,
                           Windows::UI::Xaml::Controls::SwapChainPanel ^ game_panel = nullptr);
    ~ImGuiFrontend();
    void Start(std::wstring root);
    void Stop();
    void UpdateInput(const std::function<void(FrontendInput&)>& update);
    void SetFolder(Platform::Agile<Windows::Storage::StorageFolder> folder, std::string token);
    void SetFolders(std::vector<FrontendRoot> folders);
    void SetPackage(Platform::Agile<Windows::Storage::StorageFile> file, std::string token);
    void ImportFolder(Platform::Agile<Windows::Storage::StorageFolder> folder, std::wstring target);
    std::function<void(std::wstring)> choose_folder;
    std::function<void(std::string, std::string, bool, std::function<void(std::string)>)> edit_text;
    std::function<void(ShadPS4TextInput, std::function<void(bool, std::wstring)>)> game_text_input;
    std::function<void(std::wstring)> failure;
    std::function<void()> ready;
    std::function<void()> toggle_fullscreen;
    std::function<void(bool)> set_pointer_mode;
    std::function<void(bool, float)> set_game_fps;

  private:
    struct Setting {
        std::string group;
        std::string key;
        std::string label;
        nlohmann::json value;
        nlohmann::json choices;
        double minimum{};
        double maximum{};
        bool editable{true};
        std::string profile;
    };
    struct Library {
        std::vector<FrontendGame> games;
        std::shared_ptr<const std::vector<size_t>> indices;
        uint64_t generation{};
        std::vector<std::pair<std::string, std::string>> profiles;
        struct Root {
            std::string name;
            std::string token;
            bool enabled;
        };
        std::vector<Root> roots;
    };
    void Run(std::wstring root);
    void Work();
    void ResolveApi();
    bool Queue(std::function<void()> job, bool thumbnail = false);
    void Complete(std::function<void()> update);
    void LoadSettings(std::string profile = {}, uint64_t request = 0);
    void Scan();
    void ReadMetadata(FrontendGame& game);
    void ChangeRoot(std::string token, bool enabled, bool remove);
    void PersistRoots();
    void Filter();
    std::vector<uint8_t> ReadFile(const std::string& path, uint64_t limit);
    ThumbnailPixels Decode(const std::string& key, const std::string& path);
    void RequestImage(const std::string& key, const std::string& path);
    void Draw();
    void DrawLibrary();
    void DrawSettings();
    void DrawSetting(Setting& setting);
    void DrawLogs();
    void StartGame();
    void ApplySettings(bool close);
    bool ToolbarButton(const char* label, const char* icon, bool enabled = true);
    void ReportFailure(const std::wstring& message) noexcept;
    void SubmitInput(const FrontendInput& input);
    HRESULT Attach(IDXGISwapChain* chain);
    std::shared_ptr<ShadPS4Host> core;
    bool game_surface{}, game_running{}, menu_open{true}, menu_pressed{};
    bool session_pending{}, session_paused{};
    uint64_t text_input_id{};
    std::deque<std::function<void()>> text_input_results;
    bool pointer_visible{true};
    Platform::Agile<Windows::UI::Xaml::Controls::SwapChainPanel> panel;
    Platform::Agile<Windows::UI::Core::CoreDispatcher> dispatcher;
    std::unique_ptr<FrontendRenderer> renderer;
    std::mutex input_mutex;
    FrontendInput input;
    std::mutex work_mutex;
    std::condition_variable work_ready;
    std::deque<std::function<void()>> commands;
    std::function<void()> filter_job;
    std::deque<std::function<void()>> thumbnail_jobs;
    std::deque<std::function<void()>> completed;
    std::deque<std::string> pending_logs;
    std::thread worker;
    std::atomic_bool stopping{};
    std::atomic_bool worker_stopping{};
    bool reload_core{};
    std::atomic_bool scanning{};
    std::atomic<uint32_t> scanned{};
    std::atomic<uint32_t> scan_depth{2};
    std::atomic<uint64_t> generation{};
    uint64_t filter_generation{};
    std::shared_ptr<const Library> library;
    std::shared_ptr<const std::vector<size_t>> visible;
    std::vector<Setting> settings;
    std::unordered_set<std::string> pending_images;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> failed_images;
    std::deque<ThumbnailPixels> decoded;
    std::deque<std::string> logs;
    size_t excluded{};
    bool settings_open{}, about_open{};
    bool show_game_list{true}, show_game_log{true}, grid_view{};
    float icon_size{48.0f};
    int sort_column{1};
    bool sort_descending{};
    float log_height{140.0f};
    std::string launcher_layout;
    std::string active_game_path;
    bool restart_requested{};
    std::string requested_settings_group;
    std::map<std::pair<std::string, std::string>, nlohmann::json> settings_changes;
    bool settings_saving{};
    size_t selection{};
    float ui_scale{1.0f};
    bool show_fps{true};
    decltype(&shadps4_get_game_frame_count) get_game_frame_count{};
    decltype(&shadps4_cheats_request) cheats_request{};
    bool cheats_open{}, cheats_busy{};
    std::string cheats_serial, cheats_version, cheats_title, cheats_error;
    nlohmann::json cheats_data;
    int cheats_repository{};
    void OpenCheats();
    void CheatsCommand(nlohmann::json request);
    void DrawCheats();
    std::string settings_profile;
    std::atomic<uint64_t> settings_request{};
    char search[256]{};
    std::string status{"Inicializando…"};
    int next_graphics_backend{-1};
    std::wstring user_root;
    std::vector<FrontendRoot> roots;
    std::vector<std::string> root_mounts;
    decltype(&shadps4_unmount_storage_folder) unmount{};
    decltype(&shadps4_get_settings) get_settings{};
    decltype(&shadps4_set_hdr_output) set_hdr_output{};
    decltype(&shadps4_set_host_state) set_host_state{};
    decltype(&shadps4_get_text_input) get_text_input{};
    decltype(&shadps4_complete_text_input) complete_text_input{};
    decltype(&shadps4_set_setting) set_setting{};
    decltype(&shadps4_reset_settings) reset_settings{};
    decltype(&shadps4_mount_storage_folder) mount{};
    decltype(&shadps4_mount_pkg) mount_pkg{};
    decltype(&shadps4_mount_pkg_file) mount_pkg_file{};
    decltype(&shadps4_storage_enumerate) enumerate{};
    decltype(&shadps4_storage_stat) stat{};
    decltype(&shadps4_open_mounted_file) open{};
    decltype(&shadps4_file_read) read{};
    decltype(&shadps4_file_get_size) size{};
    decltype(&shadps4_file_close) close{};
    decltype(&shadps4_get_game_settings) get_game_settings{};
    decltype(&shadps4_set_game_setting) set_game_setting{};
    decltype(&shadps4_delete_game_settings) delete_game_settings{};
};
