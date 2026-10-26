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
        return "O tamanho e o modo da janela são controlados pelo host UWP; use Exibir/Tela cheia.";
    if (group == "GPU" && key == "present_mode")
        return "O host apresenta todos os frames em FIFO, sem descarte.";
    if (group == "Vulkan" && (key.starts_with("vkvalidation") || key.starts_with("vkcrash") ||
                              key == "renderdoc_enabled"))
        return "As ferramentas de validação/captura desktop não estão incluídas no pacote UWP.";
    if (group == "GPU" && key == "userfaultfd")
        return "userfaultfd é uma API Linux, indisponível no UWP.";
    if (group == "GPU" && key == "one_core_gpu_thread_pin")
        return "A afinidade dos processadores é controlada pelo sistema no AppContainer.";
    if (group == "GPU" && key == "direct_memory_access_enabled")
        return "O Dozen usa descritores com limites; acesso BDA direto não está habilitado neste caminho.";
    if (group == "General") {
        if (key == "install_dirs" || key.ends_with("_dir")) {
            return "O UWP usa pastas brokered e armazenamento privado; use Biblioteca/Importar.";
        }
        if (key == "discord_rpc_enabled") {
            return "Discord RPC não está integrado ao AppContainer.";
        }
        if (key == "show_splash" || key.starts_with("trophy_")) {
            return "A apresentação de splash/troféus guest ainda não tem adaptador UWP.";
        }
    }
    if (group == "Network" && key == "enable_upnp") {
        return "A descoberta/configuração UPnP desktop não tem adaptador AppContainer.";
    }
    if (group == "Log" && key == "type") {
        return "O host usa callback e arquivo, sem console desktop.";
    }
    if (group == "Debug" && key == "config_version")
        return "Metadado interno, não é uma preferência.";
    if (group == "Input") {
        if (key.starts_with("gyro_") || key == "motion_controls_enabled") {
            return "O backend SDL WinRT não fornece sensores de movimento neste host.";
        }
        if (key == "usb_device_backend" || key == "camera_id") {
            return "USB/câmera desktop não têm adaptador brokered neste host.";
        }
        if (key.starts_with("cursor_") || key == "background_controller_input") {
            return "Ponteiro, foco e suspensão são controlados pelo host UWP.";
        }
    }
    if (!value.is_primitive() || value.is_null()) {
        return "Coleção sem editor compatível; o valor persistido é preservado.";
    }
    return {};
}

