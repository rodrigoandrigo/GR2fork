#include "uwp/shadps4_api.h"

#include <cctype>
#include <cmath>
#include <cstring>
#include <mutex>
#include <windows.h>
#include "common/path_util.h"
#include "core/emulator_settings.h"
#include "core/user_settings.h"
#include "uwp/session.h"

namespace {
using nlohmann::json;
std::mutex settings_mutex;
constexpr std::array groups{"General", "Network", "Log", "Debug",
                            "Input",   "Audio",   "GPU", "Vulkan"};

std::string Unsupported(std::string_view group, std::string_view key, const json& value) {
    if (group == "GPU" && (key == "window_width" || key == "window_height" ||
                           key == "full_screen" || key == "full_screen_mode"))
        return "Window size and mode are controlled by the UWP host; use View/Fullscreen.";
    if (group == "GPU" && key == "present_mode")
        return "The host presents every frame in FIFO order without dropping frames.";
    if (group == "Vulkan" && (key.starts_with("vkvalidation") || key.starts_with("vkcrash") ||
                              key == "renderdoc_enabled"))
        return "Desktop validation/capture tools are not included in the UWP package.";
    if (group == "GPU" && key == "userfaultfd")
        return "userfaultfd is a Linux API and is unavailable in UWP.";
    if (group == "GPU" && key == "one_core_gpu_thread_pin")
        return "Processor affinity is controlled by the system in AppContainer.";
    if (group == "GPU" && key == "direct_memory_access_enabled")
        return "Dozen uses bounded descriptors; direct BDA access is not enabled on this path.";
    if (group == "General") {
        if (key == "install_dirs" || key.ends_with("_dir")) {
            return "UWP uses brokered folders and private storage; use Library/Import.";
        }
        if (key == "discord_rpc_enabled") {
            return "Discord RPC is not integrated with AppContainer.";
        }
        if (key == "show_splash" || key.starts_with("trophy_")) {
            return "Guest splash/trophy presentation does not have a UWP adapter yet.";
        }
    }
    if (group == "Network" && key == "enable_upnp") {
        return "Desktop UPnP discovery/configuration does not have an AppContainer adapter.";
    }
    if (group == "Log" && key == "type") {
        return "The host uses callbacks and files, without a desktop console.";
    }
    if (group == "Debug" && key == "config_version")
        return "Internal metadata, not a preference.";
    if (group == "Input") {
        if (key.starts_with("gyro_") || key == "motion_controls_enabled") {
            return "The SDL WinRT backend does not provide motion sensors on this host.";
        }
        if (key == "usb_device_backend" || key == "camera_id") {
            return "Desktop USB/camera access does not have a brokered adapter on this host.";
        }
        if (key.starts_with("cursor_") || key == "background_controller_input") {
            return "Pointer input, focus and suspension are controlled by the UWP host.";
        }
    }
    if (!value.is_primitive() || value.is_null()) {
        return "Collection without a compatible editor; the persisted value is preserved.";
    }
    return {};
}

json Choices(std::string_view key) {
    json result = json::array();
    const auto add = [&](json value, const char* label) {
        result.push_back({{"value", value}, {"label", label}});
    };
    if (key == "readbacks_mode") {
        add(0, "Disabled");
        add(1, "Relaxed");
        add(2, "Accurate");
    } else if (key == "adaptive_skipcaches_mode") {
        add(0, "Disabled");
        add(1, "Adaptive");
        add(2, "Forced");
        add(3, "Validation only");
    } else if (key == "audio_backend") {
        add(0, "SDL3");
        add(1, "OpenAL");
        add(2, "XAudio2");
    } else if (key == "openal_hrtf") {
        add(0, "Automatic");
        add(1, "Enabled");
        add(2, "Disabled");
    } else if (key == "openal_output_mode") {
        add(0, "Automatic");
        add(1, "Stereo");
        add(2, "Quad");
        add(3, "5.1");
        add(4, "7.1");
    } else if (key == "console_language") {
        constexpr std::array languages{"Japanese",
                                       "English (US)",
                                       "French",
                                       "Spanish",
                                       "German",
                                       "Italian",
                                       "Dutch",
                                       "Portuguese (Portugal)",
                                       "Russian",
                                       "Korean",
                                       "Traditional Chinese",
                                       "Simplified Chinese",
                                       "Finnish",
                                       "Swedish",
                                       "Danish",
                                       "Norwegian",
                                       "Polish",
                                       "Portuguese (Brazil)",
                                       "English (UK)",
                                       "Turkish",
                                       "Spanish (Latin America)",
                                       "Arabic",
                                       "French (Canada)",
                                       "Czech",
                                       "Hungarian",
                                       "Greek",
                                       "Romanian",
                                       "Thai",
                                       "Vietnamese",
                                       "Indonesian",
                                       "Ukrainian"};
        for (size_t index = 0; index < languages.size(); ++index)
            add(index, languages[index]);
    } else if (key == "flush_level") {
        add("", "Default");
        add("trace", "Trace");
        add("debug", "Debug");
        add("info", "Info");
        add("warning", "Warning");
        add("error", "Error");
        add("critical", "Critical");
        add("off", "Off");
    } else if (key == "special_pad_class") {
        constexpr std::array labels{"Default",          "Guitar",  "Drums", "DJ turntable",
                                    "Dance mat", "Navigation", "Steering wheel", "Stick",
                                    "Fight stick",     "Gun"};
        for (int index = 0; index < labels.size(); ++index)
            add(index, labels[index]);
    }
    return result;
}

std::pair<double, double> Bounds(std::string_view key, const json& value) {
    if (key == "internal_screen_width" || key == "internal_screen_height")
        return {64, 16384};
    if (key == "rcas_attenuation")
        return {0, 1000};
    if (key == "gpu_id")
        return {-1, 15};
    if (key == "stream_copy_workers" || key == "stream_copy_lane_threads")
        return {1, 16};
    if (key == "findimg_memo_ways")
        return {1, 8};
    if (key == "findimg_memo_entries" || key == "shader_params_memo_entries")
        return {0, 65536};
    if (key == "spec_key_fast" || key == "spec_fp_canonical" || key == "bind_write_plan")
        return {0, 2};
    if (key == "draw_glue_memo")
        return {0, 3};
    if (key == "pipeline_sync_budget_ms")
        return {0, 50000};
    if (key == "volume_slider")
        return {0, 500};
    if (key == "extra_dmem_in_mbytes" || key == "extra_fmem_in_mbytes")
        return {0, 20000};
    if (key == "big_picture_scale")
        return {500, 2000};
    if (key == "big_picture_folder_depth")
        return {0, 5};
    if (key == "p2p_port")
        return {0, 65535};
    if (key == "vblank_frequency")
        return {1, 1000};
    if (key == "size_limit")
        return {0, 16.0 * 1024 * 1024 * 1024};
    if (key == "max_skip_duration")
        return {0, 3600000};
    return {value.is_number_unsigned() ? 0.0 : -2147483648.0, 2147483647.0};
}

bool Overrideable(std::string_view group, std::string_view key) {
    std::vector<OverrideItem> fields;
    if (group == "General")
        fields = GeneralSettings{}.GetOverrideableFields();
    else if (group == "Network")
        fields = NetworkSettings{}.GetOverrideableFields();
    else if (group == "Log")
        fields = LogSettings{}.GetOverrideableFields();
    else if (group == "Debug")
        fields = DebugSettings{}.GetOverrideableFields();
    else if (group == "Input")
        fields = InputSettings{}.GetOverrideableFields();
    else if (group == "Audio")
        fields = AudioSettings{}.GetOverrideableFields();
    else if (group == "GPU")
        fields = GPUSettings{}.GetOverrideableFields();
    else if (group == "Vulkan")
        fields = VulkanSettings{}.GetOverrideableFields();
    return std::ranges::any_of(fields, [&](const auto& field) { return key == field.key; });
}

json Snapshot(EmulatorSettingsImpl& settings = EmulatorSettings, bool specific = false) {
    json entries = json::array();
    const auto mode = settings.GetConfigMode();
    struct Restore {
        EmulatorSettingsImpl& settings;
        ConfigMode mode;
        ~Restore() {
            settings.SetConfigMode(mode);
        }
    } restore{settings, mode};
    for (const auto group : groups) {
        settings.SetConfigMode(ConfigMode::Clean);
        const auto defaults = settings.GetGroupValues(group);
        settings.SetConfigMode(mode);
        const auto values = settings.GetGroupValues(group);
        for (const auto& [key, value] : values.items()) {
            const auto reason = Unsupported(group, key, value);
            const auto [minimum, maximum] = Bounds(key, value);
            entries.push_back({{"group", group},
                               {"key", key},
                               {"value", value},
                               {"default", defaults.at(key)},
                               {"compatible", reason.empty()},
                               {"editable", !specific || Overrideable(group, key)},
                               {"reason", reason},
                               {"choices", Choices(key)},
                               {"minimum", minimum},
                               {"maximum", maximum}});
        }
    }
    return {{"version", 1}, {"entries", entries}};
}

std::filesystem::path GamePath(const char* serial) {
    if (!serial || !*serial || std::strlen(serial) > 32 ||
        std::ranges::any_of(std::string_view{serial}, [](unsigned char c) {
            return !std::isalnum(c) && c != '_' && c != '-';
        }))
        throw std::runtime_error("Invalid profile identifier.");
    return Common::FS::GetUserPath(Common::FS::PathType::CustomConfigs) /
           (std::string{serial} + ".json");
}

void WriteSnapshot(const json& document, char* data, uint32_t capacity, uint32_t* required) {
    if (!required || (!data && capacity))
        throw std::runtime_error("Invalid buffer.");
    const auto text = document.dump();
    *required = static_cast<uint32_t>(text.size() + 1);
    if (!data || !capacity)
        return;
    if (capacity < *required)
        throw std::runtime_error("Buffer too small.");
    std::memcpy(data, text.c_str(), *required);
}

template <typename F>
int32_t Call(F operation) {
    try {
        std::scoped_lock lock{settings_mutex};
        if (shadps4_get_state() != SHADPS4_READY) {
            throw std::runtime_error("Settings require an initialized, stopped core.");
        }
        operation();
        return 0;
    } catch (const std::exception& error) {
        Core::Uwp::ReportError(error.what());
        return -1;
    }
}

void Validate(std::string_view group, std::string_view key, const json& old, const json& value) {
    const auto unsupported = Unsupported(group, key, old);
    if (!unsupported.empty())
        throw std::runtime_error(unsupported);
    if (old.is_boolean()          ? !value.is_boolean()
        : old.is_string()         ? !value.is_string()
        : old.is_number_integer() ? !value.is_number_integer()
                                  : !value.is_number()) {
        throw std::runtime_error("Invalid setting type.");
    }
    if (value.is_number()) {
        const auto number = value.get<double>();
        const auto [minimum, maximum] = Bounds(key, old);
        if (!std::isfinite(number) || number < minimum || number > maximum) {
            throw std::runtime_error("Value outside the allowed range.");
        }
    }
    if (value.is_string() && value.get_ref<const std::string&>().size() > 4096) {
        throw std::runtime_error("Setting text is too long.");
    }
    const auto choices = Choices(key);
    if (!choices.empty() && std::ranges::none_of(choices, [&](const auto& option) {
            return option.at("value") == value;
        })) {
        throw std::runtime_error("Unknown setting option.");
    }
}
} // namespace

