#pragma once

#include "core/file_sys/ifile.h"

namespace Core::FileSys {
struct PkgImpl;

class PkgReader {
  public:
    struct Node {
        u64 size{};
        u32 start_block{};
        bool directory{};
    };
    explicit PkgReader(std::unique_ptr<IFile> source);
    ~PkgReader();
    bool Valid() const;
    const Node* Find(const char* relative) const;
    s64 Read(const Node& node, void* data, s64 offset, s64 length);
    void Paths(std::vector<std::string>& paths) const;
    s64 ReadPkgEntry(u32 id, std::vector<u8>& data);

  private:
    std::unique_ptr<PkgImpl> impl_;
};
} // namespace Core::FileSys
