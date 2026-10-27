// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <span>
#include <thread>
#include <vector>
#include <fmt/format.h>
#include "common/alignment.h"
#include "common/arch.h"
#include "common/assert.h"
#include "common/elf_info.h"
#include "common/error.h"
#ifdef SHADPS4_UWP
#include "core/libraries/kernel/threads/pthread.h"
#endif
#include "common/path_util.h"
#include "common/scope_exit.h"
#include "core/address_space.h"
#include "core/emulator_settings.h"
#include "core/libraries/kernel/memory.h"
#include "core/memory.h"
#include "libraries/error_codes.h"

#ifdef _WIN32
#include <windows.h>
#ifdef SHADPS4_UWP
#include <winrt/Windows.System.h>
#endif
#else
#include <fcntl.h>
#include <sys/mman.h>
#endif

#if defined(__APPLE__) && defined(ARCH_X86_64)
// Reserve space for the system address space using a zerofill section.
asm(".zerofill SYSTEM_MANAGED,SYSTEM_MANAGED,__SYSTEM_MANAGED,0x7FFBFC000");
asm(".zerofill SYSTEM_RESERVED,SYSTEM_RESERVED,__SYSTEM_RESERVED,0x7C0004000");
asm(".zerofill USER_AREA,USER_AREA,__USER_AREA,0x5F9000000000");
#endif

