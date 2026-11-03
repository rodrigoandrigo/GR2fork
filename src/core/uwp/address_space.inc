// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

struct MemoryCriticalScope {
    Libraries::Kernel::Pthread* thread = Libraries::Kernel::g_curthread;
    MemoryCriticalScope() {
        if (thread)
            thread->critical_count.fetch_add(1, std::memory_order_acq_rel);
    }
    ~MemoryCriticalScope() {
        if (thread)
            thread->critical_count.fetch_sub(1, std::memory_order_acq_rel);
    }
};

struct AddressSpace::Impl {
    Impl() : process{GetCurrentProcess()} {
        auto rollback = ::detail::MakeScopeGuard([&] {
            StopPager();
            active_backing.store(nullptr, std::memory_order_release);
            if (memory_initialized)
                uwp_memory_shutdown();
            for (const auto& [base, size] : reservations)
                ReleaseReservation(base, size);
            if (backing_base)
                ReleaseReservation(reinterpret_cast<VAddr>(backing_base), BackingSize);
        });
        SYSTEM_INFO sys_info{};
        GetSystemInfo(&sys_info);
        u64 alignment = sys_info.dwAllocationGranularity;
        auto ntdll_handle = GetModuleHandleW(L"ntdll.dll");
        ASSERT_MSG(ntdll_handle, "Failed to retrieve ntdll handle");
        s64(WINAPI * RtlGetVersion)(LPOSVERSIONINFOW);
        *(FARPROC*)&RtlGetVersion = GetProcAddress(ntdll_handle, "RtlGetVersion");
        ASSERT_MSG(RtlGetVersion, "failed to retrieve function pointer for RtlGetVersion");
        RTL_OSVERSIONINFOW os_version_info{};
        RtlGetVersion(&os_version_info);

        u64 supported_user_max = USER_MAX;
        static constexpr s32 AffectedBuildNumber = 22621;
        s32 sdk_ver = Common::ElfInfo::Instance().CompiledSdkVer();
        if (os_version_info.dwBuildNumber <= AffectedBuildNumber ||
            sdk_ver >= Common::ElfInfo::FW_300) {
            supported_user_max = 0x10000000000ULL;
            if (sdk_ver < Common::ElfInfo::FW_300) {
                LOG_WARNING(
                    Core,
                    "Older Windows version detected, reducing user max to {:#x} to avoid problems",
                    supported_user_max);
            }
        }
        VAddr next_addr = SYSTEM_MANAGED_MIN;
        MEMORY_BASIC_INFORMATION info{};
        while (next_addr <= supported_user_max) {
            ASSERT_MSG(VirtualQuery(reinterpret_cast<PVOID>(next_addr), &info, sizeof(info)),
                       "Failed to query memory information for address {:#x}", next_addr);
            next_addr = reinterpret_cast<VAddr>(info.BaseAddress) + info.RegionSize;
            next_addr = Common::AlignUp(next_addr, alignment);
            u64 size = info.RegionSize;
            if (next_addr > supported_user_max) {
                size -= (next_addr - supported_user_max);
            }
            size = Common::AlignDown(size, alignment);
            if (info.State == MEM_FREE && info.RegionSize > 0x1000000) {
                VAddr addr = Common::AlignUp(reinterpret_cast<VAddr>(info.BaseAddress), alignment);
                regions.emplace(addr,
                                MemoryRegion{addr, PAddr(-1), size, PAGE_NOACCESS, -1, false});
            }
        }
        for (auto region : regions) {
            auto addr = static_cast<u8*>(VirtualAlloc2(
                process, reinterpret_cast<PVOID>(region.second.base), region.second.size,
                MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0));
            ASSERT_MSG(addr, "Unable to reserve virtual address space: {}",
                       Common::GetLastErrorMsg());
            reservations.emplace_back(region.second.base, region.second.size);
        }
        system_managed_base = reinterpret_cast<u8*>(regions.begin()->first);
        system_managed_size = SystemManagedSize - (regions.begin()->first - SYSTEM_MANAGED_MIN);
        system_reserved_base = reinterpret_cast<u8*>(SYSTEM_RESERVED_MIN);
        system_reserved_size = SystemReservedSize;
        user_base = reinterpret_cast<u8*>(USER_MIN);
        user_size = supported_user_max - USER_MIN - 1;
        BackingSize = ORBIS_KERNEL_TOTAL_MEM_DEV_PRO + ORBIS_KERNEL_FLEXIBLE_MEMORY_SIZE +
                      EmulatorSettings.GetExtraDmemInMBytes() * 1_MB +
                      EmulatorSettings.GetExtraFmemInMBytes() * 1_MB;

        backing_base = static_cast<u8*>(VirtualAlloc2(process, nullptr, BackingSize,
                                                      MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                                      PAGE_NOACCESS, nullptr, 0));
        Check(backing_base != nullptr, "reserve canonical backing");
        const auto directory = Common::FS::GetUserPath(Common::FS::PathType::CacheDir) / "swap";
        std::filesystem::create_directories(directory);
        const auto path =
            directory / fmt::format("guest-{}-{}.swap", GetCurrentProcessId(), GetTickCount64());
        const auto utf8 = path.u8string();
        uwp_memory_set_log([](const char* message) noexcept {
            try {
                LOG_INFO(Core, "{}", message);
            } catch (...) {
            }
        });
        memory_initialized = true;
        Check(uwp_memory_init(BackingSize,
                              ORBIS_KERNEL_TOTAL_MEM_DEV_PRO +
                                  (EmulatorSettings.GetExtraDmemInMBytes() +
                                   EmulatorSettings.GetExtraFmemInMBytes()) *
                                      1_MB,
                              reinterpret_cast<uintptr_t>(backing_base),
                              reinterpret_cast<const char*>(utf8.c_str()),
                              EmulatorSettings.IsMemorySwapEnabled()),
              "initialize guest memory");
        active_backing.store(this, std::memory_order_release);
        LOG_INFO(Core, "UWP memory: reference private-first backing, 16 KiB demand pages, "
                       "compressed swap, aliases promoted on demand");
        StartPager();
        rollback.Cancel();
    }

