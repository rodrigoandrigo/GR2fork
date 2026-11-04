// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

void AddressSpace::StartPager() {
    if (auto* backing = Impl::active_backing.load(std::memory_order_acquire))
        backing->StartPager();
}

void AddressSpace::StopPager() {
    if (auto* backing = Impl::active_backing.load(std::memory_order_acquire))
        backing->StopPager();
}

bool AddressSpace::HandleBackingFault(void* address, u64 access_type, bool* mapping_failed) {
    MemoryCriticalScope critical;
    if (mapping_failed)
        *mapping_failed = false;
    if (!Impl::active_backing.load(std::memory_order_acquire))
        return false;
    int failed{};
    const auto result =
        uwp_memory_fault(reinterpret_cast<uintptr_t>(address), static_cast<int>(access_type),
                         critical.thread != nullptr, &failed);
    if (mapping_failed)
        *mapping_failed = failed != 0;
    return result != 0;
}

void AddressSpace::PinMappings(VAddr address, u64 size, bool protect) {
    MemoryCriticalScope critical;
    MemoryLock lock{impl->mutex};
    const auto key = std::pair{address, size};
    if (protect) {
        impl->mapping_leases[key].push_back(
            AcquireHostAccess(reinterpret_cast<void*>(address), size, true));
    } else if (auto it = impl->mapping_leases.find(key); it != impl->mapping_leases.end()) {
        it->second.pop_back();
        if (it->second.empty())
            impl->mapping_leases.erase(it);
    }
}

std::shared_ptr<void> AddressSpace::AcquireHostAccess(const void* address, u64 size,
                                                      bool populate) {
    MemoryCriticalScope critical;
    if (!address || !size)
        return {};
    auto* backing = Impl::active_backing.load(std::memory_order_acquire);
    if (!backing)
        return {};
    void* lease{};
    if (!uwp_memory_acquire(reinterpret_cast<uintptr_t>(address), size, populate, &lease)) {
        const auto error = GetLastError();
        uwp_memory_release(lease);
        SetLastError(error);
        Impl::Check(false, "acquire memory lease");
    }
    return std::shared_ptr<void>{lease,
                                 [lifetime = std::weak_ptr<int>{backing->lifetime}](void* lease) {
                                     MemoryCriticalScope critical;
                                     if (!lifetime.expired())
                                         uwp_memory_release(lease);
                                     else
                                         uwp_memory_free_lease(lease);
                                 }};
}

bool AddressSpace::CopyBacking(void* destination, const void* source, u64 size) {
    MemoryCriticalScope critical;
    auto* backing = Impl::active_backing.load(std::memory_order_acquire);
    if (!backing || !size)
        return false;
    const auto base = reinterpret_cast<uintptr_t>(backing->backing_base);
    if (reinterpret_cast<uintptr_t>(source) - base >= BackingSize &&
        reinterpret_cast<uintptr_t>(destination) - base >= BackingSize)
        return false;
    Impl::Check(uwp_memory_copy(destination, source, size), "copy physical backing");
    return true;
}

void AddressSpace::DiscardPhysical(PAddr physical, u64 size) {
    MemoryCriticalScope critical;
    Impl::Check(uwp_memory_discard(physical, size), "discard physical backing");
}

u64 AddressSpace::ReclaimMemory(u64 size, bool urgent) {
    MemoryCriticalScope critical;
    if (!Impl::active_backing.load(std::memory_order_acquire))
        return 0;
    if (urgent)
        Impl::pressure_epoch.fetch_add(1, std::memory_order_release);
    return uwp_memory_reclaim(size, urgent);
}

u64 AddressSpace::MemoryHeadroom() {
    static std::atomic<u64> checked{};
    static std::atomic<u64> available{UINT64_MAX};
    const auto now = GetTickCount64();
    auto previous = checked.load(std::memory_order_relaxed);
    if (now - previous >= 100 && checked.compare_exchange_strong(previous, now)) {
        try {
            const auto limit = winrt::Windows::System::MemoryManager::AppMemoryUsageLimit();
            const auto used = winrt::Windows::System::MemoryManager::AppMemoryUsage();
            available.store(limit ? limit - std::min(used, limit) : UINT64_MAX,
                            std::memory_order_release);
        } catch (const winrt::hresult_error&) {
        }
    }
    return available.load(std::memory_order_acquire);
}

u64 AddressSpace::MemoryPressureEpoch() {
    return Impl::pressure_epoch.load(std::memory_order_acquire);
}
