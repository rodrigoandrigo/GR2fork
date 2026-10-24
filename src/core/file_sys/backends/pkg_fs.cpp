#include "core/file_sys/backends/pkg_fs.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace Core::FileSys {
namespace {
std::string Normalize(std::string_view path) {
    std::string result;
    size_t start{};
    while (start < path.size()) {
        const auto end = path.find('/', start);
        const auto part =
            path.substr(start, end == std::string_view::npos ? path.size() - start : end - start);
        if (part == ".." || part.find_first_of("\\:\0", 0, 3) != std::string_view::npos)
            throw std::invalid_argument("Caminho PKG inválido.");
        if (!part.empty() && part != ".") {
            if (!result.empty())
                result += '/';
            for (const unsigned char c : part)
                result += static_cast<char>(c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c);
        }
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }
    return result;
}

class PkgFile final : public IFile {
  public:
    PkgFile(std::shared_ptr<PkgReader> source, const PkgReader::Node* entry, std::vector<u8> bytes,
            u32 inode)
        : reader(std::move(source)), node(entry), data(std::move(bytes)), inode(inode) {}
    s64 Read(void* destination, u64 length) override {
        std::scoped_lock lock{mutex};
        const auto got = ReadAt(destination, position, length);
        if (got > 0)
            position += got;
        return got;
    }
    s64 Write(const void*, u64) override { return -1; }
    bool SetSize(u64) override { return false; }
    bool Seek(s64 offset, Common::FS::SeekOrigin origin) override {
        std::scoped_lock lock{mutex};
        const u64 base = origin == Common::FS::SeekOrigin::SetOrigin ? 0
                         : origin == Common::FS::SeekOrigin::End     ? Size()
                                                                     : position;
        if (base > INT64_MAX || (offset > 0 && base > static_cast<u64>(INT64_MAX - offset)) ||
            (offset < 0 && static_cast<u64>(-(offset + 1)) + 1 > base))
            return false;
        position = static_cast<u64>(static_cast<s64>(base) + offset);
        return true;
    }
    u64 Tell() const override {
        std::scoped_lock lock{mutex};
        return position;
    }
    u64 Size() const override { return node ? node->size : data.size(); }
    bool Flush() override { return true; }
    bool IsOpen() const override { return reader != nullptr; }
    bool IsReadOnly() const override { return true; }
    void Stat(FileStat& out) override {
        out = {.size = Size(), .inode = inode};
    }
    MmapPolicy GetMmapPolicy() const override { return MmapPolicy::Copy; }
    bool Map(u8* address, u64 length, u64 offset, u32 protection,
             const FileMapContext& context) override {
        if (offset > INT64_MAX || length > INT64_MAX)
            return false;
        context.map_anonymous(address, length);
        const auto got = ReadAt(address, offset, length);
        if (got < 0)
            return false;
        std::memset(address + got, 0, length - got);
        context.protect(address, length, protection);
        return true;
    }

  private:
    s64 ReadAt(void* destination, u64 offset, u64 length) {
        if (offset >= Size())
            return 0;
        length = std::min({length, Size() - offset, static_cast<u64>(INT64_MAX)});
        if (node)
            return reader->Read(*node, destination, offset, length);
        std::memcpy(destination, data.data() + offset, length);
        return static_cast<s64>(length);
    }
    std::shared_ptr<PkgReader> reader;
    const PkgReader::Node* node{};
    std::vector<u8> data;
    u64 position{};
    u32 inode{};
    mutable std::mutex mutex;
};

class PkgDirectory final : public IDirectory {
  public:
    explicit PkgDirectory(std::vector<DirEntry> items) : entries(std::move(items)) {}
    bool Next(DirEntry& output) override {
        if (position == entries.size())
            return false;
        output = entries[position++];
        return true;
    }
    void Rewind() override { position = 0; }

  private:
    std::vector<DirEntry> entries;
    size_t position{};
};
} // namespace

