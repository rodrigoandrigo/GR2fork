// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <winrt/Windows.System.h>
#include "../guest_memory.h"
#include "../memory_diagnostics.h"
#include "common/scope_exit.h"
#include "memory_backend.h"

namespace Core::Uwp {

using Xbox::Lazy::Mode;
using Xbox::Lazy::Regions;

static bool IsResourceFailure(DWORD error) {
    return error == ERROR_NOT_ENOUGH_MEMORY || error == ERROR_OUTOFMEMORY ||
           error == ERROR_COMMITMENT_LIMIT || error == ERROR_NO_SYSTEM_RESOURCES ||
           error == ERROR_NOT_ENOUGH_QUOTA;
}

uint64_t GuestMemory::Logical(uint64_t physical) {
    return physical + WindowSize;
}

uint64_t GuestMemory::Physical(uint64_t logical) {
    return logical - WindowSize;
}

DWORD GuestMemory::WindowsProtection(uint8_t protection) {
    if (protection & 4)
        return PAGE_EXECUTE_READ;
    if (protection & 2)
        return PAGE_READWRITE;
    return protection & 1 ? PAGE_READONLY : PAGE_NOACCESS;
}

bool GuestMemory::ValidRange(uint64_t address, uint64_t bytes) {
    return bytes && bytes <= UINT64_MAX - address;
}

GuestMemory::GuestMemory(uint64_t bytes, uint64_t direct, uintptr_t base)
    : size{bytes}, direct_size{direct}, canonical{base}, physical_pages(bytes / PageSize),
      window_sections(bytes / WindowSize),
      discard_windows(bytes / WindowSize, UINT32_MAX) {
    free_sections.reserve(128);
    if (!bytes || bytes % WindowSize || direct > bytes || base % WindowSize ||
        !ValidRange(base, bytes) || !regions.Map(Logical(0), bytes)) {
        throw std::invalid_argument("invalid Kyty guest backing range");
    }
}

GuestMemory::~GuestMemory() {
    Stop();
    for (auto& [address, page] : pages)
        ReleaseMapping(page);
    if (scratch_view)
        UnmapViewOfFile2(GetCurrentProcess(), scratch_view, MEM_PRESERVE_PLACEHOLDER);
    if (scratch_address)
        VirtualFree(scratch_address, 0, MEM_RELEASE);
    for (auto section : window_sections)
        if (section)
            CloseHandle(section);
    for (auto section : free_sections)
        CloseHandle(section);
}

bool GuestMemory::OpenStore(const wchar_t* path) {
    std::lock_guard lock{mutex};
    return store.Open(path);
}

bool GuestMemory::AllocationFailed(const char* api, uint64_t address, HANDLE section) {
    const auto error = GetLastError();
    failure_api = api;
    failure_address = address;
    failure_section = section;
    failure_error = error;
    ++failure_count;
    const auto now = GetTickCount64();
    if (!last_failure_report || now - last_failure_report >= 1000) {
        last_failure_report = now;
        APP_MEMORY_INFORMATION memory{};
        const bool available = GetProcessInformation(GetCurrentProcess(), ProcessAppMemoryInfo,
                                                      &memory, sizeof(memory));
        char message[1024]{};
        std::snprintf(message, sizeof(message),
                      "[MappingDiag] api=%s win32=%lu tid=%lu address=0x%llx section=%p "
                      "failures=%llu views=%llu sections_live=%llu cached=%llu "
                      "resident_bytes=%llu commit_available=%llu private_commit=%llu "
                      "total_commit=%llu memory_info=%u",
                      api, error, GetCurrentThreadId(), address, section, failure_count,
                      view_count, sections_live, static_cast<uint64_t>(free_sections.size()),
                      Resident(), memory.AvailableCommit, memory.PrivateCommitUsage,
                      memory.TotalCommitUsage, available);
        uwp_memory_log_diagnostic(message);
    }
    SetLastError(error);
    return false;
}

HANDLE GuestMemory::AcquireSection(uint64_t physical) {
    auto& state = physical_pages[physical / PageSize];
    auto& section = window_sections[physical / WindowSize];
    if (!section) {
        if (!free_sections.empty()) {
            section = free_sections.back();
            free_sections.pop_back();
            ++sections_reused;
        } else {
            section = CreateFileMappingFromApp(INVALID_HANDLE_VALUE, nullptr,
                                               PAGE_EXECUTE_READWRITE, WindowSize, nullptr);
            if (!section) {
                AllocationFailed("CreateFileMappingFromApp", physical, nullptr);
                return nullptr;
            }
            ++sections_created;
            ++sections_live;
        }
    }
    state.section = section;
    return section;
}

void GuestMemory::ReleaseSection(uint64_t physical) {
    auto& state = physical_pages[physical / PageSize];
    if (!state.section)
        return;
    state.section = nullptr;
    const auto first = physical & ~(WindowSize - 1);
    for (auto at = first; at < first + WindowSize; at += PageSize)
        if (physical_pages[at / PageSize].section)
            return;
    auto& section = window_sections[physical / WindowSize];
    if (scratch_section == section) {
        if (!UnmapViewOfFile2(GetCurrentProcess(), scratch_view, MEM_PRESERVE_PLACEHOLDER))
            std::terminate();
        scratch_view = nullptr;
        scratch_section = nullptr;
    }
    if (free_sections.size() < 128)
        free_sections.push_back(section);
    else {
        CloseHandle(section);
        --sections_live;
        ++sections_closed;
    }
    section = nullptr;
}

void* GuestMemory::MapScratch(HANDLE section) {
    if (scratch_section == section) {
        ++scratch_reuses;
        return scratch_view;
    }
    if (scratch_view && !UnmapViewOfFile2(GetCurrentProcess(), scratch_view, MEM_PRESERVE_PLACEHOLDER)) {
        AllocationFailed("UnmapViewOfFile2.scratch", reinterpret_cast<uintptr_t>(scratch_view), scratch_section);
        return nullptr;
    }
    scratch_section = nullptr;
    scratch_view = nullptr;
    if (!scratch_address) {
        scratch_address = VirtualAlloc2FromApp(nullptr, nullptr, WindowSize,
                                               MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                               PAGE_NOACCESS, nullptr, 0);
        if (!scratch_address) {
            AllocationFailed("VirtualAlloc2FromApp.scratch", 0, section);
            return nullptr;
        }
    }
    scratch_view = MapViewOfFile3(section, GetCurrentProcess(), scratch_address, 0, WindowSize,
                                  MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
    if (!scratch_view) {
        AllocationFailed("MapViewOfFile3.scratch", reinterpret_cast<uintptr_t>(scratch_address), section);
        return nullptr;
    }
    scratch_section = section;
    ++scratch_maps;
    return scratch_view;
}

void GuestMemory::PruneViews() {
    std::shared_lock copy_lock{copy_mutex};
    std::lock_guard lock{mutex};
    for (size_t visited = 0; visited < 2048 && view_count > view_limit; ++visited) {
        const auto physical = prune_hand;
        prune_hand = (prune_hand + PageSize) % size;
        auto& state = physical_pages[physical / PageSize];
        if (!state.owner || !state.owner->resident ||
            regions.IsPinned(Logical(physical) / WindowSize))
            continue;
        if (state.owner->canonical) {
            for (auto* page = state.first; page; page = page->next) {
                if (!page->canonical && page->resident) {
                    state.owner = page;
                    break;
                }
            }
        }
        for (auto* page = state.first; page; page = page->next) {
            if (page != state.owner && page->canonical && page->view && ReleaseMapping(*page))
                ++views_pruned;
        }
    }
}

bool GuestMemory::IsolatePlaceholder(uintptr_t address, uint64_t bytes) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) ||
        info.State != MEM_RESERVE) {
        SetLastError(ERROR_INVALID_ADDRESS);
        return false;
    }
    const auto base = reinterpret_cast<uintptr_t>(info.AllocationBase);
    const auto end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    if (base > address || address + bytes > end) {
        SetLastError(ERROR_INVALID_ADDRESS);
        return false;
    }
    if (base < address && !VirtualFree(reinterpret_cast<void*>(base), address - base,
                                       MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
        return false;
    return end == address + bytes || VirtualFree(reinterpret_cast<void*>(address), bytes,
                                                 MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
}

GuestMemory::VirtualPage* GuestMemory::Find(uintptr_t address) {
    const auto it = pages.find(address & ~(PageSize - 1));
    return it == pages.end() ? nullptr : &it->second;
}

GuestMemory::VirtualPage& GuestMemory::Add(uintptr_t address, uint64_t physical,
                                           bool is_canonical) {
    auto [it, inserted] = pages.try_emplace(address);
    if (!inserted)
        throw std::invalid_argument("overlapping Kyty virtual mapping");
    auto& page = it->second;
    page.address = address;
    page.physical = physical;
    page.canonical = is_canonical;
    auto& state = physical_pages[physical / PageSize];
    page.next = state.first;
    if (state.first)
        state.first->previous = &page;
    state.first = &page;
    return page;
}

void GuestMemory::Erase(VirtualPage& page) {
    auto& state = physical_pages[page.physical / PageSize];
    if (page.previous)
        page.previous->next = page.next;
    else
        state.first = page.next;
    if (page.next)
        page.next->previous = page.previous;
    pages.erase(page.address);
}

GuestMemory::VirtualPage& GuestMemory::Owner(uint64_t physical) {
    auto& state = physical_pages[physical / PageSize];
    if (!state.owner) {
        state.owner = state.first;
        if (!state.owner)
            state.owner = &Add(canonical + physical, physical, true);
    }
    return *state.owner;
}

bool GuestMemory::ReservePrivate(VirtualPage& page) {
    if (page.placed)
        return true;
    last_operation = "isolate_private";
    last_address = page.address;
    if (!IsolatePlaceholder(page.address, PageSize))
        return false;
    last_operation = "reserve_private";
    if (!VirtualAlloc2FromApp(nullptr, reinterpret_cast<void*>(page.address), PageSize,
                              MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0))
        return false;
    page.placed = true;
    return true;
}

bool GuestMemory::ApplyProtection(VirtualPage& page) {
    if (!page.resident)
        return true;
    if (std::all_of(page.protection.begin(), page.protection.end(),
                    [&](uint8_t protection) { return protection == page.protection[0]; })) {
        last_operation = "protect_page";
        last_address = page.address;
        last_protection = WindowsProtection(page.protection[0]);
        DWORD previous{};
        return VirtualProtectFromApp(reinterpret_cast<void*>(page.address), PageSize,
                                     last_protection, &previous);
    }
    for (size_t i = 0; i < page.protection.size(); ++i) {
        last_operation = "protect_page";
        last_address = page.address + i * 4096;
        last_protection = WindowsProtection(page.protection[i]);
        DWORD previous{};
        if (!VirtualProtectFromApp(reinterpret_cast<void*>(page.address + i * 4096), 4096,
                                   WindowsProtection(page.protection[i]), &previous))
            return false;
    }
    return true;
}

bool GuestMemory::ReleaseMapping(VirtualPage& page) {
    if (page.view) {
        if (!UnmapViewOfFile2(GetCurrentProcess(), reinterpret_cast<void*>(page.address),
                              MEM_PRESERVE_PLACEHOLDER))
            return false;
        page.view = false;
        --view_count;
    } else if (page.placed) {
        if (!VirtualFree(reinterpret_cast<void*>(page.address), PageSize,
                         MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
            return false;
        page.placed = false;
    }
    page.resident = false;
    return true;
}

bool GuestMemory::Commit(uint64_t address, uint64_t bytes) {
    auto rollback = ::detail::MakeScopeGuard([&] {
        const auto error = GetLastError();
        Decommit(address, bytes, false);
        SetLastError(error);
    });
    const auto first = Physical(address);
    uint64_t done{};
    for (; done < bytes; done += PageSize) {
        auto& page = Owner(first + done);
        if (page.resident)
            continue;
        if (!ReservePrivate(page))
            break;
        last_operation = "commit_private";
        if (!VirtualAllocFromApp(reinterpret_cast<void*>(page.address), PageSize, MEM_COMMIT,
                                 PAGE_READWRITE)) {
            AllocationFailed("VirtualAllocFromApp.commit", page.address, nullptr);
            break;
        }
        page.resident = true;
        resident_bytes += PageSize;
        if (!ApplyProtection(page))
            break;
    }
    if (done != bytes) {
        const auto error = GetLastError();
        SetLastError(error);
        return false;
    }
    rollback.Cancel();
    ++commit_epoch;
    return true;
}

void GuestMemory::Decommit(uint64_t address, uint64_t bytes, bool) {
    const auto first = Physical(address);
    for (uint64_t at = first; at < first + bytes; at += PageSize) {
        auto& state = physical_pages[at / PageSize];
        const bool was_resident = state.owner && state.owner->resident;
        for (auto* page = state.first; page; page = page->next) {
            if (!ReleaseMapping(*page))
                std::terminate();
        }
        ReleaseSection(at);
        if (was_resident)
            resident_bytes -= PageSize;
    }
    ++commit_epoch;
}

bool GuestMemory::Protect(uint64_t address, uint64_t bytes, Mode mode) {
    const auto first = Physical(address);
    const auto last = first + bytes;
    for (auto at = first & ~(PageSize - 1); at < last; at += PageSize) {
        auto& state = physical_pages[at / PageSize];
        for (auto* page = state.first; page; page = page->next) {
            if (!page->resident)
                continue;
            const auto lo = std::max(first, at);
            const auto hi = std::min(last, at + PageSize);
            const bool uniform =
                mode != Mode::ReadWrite ||
                std::all_of(page->protection.begin(), page->protection.end(),
                            [&](uint8_t protection) { return protection == page->protection[0]; });
            if (lo == at && hi == at + PageSize && uniform) {
                const DWORD target = mode == Mode::NoAccess ? PAGE_NOACCESS
                                     : mode == Mode::Read ? PAGE_READONLY
                                                          : WindowsProtection(page->protection[0]);
                DWORD previous{};
                if (!VirtualProtectFromApp(reinterpret_cast<void*>(page->address), PageSize, target,
                                           &previous))
                    return false;
                continue;
            }
            for (uint64_t part = std::max(first, at); part < std::min(last, at + PageSize);
                 part += 4096) {
                const auto index = (part - at) / 4096;
                DWORD previous{};
                DWORD target = mode == Mode::NoAccess ? PAGE_NOACCESS : PAGE_READONLY;
                if (mode == Mode::ReadWrite)
                    target = WindowsProtection(page->protection[index]);
                if (!VirtualProtectFromApp(reinterpret_cast<void*>(page->address + index * 4096),
                                           4096, target, &previous))
                    return false;
            }
        }
    }
    return true;
}

bool GuestMemory::ReadWindow(uint64_t physical, void* destination) {
    auto* target = static_cast<uint8_t*>(destination);
    const bool evicting = (regions.FlagsOf(Logical(physical)) & Xbox::Lazy::kEvicting) != 0;
    for (uint64_t at = 0; at < WindowSize; at += PageSize) {
        auto& state = physical_pages[(physical + at) / PageSize];
        if (!state.owner || !state.owner->resident) {
            std::memset(target + at, 0, PageSize);
            continue;
        }
        auto& page = *state.owner;
        if (evicting) {
            std::memcpy(target + at, reinterpret_cast<void*>(page.address), PageSize);
            continue;
        }
        std::array<DWORD, 4> previous{};
        size_t changed{};
        for (; changed < previous.size(); ++changed) {
            if (!VirtualProtectFromApp(reinterpret_cast<void*>(page.address + changed * 4096), 4096,
                                       PAGE_READONLY, &previous[changed]))
                break;
        }
        if (changed == previous.size())
            std::memcpy(target + at, reinterpret_cast<void*>(page.address), PageSize);
        const auto error = GetLastError();
        const bool copied = changed == previous.size();
        while (changed) {
            --changed;
            DWORD ignored{};
            if (!VirtualProtectFromApp(reinterpret_cast<void*>(page.address + changed * 4096), 4096,
                                       previous[changed], &ignored))
                std::terminate();
        }
        if (!copied) {
            SetLastError(error);
            return false;
        }
    }
    return true;
}

bool GuestMemory::Spill(uint64_t window, uint64_t address, uint64_t bytes, uint32_t& slot) {
    if (!store.IsOpen() || bytes != WindowSize)
        return false;
    if (!ReadWindow(Physical(address), transfer.data()))
        return false;
    const auto checksum = Xbox::PagerStore::Checksum(transfer.data(), bytes);
    const auto allocated = store.Alloc();
    if (!store.Write(allocated, transfer.data(), bytes)) {
        store.Free(allocated);
        return false;
    }
    store.SetChecksum(allocated, checksum ^ (window * 0x9E3779B97F4A7C15ULL));
    slot = allocated;
    ++store.counters.evicted;
    ++store.counters.emergency;
    return true;
}

bool GuestMemory::CommitFromStore(uint64_t window, uint64_t address, uint64_t bytes,
                                  uint32_t slot) {
    if (bytes != WindowSize || !store.Read(slot, 0, transfer.data(), bytes))
        return false;
    const auto checksum = Xbox::PagerStore::Checksum(transfer.data(), bytes);
    if (!store.CheckChecksum(slot, checksum ^ (window * 0x9E3779B97F4A7C15ULL))) {
        ++store.counters.corrupt;
        SetLastError(ERROR_CRC);
        return false;
    }
    const auto first = Physical(address);
    auto rollback = ::detail::MakeScopeGuard([&] {
        const auto error = GetLastError();
        Decommit(address, bytes, true);
        SetLastError(error);
    });
    const auto section = AcquireSection(first);
    if (!section)
        return false;
    auto* scratch = MapScratch(section);
    if (!scratch)
        return false;
    std::memcpy(scratch, transfer.data(), bytes);
    uint64_t done{};
    for (; done < bytes; done += PageSize) {
        auto& state = physical_pages[(first + done) / PageSize];
        auto& page = Owner(first + done);
        state.section = section;
        if (!IsolatePlaceholder(page.address, PageSize))
            break;
        if (!MapViewOfFile3(section, GetCurrentProcess(), reinterpret_cast<void*>(page.address),
                            done, PageSize, MEM_REPLACE_PLACEHOLDER, PAGE_EXECUTE_READWRITE,
                            nullptr, 0)) {
            AllocationFailed("MapViewOfFile3.page_in", page.address, section);
            break;
        }
        page.view = true;
        page.resident = true;
        ++view_count;
        resident_bytes += PageSize;
        if (!ApplyProtection(page))
            break;
    }
    if (done != bytes) {
        const auto error = GetLastError();
        SetLastError(error);
        return false;
    }
    rollback.Cancel();
    ++commit_epoch;
    ++store.counters.page_ins;
    return true;
}

void GuestMemory::FreeSlot(uint32_t slot) {
    store.Free(slot);
}

bool GuestMemory::Tight(uint64_t bytes) {
    return store.IsOpen() && app_target &&
           (bytes >= app_target || app_usage > app_target - bytes);
}

uint64_t GuestMemory::Resident() const {
    return resident_bytes;
}

bool GuestMemory::Promote(PhysicalPage& state) {
    if (state.section)
        return true;
    if (!state.owner || !state.owner->resident) {
        SetLastError(ERROR_INVALID_ADDRESS);
        return false;
    }
    auto& owner = *state.owner;
    const auto section = AcquireSection(owner.physical);
    if (!section)
        return false;
    auto* base = static_cast<uint8_t*>(MapScratch(section));
    auto* scratch = base ? base + owner.physical % WindowSize : nullptr;
    if (!scratch) {
        const auto error = GetLastError();
        ReleaseSection(owner.physical);
        SetLastError(error);
        return false;
    }
    std::array<DWORD, 4> previous{};
    size_t changed{};
    for (; changed < previous.size(); ++changed) {
        if (!VirtualProtectFromApp(reinterpret_cast<void*>(owner.address + changed * 4096), 4096,
                                   PAGE_READONLY, &previous[changed]))
            break;
    }
    bool copied = changed == previous.size();
    if (copied)
        std::memcpy(scratch, reinterpret_cast<void*>(owner.address), PageSize);
    auto error = GetLastError();
    while (changed) {
        --changed;
        DWORD ignored{};
        if (!VirtualProtectFromApp(reinterpret_cast<void*>(owner.address + changed * 4096), 4096,
                                   previous[changed], &ignored))
            std::terminate();
    }
    if (copied && ReleaseMapping(owner)) {
        if (MapViewOfFile3(section, GetCurrentProcess(), reinterpret_cast<void*>(owner.address),
                           owner.physical % WindowSize, PageSize, MEM_REPLACE_PLACEHOLDER, PAGE_EXECUTE_READWRITE, nullptr, 0)) {
            state.section = section;
            owner.view = true;
            owner.resident = true;
            ++view_count;
            if (!ApplyProtection(owner))
                std::terminate();
            ++commit_epoch;
        } else {
            error = GetLastError();
            if (!ReservePrivate(owner) ||
                !VirtualAllocFromApp(reinterpret_cast<void*>(owner.address), PageSize, MEM_COMMIT,
                                     PAGE_READWRITE))
                std::terminate();
            std::memcpy(reinterpret_cast<void*>(owner.address), scratch, PageSize);
            owner.resident = true;
            if (!ApplyProtection(owner))
                std::terminate();
            copied = false;
        }
    } else {
        if (copied)
            error = GetLastError();
        copied = false;
    }
    if (!copied) {
        ReleaseSection(owner.physical);
        SetLastError(error);
    }
    return copied;
}

bool GuestMemory::MapAlias(VirtualPage& page) {
    auto& state = physical_pages[page.physical / PageSize];
    if (page.resident)
        return true;
    last_operation = "promote_alias";
    last_address = page.address;
    if (!Promote(state))
        return false;
    last_operation = "isolate_alias";
    last_address = page.address;
    if (!IsolatePlaceholder(page.address, PageSize))
        return false;
    last_operation = "map_alias";
    if (!MapViewOfFile3(state.section, GetCurrentProcess(), reinterpret_cast<void*>(page.address),
                        page.physical % WindowSize, PageSize, MEM_REPLACE_PLACEHOLDER, PAGE_EXECUTE_READWRITE, nullptr, 0))
        return AllocationFailed("MapViewOfFile3.alias", page.address, state.section);
    page.view = true;
    page.resident = true;
    ++view_count;
    ++commit_epoch;
    return ApplyProtection(page);
}

bool GuestMemory::Rehome(VirtualPage& page) {
    auto& state = physical_pages[page.physical / PageSize];
    if (state.owner != &page)
        return true;
    if (!page.resident) {
        state.owner = nullptr;
        return true;
    }
    for (auto* candidate = state.first; candidate; candidate = candidate->next) {
        if (candidate != &page && candidate->resident) {
            state.owner = candidate;
            return true;
        }
    }
    if (!Promote(state))
        return false;
    const auto target = canonical + page.physical;
    auto* replacement = Find(target);
    if (!replacement)
        replacement = &Add(target, page.physical, true);
    if (!MapAlias(*replacement))
        return false;
    state.owner = replacement;
    return true;
}

bool GuestMemory::HasRestrictedAlias(uint64_t physical) const {
    for (auto* page = physical_pages[physical / PageSize].first; page; page = page->next) {
        if (std::any_of(page->protection.begin(), page->protection.end(),
                        [](uint8_t protection) { return protection != 3; }))
            return true;
    }
    return false;
}

bool GuestMemory::RefreshProtection(uint64_t physical) {
    const auto start = physical & ~(WindowSize - 1);
    const auto window = Logical(start) / WindowSize;
    bool restricted{};
    for (uint64_t at = start; at < start + WindowSize; at += PageSize)
        restricted |= HasRestrictedAlias(at);
    const auto found = protected_windows.find(window);
    if (restricted && found == protected_windows.end()) {
        const auto pin = regions.TempPin(Logical(start), WindowSize);
        try {
            protected_windows.emplace(window, pin);
        } catch (...) {
            regions.TempUnpin(pin);
            throw;
        }
    } else if (!restricted && found != protected_windows.end()) {
        regions.TempUnpin(found->second);
        protected_windows.erase(found);
        ProcessPendingWindow(window);
    }
    if (restricted)
        regions.Touch(window, *this);
    return true;
}

bool GuestMemory::Map(uintptr_t address, uint64_t bytes, uint64_t physical, int protection) {
    if (!ValidRange(address, bytes) || address % PageSize || bytes % PageSize ||
        physical % PageSize || physical > size || bytes > size - physical ||
        (address < canonical + size && address + bytes > canonical)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    std::unique_lock copy_lock{copy_mutex};
    std::lock_guard lock{mutex};
    for (uint64_t at = 0; at < bytes; at += PageSize) {
        if (Find(address + at)) {
            SetLastError(ERROR_INVALID_ADDRESS);
            return false;
        }
    }
    uint64_t added{};
    try {
        for (; added < bytes; added += PageSize) {
            auto& page = Add(address + added, physical + added, false);
            page.protection.fill(static_cast<uint8_t>(protection));
        }
        if (protection != 3) {
            for (auto at = physical & ~(WindowSize - 1); at < physical + bytes; at += WindowSize)
                RefreshProtection(at);
        }
    } catch (...) {
        for (uint64_t at = 0; at <= added && at < bytes; at += PageSize) {
            if (auto* page = Find(address + at)) {
                const auto offset = page->physical;
                Erase(*page);
                RefreshProtection(offset);
            }
        }
        throw;
    }
    for (uint64_t at = physical; at < physical + bytes; at += PageSize) {
        auto& state = physical_pages[at / PageSize];
        state.discarded = false;
        state.discard_pending = false;
    }
    return true;
}

bool GuestMemory::Unmap(uintptr_t address, uint64_t bytes) {
    if (!ValidRange(address, bytes) || address % PageSize || bytes % PageSize) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    std::unique_lock copy_lock{copy_mutex};
    std::lock_guard lock{mutex};
    for (uint64_t at = 0; at < bytes; at += PageSize) {
        if (auto* page = Find(address + at); page && !page->canonical) {
            const auto physical = page->physical;
            if (!Rehome(*page) || !ReleaseMapping(*page))
                return false;
            Erase(*page);
            RefreshProtection(physical);
            if (physical >= direct_size && !physical_pages[physical / PageSize].discard_pending) {
                physical_pages[physical / PageSize].discard_pending = true;
                if (!DiscardPage(physical))
                    QueueDiscard(physical);
            }
            ProcessPendingWindow(Logical(physical) / WindowSize);
        }
    }
    return true;
}

bool GuestMemory::ProtectRange(uintptr_t address, uint64_t bytes, int protection) {
    if (!ValidRange(address, bytes) || address % 4096 || bytes % 4096) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    std::shared_lock copy_lock{copy_mutex};
    std::lock_guard lock{mutex};
    for (auto at = address; at < address + bytes; at += 4096) {
        auto* page = Find(at);
        if (!page) {
            SetLastError(ERROR_INVALID_ADDRESS);
            return false;
        }
        regions.Touch(Logical(page->physical) / WindowSize, *this);
        const auto index = (at - page->address) / 4096;
        const auto previous = page->protection[index];
        page->protection[index] = static_cast<uint8_t>(protection);
        if (!ApplyProtection(*page)) {
            page->protection[index] = previous;
            return false;
        }
        RefreshProtection(page->physical);
    }
    return true;
}

bool GuestMemory::Ensure(VirtualPage& page) {
    const auto logical = Logical(page.physical);
    const auto previous_failure = failure_count;
    const auto result = regions.CommitAt(logical, *this);
    if (result == Regions::CommitResult::Failed || result == Regions::CommitResult::NotLazy) {
        if (failure_count != previous_failure)
            SetLastError(failure_error);
        return false;
    }
    regions.Touch(logical / WindowSize, *this);
    if (regions.FlagsOf(logical) & (Xbox::Lazy::kSwept | Xbox::Lazy::kEvicting))
        return false;
    if (page.resident)
        return true;
    auto& state = physical_pages[page.physical / PageSize];
    if (!state.owner || !state.owner->resident) {
        auto& owner = Owner(page.physical);
        if (!ReservePrivate(owner) || !VirtualAllocFromApp(reinterpret_cast<void*>(owner.address),
                                                           PageSize, MEM_COMMIT, PAGE_READWRITE))
            return false;
        owner.resident = true;
        resident_bytes += PageSize;
        if (!ApplyProtection(owner))
            return false;
    }
    return &page == state.owner || MapAlias(page);
}

bool GuestMemory::FaultLocked(uintptr_t address, int access, bool& failed) {
    auto* page = Find(address);
    const auto offset = address - canonical;
    if (!page && offset < size)
        page = &Add(address & ~(PageSize - 1), offset & ~(PageSize - 1), true);
    if (!page)
        return false;
    const auto index = (address - page->address) / 4096;
    const auto wanted = page->protection[index];
    const bool permitted = access == 1 ? wanted & 2 : access == 8 ? wanted & 4 : wanted & 7;
    if (!permitted)
        return false;
    const bool resident = page->resident;
    const auto flags = regions.FlagsOf(Logical(page->physical));
    if (!Ensure(*page)) {
        failed = true;
        return false;
    }
    if (!resident || (flags & (Xbox::Lazy::kSwept | Xbox::Lazy::kEvicting)))
        return true;
    const auto subpage = address & ~uintptr_t{4095};
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<void*>(subpage), &info, sizeof(info))) {
        failed = true;
        return false;
    }
    DWORD target = WindowsProtection(wanted);
    if ((wanted & 6) == 6)
        target = access == 8 ? PAGE_EXECUTE_READ : PAGE_READWRITE;
    if (info.Protect != target) {
        DWORD previous{};
        if (!VirtualProtectFromApp(reinterpret_cast<void*>(subpage), 4096, target, &previous)) {
            failed = true;
            return false;
        }
        if (access == 8)
            FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(subpage), 4096);
        ++commit_epoch;
        return true;
    }
    static thread_local uintptr_t last_page{};
    static thread_local uint64_t last_epoch{UINT64_MAX};
    if (last_page == subpage && last_epoch == commit_epoch)
        return false;
    last_page = subpage;
    last_epoch = commit_epoch;
    return true;
}

bool GuestMemory::CopyPhysical(uint64_t physical, void* data, uint64_t bytes, bool write) {
    if (physical >= size || bytes > size - physical || bytes > WindowSize - physical % WindowSize) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    const auto start = physical & ~(WindowSize - 1);
    const auto window = Logical(start) / WindowSize;
    uint32_t slot{};
    if (regions.EvictedSlot(window, slot)) {
        if (!store.Read(slot, 0, transfer.data(), WindowSize))
            return false;
        const auto checksum = Xbox::PagerStore::Checksum(transfer.data(), WindowSize);
        if (!store.CheckChecksum(slot, checksum ^ (window * 0x9E3779B97F4A7C15ULL))) {
            ++store.counters.corrupt;
            SetLastError(ERROR_CRC);
            return false;
        }
        if (!write) {
            std::memcpy(data, transfer.data() + physical - start, bytes);
            ++store.counters.read_hits;
            return true;
        }
        std::memcpy(transfer.data() + physical - start, data, bytes);
        const auto replacement = store.Alloc();
        if (!store.Write(replacement, transfer.data(), WindowSize)) {
            const auto error = GetLastError();
            store.Free(replacement);
            SetLastError(error);
            return false;
        }
        store.SetChecksum(replacement, Xbox::PagerStore::Checksum(transfer.data(), WindowSize) ^
                                           (window * 0x9E3779B97F4A7C15ULL));
        uint32_t previous{};
        if (!regions.ReplaceEvictedSlot(window, replacement, previous)) {
            store.Free(replacement);
            SetLastError(ERROR_INVALID_STATE);
            return false;
        }
        store.Free(previous);
        for (auto at = physical & ~(PageSize - 1); at < physical + bytes; at += PageSize)
            physical_pages[at / PageSize].discarded = false;
        return true;
    }
    auto* target = static_cast<uint8_t*>(data);
    uint64_t done{};
    while (done < bytes) {
        const auto offset = physical + done;
        const auto page_offset = offset % PageSize;
        const auto count = std::min(bytes - done, PageSize - page_offset);
        auto& state = physical_pages[offset / PageSize];
        if (write)
            state.discarded = false;
        if (!write && (!state.owner || !state.owner->resident)) {
            std::memset(target + done, 0, count);
            done += count;
            continue;
        }
        auto& owner = Owner(offset - page_offset);
        if (!Ensure(owner))
            return false;
        const auto first_page = page_offset / 4096;
        const auto last_page = (page_offset + count - 1) / 4096;
        const bool accessible =
            std::all_of(owner.protection.begin() + first_page,
                        owner.protection.begin() + last_page + 1, [&](uint8_t protection) {
                            return write ? (protection & 2) && !(protection & 4) : protection != 0;
                        });
        if (accessible) {
            auto* memory = reinterpret_cast<uint8_t*>(owner.address + page_offset);
            if (write)
                std::memcpy(memory, target + done, count);
            else
                std::memcpy(target + done, memory, count);
            done += count;
            continue;
        }
        std::array<DWORD, 4> previous{};
        size_t changed{};
        for (; changed < previous.size(); ++changed) {
            if (!VirtualProtectFromApp(reinterpret_cast<void*>(owner.address + changed * 4096),
                                       4096, write ? PAGE_READWRITE : PAGE_READONLY,
                                       &previous[changed]))
                break;
        }
        const bool copied = changed == previous.size();
        if (copied) {
            auto* memory = reinterpret_cast<uint8_t*>(owner.address + page_offset);
            if (write)
                std::memcpy(memory, target + done, count);
            else
                std::memcpy(target + done, memory, count);
        }
        const auto error = GetLastError();
        while (changed) {
            --changed;
            DWORD ignored{};
            if (!VirtualProtectFromApp(reinterpret_cast<void*>(owner.address + changed * 4096),
                                       4096, previous[changed], &ignored))
                std::terminate();
        }
        if (!copied) {
            SetLastError(error);
            return false;
        }
        done += count;
    }
    return true;
}

bool GuestMemory::Copy(void* destination, const void* source, uint64_t bytes) {
    const auto dst = reinterpret_cast<uintptr_t>(destination);
    const auto src = reinterpret_cast<uintptr_t>(source);
    const bool dst_physical = dst - canonical < size;
    const bool src_physical = src - canonical < size;
    std::shared_lock copy_lock{copy_mutex};
    std::lock_guard lock{mutex};
    if (!ValidRange(dst, bytes) || !ValidRange(src, bytes) ||
        (!dst_physical && !src_physical && !Find(dst) && !Find(src)) ||
        (dst_physical && bytes > size - (dst - canonical)) ||
        (src_physical && bytes > size - (src - canonical))) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    std::array<uint8_t, PageSize> buffer{};
    uint64_t done{};
    while (done < bytes) {
        auto count = std::min(bytes - done, PageSize);
        const auto* source_page = src_physical ? nullptr : Find(src + done);
        const auto* destination_page = dst_physical ? nullptr : Find(dst + done);
        if (src_physical) {
            count = std::min(count, PageSize - (src + done - canonical) % PageSize);
        } else if (source_page) {
            count = std::min(count, PageSize - (src + done - source_page->address));
        }
        if (dst_physical) {
            count = std::min(count, PageSize - (dst + done - canonical) % PageSize);
        } else if (destination_page) {
            count = std::min(count, PageSize - (dst + done - destination_page->address));
        }
        if (src_physical || source_page) {
            const auto physical = src_physical
                                      ? src + done - canonical
                                      : source_page->physical + src + done - source_page->address;
            if (!CopyPhysical(physical, buffer.data(), count, false))
                return false;
        } else {
            std::memcpy(buffer.data(), reinterpret_cast<const void*>(src + done), count);
        }
        if (dst_physical || destination_page) {
            const auto physical =
                dst_physical ? dst + done - canonical
                             : destination_page->physical + dst + done - destination_page->address;
            if (!CopyPhysical(physical, buffer.data(), count, true))
                return false;
        } else {
            std::memcpy(reinterpret_cast<void*>(dst + done), buffer.data(), count);
        }
        done += count;
    }
    return true;
}

std::vector<uint64_t> GuestMemory::Acquire(uintptr_t address, uint64_t bytes, bool populate) {
    if (!ValidRange(address, bytes)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        throw std::invalid_argument("invalid guest lease range");
    }
    std::shared_lock copy_lock{copy_mutex};
    std::lock_guard lock{mutex};
    std::vector<uint64_t> windows;
    for (auto at = address & ~(PageSize - 1); at < address + bytes; at += PageSize) {
        auto* page = Find(at);
        if (!page && at - canonical < size)
            page = &Add(at, at - canonical, true);
        if (page)
            windows.push_back(Logical(page->physical) / WindowSize);
    }
    std::sort(windows.begin(), windows.end());
    windows.erase(std::unique(windows.begin(), windows.end()), windows.end());
    std::vector<uint64_t> pins;
    pins.reserve(windows.size());
    try {
        for (size_t i = 0; i < windows.size();) {
            const auto first = windows[i];
            auto end = first + 1;
            while (++i < windows.size() && windows[i] == end)
                ++end;
            pins.push_back(regions.TempPin(first * WindowSize, (end - first) * WindowSize));
        }
        for (const auto window : windows)
            regions.Touch(window, *this);
        if (populate) {
            for (auto at = address & ~(PageSize - 1); at < address + bytes; at += PageSize) {
                if (auto* page = Find(at); page && !Ensure(*page))
                    throw std::runtime_error("guest lease population failed");
            }
        }
    } catch (...) {
        const auto error = GetLastError();
        for (const auto pin : pins)
            regions.TempUnpin(pin);
        SetLastError(error);
        throw;
    }
    return pins;
}

void GuestMemory::Release(const std::vector<uint64_t>& pins) {
    std::shared_lock copy_lock{copy_mutex};
    std::lock_guard lock{mutex};
    ++release_calls;
    for (const auto pin : pins) {
        uint64_t first{}, end{};
        if (!regions.TempPinRange(pin, first, end))
            continue;
        regions.TempUnpin(pin);
        for (auto window = first; window < end; ++window)
            ProcessPendingWindow(window);
    }
}

void GuestMemory::ProcessPendingWindow(uint64_t window) noexcept {
    if (!window || window - 1 >= discard_windows.size())
        return;
    auto* current = &discard_windows[window - 1];
    if (*current == UINT32_MAX)
        return;
    const bool pinned = regions.IsPinned(window);
    while (*current != UINT32_MAX) {
        ++discard_visits;
        auto& state = physical_pages[*current];
        bool discarded = !state.discard_pending;
        if (!discarded && !pinned) {
            try {
                discarded = DiscardPage(uint64_t{*current} * PageSize);
            } catch (...) {
                ++store.counters.errors;
            }
        }
        if (discarded) {
            *current = state.next_discard;
            state.next_discard = UINT32_MAX;
            state.discard_queued = false;
            --pending_discard_count;
        } else {
            current = &state.next_discard;
        }
    }
}

void GuestMemory::QueueDiscard(uint64_t physical) {
    auto& state = physical_pages[physical / PageSize];
    state.discard_pending = true;
    if (state.discard_queued)
        return;
    auto& head = discard_windows[physical / WindowSize];
    state.next_discard = head;
    state.discard_queued = true;
    ++pending_discard_count;
    head = static_cast<uint32_t>(physical / PageSize);
}

bool GuestMemory::DiscardPage(uint64_t physical) {
    auto& state = physical_pages[physical / PageSize];
    const auto window = Logical(physical) / WindowSize;
    if (regions.IsPinned(window))
        return false;
    for (auto* page = state.first; page; page = page->next) {
        if (!page->canonical)
            return false;
    }
    const auto first = physical & ~(WindowSize - 1);
    bool empty = true;
    for (auto at = first; at < first + WindowSize; at += PageSize) {
        empty &= at == physical || physical_pages[at / PageSize].discarded;
        for (auto* page = physical_pages[at / PageSize].first; page; page = page->next)
            empty &= page->canonical;
    }
    if (empty) {
        if (!regions.DiscardWindow(window, *this))
            return false;
    } else if ((state.owner && state.owner->resident) || regions.IsEvicted(Logical(physical))) {
        std::array<uint8_t, PageSize> zeros{};
        if (!CopyPhysical(physical, zeros.data(), PageSize, true))
            return false;
    }
    state.discard_pending = false;
    state.discarded = true;
    return true;
}

bool GuestMemory::Discard(uint64_t physical, uint64_t bytes) {
    if (physical % PageSize || bytes % PageSize || physical > size || bytes > size - physical) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    std::unique_lock copy_lock{copy_mutex};
    std::lock_guard lock{mutex};
    for (auto at = physical; at < physical + bytes; at += PageSize) {
        auto& state = physical_pages[at / PageSize];
        if (state.discard_pending)
            continue;
        state.discard_pending = true;
        if (!DiscardPage(at))
            QueueDiscard(at);
    }
    return true;
}

bool GuestMemory::Fault(uintptr_t address, int access, bool& failed) {
    failed = false;
    {
        std::lock_guard lock{mutex};
        auto* page = Find(address);
        const auto physical = page ? page->physical : address - canonical;
        if (physical >= size)
            return false;
        if (!regions.IsEvicted(Logical(physical))) {
            const auto result = FaultLocked(address, access, failed);
            if (result || !failed || !IsResourceFailure(GetLastError()))
                return result;
        }
    }
    return QueueFault(address, access, failed);
}

bool GuestMemory::QueueFault(uintptr_t address, int access, bool& failed) {
    MemoryDiagnostics::Scope diagnostic{"fault_queue", address, static_cast<uint64_t>(access)};
    FaultRequest request{address, access};
    std::unique_lock lock{fault_mutex};
    if (!accepting_faults) {
        lock.unlock();
        std::lock_guard memory_lock{mutex};
        return FaultLocked(address, access, failed);
    }
    if (fault_last)
        fault_last->next = &request;
    else
        fault_first = &request;
    fault_last = &request;
    fault_wake.notify_one();
    diagnostic.Stage("wait_fault_worker");
    fault_done.wait(lock, [&request] { return request.done; });
    failed = request.failed;
    SetLastError(request.error);
    return request.result;
}

void GuestMemory::FaultLoop(std::stop_token stop) {
    std::unique_lock lock{fault_mutex};
    for (;;) {
        fault_wake.wait(lock, [&] { return fault_first || stop.stop_requested(); });
        if (!fault_first && stop.stop_requested())
            break;
        auto** next = &fault_first;
        FaultRequest* previous{};
        const auto now = GetTickCount64();
        while (*next && !stop.stop_requested() && (*next)->retry_at > now) {
            previous = *next;
            next = &(*next)->next;
        }
        if (!*next) {
            fault_wake.wait_for(lock, std::chrono::milliseconds(100));
            continue;
        }
        auto* request = *next;
        *next = request->next;
        if (fault_last == request)
            fault_last = previous;
        lock.unlock();
        MemoryDiagnostics::Scope diagnostic{"fault_worker", request->address,
                                            static_cast<uint64_t>(request->access)};
        request->failed = false;
        request->result = false;
        try {
            diagnostic.Stage("wait_memory_mutex");
            std::lock_guard memory_lock{mutex};
            diagnostic.Stage("resolve_fault");
            request->result = FaultLocked(request->address, request->access, request->failed);
            request->error = GetLastError();
        } catch (const std::bad_alloc&) {
            request->failed = true;
            request->error = ERROR_NOT_ENOUGH_MEMORY;
        } catch (...) {
            request->failed = true;
            request->error = GetLastError();
            if (!request->error)
                request->error = ERROR_INVALID_DATA;
        }
        bool retry{};
        if (request->failed && !stop.stop_requested() && IsResourceFailure(request->error)) {
            std::lock_guard memory_lock{mutex};
            const auto* page = Find(request->address);
            const auto physical = page ? page->physical : request->address - canonical;
            retry = physical < size;
            if (retry)
                ++fault_retries;
        }
        lock.lock();
        if (retry) {
            request->next = nullptr;
            request->retry_at = GetTickCount64() + 100;
            if (fault_last)
                fault_last->next = request;
            else
                fault_first = request;
            fault_last = request;
            diagnostic.Stage("retry_resource_pressure");
            continue;
        }
        request->done = true;
        fault_done.notify_all();
    }
}

void GuestMemory::UpdatePressure() {
    uint64_t usage{};
    uint64_t limit{};
    try {
        limit = winrt::Windows::System::MemoryManager::AppMemoryUsageLimit();
        usage = winrt::Windows::System::MemoryManager::AppMemoryUsage();
    } catch (const winrt::hresult_error&) {
    }
    std::lock_guard lock{mutex};
    app_usage = usage;
    app_limit = limit;
    app_target = limit - std::min(limit / 16, uint64_t{220} << 20);
    if (!budget_initialized) {
        const auto* views = std::getenv("SHADPS4_UWP_GUEST_VIEW_LIMIT");
        const auto configured_views = views ? std::strtoull(views, nullptr, 10) : 0;
        if (configured_views >= 16 && configured_views <= 100000)
            view_limit = configured_views;
        const auto* setting = std::getenv("SHADPS4_UWP_GUEST_RAM_MB");
        const auto megabytes = setting ? std::strtoull(setting, nullptr, 10) : 0;
        if (megabytes >= 64 && megabytes <= 32768)
            configured_budget = megabytes << 20;
        budget_initialized = true;
    }
    budget = configured_budget;
    if (app_target) {
        const auto resident = Resident();
        const auto other = usage - std::min(usage, resident);
        const auto target = app_target - std::min(app_target, other);
        budget = configured_budget ? std::min(configured_budget, target) : target;
    }
}

uint64_t GuestMemory::Reclaim(uint64_t bytes, bool urgent) {
    MemoryDiagnostics::Scope diagnostic{"reclaim", bytes, urgent};
    diagnostic.Stage("wait_copy_mutex");
    std::shared_lock copy_lock{copy_mutex};
    diagnostic.Stage("wait_memory_mutex");
    std::lock_guard lock{mutex};
    diagnostic.Stage(urgent ? "make_room" : "sweep");
    if (!store.IsOpen())
        return 0;
    if (urgent)
        return regions.MakeRoom((bytes + WindowSize - 1) / WindowSize, *this) * WindowSize;
    const auto now = GetTickCount64();
    size_t visited{};
    regions.ForEachFullWindow(hand, [&](uint64_t window) {
        hand = window + 1;
        if (regions.SweepIfCold(window, *this)) {
            swept.push_back({window, now});
            ++store.counters.swept;
        }
        return ++visited >= 2048;
    });
    if (visited < 2048)
        hand = 0;
    return 0;
}

void GuestMemory::PagerLoop(std::stop_token stop) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    std::array<uint8_t, WindowSize> copy{};
    std::array<uint8_t, WindowSize> verify{};
    auto last_report = GetTickCount64();
    while (!stop.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        try {
            MemoryDiagnostics::Scope diagnostic{"pager"};
            diagnostic.Stage("update_pressure");
            UpdatePressure();
            if (GetTickCount64() - last_report >= 5000) {
                {
                    std::lock_guard lock{mutex};
                    char message[512]{};
                    peak_views = std::max(peak_views, view_count);
                    std::snprintf(message, sizeof(message),
                                  "[MappingDiag] interval_ms=%llu views=%llu delta_views=%lld "
                                  "peak_sampled_views=%llu created_delta=%llu reused_delta=%llu "
                                  "closed_total=%llu failures_total=%llu",
                                  GetTickCount64() - last_report, view_count,
                                  static_cast<long long>(view_count) - static_cast<long long>(previous_views),
                                  peak_views, sections_created - previous_created,
                                  sections_reused - previous_reused, sections_closed, failure_count);
                    previous_views = view_count;
                    previous_created = sections_created;
                    previous_reused = sections_reused;
                    uwp_memory_log_diagnostic(message);
                }
                uwp_memory_report();
                last_report = GetTickCount64();
            }
            bool pressure{};
            {
                std::lock_guard lock{mutex};
                pressure = (configured_budget || app_target) && Resident() > budget;
            }
            if (pressure)
                Reclaim(0, false);
            PruneViews();
            for (size_t batch = 0; batch < 128 && !stop.stop_requested(); ++batch) {
                diagnostic.Stage("wait_copy_mutex");
                std::shared_lock copy_lock{copy_mutex};
                diagnostic.Stage("wait_memory_mutex");
                std::unique_lock lock{mutex};
                if (swept.empty() || GetTickCount64() - swept.front().since < 1000)
                    break;
                const auto window = swept.front().window;
                swept.pop_front();
                pressure = pressure && Resident() > budget;
                if (!pressure) {
                    regions.Touch(window, *this);
                    continue;
                }
                if (!regions.BeginEvict(window, *this))
                    continue;
                uint32_t slot{UINT32_MAX};
                auto cancel = ::detail::MakeScopeGuard([&] {
                    std::lock_guard memory_lock{mutex};
                    if (slot != UINT32_MAX)
                        store.Free(slot);
                    regions.Touch(window, *this);
                    ++store.counters.aborted;
                });
                const auto physical = Physical(window * WindowSize);
                diagnostic.Stage("read_window");
                if (!ReadWindow(physical, copy.data())) {
                    ++store.counters.errors;
                    continue;
                }
                slot = store.Alloc();
                const auto checksum = Xbox::PagerStore::Checksum(copy.data(), WindowSize);
                lock.unlock();
                diagnostic.Stage("store_write");
                const bool written = store.Write(slot, copy.data(), WindowSize);
                if (written)
                    store.SetChecksum(slot, checksum ^ (window * 0x9E3779B97F4A7C15ULL));
                diagnostic.Stage("wait_memory_after_write");
                lock.lock();
                diagnostic.Stage("verify_commit_evict");
                bool evicted{};
                if (written && (regions.FlagsOf(window * WindowSize) & Xbox::Lazy::kEvicting) &&
                    !regions.IsPinned(window)) {
                    if (ReadWindow(physical, verify.data()) && copy == verify)
                        evicted = regions.CommitEvict(window, slot, *this);
                    else
                        ++store.counters.torn;
                }
                if (evicted) {
                    cancel.Cancel();
                    ++store.counters.evicted;
                }
            }
        } catch (...) {
            ++store.counters.errors;
        }
    }
    winrt::uninit_apartment();
}

void GuestMemory::Start() {
    std::lock_guard lock{fault_mutex};
    if (fault_thread.joinable() || !store.IsOpen())
        return;
    accepting_faults = true;
    fault_thread = std::jthread([this](std::stop_token stop) { FaultLoop(stop); });
    try {
        pager_thread = std::jthread([this](std::stop_token stop) { PagerLoop(stop); });
        diagnostic_thread = std::jthread([](std::stop_token stop) {
            auto previous = GetTickCount64();
            while (!stop.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (GetTickCount64() - previous >= 5000) {
                    uwp_memory_diagnostics();
                    previous = GetTickCount64();
                }
            }
        });
    } catch (...) {
        accepting_faults = false;
        fault_thread.request_stop();
        fault_wake.notify_all();
        throw;
    }
}

void GuestMemory::Stop() {
    if (diagnostic_thread.joinable()) {
        diagnostic_thread.request_stop();
        diagnostic_thread.join();
    }
    if (pager_thread.joinable()) {
        pager_thread.request_stop();
        pager_thread.join();
    }
    {
        std::lock_guard lock{fault_mutex};
        accepting_faults = false;
        if (fault_thread.joinable())
            fault_thread.request_stop();
        fault_wake.notify_all();
    }
    if (fault_thread.joinable())
        fault_thread.join();
    std::lock_guard lock{mutex};
    regions.ForEachFullWindow(0, [&](uint64_t window) {
        regions.Touch(window, *this);
        return false;
    });
}

void GuestMemory::Report(void (*log)(const char*)) {
    if (!log)
        return;
    char message[2048]{};
    {
        std::lock_guard lock{mutex};
        std::snprintf(message, sizeof(message),
                      "[KytyMemory] resident_mb=%llu views=%llu view_limit=%llu virtual_pages=%llu "
                      "swapped_windows=%llu slots=%llu swept=%llu evicted=%llu "
                      "page_ins=%llu errors=%llu corrupt=%llu aborted=%llu "
                      "privatized=%llu app_mb=%llu limit_mb=%llu budget_mb=%llu target_mb=%llu operation=%s "
                      "address=0x%llx protection=0x%lx pins=%llu protected_windows=%llu "
                      "pending_discards=%llu lease_releases=%llu discard_visits=%llu "
                      "sections_created=%llu sections_reused=%llu sections_cached=%llu "
                      "sections_live=%llu scratch_maps=%llu scratch_reuses=%llu "
                      "views_pruned=%llu fault_retries=%llu failure_api=%s failure_error=%lu "
                      "failure_address=0x%llx failure_section=%p",
                      Resident() >> 20, view_count, view_limit, static_cast<uint64_t>(pages.size()),
                      static_cast<uint64_t>(regions.EvictedCount()), store.SlotsInUse(),
                      store.counters.swept.load(), store.counters.evicted.load(),
                      store.counters.page_ins.load(), store.counters.errors.load(),
                      store.counters.corrupt.load(), store.counters.aborted.load(),
                      store.counters.privatized.load(), app_usage >> 20, app_limit >> 20,
                      budget >> 20, app_target >> 20, last_operation, static_cast<uint64_t>(last_address),
                      last_protection, static_cast<uint64_t>(regions.TemporaryPinCount()),
                      static_cast<uint64_t>(protected_windows.size()), pending_discard_count,
                      release_calls, discard_visits, sections_created, sections_reused,
                      static_cast<uint64_t>(free_sections.size()), sections_live, scratch_maps,
                      scratch_reuses, views_pruned, fault_retries,
                      failure_api, failure_error, failure_address, failure_section);
    }
    log(message);
}

} // namespace Core::Uwp