int32_t shadps4_get_settings(char* data, uint32_t capacity, uint32_t* required) {
    return Call([&] {
        if (!required || (!data && capacity))
            throw std::runtime_error("Invalid buffer.");
        const auto text = Snapshot().dump();
        *required = static_cast<uint32_t>(text.size() + 1);
        if (!data || !capacity)
            return;
        if (capacity < *required)
            throw std::runtime_error("Buffer too small.");
        std::memcpy(data, text.c_str(), *required);
    });
}

int32_t shadps4_set_setting(const char* group, const char* key, const char* value_json) {
    return Call([&] {
        if (!group || !key || !value_json)
            throw std::runtime_error("Invalid setting.");
        const auto previous = EmulatorSettings.GetGroupValues(group);
        if (!previous.is_object() || !previous.contains(key))
            throw std::runtime_error("Unknown setting.");
        const auto value = json::parse(value_json);
        Validate(group, key, previous.at(key), value);
        if (!EmulatorSettings.SetGroupValues(group, {{key, value}}, false))
            throw std::runtime_error("Failed to apply setting.");
        if (!EmulatorSettings.Save()) {
            EmulatorSettings.SetGroupValues(group, previous, false);
            throw std::runtime_error(
                "Failed to save setting; in-memory change reverted.");
        }
        if (std::string_view{group} == "Log") {
            Common::Log::UpdateSinks();
            Common::Log::UpdateLogLevels(EmulatorSettings.GetLogFilter());
            Common::Log::UpdateLogFlushLevel(EmulatorSettings.GetLogFlushLevel());
        }
    });
}

