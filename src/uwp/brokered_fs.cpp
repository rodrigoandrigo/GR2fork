#include "uwp/brokered_fs.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <windows.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.FileProperties.h>
#include "uwp/shadps4_api.h"

namespace Core::Uwp {
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::Storage::Streams;

namespace {
std::string RelativePath(std::string_view path) {
    if (path.find_first_of("\\:\0", 0, 3) != std::string_view::npos || path.starts_with('/')) {
        winrt::throw_hresult(E_INVALIDARG);
    }
    std::string result;
    size_t start{};
    while (start < path.size()) {
        const auto end = path.find('/', start);
        const auto part =
            path.substr(start, end == std::string_view::npos ? path.size() - start : end - start);
        if (part.empty() || part == "." || part == ".." || part.ends_with('.') ||
            part.ends_with(' ') || part.find_first_of("<>\"|?*") != std::string_view::npos) {
            winrt::throw_hresult(E_INVALIDARG);
        }
        if (!result.empty()) {
            result += '/';
        }
        result += part;
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
        if (start == path.size()) {
            winrt::throw_hresult(E_INVALIDARG);
        }
    }
    return result;
}

std::pair<std::string, std::string> ParentAndName(std::string_view path) {
    const auto validated = RelativePath(path);
    if (validated.empty()) {
        winrt::throw_hresult(E_INVALIDARG);
    }
    const auto slash = validated.rfind('/');
    return slash == std::string::npos
               ? std::pair{std::string{}, validated}
               : std::pair{validated.substr(0, slash), validated.substr(slash + 1)};
}

bool Missing(const winrt::hresult_error& error) {
    return error.code() == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
           error.code() == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
}

class BrokeredDirectory final : public FileSys::IDirectory {
public:
    explicit BrokeredDirectory(StorageFolder folder) {
        for (const auto& item : folder.GetItemsAsync().get()) {
            const bool directory = item.IsOfType(StorageItemTypes::Folder);
            const auto size = directory ? 0 : item.GetBasicPropertiesAsync().get().Size();
            entries.push_back({winrt::to_string(item.Name()), directory, size});
        }
    }
    bool Next(FileSys::DirEntry& out) override {
        if (index == entries.size()) {
            return false;
        }
        out = entries[index++];
        return true;
    }
    void Rewind() override {
        index = 0;
    }

private:
    std::vector<FileSys::DirEntry> entries;
    size_t index{};
};
} // namespace

Common::FS::FileAccessMode StorageMode(uint32_t mode) {
    switch (mode) {
    case SHADPS4_FILE_READ:
        return Common::FS::FileAccessMode::Read;
    case SHADPS4_FILE_READ_WRITE:
        return Common::FS::FileAccessMode::ReadWrite;
    case SHADPS4_FILE_CREATE:
        return Common::FS::FileAccessMode::Create;
    case SHADPS4_FILE_APPEND:
        return Common::FS::FileAccessMode::ReadAppend;
    default:
        winrt::throw_hresult(E_INVALIDARG);
    }
}

void ValidateStorageRelativePath(std::string_view path) {
    RelativePath(path);
}

BrokeredFile::BrokeredFile(IRandomAccessStream source, uint32_t mode) : mode{mode} {
    StorageMode(mode);
    stream = source.CloneStream();
    if (!stream.CanRead() || (mode != SHADPS4_FILE_READ && !stream.CanWrite())) {
        winrt::throw_hresult(E_ACCESSDENIED);
    }
    if (mode == SHADPS4_FILE_CREATE) {
        stream.Size(0);
    }
    if (mode == SHADPS4_FILE_APPEND) {
        position = Size();
    }
}

BrokeredFile::BrokeredFile(void* source, uint32_t mode) : mode{mode} {
    StorageMode(mode);
    if (!source || source == INVALID_HANDLE_VALUE || GetFileType(source) != FILE_TYPE_DISK) {
        winrt::throw_hresult(E_INVALIDARG);
    }
    HANDLE duplicate{};
    winrt::check_bool(DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &duplicate,
                                      0, FALSE, DUPLICATE_SAME_ACCESS));
    handle = duplicate;
    try {
        if (mode == SHADPS4_FILE_CREATE) {
            SetSize(0);
        }
        if (mode == SHADPS4_FILE_APPEND) {
            position = Size();
        }
    } catch (...) {
        CloseHandle(handle);
        handle = nullptr;
        throw;
    }
}

BrokeredFile::~BrokeredFile() {
    if (handle) {
        CloseHandle(handle);
    }
    if (stream) {
        try {
            stream.Close();
        } catch (...) {
        }
    }
}