json Choices(std::string_view key) {
    json result = json::array();
    const auto add = [&](json value, const char* label) {
        result.push_back({{"value", value}, {"label", label}});
    };
    if (key == "readbacks_mode") {
        add(0, "Desativado");
        add(1, "Relaxado");
        add(2, "Preciso");
    } else if (key == "adaptive_skipcaches_mode") {
        add(0, "Desativado");
        add(1, "Adaptativo");
        add(2, "Forçado");
        add(3, "Somente validação");
    } else if (key == "audio_backend") {
        add(0, "SDL3");
        add(1, "OpenAL");
        add(2, "XAudio2");
    } else if (key == "openal_hrtf") {
        add(0, "Automático");
        add(1, "Ativado");
        add(2, "Desativado");
    } else if (key == "openal_output_mode") {
        add(0, "Automático");
        add(1, "Estéreo");
        add(2, "Quad");
        add(3, "5.1");
        add(4, "7.1");
    } else if (key == "console_language") {
        constexpr std::array languages{"Japonês",
                                       "Inglês (EUA)",
                                       "Francês",
                                       "Espanhol",
                                       "Alemão",
                                       "Italiano",
                                       "Holandês",
                                       "Português (Portugal)",
                                       "Russo",
                                       "Coreano",
                                       "Chinês tradicional",
                                       "Chinês simplificado",
                                       "Finlandês",
                                       "Sueco",
                                       "Dinamarquês",
                                       "Norueguês",
                                       "Polonês",
                                       "Português (Brasil)",
                                       "Inglês (Reino Unido)",
                                       "Turco",
                                       "Espanhol (América Latina)",
                                       "Árabe",
                                       "Francês (Canadá)",
                                       "Tcheco",
                                       "Húngaro",
                                       "Grego",
                                       "Romeno",
                                       "Tailandês",
                                       "Vietnamita",
                                       "Indonésio",
                                       "Ucraniano"};
        for (size_t index = 0; index < languages.size(); ++index)
            add(index, languages[index]);
    } else if (key == "flush_level") {
        add("", "Padrão");
        add("trace", "Trace");
        add("debug", "Debug");
        add("info", "Info");
        add("warning", "Warning");
        add("error", "Error");
        add("critical", "Critical");
        add("off", "Off");
    } else if (key == "special_pad_class") {
        constexpr std::array labels{"Padrão",          "Guitarra",  "Bateria", "Mesa DJ",
                                    "Tapete de dança", "Navegação", "Volante", "Stick",
                                    "Fight stick",     "Pistola"};
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
        throw std::runtime_error("Identificador de perfil inválido.");
    return Common::FS::GetUserPath(Common::FS::PathType::CustomConfigs) /
           (std::string{serial} + ".json");
}

void WriteSnapshot(const json& document, char* data, uint32_t capacity, uint32_t* required) {
    if (!required || (!data && capacity))
        throw std::runtime_error("Buffer inválido.");
    const auto text = document.dump();
    *required = static_cast<uint32_t>(text.size() + 1);
    if (!data || !capacity)
        return;
    if (capacity < *required)
        throw std::runtime_error("Buffer insuficiente.");
    std::memcpy(data, text.c_str(), *required);
}

template <typename F>
int32_t Call(F operation) {
    try {
        std::scoped_lock lock{settings_mutex};
        if (shadps4_get_state() != SHADPS4_READY) {
            throw std::runtime_error("As configurações exigem um núcleo inicializado e parado.");
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
        throw std::runtime_error("Tipo de configuração inválido.");
    }
    if (value.is_number()) {
        const auto number = value.get<double>();
        const auto [minimum, maximum] = Bounds(key, old);
        if (!std::isfinite(number) || number < minimum || number > maximum) {
            throw std::runtime_error("Valor fora da faixa permitida.");
        }
    }
    if (value.is_string() && value.get_ref<const std::string&>().size() > 4096) {
        throw std::runtime_error("Texto de configuração muito longo.");
    }
    const auto choices = Choices(key);
    if (!choices.empty() && std::ranges::none_of(choices, [&](const auto& option) {
            return option.at("value") == value;
        })) {
        throw std::runtime_error("Opção de configuração desconhecida.");
    }
}
} // namespace

int32_t shadps4_get_settings(char* data, uint32_t capacity, uint32_t* required) {
    return Call([&] {
        if (!required || (!data && capacity))
            throw std::runtime_error("Buffer inválido.");
        const auto text = Snapshot().dump();
        *required = static_cast<uint32_t>(text.size() + 1);
        if (!data || !capacity)
            return;
        if (capacity < *required)
            throw std::runtime_error("Buffer insuficiente.");
        std::memcpy(data, text.c_str(), *required);
    });
}

int32_t shadps4_set_setting(const char* group, const char* key, const char* value_json) {
    return Call([&] {
        if (!group || !key || !value_json)
            throw std::runtime_error("Configuração inválida.");
        const auto previous = EmulatorSettings.GetGroupValues(group);
        if (!previous.is_object() || !previous.contains(key))
            throw std::runtime_error("Configuração desconhecida.");
        const auto value = json::parse(value_json);
        Validate(group, key, previous.at(key), value);
        if (!EmulatorSettings.SetGroupValues(group, {{key, value}}, false))
            throw std::runtime_error("Falha ao aplicar configuração.");
        if (!EmulatorSettings.Save()) {
            EmulatorSettings.SetGroupValues(group, previous, false);
            throw std::runtime_error(
                "Falha ao salvar configuração; alteração em memória revertida.");
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
            throw std::runtime_error("Falha ao restaurar configurações.");
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
            throw std::runtime_error("Falha ao ler perfil.");
        WriteSnapshot(Snapshot(profile, true), data, capacity, required);
    });
}
int32_t shadps4_set_game_setting(const char* serial, const char* group, const char* key,
                                 const char* value_json) {
    return Call([&] {
        const auto path = GamePath(serial);
        if (!group || !key || !value_json || !Overrideable(group, key))
            throw std::runtime_error("Opção não permite override por jogo.");
        EmulatorSettingsImpl profile;
        profile = EmulatorSettings;
        profile.ClearGameSpecificOverrides();
        if (std::filesystem::exists(path) && !profile.Load(serial))
            throw std::runtime_error("Falha ao ler perfil.");
        const auto previous = profile.GetGroupValues(group);
        if (!previous.contains(key))
            throw std::runtime_error("Configuração desconhecida.");
        const auto value = json::parse(value_json);
        Validate(group, key, previous.at(key), value);
        if (!profile.SetGroupValues(group, {{key, value}}, true) || !profile.Save(serial))
            throw std::runtime_error("Falha ao salvar perfil.");
    });
}
int32_t shadps4_delete_game_settings(const char* serial) {
    return Call([&] { std::filesystem::remove(GamePath(serial)); });
}
