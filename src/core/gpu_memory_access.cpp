// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <stdexcept>
#include "common/alignment.h"
#include "common/signal_context.h"
#include "core/memory.h"
#include "core/signals.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace Core {
namespace {
std::atomic<MemoryManager*> gpu_fault_memory{};
} // namespace
struct MemoryManager::GpuAccessLease {
    explicit GpuAccessLease(MemoryManager& manager)
        : scope{&manager}, copies{manager.gpu_copy_mutex} {}
    ~GpuAccessLease() {
        if (memory)
            memory->ReleaseGpuAccess();
    }
    GuestCopyScope scope;
    std::unique_lock<Common::SharedFirstMutex> copies;
    MemoryManager* memory{};
};

std::shared_lock<Common::SharedFirstMutex> MemoryManager::LockGpuAccess() {
    std::shared_lock lock{gpu_copy_mutex, std::defer_lock};
    if (gpu_access_memory != this)
        lock.lock();
    return lock;
}

std::shared_ptr<void>
MemoryManager::AcquireGpuAccess(std::span<const std::pair<VAddr, u64>> ranges) {
#ifdef _WIN32
    Signals::Instance();
#endif
    if (gpu_access_memory)
        throw std::runtime_error("Nested guest GPU memory ownership is not supported");
    auto lease = std::make_shared<GpuAccessLease>(*this);
    std::unique_lock lock{gpu_access_mutex};
    std::vector<std::pair<PAddr, u64>> physical;
    for (const auto& [address, size] : ranges) {
        if (!address || !size)
            continue;
        if (address > UINT64_MAX - size || size > 256ULL * 1024 * 1024)
            throw std::runtime_error("Invalid guest GPU ownership range");
        std::vector<BackingSpan> spans((size + 4095) / 4096 + 1);
        const u32 count = ResolveBackingSpans(address, size, spans.data(), spans.size(), false);
        if (!count)
            throw std::runtime_error("Guest GPU ownership requires mapped physical backing");
        u64 total{};
        for (u32 i = 0; i < count; ++i) {
            const PAddr start =
                reinterpret_cast<VAddr>(spans[i].ptr) - reinterpret_cast<VAddr>(impl.BackingBase());
            if (!spans[i].size || spans[i].size > size - total ||
                start > UINT64_MAX - spans[i].size - 4095)
                throw std::runtime_error("Invalid guest GPU ownership backing");
            const auto begin = Common::AlignDown(start, u64{4096});
            const auto end = Common::AlignUp(start + spans[i].size, u64{4096});
            physical.emplace_back(begin, end - begin);
            total += spans[i].size;
        }
        if (total != size)
            throw std::runtime_error("Incomplete guest GPU ownership backing");
    }
    std::ranges::sort(physical);
    std::vector<std::pair<PAddr, u64>> merged;
    for (const auto& [address, size] : physical) {
        if (merged.empty() || address > merged.back().first + merged.back().second)
            merged.emplace_back(address, size);
        else
            merged.back().second =
                std::max(merged.back().first + merged.back().second, address + size) -
                merged.back().first;
    }
    std::vector<GpuProtectedRange> protected_ranges;
    for (const auto& [base, vma] : vma_map) {
        if (!HasPhysicalBacking(vma))
            continue;
        const auto permissions = MemoryPermission(u32(vma.prot) & 7);
        if (permissions == MemoryPermission::None)
            continue;
        for (const auto& [offset, area] : vma.phys_areas) {
            for (const auto& [address, size] : merged) {
                if (address >= area.base + area.size)
                    break;
                const auto begin = std::max(area.base, address);
                const auto end = std::min(area.base + area.size, address + size);
                if (begin < end)
                    protected_ranges.push_back(
                        {base + offset + begin - area.base, end - begin, permissions});
            }
        }
    }
    std::ranges::sort(protected_ranges, {}, &GpuProtectedRange::address);
    gpu_protected_ranges = std::move(protected_ranges);
    gpu_access_owner = std::this_thread::get_id();
    gpu_access_memory = this;
    gpu_fault_memory.store(this, std::memory_order_release);
    ++gpu_access_epoch;
    lease->memory = this;
    for (const auto& range : gpu_protected_ranges)
        impl.Protect(range.address, range.size, MemoryPermission::None);
    return lease;
}

void MemoryManager::ReleaseGpuAccess() {
    std::unique_lock lock{gpu_access_mutex};
    for (const auto& range : gpu_protected_ranges)
        impl.Protect(range.address, range.size, range.permissions);
    gpu_protected_ranges.clear();
    gpu_access_owner = {};
    gpu_access_memory = nullptr;
    ++gpu_access_epoch;
    lock.unlock();
    gpu_access_cv.notify_all();
}

bool MemoryManager::GpuAccessFault(void* context, void* address) {
    const auto memory = gpu_fault_memory.load(std::memory_order_acquire);
    return memory && memory->HandleGpuAccessFault(context, reinterpret_cast<VAddr>(address));
}

void MemoryManager::DetachGpuAccess() {
    auto expected = this;
    gpu_fault_memory.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
}

bool MemoryManager::HandleGpuAccessFault(void* context, VAddr address) {
    std::unique_lock lock{gpu_access_mutex};
    if (!gpu_access_epoch || gpu_access_owner == std::this_thread::get_id())
        return false;
    const auto owned = [&] {
        const auto it = std::ranges::upper_bound(gpu_protected_ranges, address, {},
                                                 &GpuProtectedRange::address);
        if (it == gpu_protected_ranges.begin())
            return false;
        const auto& range = *std::prev(it);
        return address - range.address < range.size;
    };
    bool waited{};
    while (owned()) {
        waited = true;
        gpu_access_cv.wait(lock);
    }
    if (waited)
        return true;
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION info{};
    if (!::VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) ||
        info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return false;
    const DWORD protection = info.Protect & 0xff;
    const bool permitted =
        Common::IsExecuteError(context)
            ? protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
                  protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY
        : Common::IsWriteError(context)
            ? protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
                  protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY
            : protection == PAGE_READONLY || protection == PAGE_READWRITE ||
                  protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_READ ||
                  protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
    static thread_local VAddr last_address{};
    static thread_local void* last_instruction{};
    static thread_local u64 last_epoch{};
    const auto instruction = Common::GetRip(context);
    if (permitted && (last_address != address || last_instruction != instruction ||
                      last_epoch != gpu_access_epoch)) {
        last_address = address;
        last_instruction = instruction;
        last_epoch = gpu_access_epoch;
        return true;
    }
#endif
    return false;
}
} // namespace Core