s64 BrokeredFile::Transfer(void* data, u64 size, bool write) {
    std::scoped_lock lock{mutex};
    if ((!data && size) || size > static_cast<u64>(std::numeric_limits<s64>::max()) ||
        position > static_cast<u64>(std::numeric_limits<s64>::max()) - size) {
        winrt::throw_hresult(E_INVALIDARG);
    }
    if (write && IsReadOnly()) {
        winrt::throw_hresult(E_ACCESSDENIED);
    }
    if (write && mode == SHADPS4_FILE_APPEND) {
        position = Size();
        if (position > static_cast<u64>(INT64_MAX) - size) {
            winrt::throw_hresult(E_INVALIDARG);
        }
    }
    u64 total{};
    while (total < size) {
        const auto count = static_cast<uint32_t>(std::min<u64>(size - total, 1024 * 1024));
        auto* bytes = static_cast<uint8_t*>(data) + total;
        DWORD transferred{};
        if (stream) {
            if (write) {
                DataWriter writer;
                writer.WriteBytes(winrt::array_view<const uint8_t>{bytes, bytes + count});
                transferred =
                    stream.GetOutputStreamAt(position).WriteAsync(writer.DetachBuffer()).get();
            } else {
                const auto buffer = stream.GetInputStreamAt(position)
                                        .ReadAsync(Buffer{count}, count, InputStreamOptions::None)
                                        .get();
                transferred = buffer.Length();
                DataReader::FromBuffer(buffer).ReadBytes(
                    winrt::array_view<uint8_t>{bytes, bytes + transferred});
            }
        } else {
            OVERLAPPED overlapped{};
            overlapped.Offset = static_cast<DWORD>(position);
            overlapped.OffsetHigh = static_cast<DWORD>(position >> 32);
            winrt::handle event{
                CreateEventExW(nullptr, nullptr, CREATE_EVENT_MANUAL_RESET, EVENT_ALL_ACCESS)};
            winrt::check_bool(static_cast<bool>(event));
            overlapped.hEvent = event.get();
            const bool succeeded = write
                                       ? WriteFile(handle, bytes, count, &transferred, &overlapped)
                                       : ReadFile(handle, bytes, count, &transferred, &overlapped);
            if (!succeeded) {
                const auto error = GetLastError();
                if (!write && error == ERROR_HANDLE_EOF) {
                    break;
                }
                if (error != ERROR_IO_PENDING) {
                    winrt::throw_hresult(HRESULT_FROM_WIN32(error));
                }
                if (!GetOverlappedResult(handle, &overlapped, &transferred, TRUE)) {
                    const auto completion_error = GetLastError();
                    if (!write && completion_error == ERROR_HANDLE_EOF) {
                        break;
                    }
                    winrt::throw_hresult(HRESULT_FROM_WIN32(completion_error));
                }
            }
        }
        total += transferred;
        position += transferred;
        if (transferred < count) {
            break;
        }
    }
    return static_cast<s64>(total);
}

