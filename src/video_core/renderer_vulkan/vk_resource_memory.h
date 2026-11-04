// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>
#include <string_view>
#include "common/logging/log.h"
#include "common/types.h"

namespace Vulkan::ResourceMemory {
struct Entry {
    uintptr_t key{};
    u64 resource{}, bytes{}, sequence{};
    u32 kind{}, memory_type{}, format{}, width{}, height{}, depth{}, mips{}, layers{}, samples{};
    std::array<char, 64> name{};
};
struct Totals {
    u64 live{}, bytes{}, peak_bytes{}, created{}, destroyed{};
};
inline std::mutex mutex;
inline std::array<Entry, 8192> entries;
inline std::array<Totals, 2> totals;
inline u64 sequence{}, dropped{}, missing{};
inline std::chrono::steady_clock::time_point reported;

inline u64 AllocatedBytes() {
    std::lock_guard lock{mutex};
    return totals[0].bytes + totals[1].bytes;
}

inline Entry* Find(uintptr_t key, bool insert) {
    Entry* reusable{};
    const auto hash = ((key >> 4) ^ (key >> 20)) % entries.size();
    for (size_t i = 0; i < entries.size(); ++i) {
        auto& entry = entries[(hash + i) % entries.size()];
        if (entry.key == key)
            return &entry;
        if (entry.key == 1 && !reusable)
            reusable = &entry;
        if (!entry.key)
            return insert ? (reusable ? reusable : &entry) : nullptr;
    }
    return insert ? reusable : nullptr;
}
inline void Add(uintptr_t allocation, u64 resource, u64 bytes, u32 kind, u32 memory_type,
                u32 format = 0, u32 width = 0, u32 height = 0, u32 depth = 0, u32 mips = 0,
                u32 layers = 0, u32 samples = 0) {
    std::lock_guard lock{mutex};
    auto& total = totals[kind];
    ++total.created;
    ++total.live;
    total.bytes += bytes;
    total.peak_bytes = std::max(total.peak_bytes, total.bytes);
    if (auto* entry = Find(allocation, true)) {
        *entry = Entry{allocation, resource, bytes, ++sequence, kind,   memory_type, format,
                       width,      height,   depth, mips,       layers, samples};
    } else {
        ++dropped;
    }
}
inline void Remove(uintptr_t allocation, u64 bytes, u32 kind) {
    std::lock_guard lock{mutex};
    auto& total = totals[kind];
    ++total.destroyed;
    total.live -= std::min<u64>(1, total.live);
    total.bytes -= std::min(bytes, total.bytes);
    if (auto* entry = Find(allocation, false))
        entry->key = 1;
    else
        ++missing;
}
inline void Name(uintptr_t allocation, std::string_view name) {
    std::lock_guard lock{mutex};
    if (auto* entry = Find(allocation, false)) {
        entry->name.fill(0);
        std::copy_n(name.begin(), std::min(name.size(), entry->name.size() - 1),
                    entry->name.begin());
    }
}
inline bool Report(const char* operation, bool force) {
    std::array<Entry, 16> largest{};
    std::array<Totals, 2> snapshot{};
    std::array<Totals, 9> categories{};
    u64 lost{}, unmatched{};
    {
        std::unique_lock lock{mutex, std::try_to_lock};
        const auto now = std::chrono::steady_clock::now();
        if (!lock.owns_lock() || (!force && now - reported < std::chrono::seconds(5)))
            return false;
        reported = now;
        snapshot = totals;
        lost = dropped;
        unmatched = missing;
        for (const auto& entry : entries) {
            if (entry.key <= 1)
                continue;
            const std::string_view name{entry.name.data()};
            const u32 category = !entry.kind && name == "Dense guest buffer arena"           ? 7
                                 : !entry.kind && name == "Dense guest buffer arena:retired" ? 8
                                 : entry.kind                                                ? 0
                                 : name == "StagingPool:Ring"                                ? 1
                                 : name == "StagingBufferPool:Dedicated"                     ? 2
                                 : name == "StagingPool:RetiredRing"                         ? 3
                                 : name == "StagingPool:RetiredLarge"                        ? 4
                                 : name == "StreamBuffer"                                    ? 5
                                                                                             : 6;
            ++categories[category].live;
            categories[category].bytes += entry.bytes;
            if (entry.bytes <= largest.back().bytes)
                continue;
            largest.back() = entry;
            std::sort(largest.begin(), largest.end(),
                      [](const Entry& a, const Entry& b) { return a.bytes > b.bytes; });
        }
    }
    for (u32 i = 0; i < snapshot.size(); ++i) {
        const auto& total = snapshot[i];
        LOG_WARNING(Render_Vulkan,
                    "[ResourceMemory] operation={} kind={} live={} bytes={} peak_bytes={} "
                    "created={} destroyed={} registry_dropped={} registry_missing={}",
                    operation, i ? "image" : "buffer", total.live, total.bytes, total.peak_bytes,
                    total.created, total.destroyed, lost, unmatched);
    }
    static constexpr std::array names{"images",        "staging_ring",  "staging_large",
                                      "retired_ring",  "retired_large", "stream",
                                      "other_buffers", "dense_active",  "dense_retired"};
    for (u32 i = 0; i < categories.size(); ++i)
        LOG_WARNING(Render_Vulkan, "[ResourceMemory] category={} tracked_live={} tracked_bytes={}",
                    names[i], categories[i].live, categories[i].bytes);
    for (u32 i = 0; i < largest.size() && largest[i].key; ++i) {
        const auto& entry = largest[i];
        LOG_WARNING(Render_Vulkan,
                    "[ResourceMemory] largest rank={} kind={} resource={:#x} allocation={:#x} "
                    "bytes={} sequence={} memory_type={} name={} format={} extent={}x{}x{} mips={} "
                    "layers={} samples={}",
                    i, entry.kind ? "image" : "buffer", entry.resource, entry.key, entry.bytes,
                    entry.sequence, entry.memory_type, entry.name.data(), entry.format, entry.width,
                    entry.height, entry.depth, entry.mips, entry.layers, entry.samples);
    }
    return true;
}
} // namespace Vulkan::ResourceMemory
