// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace Core::Uwp {

class GuestReservations {
public:
    using Range = std::pair<uint64_t, uint64_t>;

    GuestReservations() = default;
    GuestReservations(const GuestReservations&) = delete;
    GuestReservations& operator=(const GuestReservations&) = delete;

    ~GuestReservations() {
        for (const auto& [address, size] : ranges)
            VirtualFree(reinterpret_cast<void*>(address), 0, MEM_RELEASE);
    }

    bool Reserve(uint64_t first, uint64_t end) {
        constexpr uint64_t Alignment = 65536;
        constexpr uint64_t MinimumSize = 16 * 1024 * 1024;
        while (first < end) {
            MEMORY_BASIC_INFORMATION info{};
            if (!VirtualQuery(reinterpret_cast<void*>(first), &info, sizeof(info)))
                return false;
            const auto base = reinterpret_cast<uint64_t>(info.BaseAddress);
            const auto next = std::min(end, base + info.RegionSize);
            if (next <= first) {
                SetLastError(ERROR_INVALID_ADDRESS);
                return false;
            }
            const auto start = (std::max(base, first) + Alignment - 1) & ~(Alignment - 1);
            const auto limit = next & ~(Alignment - 1);
            if (info.State == MEM_FREE && limit > start && limit - start > MinimumSize) {
                if (!VirtualAlloc2FromApp(nullptr, reinterpret_cast<void*>(start), limit - start,
                                          MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS,
                                          nullptr, 0))
                    return false;
                try {
                    ranges.emplace_back(start, limit - start);
                } catch (...) {
                    VirtualFree(reinterpret_cast<void*>(start), 0, MEM_RELEASE);
                    throw;
                }
            }
            first = next;
        }
        return true;
    }

    bool Adopt(std::vector<Range>& incoming) {
        if (!ranges.empty()) {
            SetLastError(ERROR_ALREADY_INITIALIZED);
            return false;
        }
        std::sort(incoming.begin(), incoming.end());
        uint64_t previous{};
        for (const auto& [address, size] : incoming) {
            MEMORY_BASIC_INFORMATION info{};
            if (!size || address % 65536 || size % 65536 || address < previous ||
                address < 0x400000 || address >= 0x10000000000ULL ||
                size > 0x10000000000ULL - address ||
                !VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) ||
                info.State != MEM_RESERVE || info.AllocationProtect != PAGE_NOACCESS ||
                reinterpret_cast<uint64_t>(info.AllocationBase) != address ||
                info.RegionSize != size) {
                SetLastError(ERROR_INVALID_PARAMETER);
                return false;
            }
            previous = address + size;
        }
        ranges.swap(incoming);
        return true;
    }

    const std::vector<Range>& Ranges() const {
        return ranges;
    }

    std::vector<Range> Take() {
        std::vector<Range> result;
        result.swap(ranges);
        return result;
    }

private:
    std::vector<Range> ranges;
};

inline GuestReservations& StagedGuestReservations() {
    static GuestReservations reservations;
    return reservations;
}

} // namespace Core::Uwp