s64 BrokeredFile::Read(void* data, u64 size) {
    return Transfer(data, size, false);
}
s64 BrokeredFile::Write(const void* data, u64 size) {
    return Transfer(const_cast<void*>(data), size, true);
}
bool BrokeredFile::Seek(s64 offset, Common::FS::SeekOrigin origin) {
    std::scoped_lock lock{mutex};
    u64 base{};
    switch (origin) {
    case Common::FS::SeekOrigin::SetOrigin:
        break;
    case Common::FS::SeekOrigin::CurrentPosition:
        base = position;
        break;
    case Common::FS::SeekOrigin::End:
        base = Size();
        break;
    default:
        winrt::throw_hresult(E_INVALIDARG);
    }
    const u64 magnitude = offset < 0 ? static_cast<u64>(-(offset + 1)) + 1 : offset;
    if ((offset < 0 && magnitude > base) ||
        (offset >= 0 && (base > INT64_MAX || magnitude > INT64_MAX - base))) {
        winrt::throw_hresult(E_INVALIDARG);
    }
    position = offset < 0 ? base - magnitude : base + magnitude;
    return true;
}
u64 BrokeredFile::Tell() const {
    std::scoped_lock lock{mutex};
    return position;
}
u64 BrokeredFile::Size() const {
    std::scoped_lock lock{mutex};
    if (stream) {
        return stream.Size();
    }
    LARGE_INTEGER size{};
    winrt::check_bool(GetFileSizeEx(handle, &size));
    return size.QuadPart;
}
void BrokeredFile::SetSize(u64 size) {
    std::scoped_lock lock{mutex};
    if (IsReadOnly()) {
        winrt::throw_hresult(E_ACCESSDENIED);
    }
    if (size > INT64_MAX) {
        winrt::throw_hresult(E_INVALIDARG);
    }
    if (stream) {
        stream.Size(size);
    } else {
        FILE_END_OF_FILE_INFO info{};
        info.EndOfFile.QuadPart = size;
        winrt::check_bool(
            SetFileInformationByHandle(handle, FileEndOfFileInfo, &info, sizeof(info)));
    }
}
bool BrokeredFile::Flush() {
    std::scoped_lock lock{mutex};
    if (IsReadOnly()) {
        return true;
    }
    if (stream) {
        winrt::check_bool(stream.GetOutputStreamAt(position).FlushAsync().get());
    } else {
        winrt::check_bool(FlushFileBuffers(handle));
    }
    return true;
}
bool BrokeredFile::IsOpen() const {
    return stream || handle;
}
bool BrokeredFile::IsReadOnly() const {
    return mode == SHADPS4_FILE_READ;
}
FileSys::MmapPolicy BrokeredFile::GetMmapPolicy() const {
    return IsReadOnly() ? FileSys::MmapPolicy::Copy : FileSys::MmapPolicy::Unsupported;
}
bool BrokeredFile::Map(u8* addr, u64 size, u64 offset, u32 prot,
                       const FileSys::FileMapContext& ctx) {
    if (!IsReadOnly() || offset > INT64_MAX) {
        return false;
    }
    std::scoped_lock lock{mutex};
    const auto saved = position;
    try {
        ctx.map_anonymous(addr, size);
        position = offset;
        const auto transferred = Read(addr, size);
        std::memset(addr + transferred, 0, size - transferred);
        position = saved;
        ctx.protect(addr, size, prot);
        return true;
    } catch (...) {
        position = saved;
        throw;
    }
}
void BrokeredFile::Stat(FileSys::FileStat& out) {
    out = {};
    out.size = Size();
    if (handle) {
        FILE_BASIC_INFO info{};
        winrt::check_bool(GetFileInformationByHandleEx(handle, FileBasicInfo, &info, sizeof(info)));
        out.mtime_sec = info.LastWriteTime.QuadPart / 10000000 - 11644473600LL;
        out.atime_sec = info.LastAccessTime.QuadPart / 10000000 - 11644473600LL;
        out.ctime_sec = info.CreationTime.QuadPart / 10000000 - 11644473600LL;
    }
}

std::unique_ptr<BrokeredFile> OpenStorageFile(void* object, uint32_t mode) {
    if (!object) {
        winrt::throw_hresult(E_INVALIDARG);
    }
    StorageMode(mode);
    winrt::Windows::Foundation::IInspectable source{nullptr};
    winrt::copy_from_abi(source, object);
    const auto file = source.as<StorageFile>();
    const auto stream =
        file.OpenAsync(mode == SHADPS4_FILE_READ ? FileAccessMode::Read : FileAccessMode::ReadWrite,
                       StorageOpenOptions::AllowReadersAndWriters)
            .get();
    return std::make_unique<BrokeredFile>(stream, mode);
}