namespace Core {

// Constants used for mapping address space.
constexpr VAddr SYSTEM_MANAGED_MIN = 0x400000ULL;
constexpr VAddr SYSTEM_MANAGED_MAX = 0x7FFFFBFFFULL;
constexpr VAddr SYSTEM_RESERVED_MIN = 0x7FFFFC000ULL;
#if defined(__APPLE__) && defined(ARCH_X86_64)
// Commpage ranges from 0xFC0000000 - 0xFFFFFFFFF, so decrease the system reserved maximum.
constexpr VAddr SYSTEM_RESERVED_MAX = 0xFBFFFFFFFULL;
// GPU-reserved memory ranges from 0x1000000000 - 0x6FFFFFFFFF, so increase the user minimum.
constexpr VAddr USER_MIN = 0x7000000000ULL;
#else
constexpr VAddr SYSTEM_RESERVED_MAX = 0xFFFFFFFFFULL;
constexpr VAddr USER_MIN = 0x1000000000ULL;
#endif
#if defined(__linux__)
// Linux maps the shadPS4 executable around here, so limit the user maximum
constexpr VAddr USER_MAX = 0x54FFFFFFFFFFULL;
#elif defined(__FreeBSD__)
// FreeBSD address space is extremely volatile, keep this lower for safety.
constexpr VAddr USER_MAX = 0xFFFFFFFFFFFULL;
#else
constexpr VAddr USER_MAX = 0x5FFFFFFFFFFFULL;
#endif

// Constants for the sizes of the ranges in address space.
static constexpr u64 SystemManagedSize = SYSTEM_MANAGED_MAX - SYSTEM_MANAGED_MIN + 1;
static constexpr u64 SystemReservedSize = SYSTEM_RESERVED_MAX - SYSTEM_RESERVED_MIN + 1;
static constexpr u64 UserSize = USER_MAX - USER_MIN + 1;

// Required backing file size for mapping physical address space.
static u64 BackingSize = ORBIS_KERNEL_TOTAL_MEM_DEV_PRO + ORBIS_KERNEL_FLEXIBLE_MEMORY_SIZE;

#ifdef _WIN32

[[nodiscard]] constexpr u64 ToWindowsProt(Core::MemoryProt prot) {
    const bool read =
        True(prot & Core::MemoryProt::CpuRead) || True(prot & Core::MemoryProt::GpuRead);
    const bool write =
        True(prot & Core::MemoryProt::CpuWrite) || True(prot & Core::MemoryProt::GpuWrite);
    const bool execute = True(prot & Core::MemoryProt::CpuExec);

    if (write && !read) {
        // While write-only CPU mappings aren't possible, write-only GPU mappings are.
        LOG_WARNING(Core, "Converting write-only mapping to read-write");
    }

    // All cases involving execute permissions have separate permissions.
    if (execute) {
        if (write) {
            return PAGE_EXECUTE_READWRITE;
        } else if (read && !write) {
            return PAGE_EXECUTE_READ;
        } else {
            return PAGE_EXECUTE;
        }
    } else {
        if (write) {
            return PAGE_READWRITE;
        } else if (read && !write) {
            return PAGE_READONLY;
        } else {
            return PAGE_NOACCESS;
        }
    }
}

static void* MapHostView(HANDLE backing, HANDLE process, void* address, u64 offset, u64 size,
                         ULONG flags, ULONG protection, MEM_EXTENDED_PARAMETER* parameters,
                         ULONG count) {
#ifdef SHADPS4_UWP
    if (protection != PAGE_EXECUTE && protection != PAGE_EXECUTE_READ &&
        protection != PAGE_EXECUTE_READWRITE) {
        static std::atomic_bool native_data_views{};
        if (native_data_views.load(std::memory_order_relaxed)) {
            auto* native = MapViewOfFile3(backing, process, address, offset, size, flags,
                                          protection, parameters, count);
            if (native)
                return native;
            const auto error = GetLastError();
            auto* app = MapViewOfFile3FromApp(backing, process, address, offset, size, flags,
                                              protection, parameters, count);
            if (app) {
                native_data_views.store(false, std::memory_order_relaxed);
                return app;
            }
            SetLastError(error);
            return nullptr;
        }
        auto* result = MapViewOfFile3FromApp(backing, process, address, offset, size, flags,
                                             protection, parameters, count);
        if (result)
            return result;
        const auto error = GetLastError();
        if (error == ERROR_NOT_ENOUGH_MEMORY || error == ERROR_OUTOFMEMORY ||
            error == ERROR_NO_SYSTEM_RESOURCES || error == ERROR_COMMITMENT_LIMIT ||
            error == ERROR_NOT_ENOUGH_QUOTA) {
            result = MapViewOfFile3(backing, process, address, offset, size, flags, protection,
                                    parameters, count);
            if (result) {
                native_data_views.store(true, std::memory_order_relaxed);
                LOG_WARNING(Core,
                            "[MemDiag] native_map_recovered from_app_win32={} va={:#x} size={:#x} "
                            "offset={:#x} protection={:#x}",
                            error, reinterpret_cast<VAddr>(address), size, offset, protection);
                return result;
            }
            const auto native_error = GetLastError();
            static std::atomic<u64> failures{};
            const auto failure = ++failures;
            if (failure <= 8 || failure % 64 == 0)
                LOG_ERROR(Core,
                          "[MemDiag] native_map_retry_failed from_app_win32={} native_win32={} "
                          "va={:#x} size={:#x}",
                          error, native_error, reinterpret_cast<VAddr>(address), size);
            SetLastError(error);
            return nullptr;
        }
        SetLastError(error);
        return nullptr;
    }
#endif
    return MapViewOfFile3(backing, process, address, offset, size, flags, protection, parameters,
                          count);
}

struct MemoryRegion {
    VAddr base;
    PAddr phys_base;
    u64 size;
    u32 prot;
    s32 fd;
    bool is_mapped;
    bool is_resident{};
    std::vector<MEMORY_BASIC_INFORMATION> saved_protections;
    u64 residency_order{};
    bool swept{};
};

#ifdef SHADPS4_UWP
class MemoryCriticalScope {
public:
    MemoryCriticalScope() : thread{Libraries::Kernel::g_curthread} {
        if (thread)
            thread->critical_count.fetch_add(1, std::memory_order_acq_rel);
    }
    ~MemoryCriticalScope() {
        if (thread)
            thread->critical_count.fetch_sub(1, std::memory_order_acq_rel);
    }

private:
    Libraries::Kernel::Pthread* thread;
};
#endif

struct AddressSpace::Impl {
#ifdef SHADPS4_UWP
    struct BackingView {
        u64 size;
        bool is_resident{};
        u64 last_access{};
        bool swept{};
    };
#endif
    Impl() : process{GetCurrentProcess()} {
#ifdef SHADPS4_UWP
        std::vector<VAddr> reserved_regions;
        auto rollback = ::detail::MakeScopeGuard([&] {
            if (backing_base) {
                VirtualFreeEx(process, backing_base, BackingSize,
                              MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
                VirtualFreeEx(process, backing_base, 0, MEM_RELEASE);
            }
            if (backing_handle)
                CloseHandle(backing_handle);
            for (const auto address : reserved_regions)
                VirtualFreeEx(process, reinterpret_cast<void*>(address), 0, MEM_RELEASE);
        });
#endif
        // Determine the system's page alignment
        SYSTEM_INFO sys_info{};
        GetSystemInfo(&sys_info);
#ifdef SHADPS4_UWP
        commit_size = sys_info.dwPageSize;
#endif
        u64 alignment = sys_info.dwAllocationGranularity;

        // Older Windows builds have a severe performance issue with VirtualAlloc2.
        // We need to get the host's Windows version, then determine if it needs a workaround.
        auto ntdll_handle = GetModuleHandleW(L"ntdll.dll");
        ASSERT_MSG(ntdll_handle, "Failed to retrieve ntdll handle");

        // Get the RtlGetVersion function
        s64(WINAPI * RtlGetVersion)(LPOSVERSIONINFOW);
        *(FARPROC*)&RtlGetVersion = GetProcAddress(ntdll_handle, "RtlGetVersion");
        ASSERT_MSG(RtlGetVersion, "failed to retrieve function pointer for RtlGetVersion");

        // Call RtlGetVersion
        RTL_OSVERSIONINFOW os_version_info{};
        RtlGetVersion(&os_version_info);

        u64 supported_user_max = USER_MAX;
        // This is the build number for Windows 11 22H2
        static constexpr s32 AffectedBuildNumber = 22621;

        // Higher PS4 firmware versions prevent higher address mappings too.
        s32 sdk_ver = Common::ElfInfo::Instance().CompiledSdkVer();
        if (os_version_info.dwBuildNumber <= AffectedBuildNumber ||
            sdk_ver >= Common::ElfInfo::FW_300) {
            supported_user_max = 0x10000000000ULL;
            // Only log the message if we're restricting the user max due to operating system.
            // Since higher compiled SDK versions also get reduced max, we don't need to log there.
            if (sdk_ver < Common::ElfInfo::FW_300) {
                LOG_WARNING(
                    Core,
                    "Older Windows version detected, reducing user max to {:#x} to avoid problems",
                    supported_user_max);
            }
        }

        // Determine the free address ranges we can access.
        VAddr next_addr = SYSTEM_MANAGED_MIN;
        MEMORY_BASIC_INFORMATION info{};
        while (next_addr <= supported_user_max) {
            ASSERT_MSG(VirtualQuery(reinterpret_cast<PVOID>(next_addr), &info, sizeof(info)),
                       "Failed to query memory information for address {:#x}", next_addr);

            // Ensure logic uses values aligned to bage boundaries.
            next_addr = reinterpret_cast<VAddr>(info.BaseAddress) + info.RegionSize;
            next_addr = Common::AlignUp(next_addr, alignment);

            // Prevent size from going past supported_user_max
            u64 size = info.RegionSize;
            if (next_addr > supported_user_max) {
                size -= (next_addr - supported_user_max);
            }
            size = Common::AlignDown(size, alignment);

            // Check for free memory areas
            // Restrict region size to avoid overly fragmenting the virtual memory space.
            if (info.State == MEM_FREE && info.RegionSize > 0x1000000) {
                VAddr addr = Common::AlignUp(reinterpret_cast<VAddr>(info.BaseAddress), alignment);
                regions.emplace(addr,
                                MemoryRegion{addr, PAddr(-1), size, PAGE_NOACCESS, -1, false});
            }
        }

        // Reserve all detected free regions.
#ifdef SHADPS4_UWP
        reserved_regions.reserve(regions.size());
#endif
        for (auto region : regions) {
            auto addr = static_cast<u8*>(VirtualAlloc2(
                process, reinterpret_cast<PVOID>(region.second.base), region.second.size,
                MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0));
            // All marked regions should reserve fine since they're free.
            ASSERT_MSG(addr, "Unable to reserve virtual address space: {}",
                       Common::GetLastErrorMsg());
#ifdef SHADPS4_UWP
            reserved_regions.push_back(region.second.base);
#endif
        }

        // Set these constants to ensure code relying on them works.
        // These do not fully encapsulate the state of the address space.
        system_managed_base = reinterpret_cast<u8*>(regions.begin()->first);
        system_managed_size = SystemManagedSize - (regions.begin()->first - SYSTEM_MANAGED_MIN);
        system_reserved_base = reinterpret_cast<u8*>(SYSTEM_RESERVED_MIN);
        system_reserved_size = SystemReservedSize;
        user_base = reinterpret_cast<u8*>(USER_MIN);
        user_size = supported_user_max - USER_MIN - 1;

        // Increase BackingSize to account for config options.
        BackingSize += EmulatorSettings.GetExtraDmemInMBytes() * 1_MB +
                       EmulatorSettings.GetExtraFmemInMBytes() * 1_MB;

        // Allocate backing file that represents the total physical memory.
#ifndef SHADPS4_UWP
        backing_handle = CreateFileMapping2(INVALID_HANDLE_VALUE, nullptr, FILE_MAP_ALL_ACCESS,
                                            PAGE_EXECUTE_READWRITE, SEC_COMMIT, BackingSize,
                                            nullptr, nullptr, 0);
#endif

#ifndef SHADPS4_UWP
        ASSERT_MSG(backing_handle, "{}", Common::GetLastErrorMsg());
#endif
        // Allocate a virtual memory for the backing file map as placeholder
        backing_base = static_cast<u8*>(VirtualAlloc2(process, nullptr, BackingSize,
                                                      MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                                      PAGE_NOACCESS, nullptr, 0));
        ASSERT_MSG(backing_base, "{}", Common::GetLastErrorMsg());

#ifdef SHADPS4_UWP
        ram_pages.resize((BackingSize + BackingViewSize - 1) / BackingViewSize);
        swap_files.resize(ram_pages.size(), INVALID_HANDLE_VALUE);
        section_accesses.resize(ram_pages.size());
        swap_enabled = EmulatorSettings.IsMemorySwapEnabled();
        if (swap_enabled) {
            swap_directory = Common::FS::GetUserPath(Common::FS::PathType::CacheDir) / "swap";
            std::filesystem::create_directories(swap_directory);
        }
        for (u64 offset = 0; offset + BackingViewSize < BackingSize; offset += BackingViewSize) {
            ASSERT_MSG(VirtualFreeEx(process, backing_base + offset, BackingViewSize,
                                     MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER),
                       "Unable to split physical backing placeholder: {}",
                       Common::GetLastErrorMsg());
        }
        for (u64 offset = 0; offset < BackingSize; offset += BackingViewSize)
            backing_views.emplace(offset,
                                  BackingView{std::min(BackingViewSize, BackingSize - offset)});
        active_backing.store(this);
        rollback.Cancel();
        LOG_INFO(Core, "UWP memory: {} MiB reserved, {} KiB minimum demand commitment, swap {}",
                 BackingSize / 1_MB, commit_size / 1_KB, swap_enabled ? "enabled" : "disabled");
        StartPager();
#else
        void* const ret =
            MapViewOfFile3(backing_handle, process, backing_base, 0, BackingSize,
                           MEM_REPLACE_PLACEHOLDER, PAGE_EXECUTE_READWRITE, nullptr, 0);
        ASSERT_MSG(ret == backing_base, "{}", Common::GetLastErrorMsg());
#endif
    }

#ifdef SHADPS4_UWP
    static constexpr u64 HostMemoryReserve = 256_MB;
    static constexpr u64 BackingViewSize = 16_MB;
    static constexpr u64 GuestViewSize = 1_MB;
    static constexpr u64 MinimumGuestViewSize = 64_KB;
    u64 commit_size{};
    inline static std::atomic<Impl*> active_backing{};
    std::mutex backing_mutex;
    std::map<u64, BackingView> backing_views;
    std::vector<HANDLE> ram_pages;
    std::vector<HANDLE> swap_files;
    std::vector<u64> section_accesses;
    u64 section_access_clock{};
    std::mutex ram_pages_mutex;
    bool swap_enabled{};
    std::filesystem::path swap_directory;
    u64 committed_bytes{};
    struct CommitSequence {
        u64 end{~u64{0}};
        u32 length{};
    };
    std::map<DWORD, CommitSequence> commit_sequences;
    std::map<std::pair<VAddr, u64>, u32> pinned_ranges;
    std::map<std::pair<PAddr, u64>, u32> pinned_physical;
    u64 pager_budget{};
    u64 pager_evictions{};
    u64 pager_page_ins{};
    u64 pager_emergencies{};
    u64 pager_store_copies{};
    u64 pager_second_chances{};
    u64 pager_last_emergency{};
    u64 preferred_view_size{GuestViewSize};
    u64 mapping_failures{};
    u64 recovery_reports{};
    std::atomic<u64> pager_ticks{};
    std::atomic<u64> pager_lock_skips{};
    std::atomic<u64> pager_budget_errors{};
    std::mutex pager_wait_mutex;
    std::condition_variable_any pager_wait;
    std::jthread pager;

    void MemorySnapshot(const char* reason) noexcept {
        const auto saved_error = GetLastError();
        try {
            u64 guest_views{}, guest_bytes{}, backing_count{}, backing_bytes{}, pinned_views{};
            u64 executable_views{}, swept_views{}, sections{}, files{};
            for (const auto& [base, region] : regions) {
                if (!region.is_resident || region.fd != -1 || region.phys_base == PAddr(-1))
                    continue;
                ++guest_views;
                guest_bytes += region.size;
                pinned_views += IsPinnedRegion(region);
                executable_views += NeedsExecutableView(region.prot, region.saved_protections);
                swept_views += region.swept;
            }
            for (const auto& [offset, view] : backing_views)
                if (view.is_resident) {
                    ++backing_count;
                    backing_bytes += view.size;
                    swept_views += view.swept;
                }
            for (auto handle : ram_pages)
                sections += handle != nullptr;
            for (auto handle : swap_files)
                files += handle != INVALID_HANDLE_VALUE;
            LOG_INFO(Core,
                     "[MemDiag] {} guest_views={} guest_alias_bytes={:#x} backing_views={} "
                     "backing_bytes={:#x} "
                     "sections={} files={} region_nodes={} backing_nodes={} pinned_ranges={} "
                     "physical_pins={} pinned_views={} executable_views={} swept_views={}",
                     reason, guest_views, guest_bytes, backing_count, backing_bytes, sections,
                     files, regions.size(), backing_views.size(), pinned_ranges.size(),
                     pinned_physical.size(), pinned_views, executable_views, swept_views);
            LOG_INFO(Core,
                     "[MemDiag] {} tick={} lock_skips={} budget_errors={} failures={} "
                     "budget={:#x} evictions={} emergency={} second_chances={} "
                     "store_copies={} last_emergency_ms={}",
                     reason, pager_ticks.load(), pager_lock_skips.load(),
                     pager_budget_errors.load(), mapping_failures, pager_budget, pager_evictions,
                     pager_emergencies, pager_second_chances, pager_store_copies,
                     pager_last_emergency);
            u64 usage{}, limit{};
            s32 budget_error{};
            try {
                usage = winrt::Windows::System::MemoryManager::AppMemoryUsage();
                limit = winrt::Windows::System::MemoryManager::AppMemoryUsageLimit();
            } catch (const winrt::hresult_error& error) {
                budget_error = error.code().value;
            }
            MEMORYSTATUSEX system{sizeof(system)};
            const bool queried = GlobalMemoryStatusEx(&system);
            LOG_INFO(Core,
                     "[MemDiag] {} app_usage={:#x} app_limit={:#x} budget_hresult={:#x} "
                     "system_ok={} system_win32={} available_physical={:#x} total_physical={:#x} "
                     "available_pagefile={:#x} total_pagefile={:#x} available_virtual={:#x}",
                     reason, usage, limit, u32(budget_error), queried, queried ? 0 : GetLastError(),
                     system.ullAvailPhys, system.ullTotalPhys, system.ullAvailPageFile,
                     system.ullTotalPageFile, system.ullAvailVirtual);
        } catch (...) {
        }
        SetLastError(saved_error);
    }

    void MappingFailure(const char* operation, DWORD error, VAddr address, PAddr physical, u64 size,
                        DWORD protection, u64 offset = 0, HANDLE handle = nullptr,
                        DWORD flags = 0) noexcept {
        const auto failure = ++mapping_failures;
        try {
            if (failure <= 32 || failure % 64 == 0) {
                MEMORY_BASIC_INFORMATION target{};
                const auto queried =
                    VirtualQuery(reinterpret_cast<void*>(address), &target, sizeof(target));
                LOG_ERROR(Core,
                          "[MemDiag] failure={} operation={} win32={} tid={} va={:#x} phys={:#x} "
                          "size={:#x} protection={:#x} offset={:#x} handle={:#x} flags={:#x}",
                          failure, operation, error, GetCurrentThreadId(), address, physical, size,
                          protection, offset, reinterpret_cast<uintptr_t>(handle), flags);
                LOG_ERROR(Core,
                          "[MemDiag] failure={} query_ok={} state={:#x} "
                          "type={:#x} current_protection={:#x} allocation_protection={:#x} "
                          "allocation_base={:#x} region_base={:#x} region_size={:#x}",
                          failure, queried != 0, target.State, target.Type, target.Protect,
                          target.AllocationProtect, reinterpret_cast<VAddr>(target.AllocationBase),
                          reinterpret_cast<VAddr>(target.BaseAddress), target.RegionSize);
                if (failure <= 8 || failure % 64 == 0)
                    MemorySnapshot(operation);
            }
        } catch (...) {
        }
        SetLastError(error);
    }

    void RecoveryReport(const char* operation, u64 candidates, u64 pinned, u64 released,
                        u64 failures, DWORD error, u64 duration) noexcept {
        const auto saved_error = GetLastError();
        const auto report = ++recovery_reports;
        try {
            if (report <= 32 || report % 64 == 0)
                LOG_INFO(Core,
                         "[MemDiag] recovery={} operation={} candidates={} skipped_pinned={} "
                         "released={} io_failures={} first_win32={} duration_ms={}",
                         report, operation, candidates, pinned, released, failures, error,
                         duration);
        } catch (...) {
        }
        SetLastError(saved_error);
    }

    void StartPager() {
        if (swap_enabled && !pager.joinable())
            pager = std::jthread([this](std::stop_token stop) { RunPager(stop); });
    }

    void StopPager() {
        if (!pager.joinable())
            return;
        pager.request_stop();
        pager_wait.notify_all();
        pager.join();
        std::scoped_lock lock{mutex};
        MemorySnapshot("pager_stop");
        LOG_INFO(Core, "Memory pager: {} page-ins, {} evictions, {} emergency recoveries",
                 pager_page_ins, pager_evictions, pager_emergencies);
        LOG_INFO(Core, "Memory pager: {} direct store copies, {} second chances",
                 pager_store_copies, pager_second_chances);
    }

    bool IsPinnedPhysical(PAddr address, u64 size) const {
        for (const auto& [range, count] : pinned_physical)
            if (address < range.first + range.second && range.first < address + size)
                return true;
        for (const auto& [range, count] : pinned_ranges) {
            auto it = regions.upper_bound(range.first);
            if (it != regions.begin())
                --it;
            for (; it != regions.end() && it->first < range.first + range.second; ++it) {
                const auto& region = it->second;
                if (!region.is_mapped || region.fd != -1 || region.phys_base == PAddr(-1))
                    continue;
                const auto begin = std::max(region.base, range.first);
                const auto end = std::min(region.base + region.size, range.first + range.second);
                if (begin >= end)
                    continue;
                const auto physical = region.phys_base + begin - region.base;
                if (address < physical + end - begin && physical < address + size)
                    return true;
            }
        }
        return false;
    }

    void RunPager(std::stop_token stop) {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
        } catch (const winrt::hresult_error& error) {
            LOG_ERROR(Core, "Unable to initialize memory pager: {:#x}", u32(error.code().value));
            return;
        }
        auto apartment = ::detail::MakeScopeGuard([] { winrt::uninit_apartment(); });
        u64 last_report{};
        while (!stop.stop_requested()) {
            {
                std::unique_lock wait_lock{pager_wait_mutex};
                pager_wait.wait_for(wait_lock, stop, std::chrono::milliseconds(100),
                                    [] { return false; });
            }
            if (stop.stop_requested())
                break;
            ++pager_ticks;
            try {
                const u64 limit = winrt::Windows::System::MemoryManager::AppMemoryUsageLimit();
                const u64 usage = winrt::Windows::System::MemoryManager::AppMemoryUsage();
                std::unique_lock lock{mutex, std::try_to_lock};
                if (!lock) {
                    ++pager_lock_skips;
                    if (GetTickCount64() - last_report >= 5000) {
                        last_report = GetTickCount64();
                        LOG_INFO(Core, "[MemDiag] pager_lock_busy tick={} lock_skips={}",
                                 pager_ticks.load(), pager_lock_skips.load());
                    }
                    continue;
                }
                u64 resident{};
                u64 views{};
                std::vector<bool> pages((BackingSize + MinimumGuestViewSize - 1) /
                                        MinimumGuestViewSize);
                const auto count_pages = [&](u64 address, u64 size) {
                    const auto end =
                        (address + size + MinimumGuestViewSize - 1) / MinimumGuestViewSize;
                    for (auto page = address / MinimumGuestViewSize; page < end; ++page)
                        pages[page] = true;
                };
                for (const auto& [offset, view] : backing_views)
                    if (view.is_resident) {
                        count_pages(offset, view.size);
                        ++views;
                    }
                for (const auto& [base, region] : regions)
                    if (region.is_resident && region.fd == -1 && region.phys_base != PAddr(-1)) {
                        count_pages(region.phys_base, region.size);
                        ++views;
                    }
                resident = std::ranges::count(pages, true) * MinimumGuestViewSize;
                constexpr u64 headroom = 768_MB;
                const u64 other = usage > resident ? usage - resident : 0;
                const u64 available =
                    limit > other && limit - other > headroom ? limit - other - headroom : 0;
                const u64 target = limit ? std::max<u64>(512_MB, available) : 2_GB;
                if (!pager_budget || pager_budget > target)
                    pager_budget = target;
                else if (GetTickCount64() - pager_last_emergency >= 30000)
                    pager_budget = std::min(target, pager_budget + 8_MB / 10);
                const bool tight = limit && (usage >= limit || limit - usage < 384_MB);
                if (tight || resident > pager_budget || views > 6000)
                    TrimResidentViews(tight ? 64_MB : 8_MB, views > 6000);
                if (GetTickCount64() - last_report >= 5000) {
                    last_report = GetTickCount64();
                    LOG_INFO(Core,
                             "[MemDiag] pager_policy usage={:#x} limit={:#x} resident={:#x} "
                             "other={:#x} target={:#x} budget={:#x} views={} tight={}",
                             usage, limit, resident, other, target, pager_budget, views, tight);
                    MemorySnapshot("pager_sample");
                }
            } catch (const winrt::hresult_error& error) {
                const auto errors = ++pager_budget_errors;
                if (errors == 1 || GetTickCount64() - last_report >= 5000) {
                    last_report = GetTickCount64();
                    LOG_ERROR(Core, "[MemDiag] pager_budget_failed hresult={:#x} count={} tick={}",
                              u32(error.code().value), errors, pager_ticks.load());
                }
            } catch (const std::exception& error) {
                LOG_ERROR(Core, "Memory pager: {}", error.what());
            }
        }
    }

