#include "uwp/shadps4_api.h"

#include <mutex>
#include <unordered_map>
#include <objbase.h>
#include <windows.h>
#include <winrt/Windows.Storage.h>
#include "common/singleton.h"
#include "core/file_sys/fs.h"
#include "uwp/brokered_fs.h"
#include "uwp/session.h"

namespace {
using Core::Uwp::BrokeredFile;
using Core::Uwp::BrokeredFsBackend;
std::mutex storage_mutex;
std::unordered_map<std::string, std::shared_ptr<BrokeredFsBackend>> folders;
std::unordered_map<uint64_t, std::unique_ptr<Core::FileSys::IFile>> files;
uint64_t next_token{1};

void Require(bool valid, HRESULT result = E_INVALIDARG) {
    if (!valid) {
        winrt::throw_hresult(result);
    }
}

template <typename F>
int32_t StorageCall(F operation) {
    try {
        APTTYPE type;
        APTTYPEQUALIFIER qualifier;
        winrt::check_hresult(CoGetApartmentType(&type, &qualifier));
        Require(type == APTTYPE_MTA || type == APTTYPE_NA, RPC_E_WRONG_THREAD);
        std::scoped_lock lock{storage_mutex};
        operation();
        return 0;
    } catch (const winrt::hresult_error& error) {
        Core::Uwp::ReportError("Brokered storage HRESULT " +
                               std::to_string(static_cast<uint32_t>(error.code().value)) + ": " +
                               winrt::to_string(error.message()));
        return error.code().value;
    } catch (const std::exception& error) {
        Core::Uwp::ReportError(error.what());
        return E_FAIL;
    } catch (...) {
        Core::Uwp::ReportError("Unknown brokered storage error");
        return E_FAIL;
    }
}

Core::FileSys::MntPoints* Mounts() {
    return Common::Singleton<Core::FileSys::MntPoints>::Instance();
}

void RequireIdle() {
    Require(shadps4_get_state() != SHADPS4_RUNNING, HRESULT_FROM_WIN32(ERROR_BUSY));
}

std::string MountName(const char* path) {
    Require(path && path[0] == '/' && path[1]);
    const std::string result{path};
    Require(result.find_first_of("/\\:", 1) == std::string::npos && result != "/." &&
            result != "/..");
    return result;
}

std::pair<std::shared_ptr<BrokeredFsBackend>, std::string> Resolve(const char* path) {
    Require(path && path[0] == '/');
    const std::string full{path};
    const auto slash = full.find('/', 1);
    const auto mount = full.substr(0, slash);
    const auto found = folders.find(mount);
    Require(found != folders.end(), HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND));
    const auto relative = slash == std::string::npos ? std::string{} : full.substr(slash + 1);
    Core::Uwp::ValidateStorageRelativePath(relative);
    return {found->second, relative};
}

Core::FileSys::IFile& File(uint64_t token) {
    const auto found = files.find(token);
    Require(found != files.end(), HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE));
    return *found->second;
}

void Register(std::unique_ptr<Core::FileSys::IFile> file, uint64_t* token) {
    Require(file && file->IsOpen(), HRESULT_FROM_WIN32(ERROR_OPEN_FAILED));
    Require(next_token != 0, HRESULT_FROM_WIN32(ERROR_TOO_MANY_OPEN_FILES));
    const auto id = next_token++;
    files.emplace(id, std::move(file));
    *token = id;
}

ShadPS4FileInfo Info(const Core::FileSys::FileStat& stat) {
    return {sizeof(ShadPS4FileInfo), stat.is_directory ? 1U : 0U, stat.size, stat.mtime_sec};
}
} // namespace