BrokeredFsBackend::BrokeredFsBackend(void* folder, bool read_only) : read_only{read_only} {
    if (!folder) {
        winrt::throw_hresult(E_INVALIDARG);
    }
    winrt::Windows::Foundation::IInspectable object{nullptr};
    winrt::copy_from_abi(object, folder);
    root = object.as<StorageFolder>();
}
StorageFolder BrokeredFsBackend::Folder(std::string_view relative) const {
    const auto path = RelativePath(relative);
    auto folder = root;
    size_t start{};
    while (start < path.size()) {
        const auto slash = path.find('/', start);
        const auto part =
            path.substr(start, slash == std::string::npos ? path.size() - start : slash - start);
        folder = folder.GetFolderAsync(winrt::to_hstring(part)).get();
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    return folder;
}

std::shared_ptr<BrokeredFsBackend> BrokeredFsBackend::Subdirectory(
    std::string_view relative) const {
    const auto folder = Folder(relative);
    return std::make_shared<BrokeredFsBackend>(winrt::get_abi(folder), read_only);
}
IStorageItem BrokeredFsBackend::Item(std::string_view relative) const {
    if (relative.empty()) {
        return root;
    }
    const auto [parent, name] = ParentAndName(relative);
    return Folder(parent).GetItemAsync(winrt::to_hstring(name)).get();
}
bool BrokeredFsBackend::Exists(std::string_view relative) {
    try {
        return static_cast<bool>(Item(relative));
    } catch (const winrt::hresult_error& error) {
        if (Missing(error)) {
            return false;
        }
        throw;
    }
}
bool BrokeredFsBackend::IsDirectory(std::string_view relative) {
    try {
        return Item(relative).IsOfType(StorageItemTypes::Folder);
    } catch (const winrt::hresult_error& error) {
        if (Missing(error)) {
            return false;
        }
        throw;
    }
}
std::unique_ptr<FileSys::IFile> BrokeredFsBackend::Open(std::string_view relative,
                                                        Common::FS::FileAccessMode mode) {
    const bool writable = mode != Common::FS::FileAccessMode::Read;
    if (writable && read_only) {
        winrt::throw_hresult(E_ACCESSDENIED);
    }
    const auto [parent, name] = ParentAndName(relative);
    const auto folder = Folder(parent);
    StorageFile file{nullptr};
    uint32_t public_mode = writable ? SHADPS4_FILE_READ_WRITE : SHADPS4_FILE_READ;
    if (mode == Common::FS::FileAccessMode::Create) {
        file =
            folder
                .CreateFileAsync(winrt::to_hstring(name), CreationCollisionOption::ReplaceExisting)
                .get();
        public_mode = SHADPS4_FILE_CREATE;
    } else if (mode == Common::FS::FileAccessMode::Append ||
               mode == Common::FS::FileAccessMode::ReadAppend) {
        file =
            folder.CreateFileAsync(winrt::to_hstring(name), CreationCollisionOption::OpenIfExists)
                .get();
        public_mode = SHADPS4_FILE_APPEND;
    } else {
        file = folder.GetFileAsync(winrt::to_hstring(name)).get();
    }
    return OpenStorageFile(winrt::get_abi(file), public_mode);
}
std::unique_ptr<FileSys::IDirectory> BrokeredFsBackend::OpenDir(std::string_view relative) {
    return std::make_unique<BrokeredDirectory>(Folder(relative));
}
bool BrokeredFsBackend::IsReadOnly() const {
    return read_only;
}
std::filesystem::path BrokeredFsBackend::RootPath() const {
    return {};
}
std::optional<std::vector<u8>> BrokeredFsBackend::ReadFile(std::string_view relative) const {
    const auto item = Item(relative).as<StorageFile>();
    auto file = OpenStorageFile(winrt::get_abi(item), SHADPS4_FILE_READ);
    const auto length = file->Size();
    if (length > std::vector<u8>{}.max_size()) {
        winrt::throw_hresult(E_OUTOFMEMORY);
    }
    std::vector<u8> result(length);
    if (file->Read(result.data(), length) != static_cast<s64>(length)) {
        winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_HANDLE_EOF));
    }
    return result;
}
FileSys::FileStat BrokeredFsBackend::Stat(std::string_view relative) const {
    const auto item = Item(relative);
    const auto properties = item.GetBasicPropertiesAsync().get();
    FileSys::FileStat result{};
    result.is_directory = item.IsOfType(StorageItemTypes::Folder);
    result.size = result.is_directory ? 0 : properties.Size();
    result.mtime_sec =
        properties.DateModified().time_since_epoch().count() / 10000000 - 11644473600LL;
    return result;
}
void BrokeredFsBackend::CreateSubdirectory(std::string_view relative) {
    if (read_only) {
        winrt::throw_hresult(E_ACCESSDENIED);
    }
    const auto [parent, name] = ParentAndName(relative);
    Folder(parent)
        .CreateFolderAsync(winrt::to_hstring(name), CreationCollisionOption::FailIfExists)
        .get();
}
void BrokeredFsBackend::Delete(std::string_view relative) {
    if (read_only || relative.empty()) {
        winrt::throw_hresult(E_ACCESSDENIED);
    }
    Item(relative).DeleteAsync(StorageDeleteOption::PermanentDelete).get();
}
void BrokeredFsBackend::Rename(std::string_view from, std::string_view to) {
    if (read_only || from.empty() || to.empty()) {
        winrt::throw_hresult(E_ACCESSDENIED);
    }
    const auto [source_parent, source_name] = ParentAndName(from);
    const auto [target_parent, target_name] = ParentAndName(to);
    const auto item = Item(from);
    if (source_parent == target_parent) {
        item.RenameAsync(winrt::to_hstring(target_name), NameCollisionOption::FailIfExists).get();
    } else if (item.IsOfType(StorageItemTypes::File)) {
        item.as<StorageFile>()
            .MoveAsync(Folder(target_parent), winrt::to_hstring(target_name),
                       NameCollisionOption::FailIfExists)
            .get();
    } else {
        winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
    }
}
} // namespace Core::Uwp