    void TrimResidentViews(u64 bytes, bool view_pressure) {
        const auto now = GetTickCount64();
        u64 released{};
        u64 scheduled{};
        for (auto& [base, region] : regions) {
            if (!region.is_resident || region.fd != -1 || region.phys_base == PAddr(-1) ||
                IsPinnedRegion(region) ||
                NeedsExecutableView(region.prot, region.saved_protections) ||
                (!view_pressure && now - region.residency_order < (region.swept ? 100 : 1000)))
                continue;
            const auto size = region.size;
            if (!view_pressure && !region.swept) {
                ApplyCommittedProtection(base, size, PAGE_NOACCESS);
                region.swept = true;
                region.residency_order = now;
            } else {
                UnmapRegion(&region);
                released += size;
                ++pager_evictions;
            }
            scheduled += size;
            if (scheduled >= bytes)
                break;
        }
        scheduled = 0;
        for (auto& [offset, view] : backing_views) {
            if (!view.is_resident || IsPinnedPhysical(offset, view.size) ||
                (!view_pressure && now - view.last_access < (view.swept ? 100 : 1000)))
                continue;
            if (!view_pressure && !view.swept) {
                ApplyCommittedProtection(reinterpret_cast<VAddr>(backing_base + offset), view.size,
                                         PAGE_NOACCESS);
                view.swept = true;
                view.last_access = now;
                scheduled += view.size;
                if (scheduled >= bytes)
                    break;
                continue;
            }
            if (!UnmapViewOfFile2(process, backing_base + offset, MEM_PRESERVE_PLACEHOLDER))
                continue;
            view.is_resident = false;
            view.swept = false;
            released += view.size;
            ++pager_evictions;
            if (released >= bytes)
                break;
        }
        std::vector<bool> active(ram_pages.size());
        for (const auto& [base, region] : regions)
            if (region.is_resident && region.fd == -1 && region.phys_base != PAddr(-1))
                active[region.phys_base / BackingViewSize] = true;
        for (const auto& [offset, view] : backing_views)
            if (view.is_resident)
                active[offset / BackingViewSize] = true;
        for (u64 index = 0; index < ram_pages.size(); ++index) {
            if (!ram_pages[index] || IsPinnedPhysical(index * BackingViewSize, BackingViewSize))
                continue;
            if (!active[index] && FlushFileBuffers(swap_files[index]) &&
                CloseHandle(ram_pages[index]))
                ram_pages[index] = nullptr;
        }
    }

    bool IsPinnedRegion(const MemoryRegion& region) const {
        return std::ranges::any_of(pinned_ranges, [&](const auto& entry) {
            const auto& [range, count] = entry;
            return region.base < range.first + range.second &&
                   range.first < region.base + region.size;
        });
    }

    bool ReleaseSwapSections(u64 requested_index) {
        if (!swap_enabled)
            return false;
        std::scoped_lock lock{backing_mutex, ram_pages_mutex};
        const auto started = GetTickCount64();
        u64 skipped_pinned{}, io_failures{};
        DWORD first_error{};
        const auto failed_io = [&] {
            if (!io_failures)
                first_error = GetLastError();
            ++io_failures;
        };
        std::vector<u64> candidates;
        for (u64 index = 0; index < ram_pages.size(); ++index) {
            if (index != requested_index && ram_pages[index])
                candidates.push_back(index);
        }
        std::ranges::sort(candidates, [&](u64 lhs, u64 rhs) {
            return section_accesses[lhs] < section_accesses[rhs];
        });
        u32 released{};
        for (const auto index : candidates) {
            if (released == 4)
                break;
            if (IsPinnedPhysical(index * BackingViewSize, BackingViewSize)) {
                ++skipped_pinned;
                continue;
            }
            const bool pinned = std::ranges::any_of(regions, [&](const auto& entry) {
                const auto& region = entry.second;
                return region.is_mapped && region.fd == -1 && region.phys_base != PAddr(-1) &&
                       region.phys_base / BackingViewSize == index &&
                       (IsPinnedRegion(region) ||
                        NeedsExecutableView(region.prot, region.saved_protections));
            });
            if (pinned) {
                ++skipped_pinned;
                continue;
            }
            bool flushed = true;
            for (auto& [base, region] : regions) {
                if (!region.is_resident || region.fd != -1 || region.phys_base == PAddr(-1) ||
                    region.phys_base / BackingViewSize != index)
                    continue;
                if (!FlushViewOfFile(reinterpret_cast<void*>(base), region.size)) {
                    failed_io();
                    flushed = false;
                    break;
                }
                UnmapRegion(&region);
            }
            if (!flushed)
                continue;
            for (auto& [offset, view] : backing_views) {
                if (offset / BackingViewSize != index || !view.is_resident)
                    continue;
                auto* base = backing_base + offset;
                if (!FlushViewOfFile(base, view.size) ||
                    !UnmapViewOfFile2(process, base, MEM_PRESERVE_PLACEHOLDER)) {
                    failed_io();
                    flushed = false;
                    break;
                }
                view.is_resident = false;
            }
            if (!flushed)
                continue;
            if (!FlushFileBuffers(swap_files[index])) {
                failed_io();
                continue;
            }
            if (CloseHandle(ram_pages[index])) {
                ram_pages[index] = nullptr;
                ++released;
            } else
                failed_io();
        }
        if (released)
            LOG_INFO(Core, "Released {} swap sections ({} MiB), retaining file contents", released,
                     released * BackingViewSize / 1_MB);
        pager_emergencies += released != 0;
        if (released)
            pager_last_emergency = GetTickCount64();
        if (released && pager_budget)
            pager_budget = std::max<u64>(512_MB, pager_budget > 256_MB ? pager_budget - 256_MB : 0);
        RecoveryReport("swap_sections", candidates.size(), skipped_pinned, released, io_failures,
                       first_error, GetTickCount64() - started);
        return released != 0;
    }

    bool ReleaseIdleGuestViews(const MemoryRegion* requested) {
        std::vector<MemoryRegion*> candidates;
        for (auto& [base, region] : regions) {
            if (&region == requested || !region.is_resident || region.fd != -1 ||
                region.phys_base == PAddr(-1) ||
                NeedsExecutableView(region.prot, region.saved_protections))
                continue;
            if (!IsPinnedRegion(region))
                candidates.push_back(&region);
        }
        std::ranges::sort(candidates, {}, &MemoryRegion::residency_order);
        const auto count = std::min<size_t>(16, candidates.size());
        u64 bytes{};
        for (size_t i = 0; i < count; ++i) {
            bytes += candidates[i]->size;
            UnmapRegion(candidates[i]);
        }
        if (count)
            LOG_INFO(Core, "Released {} idle RAM alias views ({} KiB) under mapping pressure",
                     count, bytes / 1_KB);
        RecoveryReport("guest_alias_views", candidates.size(), 0, count, 0, 0, 0);
        return count != 0;
    }

    void* PopulateGuestView(MemoryRegion* region) {
        auto* result = MapRegion(region, {}, true);
        if (!result && IsMappingResourceError(GetLastError()) && ReleaseIdleGuestViews(region))
            result = MapRegion(region, {}, true);
        if (!result) {
            const auto error = GetLastError();
            if (IsMappingResourceError(error) &&
                ReleaseSwapSections(region->phys_base / BackingViewSize))
                result = MapRegion(region, {}, true);
            else
                SetLastError(error);
        }
        if (!result && IsMappingResourceError(GetLastError()) &&
            region->size > MinimumGuestViewSize) {
            const auto error = GetLastError();
            if (preferred_view_size != MinimumGuestViewSize) {
                preferred_view_size = MinimumGuestViewSize;
                LOG_WARNING(Core, "[MemDiag] mapping_window_reduced size={:#x} win32={} va={:#x}",
                            preferred_view_size, error, region->base);
            }
            SetLastError(error);
            return nullptr;
        }
        if (!result && region->size <= MinimumGuestViewSize &&
            IsMappingResourceError(GetLastError())) {
            const auto started = GetTickCount64();
            for (u32 attempt = 0; attempt < 8 && GetTickCount64() - started < 250; ++attempt) {
                ReleaseIdleGuestViews(region);
                if (!(attempt % 2))
                    ReleaseSwapSections(region->phys_base / BackingViewSize);
                Sleep(std::min<DWORD>(16, 1u << attempt));
                result = MapRegion(region, {}, true);
                if (result) {
                    LOG_INFO(Core,
                             "[MemDiag] guest_map_recovered va={:#x} attempts={} elapsed_ms={}",
                             region->base, attempt + 1, GetTickCount64() - started);
                    break;
                }
                if (!IsMappingResourceError(GetLastError()))
                    break;
            }
        }
        return result;
    }

