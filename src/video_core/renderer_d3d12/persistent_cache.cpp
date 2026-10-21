#include "video_core/renderer_d3d12/persistent_cache.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <fstream>
#include <mutex>
#include <dxgi1_4.h>
#include <fmt/format.h>
#include <wrl/client.h>
#include <xxhash.h>
#ifdef SHADPS4_UWP
#include <winrt/Windows.System.Profile.h>
#endif
#include "common/path_util.h"
#include "video_core/renderer_d3d12/profile.h"

namespace D3D12 {
namespace {
constexpr u64 Magic = 0x3145484341433344ULL;
constexpr u64 EntryLimit = 64 * 1024 * 1024;
constexpr u64 DiskLimit = 256 * 1024 * 1024;
constexpr size_t EntryCount = 2048;
std::mutex disk_mutex;
Profile disk_profile;
std::atomic<u64> temporary_id{};
void Append(std::vector<u8>& data, const void* source, size_t size) {
    if (!size)
        return;
    const auto* bytes = static_cast<const u8*>(source);
    data.insert(data.end(), bytes, bytes + size);
}
std::string Digest(std::span<const u8> bytes) {
    const auto hash = XXH3_128bits(bytes.data(), bytes.size());
    return fmt::format("{:016x}{:016x}", hash.high64, hash.low64);
}
std::vector<u8> CompilerIdentity() {
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&CompilerIdentity), &module))
        return {};
    std::array<wchar_t, 32768> name{};
    const auto length = GetModuleFileNameW(module, name.data(), name.size());
    if (!length || length >= name.size())
        return {};
    const std::filesystem::path core{std::wstring_view{name.data(), length}};
    std::vector<u8> result;
    for (const auto& path :
         {core, core.parent_path() / L"dxcompiler.dll", core.parent_path() / L"dxil.dll",
          core.parent_path() / L"spirv_to_dxil.dll"}) {
        std::ifstream file{path, std::ios::binary};
        if (!file)
            return {};
        auto* state = XXH3_createState();
        if (!state)
            return {};
        XXH3_128bits_reset(state);
        std::array<char, 65536> block{};
        while (file) {
            file.read(block.data(), block.size());
            XXH3_128bits_update(state, block.data(), size_t(file.gcount()));
        }
        const bool valid = file.eof();
        const auto digest = XXH3_128bits_digest(state);
        XXH3_freeState(state);
        if (!valid)
            return {};
        Append(result, &digest, sizeof(digest));
    }
    return result;
}
bool MakeSpace(const std::filesystem::path& directory, u64 incoming) {
    struct Entry {
        std::filesystem::path path;
        std::filesystem::file_time_type time;
        u64 size;
    };
    std::vector<Entry> entries;
    u64 bytes{};
    for (const auto& file : std::filesystem::directory_iterator(directory)) {
        if (!file.is_regular_file() || file.path().extension() != ".bin")
            continue;
        entries.push_back({file.path(), file.last_write_time(), file.file_size()});
        bytes += entries.back().size;
    }
    std::ranges::sort(entries, {}, &Entry::time);
    size_t count = entries.size();
    for (const auto& entry : entries) {
        if (count < EntryCount && bytes + incoming <= DiskLimit)
            return true;
        if (!std::filesystem::remove(entry.path))
            continue;
        bytes -= entry.size;
        --count;
    }
    return count < EntryCount && bytes + incoming <= DiskLimit;
}
} // namespace
void PersistentCache::Initialize(std::string_view category, ID3D12Device* device) {
    directory.clear();
    identity.clear();
    statistics = {};
    try {
#ifdef SHADPS4_UWP
        if (device) {
            try {
                if (winrt::Windows::System::Profile::AnalyticsInfo::VersionInfo().DeviceFamily() ==
                    L"Windows.Xbox")
                    return;
            } catch (const winrt::hresult_error&) {
                return;
            }
        }
#endif
        static const auto compiler = CompilerIdentity();
        if (compiler.empty())
            return;
        identity = compiler;
        if (device) {
            Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
            Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
            LARGE_INTEGER version{};
            DXGI_ADAPTER_DESC description{};
            const auto luid = device->GetAdapterLuid();
            if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
                FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))) ||
                FAILED(adapter->GetDesc(&description)) ||
                FAILED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &version)))
                return;
            Append(identity, &luid, sizeof(luid));
            Append(identity, &version.QuadPart, sizeof(version.QuadPart));
            Append(identity, &description.VendorId, sizeof(description.VendorId));
            Append(identity, &description.DeviceId, sizeof(description.DeviceId));
            Append(identity, &description.SubSysId, sizeof(description.SubSysId));
            Append(identity, &description.Revision, sizeof(description.Revision));
        }
        const auto root = Common::FS::GetUserPath(Common::FS::PathType::CacheDir);
        if (root.empty())
            return;
        directory = root / "d3d12-native" / "v1" / category;
        std::filesystem::create_directories(directory);
    } catch (const std::exception&) {
        directory.clear();
    }
}
std::vector<u8> PersistentCache::Key(std::span<const u8> key) const {
    auto result = identity;
    Append(result, key.data(), key.size());
    return result;
}
std::filesystem::path PersistentCache::Path(std::span<const u8> key) const {
    return directory / (Digest(key) + ".bin");
}
std::vector<u8> PersistentCache::Load(std::span<const u8> key) {
    ProfileScope timing{disk_profile, "disk_load", 32};
    ++statistics.misses;
    if (directory.empty() || key.size() > EntryLimit)
        return {};
    auto lock = ProfileSample(disk_profile, "disk_lock", 128,
                              [&] { return std::unique_lock{disk_mutex}; });
    try {
        const auto full_key = Key(key);
        const auto path = Path(full_key);
        std::ifstream file{path, std::ios::binary | std::ios::ate};
        if (!file)
            return {};
        const auto size = file.tellg();
        if (size < 40 || size > EntryLimit)
            throw std::runtime_error("Invalid persistent cache size");
        file.seekg(0);
        std::array<u64, 5> header{};
        file.read(reinterpret_cast<char*>(header.data()), sizeof(header));
        if (!file || header[0] != Magic || header[1] != full_key.size() || header[1] > EntryLimit ||
            header[2] > EntryLimit || header[2] == 0 ||
            header[1] + header[2] != u64(size) - sizeof(header))
            throw std::runtime_error("Invalid persistent cache header");
        std::vector<u8> bytes(size_t(size) - sizeof(header));
        file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
        const auto hash = XXH3_128bits(bytes.data(), bytes.size());
        if (!file || hash.low64 != header[3] || hash.high64 != header[4] ||
            !std::equal(full_key.begin(), full_key.end(), bytes.begin()))
            throw std::runtime_error("Invalid persistent cache contents");
        ++statistics.hits;
        --statistics.misses;
        return {bytes.begin() + full_key.size(), bytes.end()};
    } catch (const std::exception&) {
        ++statistics.rejected;
        std::error_code error;
        std::filesystem::remove(Path(Key(key)), error);
        return {};
    }
}
void PersistentCache::Store(std::span<const u8> key, std::span<const u8> data) {
    ProfileScope timing{disk_profile, "disk_store", 32, data.size()};
    if (directory.empty() || data.empty() || key.size() > EntryLimit || data.size() > EntryLimit)
        return;
    auto lock = ProfileSample(disk_profile, "disk_lock", 128,
                              [&] { return std::unique_lock{disk_mutex}; });
    std::filesystem::path temporary;
    try {
        auto bytes = Key(key);
        const auto path = Path(bytes);
        const u64 key_size = bytes.size();
        if (key_size + data.size() + 40 > EntryLimit)
            return;
        Append(bytes, data.data(), data.size());
        const auto hash = XXH3_128bits(bytes.data(), bytes.size());
        const std::array<u64, 5> header{Magic, key_size, data.size(), hash.low64, hash.high64};
        if (!MakeSpace(directory, sizeof(header) + bytes.size()))
            return;
        temporary = path;
        temporary += fmt::format(".tmp-{}-{}", GetCurrentProcessId(), ++temporary_id);
        {
            std::ofstream file{temporary, std::ios::binary | std::ios::trunc};
            file.write(reinterpret_cast<const char*>(header.data()), sizeof(header));
            file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            file.flush();
            if (!file)
                throw std::runtime_error("Persistent cache write failed");
        }
        std::filesystem::rename(temporary, path);
        ++statistics.writes;
    } catch (const std::exception&) {
        std::error_code error;
        if (!temporary.empty())
            std::filesystem::remove(temporary, error);
    }
}
void PersistentCache::Remove(std::span<const u8> key) {
    if (directory.empty())
        return;
    auto lock = ProfileSample(disk_profile, "disk_lock", 128,
                              [&] { return std::unique_lock{disk_mutex}; });
    std::error_code error;
    std::filesystem::remove(Path(Key(key)), error);
    ++statistics.rejected;
}
} // namespace D3D12