namespace Core::Uwp {
std::string PrepareBootStorage(std::string executable) {
    std::scoped_lock lock{storage_mutex};
    auto [backend, relative] = Resolve(executable.c_str());
    const auto slash = relative.find_last_of('/');
    const auto parent = slash == std::string::npos ? std::string{} : relative.substr(0, slash);
    const auto filename = slash == std::string::npos ? relative : relative.substr(slash + 1);
    Require(!filename.empty());
    const auto info = backend->Stat(relative);
    Require(!info.is_directory, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
    auto game = backend->GameDirectory(parent);
    Mounts()->MountBackend(game, "/app0");
    Mounts()->MountBackend(game, "/hostapp");
    LOG_INFO(Loader, "Boot executable mounted through brokered storage: /app0/{}", filename);
    folders.insert_or_assign("/app0", game);
    folders.insert_or_assign("/hostapp", game);
    return "/app0/" + filename;
}

void ClearStorage() {
    std::scoped_lock lock{storage_mutex};
    files.clear();
    Mounts()->Unmount("/app0");
    Mounts()->Unmount("/hostapp");
    for (const auto& [mount, backend] : folders) {
        Mounts()->Unmount(mount);
    }
    folders.clear();
}
} // namespace Core::Uwp

int32_t shadps4_mount_storage_folder(const char* mount, void* folder, int32_t read_only) {
    return StorageCall([&] {
        RequireIdle();
        const auto name = MountName(mount);
        auto backend = std::make_shared<BrokeredFsBackend>(folder, read_only != 0);
        backend = backend->GameDirectory({});
        backend->Stat({});
        Mounts()->MountBackend(backend, name);
        folders.insert_or_assign(name, std::move(backend));
    });
}
int32_t shadps4_unmount_storage_folder(const char* mount) {
    return StorageCall([&] {
        RequireIdle();
        const auto name = MountName(mount);
        Require(folders.contains(name), HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND));
        Mounts()->Unmount(name);
        folders.erase(name);
    });
}
int32_t shadps4_open_storage_file(void* file, uint32_t mode, uint64_t* token) {
    if (token) {
        *token = 0;
    }
    return StorageCall([&] {
        Require(token != nullptr);
        Register(Core::Uwp::OpenStorageFile(file, mode), token);
    });
}
int32_t shadps4_open_random_access_stream(void* stream, uint32_t mode, uint64_t* token) {
    if (token) {
        *token = 0;
    }
    return StorageCall([&] {
        Require(stream && token);
        winrt::Windows::Foundation::IInspectable object{nullptr};
        winrt::copy_from_abi(object, stream);
        Register(std::make_unique<BrokeredFile>(
                     object.as<winrt::Windows::Storage::Streams::IRandomAccessStream>(), mode),
                 token);
    });
}
int32_t shadps4_open_file_handle(void* handle, uint32_t mode, uint64_t* token) {
    if (token) {
        *token = 0;
    }
    return StorageCall([&] {
        Require(token != nullptr);
        Register(std::make_unique<BrokeredFile>(handle, mode), token);
    });
}
int32_t shadps4_open_mounted_file(const char* path, uint32_t mode, uint64_t* token) {
    if (token) {
        *token = 0;
    }
    return StorageCall([&] {
        Require(token != nullptr);
        const auto [folder, relative] = Resolve(path);
        const auto access_mode = Core::Uwp::StorageMode(mode);
        Require(mode == SHADPS4_FILE_READ || !folder->IsReadOnly(), E_ACCESSDENIED);
        Require(mode == SHADPS4_FILE_CREATE || mode == SHADPS4_FILE_APPEND ||
                    folder->Exists(relative),
                HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
        if (mode == SHADPS4_FILE_APPEND && !folder->Exists(relative)) {
            Register(folder->Open(relative, access_mode), token);
        } else {
            Register(Mounts()->Open(path, access_mode), token);
        }
    });
}
int32_t shadps4_file_read(uint64_t token, void* data, uint64_t size, uint64_t* transferred) {
    if (transferred) {
        *transferred = 0;
    }
    return StorageCall([&] {
        Require(transferred != nullptr);
        *transferred = File(token).Read(data, size);
    });
}
int32_t shadps4_file_write(uint64_t token, const void* data, uint64_t size, uint64_t* transferred) {
    if (transferred) {
        *transferred = 0;
    }
    return StorageCall([&] {
        Require(transferred != nullptr);
        *transferred = File(token).Write(data, size);
    });
}
int32_t shadps4_file_seek(uint64_t token, int64_t offset, uint32_t origin, uint64_t* position) {
    return StorageCall([&] {
        Require(position && origin <= 2);
        auto& file = File(token);
        Require(file.Seek(offset, static_cast<Common::FS::SeekOrigin>(origin)), E_FAIL);
        *position = file.Tell();
    });
}
int32_t shadps4_file_get_size(uint64_t token, uint64_t* size) {
    return StorageCall([&] {
        Require(size != nullptr);
        *size = File(token).Size();
    });
}
int32_t shadps4_file_set_size(uint64_t token, uint64_t size) {
    return StorageCall([&] { static_cast<BrokeredFile&>(File(token)).SetSize(size); });
}
int32_t shadps4_file_flush(uint64_t token) {
    return StorageCall([&] { Require(File(token).Flush(), E_FAIL); });
}
int32_t shadps4_file_close(uint64_t token) {
    return StorageCall(
        [&] { Require(files.erase(token) != 0, HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE)); });
}
int32_t shadps4_storage_stat(const char* path, ShadPS4FileInfo* info) {
    return StorageCall([&] {
        Require(info && info->size == sizeof(ShadPS4FileInfo));
        const auto [folder, relative] = Resolve(path);
        *info = Info(folder->Stat(relative));
    });
}
int32_t shadps4_storage_enumerate(const char* path, ShadPS4DirectoryCallback callback,
                                  void* context) {
    std::vector<Core::FileSys::DirEntry> entries;
    const auto result = StorageCall([&] {
        Require(callback != nullptr);
        const auto [folder, relative] = Resolve(path);
        auto directory = Mounts()->OpenDir(path);
        Require(directory != nullptr, HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND));
        Core::FileSys::DirEntry entry;
        while (directory->Next(entry)) {
            entries.push_back(entry);
        }
    });
    if (result) {
        return result;
    }
    try {
        for (const auto& entry : entries) {
            const ShadPS4FileInfo info{sizeof(ShadPS4FileInfo), entry.is_directory ? 1U : 0U,
                                       entry.size, 0};
            callback(context, entry.name.c_str(), &info);
        }
        return 0;
    } catch (...) {
        Core::Uwp::ReportError("Directory callback threw an exception");
        return E_FAIL;
    }
}
int32_t shadps4_storage_create_directory(const char* path) {
    return StorageCall([&] {
        const auto [folder, relative] = Resolve(path);
        folder->CreateSubdirectory(relative);
    });
}
int32_t shadps4_storage_delete(const char* path) {
    return StorageCall([&] {
        const auto [folder, relative] = Resolve(path);
        folder->Delete(relative);
    });
}
int32_t shadps4_storage_rename(const char* from, const char* to) {
    return StorageCall([&] {
        const auto [source, source_relative] = Resolve(from);
        const auto [target, target_relative] = Resolve(to);
        Require(source == target, HRESULT_FROM_WIN32(ERROR_NOT_SAME_DEVICE));
        source->Rename(source_relative, target_relative);
    });
}