PkgBackend::PkgBackend(std::unique_ptr<IFile> source) {
    FileStat stat{};
    source->Stat(stat);
    static std::atomic<u64> next_source{};
    source_inode = stat.inode;
    if (!source_inode) {
        source_inode = GetGuestInode(FileIdentity{
            .kind = FileIdentity::Kind::VirtualFile,
            .path = "pkg-source:" + std::to_string(++next_source)}).value_or(0);
    }
    reader = std::make_shared<PkgReader>(std::move(source));
    std::vector<std::string> paths;
    reader->Paths(paths);
    for (const auto& path : paths) {
        const auto node = reader->Find(path.c_str());
        if (node->directory)
            directories.try_emplace(Normalize(path));
        else
            files.emplace(Normalize(path), node->size);
    }
    for (const auto& [id, path] :
         {std::pair{0x1000U, "sce_sys/param.sfo"}, std::pair{0x1200U, "sce_sys/icon0.png"}}) {
        if (files.contains(path))
            continue;
        std::vector<u8> bytes;
        if (reader->ReadPkgEntry(id, bytes) >= 0) {
            files.emplace(path, bytes.size());
            metadata.emplace(path, std::move(bytes));
        }
    }
    directories.try_emplace("");
    for (const auto& [path, size] : files) {
        size_t start{};
        std::string parent;
        for (;;) {
            const auto end = path.find('/', start);
            const auto name =
                path.substr(start, end == std::string::npos ? path.size() - start : end - start);
            auto& entries = directories[parent];
            if (std::ranges::none_of(entries,
                                     [&](const auto& entry) { return entry.name == name; }))
                entries.push_back(
                    {name, end != std::string::npos, end == std::string::npos ? size : 0});
            if (end == std::string::npos)
                break;
            parent = path.substr(0, end);
            directories.try_emplace(parent);
            start = end + 1;
        }
    }
    for (const auto& [path, children] : directories) {
        if (path.empty())
            continue;
        const auto slash = path.find_last_of('/');
        const auto parent = slash == std::string::npos ? std::string{} : path.substr(0, slash);
        const auto name = slash == std::string::npos ? path : path.substr(slash + 1);
        auto& entries = directories[parent];
        if (std::ranges::none_of(entries, [&](const auto& entry) { return entry.name == name; }))
            entries.push_back({name, true, 0});
    }
}
bool PkgBackend::Exists(std::string_view relative) {
    const auto path = Normalize(relative);
    return files.contains(path) || directories.contains(path);
}
bool PkgBackend::IsDirectory(std::string_view relative) {
    return directories.contains(Normalize(relative));
}
std::unique_ptr<IFile> PkgBackend::Open(std::string_view relative,
                                        Common::FS::FileAccessMode mode) {
    if (mode != Common::FS::FileAccessMode::Read)
        return nullptr;
    const auto path = Normalize(relative);
    if (!files.contains(path))
        return nullptr;
    const auto found = metadata.find(path);
    return std::make_unique<PkgFile>(reader, reader->Find(path.c_str()),
                                     found == metadata.end() ? std::vector<u8>{} : found->second,
                                     Stat(path).inode);
}
std::unique_ptr<IDirectory> PkgBackend::OpenDir(std::string_view relative) {
    const auto found = directories.find(Normalize(relative));
    return found == directories.end() ? nullptr : std::make_unique<PkgDirectory>(found->second);
}
bool PkgBackend::IsReadOnly() const { return true; }
std::filesystem::path PkgBackend::RootPath() const { return {}; }
std::optional<std::vector<u8>> PkgBackend::ReadFile(std::string_view relative) const {
    auto file = const_cast<PkgBackend*>(this)->Open(relative, Common::FS::FileAccessMode::Read);
    if (!file || file->Size() > 256 * 1024 * 1024)
        return std::nullopt;
    std::vector<u8> bytes(file->Size());
    if (file->Read(bytes.data(), bytes.size()) != static_cast<s64>(bytes.size()))
        return std::nullopt;
    return bytes;
}
FileStat PkgBackend::Stat(std::string_view relative) const {
    const auto path = Normalize(relative);
    FileStat result{};
    if (directories.contains(path))
        result.is_directory = true;
    else if (const auto found = files.find(path); found != files.end())
        result.size = found->second;
    else
        throw std::runtime_error("Arquivo não encontrado no PKG.");
    result.inode = GetGuestInode(FileIdentity{
        .kind = FileIdentity::Kind::VirtualFile,
        .host = {.device = source_inode},
        .path = path}).value_or(0);
    return result;
}
bool PkgBackend::Stat(std::string_view relative, FileStat& out) {
    if (!Exists(relative))
        return false;
    out = Stat(relative);
    return out.inode != 0;
}
bool PkgBackend::CanBoot() const { return reader->Valid() && files.contains("eboot.bin"); }
} // namespace Core::FileSys