int32_t shadps4_reset_settings() {
    return Call([&] {
        std::array<json, groups.size()> previous;
        const auto snapshot = Snapshot();
        for (size_t index = 0; index < groups.size(); ++index)
            previous[index] = EmulatorSettings.GetGroupValues(groups[index]);
        for (const auto& entry : snapshot.at("entries")) {
            if (!entry.at("compatible").get<bool>())
                continue;
            EmulatorSettings.SetGroupValues(
                entry.at("group").get<std::string>(),
                {{entry.at("key").get<std::string>(), entry.at("default")}}, false);
        }
        if (!EmulatorSettings.Save()) {
            for (size_t index = 0; index < groups.size(); ++index)
                EmulatorSettings.SetGroupValues(groups[index], previous[index], false);
            throw std::runtime_error("Failed to restore settings.");
        }
        Common::Log::UpdateSinks();
        Common::Log::UpdateLogLevels(EmulatorSettings.GetLogFilter());
        Common::Log::UpdateLogFlushLevel(EmulatorSettings.GetLogFlushLevel());
    });
}

int32_t shadps4_get_game_settings(const char* serial, char* data, uint32_t capacity,
                                  uint32_t* required) {
    return Call([&] {
        const auto path = GamePath(serial);
        EmulatorSettingsImpl profile;
        profile = EmulatorSettings;
        profile.ClearGameSpecificOverrides();
        if (std::filesystem::exists(path) && !profile.Load(serial))
            throw std::runtime_error("Failed to read profile.");
        WriteSnapshot(Snapshot(profile, true), data, capacity, required);
    });
}
int32_t shadps4_set_game_setting(const char* serial, const char* group, const char* key,
                                 const char* value_json) {
    return Call([&] {
        const auto path = GamePath(serial);
        if (!group || !key || !value_json || !Overrideable(group, key))
            throw std::runtime_error("This option does not allow per-game overrides.");
        EmulatorSettingsImpl profile;
        profile = EmulatorSettings;
        profile.ClearGameSpecificOverrides();
        if (std::filesystem::exists(path) && !profile.Load(serial))
            throw std::runtime_error("Failed to read profile.");
        const auto previous = profile.GetGroupValues(group);
        if (!previous.contains(key))
            throw std::runtime_error("Unknown setting.");
        const auto value = json::parse(value_json);
        Validate(group, key, previous.at(key), value);
        if (!profile.SetGroupValues(group, {{key, value}}, true) || !profile.Save(serial))
            throw std::runtime_error("Failed to save profile.");
    });
}
int32_t shadps4_delete_game_settings(const char* serial) {
    return Call([&] { std::filesystem::remove(GamePath(serial)); });
}
