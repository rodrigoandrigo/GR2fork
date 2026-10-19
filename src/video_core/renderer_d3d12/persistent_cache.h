#pragma once

#include <filesystem>
#include <span>
#include <string_view>
#include <vector>
#include <d3d12.h>
#include "common/types.h"

namespace D3D12 {
class PersistentCache {
public:
    struct Statistics {
        u64 hits{}, misses{}, writes{}, rejected{};
    };
    void Initialize(std::string_view category, ID3D12Device* device = nullptr);
    std::vector<u8> Load(std::span<const u8> key);
    void Store(std::span<const u8> key, std::span<const u8> data);
    void Remove(std::span<const u8> key);
    bool Enabled() const {
        return !directory.empty();
    }
    const Statistics& GetStatistics() const {
        return statistics;
    }

private:
    std::filesystem::path directory;
    std::vector<u8> identity;
    Statistics statistics{};
    std::vector<u8> Key(std::span<const u8> key) const;
    std::filesystem::path Path(std::span<const u8> key) const;
};
} // namespace D3D12