    std::map<u64, BackingView>::iterator SelectBackingView(u64 offset, u64 maximum_size) {
        auto it = std::prev(backing_views.upper_bound(offset));
        const auto begin = std::max(it->first, Common::AlignDown(offset, maximum_size));
        if (begin != it->first) {
            const auto prefix_size = begin - it->first;
            if (!VirtualFreeEx(process, backing_base + it->first, prefix_size,
                               MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
                return backing_views.end();
            const auto remaining_size = it->second.size - prefix_size;
            it->second.size = prefix_size;
            it = backing_views.emplace(begin, BackingView{remaining_size}).first;
        }
        const auto size = std::min(maximum_size, it->second.size);
        if (size != it->second.size) {
            if (!VirtualFreeEx(process, backing_base + begin, size,
                               MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
                return backing_views.end();
            backing_views.emplace(begin + size, BackingView{it->second.size - size});
            it->second.size = size;
        }
        return it;
    }

    bool MapBackingView(void* address, bool recover = true, u64 maximum_size = GuestViewSize) {
        maximum_size = std::min(maximum_size, preferred_view_size);
        const auto offset =
            reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(backing_base);
        if (offset >= BackingSize)
            return false;
        const u64 index = offset / BackingViewSize;
        std::unique_lock lock{backing_mutex};
        auto it = std::prev(backing_views.upper_bound(offset));
        if (it->second.is_resident) {
            if (it->second.swept) {
                ApplyCommittedProtection(reinterpret_cast<VAddr>(backing_base + it->first),
                                         it->second.size, PAGE_READWRITE);
                it->second.swept = false;
                ++pager_second_chances;
            }
            it->second.last_access = GetTickCount64();
            return true;
        }
        it = SelectBackingView(offset, maximum_size);
        if (it == backing_views.end()) {
            MappingFailure("backing_placeholder_split", GetLastError(),
                           reinterpret_cast<VAddr>(address), offset, maximum_size, PAGE_NOACCESS);
            LOG_ERROR(Core, "Unable to split physical backing view at {:#x}: {}", offset,
                      Common::GetLastErrorMsg());
            return false;
        }
        const u64 view_offset = it->first;
        const u64 view_size = it->second.size;
        auto* base = backing_base + view_offset;
        const auto handle = GetRamPage(index);
        const u64 map_offset = view_offset % BackingViewSize;
        if (!handle || MapHostView(handle, process, base, map_offset, view_size,
                                   MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0) != base) {
            const auto error = GetLastError();
            if (handle)
                MappingFailure("backing_view_map", error, reinterpret_cast<VAddr>(base),
                               view_offset, view_size, PAGE_READWRITE, map_offset, handle,
                               MEM_REPLACE_PLACEHOLDER);
            lock.unlock();
            if (IsMappingResourceError(error) && view_size > MinimumGuestViewSize) {
                preferred_view_size = MinimumGuestViewSize;
                LOG_WARNING(Core, "Reducing physical backing view at {:#x} after Win32 {}", offset,
                            error);
                return MapBackingView(address, recover, MinimumGuestViewSize);
            }
            if (recover && IsMappingResourceError(error) && ReleaseSwapSections(index))
                return MapBackingView(address, false, maximum_size);
            SetLastError(error);
            LOG_ERROR(Core, "Unable to map physical backing view at {:#x}: {}", view_offset,
                      Common::GetLastErrorMsg());
            return false;
        }
        it->second.is_resident = true;
        it->second.swept = false;
        it->second.last_access = GetTickCount64();
        ++pager_page_ins;
        return true;
    }
#endif

#ifdef SHADPS4_UWP
    bool MapGuestView(void* address, u64 access_type, bool* mapping_failed) {
        const auto addr = reinterpret_cast<VAddr>(address);
        std::scoped_lock lock{mutex};
        auto it = regions.upper_bound(addr);
        if (it == regions.begin())
            return false;
        auto* region = &std::prev(it)->second;
        if (!region->is_mapped || region->fd != -1 || region->phys_base == PAddr(-1) ||
            addr - region->base >= region->size)
            return false;
        RestoreSweptRegion(*region);
        if (!region->is_resident) {
            const auto select_view = [&](u64 view_size) {
                const auto begin = std::max(region->base, Common::AlignDown(addr, view_size));
                const auto size = std::min(view_size, region->base + region->size - begin);
                SplitRegion(begin, size);
                region = &std::prev(regions.upper_bound(addr))->second;
            };
            if (region->size > preferred_view_size)
                select_view(preferred_view_size);
            if (!PopulateGuestView(region)) {
                const auto error = GetLastError();
                if (!IsMappingResourceError(error) || region->size <= MinimumGuestViewSize) {
                    if (mapping_failed)
                        *mapping_failed = true;
                    MemorySnapshot("guest_mapping_terminal");
                    LOG_ERROR(Core,
                              "Unable to map RAM alias at {:#x}, physical {:#x}, size {:#x}: "
                              "Win32 {}",
                              region->base, region->phys_base, region->size, error);
                    return false;
                }
                LOG_WARNING(Core, "Reducing RAM alias view at {:#x} after Win32 {}", addr, error);
                select_view(MinimumGuestViewSize);
                if (!PopulateGuestView(region)) {
                    if (mapping_failed)
                        *mapping_failed = true;
                    MemorySnapshot("minimum_guest_mapping_terminal");
                    LOG_ERROR(Core,
                              "Unable to map minimum RAM alias at {:#x}, physical {:#x}, "
                              "size {:#x}: Win32 {}",
                              region->base, region->phys_base, region->size, GetLastError());
                    return false;
                }
            }
        }
        region->residency_order = GetTickCount64();
        CommitPhysical(region->phys_base + addr - region->base);
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT ||
            (info.Protect & PAGE_GUARD))
            return false;
        const auto prot = info.Protect & 0xff;
        if (access_type == 1)
            return prot == PAGE_READWRITE || prot == PAGE_EXECUTE_READWRITE;
        if (access_type == 8)
            return prot == PAGE_EXECUTE || prot == PAGE_EXECUTE_READ ||
                   prot == PAGE_EXECUTE_READWRITE;
        return prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_EXECUTE_READ ||
               prot == PAGE_EXECUTE_READWRITE;
    }

    void ApplyCommittedProtection(VAddr address, u64 size, DWORD protection) {
        const auto end = address + size;
        while (address < end) {
            MEMORY_BASIC_INFORMATION info{};
            ASSERT_MSG(VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)),
                       "Unable to query lazy memory protection");
            const auto next =
                std::min(end, reinterpret_cast<VAddr>(info.BaseAddress) + info.RegionSize);
            if (info.State == MEM_COMMIT) {
                DWORD previous{};
                ASSERT_MSG(VirtualProtect(reinterpret_cast<void*>(address), next - address,
                                          protection, &previous),
                           "Unable to protect lazy memory at {:#x}, size {:#x}, protection {:#x}, "
                           "previous {:#x}, allocation {}: {}",
                           address, next - address, protection, info.Protect, info.AllocationBase,
                           Common::GetLastErrorMsg());
            }
            address = next;
        }
    }

    void RestoreSweptRegion(MemoryRegion& region) {
        if (!region.swept)
            return;
        region.swept = false;
        if (!region.is_resident)
            return;
        ApplyCommittedProtection(region.base, region.size, region.prot);
        for (const auto& info : region.saved_protections)
            ApplyCommittedProtection(reinterpret_cast<VAddr>(info.BaseAddress), info.RegionSize,
                                     info.Protect);
        region.residency_order = GetTickCount64();
        ++pager_second_chances;
    }

    void CommitPhysical(u64 offset) {
        if (swap_enabled)
            return;
        const auto begin = Common::AlignDown(offset, commit_size);
        auto* address = backing_base + begin;
        ASSERT_MSG(MapBackingView(address), "Unable to map lazy physical memory");
        MEMORY_BASIC_INFORMATION info{};
        ASSERT_MSG(VirtualQuery(address, &info, sizeof(info)),
                   "Unable to query lazy physical memory");
        if (info.State == MEM_COMMIT)
            return;
        auto& sequence = commit_sequences[GetCurrentThreadId()];
        const bool sequential = begin == sequence.end;
        const u64 window =
            sequential ? std::max(commit_size, sequence.length ? 64_KB : 16_KB) : commit_size;
        u64 size = std::min({window, BackingSize - begin, BackingViewSize - begin % BackingViewSize,
                             reinterpret_cast<u64>(info.BaseAddress) + info.RegionSize -
                                 reinterpret_cast<u64>(address)});
        try {
            const u64 limit = winrt::Windows::System::MemoryManager::AppMemoryUsageLimit();
            const u64 usage = winrt::Windows::System::MemoryManager::AppMemoryUsage();
            if (limit && (usage >= limit || size > limit - usage ||
                          HostMemoryReserve > limit - usage - size))
                throw std::runtime_error(fmt::format(
                    "UWP RAM budget exhausted: usage {} MiB, limit {} MiB, request {} MiB; "
                    "demand commitment",
                    usage / 1_MB, limit / 1_MB, size / 1_MB));
        } catch (const winrt::hresult_error&) {
        }
        auto* committed = VirtualAllocFromApp(address, size, MEM_COMMIT, PAGE_READWRITE);
        if (committed != address && size > commit_size) {
            const auto error = GetLastError();
            if (error == ERROR_NOT_ENOUGH_MEMORY || error == ERROR_OUTOFMEMORY ||
                error == ERROR_COMMITMENT_LIMIT) {
                size = commit_size;
                committed = VirtualAllocFromApp(address, size, MEM_COMMIT, PAGE_READWRITE);
            }
        }
        if (committed != address) {
            const auto error = GetLastError();
            u64 usage{}, limit{};
            try {
                usage = winrt::Windows::System::MemoryManager::AppMemoryUsage();
                limit = winrt::Windows::System::MemoryManager::AppMemoryUsageLimit();
            } catch (const winrt::hresult_error&) {
            }
            throw std::runtime_error(
                fmt::format("Unable to commit physical memory at {:#x}, size {:#x}: Win32 {}; "
                            "committed {} MiB, app usage {} MiB, limit {} MiB: {}",
                            begin, size, error, committed_bytes / 1_MB, usage / 1_MB, limit / 1_MB,
                            Common::NativeErrorToString(error)));
        }
        committed_bytes += size;
        sequence.end = begin + size;
        sequence.length = sequential ? std::min(sequence.length + 1, 2u) : 0;
        for (auto& [base, region] : regions) {
            if (!region.is_resident || region.fd != -1 || region.phys_base == PAddr(-1))
                continue;
            const auto first = std::max(begin, region.phys_base);
            const auto last = std::min(begin + size, region.phys_base + region.size);
            if (first >= last)
                continue;
            const auto virtual_first = base + first - region.phys_base;
            const auto virtual_last = base + last - region.phys_base;
            ApplyCommittedProtection(virtual_first, last - first, region.prot);
            for (const auto& protection : region.saved_protections) {
                const auto protect_first =
                    std::max(virtual_first, reinterpret_cast<VAddr>(protection.BaseAddress));
                const auto protect_last =
                    std::min(virtual_last, reinterpret_cast<VAddr>(protection.BaseAddress) +
                                               protection.RegionSize);
                if (protect_first < protect_last)
                    ApplyCommittedProtection(protect_first, protect_last - protect_first,
                                             protection.Protect);
            }
        }
    }

    HANDLE GetRamPage(u64 index) {
        std::scoped_lock lock{ram_pages_mutex};
        section_accesses[index] = ++section_access_clock;
        if (ram_pages[index])
            return ram_pages[index];
        const u64 size = std::min(BackingViewSize, BackingSize - index * BackingViewSize);
        HANDLE file = swap_files[index];
        if (swap_enabled && file == INVALID_HANDLE_VALUE) {
            const auto path =
                swap_directory / fmt::format("{}-{}-{}.swap", GetCurrentProcessId(),
                                             reinterpret_cast<uintptr_t>(this), index);
            CREATEFILE2_EXTENDED_PARAMETERS parameters{sizeof(parameters)};
            parameters.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
            parameters.dwFileFlags = FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_RANDOM_ACCESS;
            file = CreateFile2FromAppW(
                path.c_str(), GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE | DELETE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, CREATE_ALWAYS, &parameters);
            if (file == INVALID_HANDLE_VALUE) {
                MappingFailure("swap_file_create", GetLastError(), 0, index * BackingViewSize, size,
                               PAGE_EXECUTE_READWRITE);
                throw std::runtime_error("Unable to create memory swap: " +
                                         Common::GetLastErrorMsg());
            }
            swap_files[index] = file;
        }
        ram_pages[index] =
            CreateFileMapping2(file, nullptr, FILE_MAP_ALL_ACCESS, PAGE_EXECUTE_READWRITE,
                               swap_enabled ? SEC_COMMIT : SEC_RESERVE, size, nullptr, nullptr, 0);
        if (!ram_pages[index])
            MappingFailure("ram_section_create", GetLastError(), 0, index * BackingViewSize, size,
                           PAGE_EXECUTE_READWRITE, 0, file,
                           swap_enabled ? SEC_COMMIT : SEC_RESERVE);
        return ram_pages[index];
    }

    static bool IsMappingResourceError(DWORD error) {
        return error == ERROR_NOT_ENOUGH_MEMORY || error == ERROR_OUTOFMEMORY ||
               error == ERROR_COMMITMENT_LIMIT || error == ERROR_NO_SYSTEM_RESOURCES ||
               error == ERROR_WORKING_SET_QUOTA || error == ERROR_PAGEFILE_QUOTA ||
               error == ERROR_NOT_ENOUGH_QUOTA;
    }

    static bool IsExecutableProtection(DWORD protection) {
        return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
               protection == PAGE_EXECUTE_READWRITE;
    }

    static bool NeedsExecutableView(DWORD protection,
                                    std::span<const MEMORY_BASIC_INFORMATION> protections) {
        if (IsExecutableProtection(protection))
            return true;
        return std::ranges::any_of(
            protections, [](const auto& info) { return IsExecutableProtection(info.Protect); });
    }

    static void SaveProtections(MemoryRegion* region,
                                std::span<const MEMORY_BASIC_INFORMATION> protections) {
        region->saved_protections.clear();
        for (auto info : protections) {
            const auto first = std::max(region->base, reinterpret_cast<VAddr>(info.BaseAddress));
            const auto last = std::min(region->base + region->size,
                                       reinterpret_cast<VAddr>(info.BaseAddress) + info.RegionSize);
            if (first >= last)
                continue;
            info.BaseAddress = reinterpret_cast<void*>(first);
            info.RegionSize = last - first;
            region->saved_protections.push_back(info);
        }
    }
#endif

    ~Impl() {
#ifdef SHADPS4_UWP
        StopPager();
        LOG_INFO(Core, "UWP demand memory: {} KiB committed, swap {}", committed_bytes / 1_KB,
                 swap_enabled ? "enabled" : "disabled");
        for (const auto& [address, region] : regions) {
            if (region.is_mapped && region.is_resident) {
                if ((region.fd != -1 && region.prot != PAGE_READONLY) ||
                    (region.fd == -1 && region.phys_base != PAddr(-1))) {
                    if (!UnmapViewOfFile2(process, reinterpret_cast<void*>(address),
                                          MEM_PRESERVE_PLACEHOLDER))
                        LOG_ERROR(Core, "Failed to unmap guest region {:#x}: {}", address,
                                  GetLastError());
                } else if (!VirtualFreeEx(process, reinterpret_cast<void*>(address), region.size,
                                          MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
                    LOG_ERROR(Core, "Failed to restore guest placeholder {:#x}: {}", address,
                              GetLastError());
                }
            }
            if (!VirtualFreeEx(process, reinterpret_cast<void*>(address), 0, MEM_RELEASE))
                LOG_ERROR(Core, "Failed to release guest region {:#x}: {}", address,
                          GetLastError());
        }
#endif
        if (virtual_base) {
            if (!VirtualFree(virtual_base, 0, MEM_RELEASE)) {
                LOG_CRITICAL(Core, "Failed to free virtual memory");
            }
        }
        if (backing_base) {
#ifdef SHADPS4_UWP
            active_backing.store(nullptr);
            for (const auto& [offset, view] : backing_views) {
                if (view.is_resident)
                    UnmapViewOfFile2(process, backing_base + offset, MEM_PRESERVE_PLACEHOLDER);
            }
            VirtualFreeEx(process, backing_base, BackingSize,
                          MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
            VirtualFreeEx(process, backing_base, 0, MEM_RELEASE);
#else
            if (!UnmapViewOfFile2(process, backing_base, MEM_PRESERVE_PLACEHOLDER)) {
                LOG_CRITICAL(Core, "Failed to unmap backing memory placeholder");
            }
            if (!VirtualFreeEx(process, backing_base, 0, MEM_RELEASE)) {
                LOG_CRITICAL(Core, "Failed to free backing memory");
            }
#endif
        }
#ifdef SHADPS4_UWP
        for (auto page : ram_pages) {
            if (page)
                CloseHandle(page);
        }
        for (auto file : swap_files) {
            if (file != INVALID_HANDLE_VALUE)
                CloseHandle(file);
        }
#endif
        if (backing_handle && !CloseHandle(backing_handle)) {
            LOG_CRITICAL(Core, "Failed to free backing memory file handle");
        }
    }

    void* MapRegion(MemoryRegion* region,
                    std::span<const MEMORY_BASIC_INFORMATION> protections = {},
                    bool populate = false) {
        VAddr virtual_addr = region->base;
        PAddr phys_addr = region->phys_base;
        PAddr map_offset = phys_addr;
        auto saved_protections = std::move(region->saved_protections);
        if (protections.empty())
            protections = saved_protections;
        u64 size = region->size;
        ULONG prot = region->prot;
        s32 fd = region->fd;

        void* ptr = nullptr;
        if (phys_addr != -1) {
            HANDLE backing = fd != -1 ? reinterpret_cast<HANDLE>(fd) : backing_handle;
#ifdef SHADPS4_UWP
            if (fd == -1) {
                ASSERT_MSG(phys_addr <= BackingSize && size <= BackingSize - phys_addr,
                           "Physical backing range exceeds reserved memory");
                if (!populate) {
                    SaveProtections(region, protections);
                    return reinterpret_cast<void*>(virtual_addr);
                }
                ASSERT_MSG(size <= BackingViewSize - phys_addr % BackingViewSize,
                           "RAM alias crosses resident page boundary");
                backing = GetRamPage(phys_addr / BackingViewSize);
                if (!backing) {
                    const auto error = GetLastError();
                    SaveProtections(region, protections);
                    SetLastError(error);
                    return nullptr;
                }
                map_offset = phys_addr % BackingViewSize;
            }
#endif
            if (fd != -1 && prot == PAGE_READONLY) {
                // Allocate the memory for the mapping
                DWORD resultvar;
                ptr = VirtualAlloc2(process, reinterpret_cast<PVOID>(virtual_addr), size,
                                    MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
                                    PAGE_READWRITE, nullptr, 0);

                // Use ReadFile to read file contents into the memory area.
                // Create an OVERLAPPED with the file offset, then supply that to ReadFile
                OVERLAPPED param{};
                // Offset is the least-significant 32 bits, OffsetHigh is the most-significant.
                param.Offset = phys_addr & 0xffffffffull;
                param.OffsetHigh = (phys_addr & 0xffffffff00000000ull) >> 32;
                bool ret = ReadFile(backing, ptr, size, &resultvar, &param);
                ASSERT_MSG(ret, "ReadFile failed. {}", Common::GetLastErrorMsg());

                // ReadFile moves the file pointer, restore it with SetFilePointer
                s64 size_to_move = -size;
                LONG size_low = size_to_move & 0xffffffffull;
                LONG size_high = (size_to_move & 0xffffffff00000000ull) >> 32;
                ret = SetFilePointer(backing, size_low, &size_high, FILE_CURRENT);

                // Protect the memory area appropriately
                ret = VirtualProtect(ptr, size, prot, &resultvar);
                ASSERT_MSG(ret, "VirtualProtect failed. {}", Common::GetLastErrorMsg());
            } else {
#ifdef SHADPS4_UWP
                if (fd == -1) {
                    const DWORD view_protection = NeedsExecutableView(prot, protections)
                                                      ? PAGE_EXECUTE_READWRITE
                                                      : PAGE_READWRITE;
                    ptr = MapHostView(backing, process, reinterpret_cast<PVOID>(virtual_addr),
                                      map_offset, size, MEM_REPLACE_PLACEHOLDER, view_protection,
                                      nullptr, 0);
                    if (!ptr) {
                        const auto error = GetLastError();
                        MappingFailure(view_protection == PAGE_READWRITE
                                           ? "guest_alias_map_from_app"
                                           : "guest_alias_map_native",
                                       error, virtual_addr, phys_addr, size, view_protection,
                                       map_offset, backing, MEM_REPLACE_PLACEHOLDER);
                        SaveProtections(region, protections);
                        SetLastError(error);
                        return nullptr;
                    }
                    ApplyCommittedProtection(virtual_addr, size, prot);
                } else
#endif
                    if (prot == PAGE_NOACCESS) {
                    DWORD resultvar;
                    ptr = MapHostView(backing, process, reinterpret_cast<PVOID>(virtual_addr),
                                      map_offset, size, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
                                      nullptr, 0);
                    ASSERT_MSG(ptr, "MapViewOfFile3 failed. {}", Common::GetLastErrorMsg());
#ifdef SHADPS4_UWP
                    ApplyCommittedProtection(virtual_addr, size, prot);
#else
                    bool ret = VirtualProtect(ptr, size, prot, &resultvar);
                    ASSERT_MSG(ret, "VirtualProtect failed. {}", Common::GetLastErrorMsg());
#endif
                } else {
                    ptr = MapHostView(backing, process, reinterpret_cast<PVOID>(virtual_addr),
                                      map_offset, size, MEM_REPLACE_PLACEHOLDER, prot, nullptr, 0);
                    ASSERT_MSG(ptr, "MapViewOfFile3 failed. {}", Common::GetLastErrorMsg());
                }
            }
        } else {
            ptr =
                VirtualAlloc2(process, reinterpret_cast<PVOID>(virtual_addr), size,
                              MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, prot, nullptr, 0);
        }
        ASSERT_MSG(ptr, "{}", Common::GetLastErrorMsg());
        region->is_resident = true;

        for (const auto& info : protections) {
            const VAddr info_base = reinterpret_cast<VAddr>(info.BaseAddress);
            const VAddr begin = std::max(info_base, virtual_addr);
            const VAddr end = std::min(info_base + info.RegionSize, virtual_addr + size);
            if (begin >= end) {
                continue;
            }
#ifdef SHADPS4_UWP
            ApplyCommittedProtection(begin, end - begin, info.Protect);
#else
            DWORD old_flags{};
            const bool ret =
                VirtualProtectEx(process, LPVOID(begin), end - begin, info.Protect, &old_flags);
            ASSERT_MSG(ret, "VirtualProtectEx failed. {}", Common::GetLastErrorMsg());
#endif
        }
#ifdef SHADPS4_UWP
        if (fd == -1 && phys_addr != PAddr(-1)) {
            SaveProtections(region, protections);
        }
#endif
        return ptr;
    }

    std::vector<MEMORY_BASIC_INFORMATION> QueryProtections(const MemoryRegion& region,
                                                           VAddr skip_addr, u64 skip_size) {
        std::vector<MEMORY_BASIC_INFORMATION> protections;
#ifdef SHADPS4_UWP
        if (!region.is_resident || (region.fd == -1 && region.phys_base != PAddr(-1)))
            return region.saved_protections;
#endif
        const VAddr end = region.base + region.size;
        for (VAddr addr = region.base; addr < end;) {
            if (addr >= skip_addr && addr < skip_addr + skip_size) {
                addr = skip_addr + skip_size;
                continue;
            }
            MEMORY_BASIC_INFORMATION info{};
            const bool ret = VirtualQueryEx(process, LPCVOID(addr), &info, sizeof(info));
            ASSERT_MSG(ret, "VirtualQueryEx failed. {}", Common::GetLastErrorMsg());
            if (info.Protect != region.prot) {
                protections.push_back(info);
            }
            addr = reinterpret_cast<VAddr>(info.BaseAddress) + info.RegionSize;
        }
        return protections;
    }

    void UnmapRegion(MemoryRegion* region) {
#ifdef SHADPS4_UWP
        region->swept = false;
        if (!region->is_resident)
            return;
#endif
        VAddr virtual_addr = region->base;
        PAddr phys_base = region->phys_base;
        u64 size = region->size;
        ULONG prot = region->prot;
        s32 fd = region->fd;

        bool ret = false;
        if ((fd != -1 && prot != PAGE_READONLY) || (fd == -1 && phys_base != -1)) {
            ret = UnmapViewOfFile2(process, reinterpret_cast<PVOID>(virtual_addr),
                                   MEM_PRESERVE_PLACEHOLDER);
        } else {
            ret = VirtualFreeEx(process, reinterpret_cast<PVOID>(virtual_addr), size,
                                MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
        }
        ASSERT_MSG(ret, "Unmap on virtual_addr {:#x}, size {:#x} failed: {}", virtual_addr, size,
                   Common::GetLastErrorMsg());
        region->is_resident = false;
    }

    void SplitRegion(VAddr virtual_addr, u64 size) {
        // First, get the region this range covers
        auto it = std::prev(regions.upper_bound(virtual_addr));

        // All unmapped areas will coalesce, so there should be a region
        // containing the full requested range. If not, then something is mapped here.
        ASSERT_MSG(it->second.base + it->second.size >= virtual_addr + size,
                   "Cannot fit region into one placeholder");

        // If the region is mapped, we need to unmap first before we can modify the placeholders.
        std::vector<MEMORY_BASIC_INFORMATION> protections;
        if (it->second.is_mapped) {
            ASSERT_MSG(it->second.phys_base != -1 || !it->second.is_mapped,
                       "Cannot split unbacked mapping");
            protections = QueryProtections(it->second, virtual_addr, size);
            UnmapRegion(&it->second);
        }

        // We need to split this region to create a matching placeholder.
        if (it->second.base != virtual_addr) {
            // Requested address is not the start of the containing region,
            // create a new region to represent the memory before the requested range.
            auto& region = it->second;
            u64 base_offset = virtual_addr - region.base;
            u64 next_region_size = region.size - base_offset;
            PAddr next_region_phys_base = -1;
            if (region.is_mapped) {
                next_region_phys_base = region.phys_base + base_offset;
            }
            region.size = base_offset;

            // Use VirtualFreeEx to create the split.
            if (!VirtualFreeEx(process, LPVOID(region.base), region.size,
                               MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
                UNREACHABLE_MSG("Region splitting failed: {}", Common::GetLastErrorMsg());
            }

            // If the mapping was mapped, remap the region.
            if (region.is_mapped) {
                MapRegion(&region, protections);
            }

            // Store a new region matching the removed area.
            it = regions.emplace_hint(std::next(it), virtual_addr,
                                      MemoryRegion(virtual_addr, next_region_phys_base,
                                                   next_region_size, region.prot, region.fd,
                                                   region.is_mapped));
        }

        // At this point, the region's base will match virtual_addr.
        // Now check for a size difference.
        if (it->second.size != size) {
            // The requested size is smaller than the current region placeholder.
            // Update region to match the requested region,
            // then make a new region to represent the remaining space.
            auto& region = it->second;
            VAddr next_region_addr = region.base + size;
            u64 next_region_size = region.size - size;
            PAddr next_region_phys_base = -1;
            if (region.is_mapped) {
                next_region_phys_base = region.phys_base + size;
            }
            region.size = size;

            // Store the new region matching the remaining space
            regions.emplace_hint(std::next(it), next_region_addr,
                                 MemoryRegion(next_region_addr, next_region_phys_base,
                                              next_region_size, region.prot, region.fd,
                                              region.is_mapped));

            // Use VirtualFreeEx to create the split.
            if (!VirtualFreeEx(process, LPVOID(region.base), region.size,
                               MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
                UNREACHABLE_MSG("Region splitting failed: {}", Common::GetLastErrorMsg());
            }

            // If these regions were mapped, then map the unmapped area beyond the requested range.
            if (region.is_mapped) {
                MapRegion(&std::next(it)->second, protections);
            }
        }

        // If the requested region was mapped, remap it.
        if (it->second.is_mapped) {
            MapRegion(&it->second, protections);
        }
    }

    void* Map(VAddr virtual_addr, PAddr phys_addr, u64 size, ULONG prot, s32 fd = -1) {
#ifdef SHADPS4_UWP
        if (fd == -1 && phys_addr != PAddr(-1) &&
            size > BackingViewSize - phys_addr % BackingViewSize) {
            const auto first_size = BackingViewSize - phys_addr % BackingViewSize;
            auto* result = Map(virtual_addr, phys_addr, first_size, prot, fd);
            Map(virtual_addr + first_size, phys_addr + first_size, size - first_size, prot, fd);
            return result;
        }
        if (fd == -1 && phys_addr != PAddr(-1) && size > BackingViewSize) {
            void* result{};
            for (u64 offset = 0; offset < size;) {
                const auto view_size = std::min(BackingViewSize, size - offset);
                auto* mapped = Map(virtual_addr + offset, phys_addr + offset, view_size, prot, fd);
                if (!offset)
                    result = mapped;
                offset += view_size;
            }
            return result;
        }

#endif
        std::scoped_lock lk{mutex};
        // Get a pointer to the region containing virtual_addr
        auto it = std::prev(regions.upper_bound(virtual_addr));

        // If needed, split surrounding regions to create a placeholder
        if (it->first != virtual_addr || it->second.size != size) {
            SplitRegion(virtual_addr, size);
            it = std::prev(regions.upper_bound(virtual_addr));
        }

        // Get the address and region for this range.
        auto& [base, region] = *it;
        ASSERT_MSG(!region.is_mapped, "Cannot overwrite mapped region");

        // Now we have a region matching the requested region, perform the actual mapping.
        region.is_mapped = true;
        region.phys_base = phys_addr;
        region.prot = prot;
        region.fd = fd;
        return MapRegion(&region);
    }

    void CoalesceFreeRegions(VAddr virtual_addr) {
        // First, get the region to update
        auto it = std::prev(regions.upper_bound(virtual_addr));
        ASSERT_MSG(!it->second.is_mapped, "Cannot coalesce mapped regions");

        // Check if there are adjacent free placeholders before this area.
        bool can_coalesce = false;
        auto it_prev = it != regions.begin() ? std::prev(it) : regions.end();
        while (it_prev != regions.end() && !it_prev->second.is_mapped &&
               it_prev->first + it_prev->second.size == it->first) {
            // If there is an earlier region, move our iterator to that and increase size.
            it_prev->second.size = it_prev->second.size + it->second.size;
            regions.erase(it);
            it = it_prev;

            // Mark this region as coalesce-able.
            can_coalesce = true;

            // Get the next previous region.
            it_prev = it != regions.begin() ? std::prev(it) : regions.end();
        }

        // Check if there are adjacent free placeholders after this area.
        auto it_next = std::next(it);
        while (it_next != regions.end() && !it_next->second.is_mapped &&
               it->first + it->second.size == it_next->first) {
            // If there is a later region, increase our current region's size
            it->second.size = it->second.size + it_next->second.size;
            regions.erase(it_next);

            // Mark this region as coalesce-able.
            can_coalesce = true;

            // Get the next region
            it_next = std::next(it);
        }

        // If there are placeholders to coalesce, then coalesce them.
        if (can_coalesce) {
            if (!VirtualFreeEx(process, LPVOID(it->first), it->second.size,
                               MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS)) {
                UNREACHABLE_MSG("Region coalescing failed: {}", Common::GetLastErrorMsg());
            }
        }
    }

    VAddr Unmap(VAddr virtual_addr, u64* size) {
        std::scoped_lock lk{mutex};
        // Loop through all regions in the requested range
        u64 remaining_size = *size;
        VAddr current_addr = virtual_addr;
        while (remaining_size > 0) {
            // Get a pointer to the region containing virtual_addr
            auto it = std::prev(regions.upper_bound(current_addr));

            // If necessary, split regions to ensure a valid unmap.
            // To prevent complication, ensure size is within the bounds of the current region.
            u64 base_offset = current_addr - it->second.base;
            u64 size_to_unmap = std::min<u64>(it->second.size - base_offset, remaining_size);
            if (current_addr != it->second.base || size_to_unmap != it->second.size) {
                SplitRegion(current_addr, size_to_unmap);
                it = std::prev(regions.upper_bound(current_addr));
            }

            // Get the address and region corresponding to this range.
            auto& [base, region] = *it;

            // Unmap the region if it was previously mapped
            if (region.is_mapped) {
                UnmapRegion(&region);
            }

            // Update region data
            region.is_mapped = false;
            region.fd = -1;
            region.phys_base = -1;
            region.prot = PAGE_NOACCESS;
#ifdef SHADPS4_UWP
            region.saved_protections.clear();
#endif

            // Update loop variables
            remaining_size -= size_to_unmap;
            current_addr += size_to_unmap;
        }

        // Coalesce any free space produced from these unmaps.
        CoalesceFreeRegions(virtual_addr);

        return virtual_addr;
    }

    void Protect(VAddr virtual_addr, u64 size, bool read, bool write, bool execute) {
        std::scoped_lock lk{mutex};
        DWORD new_flags{};

        if (write && !read) {
            // While write-only CPU protection isn't possible, write-only GPU protection is.
            LOG_WARNING(Core, "Converting write-only protection to read-write");
        }

        // All cases involving execute permissions have separate permissions.
        if (execute) {
            // If there's some form of write protection requested, provide read-write permissions.
            if (write) {
                new_flags = PAGE_EXECUTE_READWRITE;
            } else if (read && !write) {
                new_flags = PAGE_EXECUTE_READ;
            } else {
                new_flags = PAGE_EXECUTE;
            }
        } else {
            if (write) {
                new_flags = PAGE_READWRITE;
            } else if (read && !write) {
                new_flags = PAGE_READONLY;
            } else {
                new_flags = PAGE_NOACCESS;
            }
        }

        // If no flags are assigned, then something's gone wrong.
        if (new_flags == 0) {
            LOG_CRITICAL(Core,
                         "Unsupported protection flag combination for address {:#x}, size {}, "
                         "read={}, write={}, execute={}",
                         virtual_addr, size, read, write, execute);
            return;
        }

        const VAddr virtual_end = virtual_addr + size;
        auto it = --regions.upper_bound(virtual_addr);
        ASSERT_MSG(it != regions.end(), "addr {:#x} out of bounds", virtual_addr);
        for (; it->first < virtual_end; it++) {
            if (!it->second.is_mapped) {
                continue;
            }
            auto& region = it->second;
            const u64 range_addr = std::max(region.base, virtual_addr);
            const u64 range_size = std::min(region.base + region.size, virtual_end) - range_addr;
#ifdef SHADPS4_UWP
            if (region.fd == -1 && region.phys_base != PAddr(-1)) {
                RestoreSweptRegion(region);
                const bool was_executable =
                    NeedsExecutableView(region.prot, region.saved_protections);
                if (range_addr == region.base && range_size == region.size) {
                    region.prot = new_flags;
                    region.saved_protections.clear();
                } else {
                    MEMORY_BASIC_INFORMATION info{};
                    info.BaseAddress = reinterpret_cast<void*>(range_addr);
                    info.RegionSize = range_size;
                    info.Protect = new_flags;
                    std::vector<MEMORY_BASIC_INFORMATION> protections;
                    for (auto previous : region.saved_protections) {
                        const auto first = reinterpret_cast<VAddr>(previous.BaseAddress);
                        const auto last = first + previous.RegionSize;
                        if (first < range_addr) {
                            auto prefix = previous;
                            prefix.RegionSize = std::min(last, range_addr) - first;
                            protections.push_back(prefix);
                        }
                        if (last > range_addr + range_size) {
                            const auto suffix = std::max(first, range_addr + range_size);
                            previous.BaseAddress = reinterpret_cast<void*>(suffix);
                            previous.RegionSize = last - suffix;
                            protections.push_back(previous);
                        }
                    }
                    if (new_flags != region.prot)
                        protections.push_back(info);
                    region.saved_protections = std::move(protections);
                }
                if (region.is_resident) {
                    if (was_executable !=
                        NeedsExecutableView(region.prot, region.saved_protections))
                        UnmapRegion(&region);
                    else
                        ApplyCommittedProtection(range_addr, range_size, new_flags);
                }
                continue;
            }
#endif
            DWORD old_flags{};
            if (!VirtualProtectEx(process, LPVOID(range_addr), range_size, new_flags, &old_flags)) {
                UNREACHABLE_MSG(
                    "Failed to change virtual memory protection for address {:#x}, size "
                    "{:#x}, error {}",
                    virtual_addr, size, Common::GetLastErrorMsg());
            }
        }
    }

    boost::icl::interval_set<VAddr> GetUsableRegions() {
        boost::icl::interval_set<VAddr> reserved_regions;
        for (auto region : regions) {
            reserved_regions.insert({region.second.base, region.second.base + region.second.size});
        }
        return reserved_regions;
    }

    std::recursive_mutex mutex;
    HANDLE process{};
    HANDLE backing_handle{};
    u8* backing_base{};
    u8* virtual_base{};
    u8* system_managed_base{};
    u64 system_managed_size{};
    u8* system_reserved_base{};
    u64 system_reserved_size{};
    u8* user_base{};
    u64 user_size{};
    std::map<VAddr, MemoryRegion> regions;
};
#else

enum PosixPageProtection {
    PAGE_NOACCESS = 0,
    PAGE_READONLY = PROT_READ,
    PAGE_READWRITE = PROT_READ | PROT_WRITE,
    PAGE_EXECUTE = PROT_EXEC,
    PAGE_EXECUTE_READ = PROT_EXEC | PROT_READ,
    PAGE_EXECUTE_READWRITE = PROT_EXEC | PROT_READ | PROT_WRITE
};

[[nodiscard]] constexpr PosixPageProtection ToPosixProt(Core::MemoryProt prot) {
    const bool read =
        True(prot & Core::MemoryProt::CpuRead) || True(prot & Core::MemoryProt::GpuRead);
    const bool write =
        True(prot & Core::MemoryProt::CpuWrite) || True(prot & Core::MemoryProt::GpuWrite);
    const bool execute = True(prot & Core::MemoryProt::CpuExec);

    if (write && !read) {
        // While write-only CPU mappings aren't possible, write-only GPU mappings are.
        LOG_WARNING(Core, "Converting write-only mapping to read-write");
    }

    // All cases involving execute permissions have separate permissions.
    if (execute) {
        if (write) {
            return PAGE_EXECUTE_READWRITE;
        } else if (read && !write) {
            return PAGE_EXECUTE_READ;
        } else {
            return PAGE_EXECUTE;
        }
    } else {
        if (write) {
            return PAGE_READWRITE;
        } else if (read && !write) {
            return PAGE_READONLY;
        } else {
            return PAGE_NOACCESS;
        }
    }
}

struct AddressSpace::Impl {
    Impl() {
        BackingSize += EmulatorSettings.GetExtraDmemInMBytes() * 1_MB +
                       EmulatorSettings.GetExtraFmemInMBytes() * 1_MB;
        // Allocate virtual address placeholder for our address space.
        system_managed_size = SystemManagedSize;
        system_reserved_size = SystemReservedSize;
        user_size = UserSize;

        constexpr int protection_flags = PROT_READ | PROT_WRITE;
        int map_flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED; // compiler knows its constexpr
#if !defined(__FreeBSD__)
        map_flags |= MAP_NORESERVE;
#endif

#if defined(__APPLE__) && defined(ARCH_X86_64)
        // On ARM64 Macs, we run into limitations due to the commpage from 0xFC0000000 - 0xFFFFFFFFF
        // and the GPU carveout region from 0x1000000000 - 0x6FFFFFFFFF. Because this creates gaps
        // in the available virtual memory region, we map memory space using three distinct parts.
        system_managed_base =
            reinterpret_cast<u8*>(mmap(reinterpret_cast<void*>(SYSTEM_MANAGED_MIN),
                                       system_managed_size, protection_flags, map_flags, -1, 0));
        system_reserved_base =
            reinterpret_cast<u8*>(mmap(reinterpret_cast<void*>(SYSTEM_RESERVED_MIN),
                                       system_reserved_size, protection_flags, map_flags, -1, 0));
        user_base = reinterpret_cast<u8*>(
            mmap(reinterpret_cast<void*>(USER_MIN), user_size, protection_flags, map_flags, -1, 0));
#else
        const auto virtual_size = system_managed_size + system_reserved_size + user_size;
#if defined(ARCH_X86_64) && !defined(__FreeBSD__)
        const auto virtual_base =
            reinterpret_cast<u8*>(mmap(reinterpret_cast<void*>(SYSTEM_MANAGED_MIN), virtual_size,
                                       protection_flags, map_flags, -1, 0));
        system_managed_base = virtual_base;
        system_reserved_base = reinterpret_cast<u8*>(SYSTEM_RESERVED_MIN);
        user_base = reinterpret_cast<u8*>(USER_MIN);
#else
        // FreeBSD can't stand MAP_FIXED or it may overwrite mmap() itself!
        // Map memory wherever possible and instruction translation can handle offsetting to the
        // base.
        map_flags &= ~MAP_FIXED;
        const auto virtual_base =
            reinterpret_cast<u8*>(mmap(nullptr, virtual_size, protection_flags, map_flags, -1, 0));
        system_managed_base = virtual_base;
        system_reserved_base = virtual_base + SYSTEM_RESERVED_MIN - SYSTEM_MANAGED_MIN;
        user_base = virtual_base + USER_MIN - SYSTEM_MANAGED_MIN;
#endif
#endif
        if (system_managed_base == MAP_FAILED || system_reserved_base == MAP_FAILED ||
            user_base == MAP_FAILED) {
            LOG_CRITICAL(Kernel_Vmm, "mmap failed: {}", strerror(errno));
            throw std::bad_alloc{};
        }

        LOG_INFO(Kernel_Vmm, "System managed virtual memory region: {} - {}",
                 fmt::ptr(system_managed_base),
                 fmt::ptr(system_managed_base + system_managed_size - 1));
        LOG_INFO(Kernel_Vmm, "System reserved virtual memory region: {} - {}",
                 fmt::ptr(system_reserved_base),
                 fmt::ptr(system_reserved_base + system_reserved_size - 1));
        LOG_INFO(Kernel_Vmm, "User virtual memory region: {} - {}", fmt::ptr(user_base),
                 fmt::ptr(user_base + user_size - 1));

#ifdef __APPLE__
        const auto shm_path = fmt::format("/BackingDmem{}", getpid());
        backing_fd = shm_open(shm_path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
        if (backing_fd < 0) {
            LOG_CRITICAL(Kernel_Vmm, "shm_open failed: {}", strerror(errno));
            throw std::bad_alloc{};
        }
        shm_unlink(shm_path.c_str());
#else
#ifndef __FreeBSD__
        madvise(virtual_base, virtual_size, MADV_HUGEPAGE);
#endif
        // NOTE: If you add MFD_HUGETLB or whatever, remember that FBSD will break (libc bug)
        // so please, do not, add MFD_* whatever unless you ifdef it away (must be 0 for FBSD)
        // using sized pages as well causes incessant vm_reclaim calls in kernel, do not use on FBSD
        // under any circumstances.
        backing_fd = memfd_create("BackingDmem", 0);
        if (backing_fd < 0) {
            LOG_CRITICAL(Kernel_Vmm, "memfd_create failed: {}", strerror(errno));
            throw std::bad_alloc{};
        }
#endif

        // Defined to extend the file with zeros
        int ret = ftruncate(backing_fd, BackingSize);
        if (ret != 0) {
            LOG_CRITICAL(Kernel_Vmm, "ftruncate failed with {}, are you out-of-memory?",
                         strerror(errno));
            throw std::bad_alloc{};
        }

        // Map backing dmem handle.
        backing_base = static_cast<u8*>(
            mmap(nullptr, BackingSize, PROT_READ | PROT_WRITE, MAP_SHARED, backing_fd, 0));
        if (backing_base == MAP_FAILED) {
            LOG_CRITICAL(Kernel_Vmm, "mmap failed: {}", strerror(errno));
            throw std::bad_alloc{};
        }
    }

    void* Map(VAddr virtual_addr, PAddr phys_addr, u64 size, PosixPageProtection prot,
              int fd = -1) {
#ifdef __APPLE__
        if ((prot & PROT_EXEC) != 0) {
            ASSERT_MSG(fd == -1, "Requested execute permissions for file mapping");
            phys_addr = -1;
        }
#endif
        const int handle = phys_addr != -1 ? (fd == -1 ? backing_fd : fd) : -1;
        const off_t host_offset = phys_addr != -1 ? phys_addr : 0;
        const int flag = phys_addr != -1 ? MAP_SHARED : (MAP_ANONYMOUS | MAP_PRIVATE);
        void* ret = mmap(reinterpret_cast<void*>(virtual_addr), size, prot, MAP_FIXED | flag,
                         handle, host_offset);
        ASSERT_MSG(ret != MAP_FAILED, "mmap failed: {}", strerror(errno));
        return ret;
    }

    VAddr Unmap(VAddr virtual_addr, u64* size) {
        ASSERT_MSG(size != nullptr && *size > 0, "Invalid unmap size");

        int map_flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED;
#if !defined(__FreeBSD__)
        map_flags |= MAP_NORESERVE;
#endif
        void* ret = mmap(reinterpret_cast<void*>(virtual_addr), *size, PROT_NONE, map_flags, -1, 0);
        ASSERT_MSG(ret != MAP_FAILED, "mmap failed: {}", strerror(errno));

        return virtual_addr;
    }

    void Protect(VAddr virtual_addr, u64 size, bool read, bool write, bool execute) {
        int flags = PROT_NONE;
        if (read) {
            flags |= PROT_READ;
        }
        if (write) {
            flags |= PROT_WRITE;
        }
#ifdef ARCH_X86_64
        if (execute) {
            flags |= PROT_EXEC;
        }
#endif
        int ret = mprotect(reinterpret_cast<void*>(virtual_addr), size, flags);
        ASSERT_MSG(ret == 0, "mprotect failed: {}", strerror(errno));
    }

    int backing_fd;
    u8* backing_base{};
    u8* system_managed_base{};
    u64 system_managed_size{};
    u8* system_reserved_base{};
    u64 system_reserved_size{};
    u8* user_base{};
    u64 user_size{};
};
#endif

AddressSpace::AddressSpace() : impl{std::make_unique<Impl>()} {
    backing_base = impl->backing_base;
    system_managed_base = impl->system_managed_base;
    system_managed_size = impl->system_managed_size;
    system_reserved_base = impl->system_reserved_base;
    system_reserved_size = impl->system_reserved_size;
    user_base = impl->user_base;
    user_size = impl->user_size;
}

AddressSpace::~AddressSpace() = default;

#ifdef SHADPS4_UWP
void AddressSpace::StartPager() {
    if (auto* backing = Impl::active_backing.load(std::memory_order_acquire))
        backing->StartPager();
}

void AddressSpace::StopPager() {
    if (auto* backing = Impl::active_backing.load(std::memory_order_acquire))
        backing->StopPager();
}

bool AddressSpace::HandleBackingFault(void* address, u64 access_type, bool* mapping_failed) {
    if (mapping_failed)
        *mapping_failed = false;
    auto* thread = Libraries::Kernel::g_curthread;
    if (thread)
        thread->critical_count.fetch_add(1, std::memory_order_acq_rel);
    auto leave_critical = ::detail::MakeScopeGuard([&] {
        if (thread)
            thread->critical_count.fetch_sub(1, std::memory_order_acq_rel);
    });
    auto* backing = Impl::active_backing.load();
    if (!backing)
        return false;
    const auto offset =
        reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(backing->backing_base);
    if (offset < BackingSize) {
        std::scoped_lock lock{backing->mutex};
        if (!backing->MapBackingView(address)) {
            MEMORY_BASIC_INFORMATION info{};
            if (!VirtualQuery(address, &info, sizeof(info)) || info.Type != MEM_MAPPED) {
                if (mapping_failed)
                    *mapping_failed = true;
                return false;
            }
        }
        backing->CommitPhysical(offset);
        return true;
    }
    return backing->MapGuestView(address, access_type, mapping_failed);
}

void AddressSpace::PinMappings(VAddr address, u64 size, bool protect) {
    MemoryCriticalScope critical;
    if (!address || !size || address > UINT64_MAX - size)
        return;
    std::scoped_lock lock{impl->mutex};
    const auto key = std::pair{address, size};
    if (protect) {
        ++impl->pinned_ranges[key];
    } else if (auto it = impl->pinned_ranges.find(key); it != impl->pinned_ranges.end()) {
        if (--it->second == 0)
            impl->pinned_ranges.erase(it);
    }
}

std::shared_ptr<void> AddressSpace::AcquireHostAccess(const void* address, u64 size,
                                                      bool populate) {
    MemoryCriticalScope critical;
    const auto begin = reinterpret_cast<VAddr>(address);
    if (!begin || !size || begin > UINT64_MAX - size)
        return {};
    auto* backing = Impl::active_backing.load(std::memory_order_acquire);
    if (!backing)
        return {};
    std::scoped_lock lock{backing->mutex};
    const auto physical = begin - reinterpret_cast<VAddr>(backing->backing_base);
    if (physical < BackingSize) {
        if (size > BackingSize - physical)
            throw std::runtime_error("Host buffer exceeds physical memory");
        const auto key = std::pair{physical, size};
        ++backing->pinned_physical[key];
        auto lease = std::shared_ptr<void>{backing, [key](void* pointer) {
                                               MemoryCriticalScope critical;
                                               auto* backing = static_cast<Impl*>(pointer);
                                               std::scoped_lock lock{backing->mutex};
                                               if (--backing->pinned_physical[key] == 0)
                                                   backing->pinned_physical.erase(key);
                                           }};
        for (auto offset = physical; populate && offset < physical + size;) {
            if (!backing->MapBackingView(backing->backing_base + offset, true,
                                         Impl::MinimumGuestViewSize))
                throw std::runtime_error("Unable to populate host physical buffer: " +
                                         Common::GetLastErrorMsg());
            backing->CommitPhysical(offset);
            offset =
                std::min(physical + size, Common::AlignDown(offset, Impl::MinimumGuestViewSize) +
                                              Impl::MinimumGuestViewSize);
        }
        return lease;
    }
    auto it = backing->regions.upper_bound(begin);
    if (it == backing->regions.begin())
        return {};
    const auto& region = std::prev(it)->second;
    if (!region.is_mapped || begin - region.base >= region.size)
        return {};
    const auto key = std::pair{begin, size};
    ++backing->pinned_ranges[key];
    auto lease = std::shared_ptr<void>{backing, [key](void* pointer) {
                                           MemoryCriticalScope critical;
                                           auto* backing = static_cast<Impl*>(pointer);
                                           std::scoped_lock lock{backing->mutex};
                                           if (--backing->pinned_ranges[key] == 0)
                                               backing->pinned_ranges.erase(key);
                                       }};
    for (auto cursor = begin; cursor < begin + size;) {
        auto next = backing->regions.upper_bound(cursor);
        if (next == backing->regions.begin())
            throw std::runtime_error("Unmapped host guest buffer");
        const auto& part = std::prev(next)->second;
        if (!part.is_mapped || cursor - part.base >= part.size)
            throw std::runtime_error("Host guest buffer contains an unmapped gap");
        if (part.fd == -1 && part.phys_base != PAddr(-1)) {
            bool failed{};
            backing->MapGuestView(reinterpret_cast<void*>(cursor), 0, &failed);
            if (failed)
                throw std::runtime_error("Unable to populate host guest buffer");
        }
        cursor = std::min(begin + size, Common::AlignDown(cursor, u64{4096}) + 4096);
    }
    return lease;
}

bool AddressSpace::CopyBacking(void* destination, const void* source, u64 size) {
    MemoryCriticalScope critical;
    auto* backing = Impl::active_backing.load(std::memory_order_acquire);
    if (!backing || !size)
        return false;
    auto* output = static_cast<u8*>(destination);
    auto* input = static_cast<const u8*>(source);
    const auto base = reinterpret_cast<VAddr>(backing->backing_base);
    const auto src = reinterpret_cast<VAddr>(source) - base;
    const auto dst = reinterpret_cast<VAddr>(destination) - base;
    if (src >= BackingSize && dst >= BackingSize)
        return false;
    if ((src < BackingSize && size > BackingSize - src) ||
        (dst < BackingSize && size > BackingSize - dst))
        throw std::runtime_error("Physical memory copy exceeds backing");
    while (size) {
        auto chunk = std::min(size, Impl::MinimumGuestViewSize);
        if (src < BackingSize)
            chunk = std::min(chunk,
                             Impl::MinimumGuestViewSize - (reinterpret_cast<VAddr>(input) - base) %
                                                              Impl::MinimumGuestViewSize);
        if (dst < BackingSize)
            chunk = std::min(chunk,
                             Impl::MinimumGuestViewSize - (reinterpret_cast<VAddr>(output) - base) %
                                                              Impl::MinimumGuestViewSize);
        std::scoped_lock lock{backing->mutex};
        auto read = AcquireHostAccess(input, chunk, false);
        auto write = AcquireHostAccess(output, chunk, false);
        std::array<void*, 2> scratch{};
        auto cleanup = ::detail::MakeScopeGuard([&] {
            for (auto* view : scratch)
                if (view)
                    UnmapViewOfFile2(backing->process, view, 0);
        });
        const auto access = [&](const void* pointer, u32 slot) -> void* {
            const auto offset = reinterpret_cast<VAddr>(pointer) - base;
            if (offset >= BackingSize)
                return const_cast<void*>(pointer);
            const auto view = std::prev(backing->backing_views.upper_bound(offset));
            if (view->second.is_resident) {
                if (!backing->MapBackingView(const_cast<void*>(pointer)))
                    throw std::runtime_error("Unable to restore physical copy protection");
                return const_cast<void*>(pointer);
            }
            if (!backing->swap_enabled) {
                if (!backing->MapBackingView(const_cast<void*>(pointer), true,
                                             Impl::MinimumGuestViewSize))
                    throw std::runtime_error("Unable to populate physical copy");
                backing->CommitPhysical(offset);
                return const_cast<void*>(pointer);
            }
            if (backing->MapBackingView(const_cast<void*>(pointer), false,
                                        Impl::MinimumGuestViewSize))
                return const_cast<void*>(pointer);
            const auto index = offset / Impl::BackingViewSize;
            const auto aligned = Common::AlignDown(offset, Impl::MinimumGuestViewSize);
            const auto length = std::min(Impl::MinimumGuestViewSize, BackingSize - aligned);
            const auto map = [&] {
                const auto handle = backing->GetRamPage(index);
                return handle ? MapHostView(handle, backing->process, nullptr,
                                            aligned % Impl::BackingViewSize, length, 0,
                                            PAGE_READWRITE, nullptr, 0)
                              : nullptr;
            };
            scratch[slot] = map();
            if (!scratch[slot] && Impl::IsMappingResourceError(GetLastError())) {
                backing->ReleaseIdleGuestViews(nullptr);
                backing->ReleaseSwapSections(index);
                scratch[slot] = map();
            }
            if (!scratch[slot])
                backing->MappingFailure("store_view_map", GetLastError(),
                                        reinterpret_cast<VAddr>(pointer), offset, length,
                                        PAGE_READWRITE, aligned % Impl::BackingViewSize);
            if (!scratch[slot])
                throw std::runtime_error("Unable to access memory store: " +
                                         Common::GetLastErrorMsg());
            ++backing->pager_store_copies;
            return static_cast<u8*>(scratch[slot]) + offset - aligned;
        };
        const auto* from = access(input, 0);
        auto* to = access(output, 1);
        std::memcpy(to, from, chunk);
        output += chunk;
        input += chunk;
        size -= chunk;
    }
    return true;
}
#endif

void* AddressSpace::Map(VAddr virtual_addr, u64 size, PAddr phys_addr, bool is_exec) {
    const VAddr end_addr = virtual_addr + size - 1;
    if (phys_addr != -1 && std::bit_width(end_addr) <= Traits::ADDRESS_SPACE_BITS) {
        const u64 base_page = virtual_addr >> Traits::PAGE_BITS;
        for (u64 offset = 0; offset < size; offset += 16_KB) {
            backing_pages[base_page + (offset >> Traits::PAGE_BITS)] =
                backing_base + phys_addr + offset;
        }
    }
#if ARCH_X86_64
    const auto prot = is_exec ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
#else
    // On non-native architectures, we can simplify things by ignoring the execute flag for the
    // canonical copy of the memory and rely on the JIT to map translated code as executable.
    constexpr auto prot = PAGE_READWRITE;
#endif
    return impl->Map(virtual_addr, phys_addr, size, prot);
}

void* AddressSpace::MapFile(VAddr virtual_addr, u64 size, u64 offset, u32 prot, uintptr_t fd) {
#ifdef _WIN32
    return impl->Map(virtual_addr, offset, size,
                     ToWindowsProt(std::bit_cast<Core::MemoryProt>(prot)), fd);
#else
    return impl->Map(virtual_addr, offset, size, ToPosixProt(std::bit_cast<Core::MemoryProt>(prot)),
                     fd);
#endif
}

VAddr AddressSpace::Unmap(VAddr virtual_addr, u64* size) {
    const VAddr end_addr = virtual_addr + *size - 1;
    if (std::bit_width(end_addr) <= Traits::ADDRESS_SPACE_BITS) {
        const u64 base_page = virtual_addr >> Traits::PAGE_BITS;
        for (u64 offset = 0; offset < *size; offset += 16_KB) {
            backing_pages[base_page + (offset >> Traits::PAGE_BITS)] = nullptr;
        }
    }
    return impl->Unmap(virtual_addr, size);
}

static std::atomic<ProtectObserver> g_protect_observer{nullptr};
static std::atomic<void*> g_protect_observer_user{nullptr};
static thread_local bool g_tracker_origin_protect = false;

void SetProtectObserver(ProtectObserver observer, void* user) {
    g_protect_observer_user.store(user, std::memory_order_release);
    g_protect_observer.store(observer, std::memory_order_release);
}

void NotifyProtectObserver(VAddr virtual_addr, u64 size, bool write_granted) {
    if (const auto observer = g_protect_observer.load(std::memory_order_acquire)) {
        observer(g_protect_observer_user.load(std::memory_order_acquire), virtual_addr, size,
                 write_granted, g_tracker_origin_protect);
    }
}

TrackerProtectScope::TrackerProtectScope() {
    g_tracker_origin_protect = true;
}

TrackerProtectScope::~TrackerProtectScope() {
    g_tracker_origin_protect = false;
}

void AddressSpace::Protect(VAddr virtual_addr, u64 size, MemoryPermission perms) {
    const bool read = True(perms & MemoryPermission::Read);
    const bool write = True(perms & MemoryPermission::Write);
    const bool execute = True(perms & MemoryPermission::Execute);
    NotifyProtectObserver(virtual_addr, size, write);
    return impl->Protect(virtual_addr, size, read, write, execute);
}

boost::icl::interval_set<VAddr> AddressSpace::GetUsableRegions() {
#ifdef _WIN32
    // On Windows, we need to obtain the accessible intervals from the implementation's regions.
    return impl->GetUsableRegions();
#else
    // On Linux and Mac, the memory space is fully represented by the three major regions
    boost::icl::interval_set<VAddr> reserved_regions;
    VAddr system_managed_addr = reinterpret_cast<VAddr>(system_managed_base);
    VAddr system_reserved_addr = reinterpret_cast<VAddr>(system_reserved_base);
    VAddr user_addr = reinterpret_cast<VAddr>(user_base);

    reserved_regions.insert({system_managed_addr, system_managed_addr + system_managed_size});
    reserved_regions.insert({system_reserved_addr, system_reserved_addr + system_reserved_size});
    reserved_regions.insert({user_addr, user_addr + user_size});
    return reserved_regions;
#endif
}

} // namespace Core
