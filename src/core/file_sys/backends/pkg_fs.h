#pragma once

#include <map>
#include "core/file_sys/backends/pkg_reader.h"

namespace Core::FileSys {
class PkgBackend final : public IBackend {
  public:
    explicit PkgBackend(std::unique_ptr<IFile> source);
    bool Exists(std::string_view relative) override;
    bool IsDirectory(std::string_view relative) override;
    std::unique_ptr<IFile> Open(std::string_view relative,
                                Common::FS::FileAccessMode mode) override;
    std::unique_ptr<IDirectory> OpenDir(std::string_view relative) override;
    bool IsReadOnly() const override;
    std::filesystem::path RootPath() const override;
    std::optional<std::vector<u8>> ReadFile(std::string_view relative) const override;
    FileStat Stat(std::string_view relative) const;
    bool CanBoot() const;

  private:
    std::shared_ptr<PkgReader> reader;
    std::map<std::string, u64> files;
    std::map<std::string, std::vector<u8>> metadata;
    std::map<std::string, std::vector<DirEntry>> directories;
};
} // namespace Core::FileSys
