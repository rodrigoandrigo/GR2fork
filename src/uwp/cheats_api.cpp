#include "uwp/shadps4_api.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <miniz.h>
#include <nlohmann/json.hpp>
#include <pugixml.hpp>
#include <windows.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.h>
#include "common/memory_patcher.h"
#include "common/path_util.h"
#include "common/scope_exit.h"
#include "common/singleton.h"
#include "core/file_format/psf.h"
#include "core/memory.h"

namespace {
using nlohmann::json;
namespace fs = std::filesystem;
std::mutex cheats_mutex;
std::map<std::string, bool> enabled_cheats;

std::string Component(const std::string& name) {
    if (name.empty() || name.size() > 200 || name == "." || name == ".." ||
        name.find_first_of("/\\:") != std::string::npos)
        throw std::runtime_error("Nome de arquivo inválido.");
    return name;
}
std::string Version(std::string value) {
    const auto first = value.find_first_not_of('0');
    return first == std::string::npos ? "0" : value.substr(first);
}
bool Compatible(std::string version, const std::string& game) {
    return version == "mask" || Version(version) == Version(game);
}
std::string Read(const fs::path& path) {
    if (fs::file_size(path) > 4 * 1024 * 1024)
        throw std::runtime_error("Arquivo de cheats/patches muito grande.");
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        throw std::runtime_error("Não foi possível ler o arquivo.");
    return {std::istreambuf_iterator<char>(stream), {}};
}
void Write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    auto temporary = path;
    temporary += L".tmp";
    SCOPE_EXIT {
        std::error_code ec;
        fs::remove(temporary, ec);
    };
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream.write(text.data(), text.size());
        stream.close();
        if (!stream)
            throw std::runtime_error("Falha ao salvar cheats/patches.");
    }
    if (!(fs::exists(path)
              ? ReplaceFileFromAppW(path.c_str(), temporary.c_str(), nullptr, 0, nullptr, nullptr)
              : MoveFileFromAppW(temporary.c_str(), path.c_str())))
        throw std::runtime_error("Falha ao substituir o arquivo de cheats/patches.");
}
template <typename T>
auto Wait(T operation) {
    if (operation.wait_for(std::chrono::seconds(30)) ==
        winrt::Windows::Foundation::AsyncStatus::Started) {
        operation.Cancel();
        throw std::runtime_error("Tempo limite do download excedido.");
    }
    return operation.GetResults();
}
std::string Download(const std::string& url, uint32_t limit = 4 * 1024 * 1024) {
    using namespace winrt::Windows::Web::Http;
    HttpClient client;
    client.DefaultRequestHeaders().UserAgent().ParseAdd(L"shadPS4-UWP");
    auto response = Wait(client.GetAsync(winrt::Windows::Foundation::Uri(winrt::to_hstring(url)),
                                         HttpCompletionOption::ResponseHeadersRead));
    response.EnsureSuccessStatusCode();
    if (const auto length = response.Content().Headers().ContentLength();
        length && length.Value() > limit)
        throw std::runtime_error("Download muito grande.");
    auto stream = Wait(response.Content().ReadAsInputStreamAsync());
    std::string result;
    for (;;) {
        auto buffer =
            Wait(stream.ReadAsync(winrt::Windows::Storage::Streams::Buffer(65536), 65536,
                                  winrt::Windows::Storage::Streams::InputStreamOptions::Partial));
        if (!buffer.Length())
            break;
        if (buffer.Length() > limit - result.size())
            throw std::runtime_error("Download muito grande.");
        result.append(reinterpret_cast<const char*>(buffer.data()), buffer.Length());
    }
    return result;
}
fs::path Root(bool patch) {
    return Common::FS::GetUserPath(patch ? Common::FS::PathType::PatchesDir
                                         : Common::FS::PathType::CheatsDir);
}
fs::path File(const json& request, bool patch) {
    if (!patch)
        return Root(false) / Component(request.at("file").get<std::string>());
    const auto repo = request.at("repository").get<std::string>();
    if (repo != "shadPS4" && repo != "GoldHEN")
        throw std::runtime_error("Repositório desconhecido.");
    return Root(true) / repo / Component(request.at("file").get<std::string>());
}
json Ids(pugi::xml_document& doc) {
    json ids = json::array();
    for (auto id : doc.child("Patch").child("TitleID").children("ID"))
        ids.push_back(id.text().as_string());
    return ids;
}
std::string Xml(pugi::xml_document& doc) {
    std::ostringstream stream;
    doc.save(stream);
    return stream.str();
}
bool LoadXml(pugi::xml_document& doc, const std::string& text) {
    return doc.load_buffer(text.data(), text.size()) && doc.child("Patch");
}
void Reindex(const fs::path& directory) {
    json index = json::object();
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".xml")
            continue;
        pugi::xml_document doc;
        if (LoadXml(doc, Read(entry.path())))
            index[entry.path().filename().string()] = Ids(doc);
    }
    Write(directory / "files.json", index.dump(2));
}
json Snapshot(const std::string& serial, const std::string& version) {
    json result{{"cheats", json::array()}, {"patches", json::array()}};
    for (const auto& entry : fs::directory_iterator(Root(false))) {
        const auto filename = entry.path().filename().string();
        if (!entry.is_regular_file() || entry.path().extension() != ".json" ||
            !filename.starts_with(serial + "_"))
            continue;
        try {
            const auto data = json::parse(Read(entry.path()));
            json mods = json::array();
            size_t index{};
            for (const auto& mod : data.at("mods")) {
                const auto key = serial + "/" + filename + "/" + std::to_string(index);
                mods.push_back({{"index", index++},
                                {"name", mod.at("name")},
                                {"type", mod.value("type", "checkbox")},
                                {"hint", mod.value("hint", json{})},
                                {"enabled", enabled_cheats[key]}});
            }
            result["cheats"].push_back(
                {{"file", filename},
                 {"version", data.value("version", "")},
                 {"compatible", Compatible(data.value("version", ""), version)},
                 {"credits", data.value("credits", json::array())},
                 {"mods", mods}});
        } catch (const std::exception& error) {
            result["cheats"].push_back({{"file", filename}, {"error", error.what()}});
        }
    }
    for (const auto repo : {"shadPS4", "GoldHEN"}) {
        const auto directory = Root(true) / repo;
        if (!fs::exists(directory))
            continue;
        for (const auto& entry : fs::directory_iterator(directory)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".xml")
                continue;
            pugi::xml_document doc;
            if (!LoadXml(doc, Read(entry.path())))
                continue;
            const auto ids = Ids(doc);
            if (std::find(ids.begin(), ids.end(), serial) == ids.end())
                continue;
            size_t index{};
            for (auto meta : doc.child("Patch").children("Metadata")) {
                result["patches"].push_back(
                    {{"repository", repo},
                     {"file", entry.path().filename().string()},
                     {"index", index++},
                     {"name", meta.attribute("Name").as_string()},
                     {"author", meta.attribute("Author").as_string()},
                     {"note", meta.attribute("Note").as_string()},
                     {"version", meta.attribute("AppVer").as_string()},
                     {"compatible", Compatible(meta.attribute("AppVer").as_string(), version)},
                     {"enabled", meta.attribute("isEnabled").as_bool()}});
            }
        }
    }
    return result;
}
void DownloadCheats(const std::string& repo, const std::string& serial) {
    const bool gold = repo == "GoldHEN";
    const std::string base =
        gold ? "https://raw.githubusercontent.com/GoldHEN/GoldHEN_Cheat_Repository/main/"
             : "https://raw.githubusercontent.com/shadps4-emu/ps4_cheats/main/";
    std::istringstream list(Download(base + (gold ? "json.txt" : "CHEATS_JSON.txt")));
    std::string line;
    while (std::getline(list, line)) {
        const auto filename = line.substr(0, line.find('='));
        if (!filename.starts_with(serial + "_") || !filename.ends_with(".json"))
            continue;
        Component(filename);
        const auto data = Download(base + (gold ? "json/" : "CHEATS/") + filename);
        const auto document = json::parse(data);
        if (!document.contains("mods") || !document["mods"].is_array())
            throw std::runtime_error("Formato de cheats inválido.");
        Write(Root(false) / filename, data);
    }
}
void DownloadPatches(const std::string& repo, const std::string& serial) {
    const bool gold = repo == "GoldHEN";
    const auto archive =
        Download(gold ? "https://codeload.github.com/illusionyy/PS-Game-Patch/zip/refs/heads/main"
                      : "https://codeload.github.com/shadps4-emu/ps4_cheats/zip/refs/heads/main",
                 64 * 1024 * 1024);
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, archive.data(), archive.size(), 0))
        throw std::runtime_error("Arquivo ZIP de patches inválido.");
    SCOPE_EXIT {
        mz_zip_reader_end(&zip);
    };
    const auto directory = Root(true) / repo;
    fs::create_directories(directory);
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat))
            throw std::runtime_error("Falha ao ler catálogo de patches.");
        const std::string name = stat.m_filename;
        const auto slash = name.find('/');
        if (slash == std::string::npos)
            continue;
        const auto relative = name.substr(slash + 1);
        const std::string prefix = gold ? "patches/xml/" : "PATCHES/";
        if (!relative.starts_with(prefix) || !relative.ends_with(".xml"))
            continue;
        const auto filename = Component(relative.substr(prefix.size()));
        if (stat.m_uncomp_size > 4 * 1024 * 1024)
            throw std::runtime_error("Patch muito grande.");
        std::string data(stat.m_uncomp_size, '\0');
        if (!mz_zip_reader_extract_to_mem(&zip, i, data.data(), data.size(), 0))
            throw std::runtime_error("Falha ao extrair patch.");
        pugi::xml_document doc;
        if (!LoadXml(doc, data))
            throw std::runtime_error("XML de patch inválido: " + filename);
        const auto ids = Ids(doc);
        if (std::find(ids.begin(), ids.end(), serial) == ids.end())
            continue;
        const auto path = directory / filename;
        pugi::xml_document previous;
        if (fs::exists(path))
            LoadXml(previous, Read(path));
        for (auto meta : doc.child("Patch").children("Metadata")) {
            bool enabled{};
            for (auto old : previous.child("Patch").children("Metadata"))
                if (std::string_view{meta.attribute("Name").value()} ==
                        old.attribute("Name").value() &&
                    std::string_view{meta.attribute("AppVer").value()} ==
                        old.attribute("AppVer").value() &&
                    std::string_view{meta.attribute("AppElf").value()} ==
                        old.attribute("AppElf").value())
                    enabled = old.attribute("isEnabled").as_bool();
            auto attr = meta.attribute("isEnabled");
            if (!attr)
                attr = meta.append_attribute("isEnabled");
            attr.set_value(enabled);
        }
        Write(path, Xml(doc));
    }
    Reindex(directory);
}
void ApplyCheat(const json& request, const std::string& serial, const std::string& version) {
    if (shadps4_get_state() != SHADPS4_RUNNING)
        throw std::runtime_error("Inicie este jogo antes de aplicar cheats.");
    auto* memory_manager = Common::Singleton<Core::MemoryManager>::Instance();
    Core::MemoryManager::GuestCopyScope memory_scope(memory_manager);
    if (shadps4_get_state() != SHADPS4_RUNNING || MemoryPatcher::g_game_serial != serial ||
        !MemoryPatcher::g_eboot_address || !MemoryPatcher::g_eboot_image_size)
        throw std::runtime_error("Inicie este jogo antes de aplicar cheats.");
    const auto actual_version =
        Common::Singleton<PSF>::Instance()->GetString("APP_VER").value_or("");
    if (!Compatible(version, std::string(actual_version)))
        throw std::runtime_error("Versão do jogo em execução diferente da selecionada.");
    const auto doc = json::parse(Read(File(request, false)));
    if (!Compatible(doc.value("version", ""), version) ||
        doc.value("process", "eboot.bin") != MemoryPatcher::g_eboot_name)
        throw std::runtime_error("Cheat incompatível com a versão ou executável.");
    const auto& mod = doc.at("mods").at(request.at("index").get<size_t>());
    const bool enabled = request.at("enabled").get<bool>();
    std::vector<std::pair<uintptr_t, std::vector<uint8_t>>> writes;
    for (const auto& memory : mod.at("memory")) {
        const auto offset_text = memory.at("offset").get<std::string>();
        const auto offset = std::stoull(offset_text, nullptr, 16);
        const auto base = MemoryPatcher::g_eboot_address;
        if (offset > INT32_MAX || (mod.contains("hint") && offset < 0x400000))
            throw std::runtime_error("Endereço de cheat inválido.");
        const auto address = base + offset - (mod.contains("hint") ? 0x400000 : 0);
        const auto value = memory.at(enabled ? "on" : "off").get<std::string>();
        if (value.empty() || value.size() % 2 || value.size() > 65536 ||
            !std::ranges::all_of(value, [](unsigned char c) { return std::isxdigit(c); }))
            throw std::runtime_error("Bytes de cheat inválidos.");
        std::vector<uint8_t> bytes;
        for (size_t i = 0; i < value.size(); i += 2)
            bytes.push_back(static_cast<uint8_t>(std::stoul(value.substr(i, 2), nullptr, 16)));
        if (!memory_manager->IsValidMapping(address, bytes.size()))
            throw std::runtime_error("Cheat aponta para memória guest não mapeada.");
        writes.emplace_back(address, std::move(bytes));
    }
    for (const auto& [address, bytes] : writes) {
        if (!memory_manager->TryWriteBacking(reinterpret_cast<void*>(address), bytes.data(),
                                             bytes.size()))
            throw std::runtime_error("Não foi possível escrever na memória guest.");
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), bytes.size());
    }
    const auto key = serial + "/" + request.at("file").get<std::string>() + "/" +
                     std::to_string(request.at("index").get<size_t>());
    enabled_cheats[key] = enabled;
}
json Request(const json& request) {
    const auto serial = Component(request.at("serial").get<std::string>());
    const auto version = request.at("version").get<std::string>();
    const auto operation = request.value("operation", "list");
    fs::create_directories(Root(false));
    fs::create_directories(Root(true));
    const auto repo = request.value("repository", "shadPS4");
    if (repo != "shadPS4" && repo != "GoldHEN")
        throw std::runtime_error("Repositório desconhecido.");
    if (operation == "download_cheats")
        DownloadCheats(repo, serial);
    else if (operation == "download_patches")
        DownloadPatches(repo, serial);
    else if (operation == "apply")
        ApplyCheat(request, serial, version);
    else if (operation == "patch") {
        const auto path = File(request, true);
        pugi::xml_document doc;
        if (!LoadXml(doc, Read(path)))
            throw std::runtime_error("XML de patch inválido.");
        const auto ids = Ids(doc);
        if (std::find(ids.begin(), ids.end(), serial) == ids.end())
            throw std::runtime_error("Patch pertence a outro jogo.");
        size_t index{};
        bool found{};
        for (auto meta : doc.child("Patch").children("Metadata")) {
            if (index++ != request.at("index").get<size_t>())
                continue;
            const bool enabled = request.at("enabled").get<bool>();
            if (enabled && !Compatible(meta.attribute("AppVer").as_string(), version))
                throw std::runtime_error("Patch incompatível com a versão selecionada.");
            auto attr = meta.attribute("isEnabled");
            if (!attr)
                attr = meta.append_attribute("isEnabled");
            attr.set_value(enabled);
            found = true;
        }
        if (!found)
            throw std::runtime_error("Patch desconhecido.");
        Write(path, Xml(doc));
        Reindex(path.parent_path());
    } else if (operation == "delete_cheats" || operation == "delete_patches") {
        const auto path = File(request, operation == "delete_patches");
        fs::remove(path);
        if (operation == "delete_patches")
            Reindex(path.parent_path());
    } else if (operation != "list")
        throw std::runtime_error("Operação desconhecida.");
    if (shadps4_get_state() != SHADPS4_RUNNING)
        enabled_cheats.clear();
    return Snapshot(serial, version);
}
} // namespace

int32_t shadps4_cheats_request(const char* request, char* response, uint32_t capacity) {
    if (!request || !response || capacity < 1024 || capacity > 8 * 1024 * 1024)
        return -1;
    std::scoped_lock lock{cheats_mutex};
    json result;
    int32_t status{};
    try {
        if (shadps4_get_state() != SHADPS4_READY && shadps4_get_state() != SHADPS4_RUNNING)
            throw std::runtime_error("Aguarde a inicialização do núcleo.");
        result = Request(json::parse(request));
    } catch (const winrt::hresult_error& error) {
        result = {{"error", winrt::to_string(error.message())}};
        status = -1;
    } catch (const std::exception& error) {
        result = {{"error", error.what()}};
        status = -1;
    }
    auto text = result.dump();
    if (text.size() >= capacity) {
        text = R"({"error":"Resposta de cheats/patches muito grande."})";
        status = -1;
    }
    std::memcpy(response, text.c_str(), text.size() + 1);
    return status;
}
