#pragma once

#include <mutex>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Storage.h>
#include "core/file_sys/ifile.h"

namespace Core::Uwp {

class BrokeredFile final : public FileSys::IFile {
public:
    BrokeredFile(winrt::Windows::Storage::Streams::IRandomAccessStream stream, uint32_t mode);
    BrokeredFile(void* handle, uint32_t mode);
    ~BrokeredFile() override;
    s64 Read(void* dst, u64 size) override;
    s64 Write(const void* src, u64 size) override;
    bool Seek(s64 offset, Common::FS::SeekOrigin origin) override;
    u64 Tell() const override;
    u64 Size() const override;
    bool Flush() override;
    bool IsOpen() const override;
    bool IsReadOnly() const override;
    FileSys::MmapPolicy GetMmapPolicy() const override;
    bool Map(u8* addr, u64 size, u64 offset, u32 prot, const FileSys::FileMapContext& ctx) override;
    void Stat(FileSys::FileStat& out) override;
    bool SetSize(u64 size) override;
    void SetInode(u32 value) {
        inode = value;
    }

private:
    s64 Transfer(void* data, u64 size, bool write);
    winrt::Windows::Storage::Streams::IRandomAccessStream stream{nullptr};
    void* handle{};
    uint32_t mode{};
    u64 position{};
    u32 inode{};
    mutable std::recursive_mutex mutex;
};

class BrokeredFsBackend final : public FileSys::IBackend {
public:
    BrokeredFsBackend(void* folder, bool read_only);
    bool Exists(std::string_view relative) override;
    bool IsDirectory(std::string_view relative) override;
    std::unique_ptr<FileSys::IFile> Open(std::string_view relative,
                                         Common::FS::FileAccessMode mode) override;
    std::unique_ptr<FileSys::IDirectory> OpenDir(std::string_view relative) override;
    bool IsReadOnly() const override;
    std::filesystem::path RootPath() const override;
    std::optional<std::vector<u8>> ReadFile(std::string_view relative) const override;
    FileSys::FileStat Stat(std::string_view relative) const;
    bool Stat(std::string_view relative, FileSys::FileStat& out) override;
    void CreateSubdirectory(std::string_view relative);
    void Delete(std::string_view relative);
    void Rename(std::string_view from, std::string_view to);
    std::shared_ptr<BrokeredFsBackend> Subdirectory(std::string_view relative) const;
    std::shared_ptr<BrokeredFsBackend> GameDirectory(std::string_view relative) const;

private:
    winrt::Windows::Storage::StorageFolder Folder(std::string_view relative) const;
    winrt::Windows::Storage::IStorageItem Item(std::string_view relative) const;
    winrt::Windows::Storage::StorageFolder root{nullptr};
    std::shared_ptr<BrokeredFsBackend> metadata;
    bool read_only;
};

Common::FS::FileAccessMode StorageMode(uint32_t mode);
void ValidateStorageRelativePath(std::string_view path);
std::unique_ptr<BrokeredFile> OpenStorageFile(void* file, uint32_t mode);
} // namespace Core::Uwp