    static void Check(bool success, const char* operation) {
        if (!success)
            throw std::runtime_error(fmt::format("{}: {}", operation, Common::GetLastErrorMsg()));
    }

    ~Impl() {
        StopPager();
        active_backing.store(nullptr, std::memory_order_release);
        mapping_leases.clear();
        lifetime.reset();
        uwp_memory_shutdown();
        for (const auto& [base, region] : regions) {
            if (region.is_mapped && (region.phys_base == PAddr(-1) || region.prot == PAGE_READONLY))
                VirtualFree(reinterpret_cast<void*>(base), region.size,
                            MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
            else if (region.is_mapped && region.fd != -1)
                UnmapViewOfFile2(process, reinterpret_cast<void*>(base), MEM_PRESERVE_PLACEHOLDER);
        }
        for (const auto& [base, size] : reservations)
            ReleaseReservation(base, size);
        ReleaseReservation(reinterpret_cast<VAddr>(backing_base), BackingSize);
    }

    static void ReleaseReservation(VAddr base, u64 size) {
        for (auto at = base; at < base + size;) {
            MEMORY_BASIC_INFORMATION info{};
            if (!VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info)))
                break;
            const auto allocation = reinterpret_cast<VAddr>(info.AllocationBase);
            const auto end = reinterpret_cast<VAddr>(info.BaseAddress) + info.RegionSize;
            if (allocation >= base && allocation < base + size)
                VirtualFree(reinterpret_cast<void*>(allocation), 0, MEM_RELEASE);
            at = std::max(at + 4096, end);
        }
    }

    static int Protection(ULONG prot) {
        switch (prot & 0xff) {
        case PAGE_READONLY:
            return 1;
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
            return 3;
        case PAGE_EXECUTE:
            return 4;
        case PAGE_EXECUTE_READ:
            return 5;
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return 7;
        default:
            return 0;
        }
    }

    void PreparePlaceholder(VAddr base, u64 size) {
        const auto end = base + size;
        for (auto at = base; at < end;) {
            MEMORY_BASIC_INFORMATION info{};
            Check(VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info)) &&
                      info.State == MEM_RESERVE,
                  "query placeholder");
            const auto first = reinterpret_cast<VAddr>(info.AllocationBase);
            const auto last = reinterpret_cast<VAddr>(info.BaseAddress) + info.RegionSize;
            if (first < at)
                Check(VirtualFree(reinterpret_cast<void*>(first), at - first,
                                  MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER),
                      "split placeholder prefix");
            if (last > end)
                Check(VirtualFree(reinterpret_cast<void*>(at), end - at,
                                  MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER),
                      "split placeholder suffix");
            at = std::min(last, end);
        }
        MEMORY_BASIC_INFORMATION info{};
        Check(VirtualQuery(reinterpret_cast<void*>(base), &info, sizeof(info)),
              "query split range");
        if (info.RegionSize < size)
            Check(VirtualFree(reinterpret_cast<void*>(base), size,
                              MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS),
                  "coalesce file placeholder");
    }

    static void ProtectHostRange(VAddr base, u64 size, ULONG protection) {
        for (auto at = base; at < base + size;) {
            MEMORY_BASIC_INFORMATION info{};
            Check(VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info)),
                  "query host protection");
            const auto end =
                std::min(base + size, reinterpret_cast<VAddr>(info.BaseAddress) + info.RegionSize);
            DWORD old{};
            Check(VirtualProtect(reinterpret_cast<void*>(at), end - at, protection, &old),
                  "protect host range");
            at = end;
        }
    }

    void SplitRegion(VAddr address) {
        auto it = regions.upper_bound(address);
        Check(it != regions.begin(), "locate guest mapping");
        --it;
        auto& region = it->second;
        if (address == region.base || address == region.base + region.size)
            return;
        Check(address > region.base && address < region.base + region.size, "split guest mapping");
        if (region.is_mapped && region.fd != -1) {
            const auto original = region;
            std::vector<MEMORY_BASIC_INFORMATION> protections;
            for (auto at = original.base; at < original.base + original.size;) {
                MEMORY_BASIC_INFORMATION info{};
                Check(VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info)),
                      "query file protection");
                protections.push_back(info);
                at = reinterpret_cast<VAddr>(info.BaseAddress) + info.RegionSize;
            }
            std::vector<u8> snapshot;
            if (original.prot == PAGE_READONLY) {
                snapshot.resize(original.size);
                for (const auto& info : protections) {
                    const auto first =
                        std::max(original.base, reinterpret_cast<VAddr>(info.BaseAddress));
                    const auto last =
                        std::min(original.base + original.size,
                                 reinterpret_cast<VAddr>(info.BaseAddress) + info.RegionSize);
                    DWORD old{};
                    Check(VirtualProtect(reinterpret_cast<void*>(first), last - first,
                                         PAGE_READONLY, &old),
                          "read file snapshot");
                    std::memcpy(snapshot.data() + first - original.base,
                                reinterpret_cast<void*>(first), last - first);
                }
                Check(VirtualFree(reinterpret_cast<void*>(original.base), original.size,
                                  MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER),
                      "split file copy");
            } else {
                Check(UnmapViewOfFile2(process, reinterpret_cast<void*>(original.base),
                                       MEM_PRESERVE_PLACEHOLDER),
                      "split file view");
            }
            region.is_mapped = false;
            SplitRegion(address);
            Map(original.base, original.phys_base, address - original.base, original.prot,
                original.fd);
            Map(address, original.phys_base + address - original.base,
                original.base + original.size - address, original.prot, original.fd);
            if (!snapshot.empty()) {
                ProtectHostRange(original.base, original.size, PAGE_READWRITE);
                std::memcpy(reinterpret_cast<void*>(original.base), snapshot.data(),
                            snapshot.size());
            }
            for (const auto& info : protections) {
                const auto first =
                    std::max(original.base, reinterpret_cast<VAddr>(info.BaseAddress));
                const auto last =
                    std::min(original.base + original.size,
                             reinterpret_cast<VAddr>(info.BaseAddress) + info.RegionSize);
                ProtectHostRange(first, last - first, info.Protect);
            }
            return;
        }
        Check(!region.is_mapped || (region.fd == -1 && region.phys_base != PAddr(-1)),
              "split nonphysical mapping");
        auto suffix = region;
        suffix.base = address;
        suffix.size -= address - region.base;
        if (suffix.phys_base != PAddr(-1))
            suffix.phys_base += address - region.base;
        region.size = address - region.base;
        regions.emplace(address, std::move(suffix));
    }

    void* Map(VAddr address, PAddr physical, u64 size, ULONG prot, uintptr_t fd = -1,
              s32 memory_type = 0) {
        MemoryCriticalScope critical;
        MemoryLock lock{mutex};
        SplitRegion(address);
        SplitRegion(address + size);
        auto it = regions.find(address);
        Check(it != regions.end() && it->second.size == size && !it->second.is_mapped,
              "map guest range");
        auto& region = it->second;
        if (fd == uintptr_t(-1) && physical != PAddr(-1)) {
            Check(uwp_memory_map(address, size, physical, Protection(prot),
                                 memory_type == 3 && size >= 16_MB),
                  "map private-first backing");
        } else {
            PreparePlaceholder(address, size);
            if (fd == uintptr_t(-1)) {
                Check(VirtualAlloc2(process, reinterpret_cast<void*>(address), size,
                                    MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, prot,
                                    nullptr, 0) != nullptr,
                      "map private guest range");
            } else if (prot == PAGE_READONLY) {
                auto* mapped = VirtualAlloc2(process, reinterpret_cast<void*>(address), size,
                                             MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
                                             PAGE_READWRITE, nullptr, 0);
                Check(mapped != nullptr, "allocate guest file copy");
                LARGE_INTEGER position{};
                Check(SetFilePointerEx(reinterpret_cast<HANDLE>(fd), {}, &position, FILE_CURRENT),
                      "query mapped file position");
                auto restore_position = ::detail::MakeScopeGuard([&] {
                    SetFilePointerEx(reinterpret_cast<HANDLE>(fd), position, nullptr, FILE_BEGIN);
                });
                u64 done{};
                while (done < size) {
                    OVERLAPPED offset{};
                    offset.Offset = static_cast<DWORD>(physical + done);
                    offset.OffsetHigh = static_cast<DWORD>((physical + done) >> 32);
                    DWORD count{};
                    Check(ReadFile(reinterpret_cast<HANDLE>(fd), static_cast<u8*>(mapped) + done,
                                   static_cast<DWORD>(std::min(size - done, 16_MB)), &count,
                                   &offset),
                          "read guest file");
                    if (!count)
                        break;
                    done += count;
                }
                DWORD old{};
                Check(VirtualProtect(mapped, size, prot, &old), "protect guest file copy");
            } else {
                Check(MapHostView(reinterpret_cast<HANDLE>(fd), process,
                                  reinterpret_cast<void*>(address), physical, size,
                                  MEM_REPLACE_PLACEHOLDER, prot, nullptr, 0) != nullptr,
                      "map guest file");
            }
        }
        region.phys_base = physical;
        region.fd = fd;
        region.prot = prot;
        region.is_mapped = true;
        return reinterpret_cast<void*>(address);
    }

    VAddr Unmap(VAddr address, u64* size) {
        MemoryCriticalScope critical;
        MemoryLock lock{mutex};
        SplitRegion(address);
        SplitRegion(address + *size);
        for (auto it = regions.lower_bound(address);
             it != regions.end() && it->first < address + *size; ++it) {
            auto& region = it->second;
            if (!region.is_mapped)
                continue;
            if (region.fd == -1 && region.phys_base != PAddr(-1)) {
                Check(uwp_memory_unmap(region.base, region.size), "unmap guest physical pages");
            } else if (region.fd != -1 && region.prot != PAGE_READONLY) {
                Check(UnmapViewOfFile2(process, reinterpret_cast<void*>(region.base),
                                       MEM_PRESERVE_PLACEHOLDER),
                      "unmap guest file");
            } else {
                Check(VirtualFree(reinterpret_cast<void*>(region.base), region.size,
                                  MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER),
                      "unmap private guest range");
            }
            region.is_mapped = false;
            region.phys_base = -1;
            region.fd = -1;
        }
        for (auto it = regions.begin(); it != regions.end();) {
            auto next = std::next(it);
            if (next != regions.end() && !it->second.is_mapped && !next->second.is_mapped &&
                it->first + it->second.size == next->first) {
                it->second.size += next->second.size;
                regions.erase(next);
            } else
                ++it;
        }
        return address;
    }

    void Protect(VAddr address, u64 size, bool read, bool write, bool execute) {
        MemoryCriticalScope critical;
        MemoryLock lock{mutex};
        const int prot = (read ? 1 : 0) | (write ? 3 : 0) | (execute ? 4 : 0);
        for (auto it = std::prev(regions.upper_bound(address));
             it != regions.end() && it->first < address + size; ++it) {
            const auto& region = it->second;
            if (!region.is_mapped)
                continue;
            auto first = std::max(address, region.base);
            auto last = std::min(address + size, region.base + region.size);
            if (region.fd == -1 && region.phys_base != PAddr(-1)) {
                Check(uwp_memory_protect(first, last - first, prot),
                      "protect guest physical pages");
            } else {
                DWORD old{};
                Check(VirtualProtect(reinterpret_cast<void*>(first), last - first,
                                     ToWindowsProt(static_cast<MemoryProt>(
                                         (read ? static_cast<u32>(MemoryProt::CpuRead) : 0) |
                                         (write ? static_cast<u32>(MemoryProt::CpuWrite) : 0) |
                                         (execute ? static_cast<u32>(MemoryProt::CpuExec) : 0))),
                                     &old),
                      "protect guest mapping");
            }
        }
    }

    boost::icl::interval_set<VAddr> GetUsableRegions() {
        boost::icl::interval_set<VAddr> result;
        for (const auto& [base, region] : regions)
            result.insert({base, base + region.size});
        return result;
    }

    void StartPager() {
        std::scoped_lock lock{pager_mutex};
        if (pager.joinable() || !EmulatorSettings.IsMemorySwapEnabled())
            return;
        pager = std::jthread([](std::stop_token stop) {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            bool draining{};
            u64 retry_at{};
            u64 unreclaimed_usage{};
            u64 retry_delay{40};
            auto reported = GetTickCount64();
            while (!stop.stop_requested()) {
                u64 reclaimed{};
                try {
                    const auto limit = winrt::Windows::System::MemoryManager::AppMemoryUsageLimit();
                    const auto used = winrt::Windows::System::MemoryManager::AppMemoryUsage();
                    if (limit > 1024_MB) {
                        if (used > limit - 640_MB)
                            draining = true;
                        if (used <= limit - 768_MB)
                            draining = false;
                        if (!draining) {
                            retry_at = 0;
                            retry_delay = 40;
                        } else if (GetTickCount64() >= retry_at ||
                                   (used > unreclaimed_usage &&
                                    used - unreclaimed_usage >= 32_MB)) {
                            reclaimed = uwp_memory_reclaim(2_MB, false);
                            if (reclaimed) {
                                retry_at = 0;
                                retry_delay = 40;
                            } else {
                                unreclaimed_usage = used;
                                retry_at = GetTickCount64() + retry_delay;
                                retry_delay = std::min<u64>(retry_delay * 2, 500);
                            }
                        }
                    }
                } catch (const winrt::hresult_error&) {
                }
                if (GetTickCount64() - reported >= 5000) {
                    uwp_memory_report();
                    if (auto driver = GetModuleHandleW(L"vulkan_dzn.dll")) {
                        using ReportCompiles = void (*)();
                        if (auto report = reinterpret_cast<ReportCompiles>(
                                GetProcAddress(driver, "mesa_uwp_report_compiles")))
                            report();
                    }
                    reported = GetTickCount64();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(reclaimed ? 1 : 40));
            }
            winrt::uninit_apartment();
        });
    }

    void StopPager() {
        std::scoped_lock lock{pager_mutex};
        if (pager.joinable()) {
            pager.request_stop();
            pager.join();
            uwp_memory_report();
        }
    }

    inline static std::atomic<Impl*> active_backing{};
    inline static std::atomic<u64> pressure_epoch{};
    std::shared_ptr<int> lifetime = std::make_shared<int>();
    std::map<std::pair<VAddr, u64>, std::vector<std::shared_ptr<void>>> mapping_leases;
    MemoryMutex mutex;
    bool memory_initialized{};
    HANDLE process{};
    u8* backing_base{};
    u8* system_managed_base{};
    u64 system_managed_size{};
    u8* system_reserved_base{};
    u64 system_reserved_size{};
    u8* user_base{};
    u64 user_size{};
    std::map<VAddr, MemoryRegion> regions;
    std::vector<std::pair<VAddr, u64>> reservations;
    std::mutex pager_mutex;
    std::jthread pager;
};
