// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include "../guest_memory.h"
#include "memory_backend.h"
#include "../memory_diagnostics.h"

namespace {

auto& MemoryState() {
    using State = std::atomic<std::shared_ptr<Core::Uwp::GuestMemory>>;
    alignas(State) static std::byte storage[sizeof(State)]{};
    static auto* const state = std::construct_at(reinterpret_cast<State*>(storage));
    return *state;
}
std::atomic<void (*)(const char*)> logger;

struct MemoryLease {
    std::weak_ptr<Core::Uwp::GuestMemory> owner;
    std::vector<uint64_t> pins;
};

template <typename F>
int Guard(F&& operation) noexcept {
    try {
        return operation() ? 1 : 0;
    } catch (const std::bad_alloc&) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    } catch (const std::invalid_argument&) {
        SetLastError(ERROR_INVALID_PARAMETER);
    } catch (...) {
        const auto error = GetLastError();
        SetLastError(error ? error : ERROR_GEN_FAILURE);
    }
    return 0;
}

auto Current() {
    auto current = MemoryState().load(std::memory_order_acquire);
    if (!current)
        SetLastError(ERROR_INVALID_STATE);
    return current;
}

} // namespace

extern "C" {

void uwp_memory_log_diagnostic(const char* message) {
    const auto error = GetLastError();
    try {
        if (auto callback = logger.load(std::memory_order_acquire))
            callback(message);
    } catch (...) {
    }
    SetLastError(error);
}

void uwp_memory_diagnostics(void) {
    Core::Uwp::MemoryDiagnostics::Report(logger.load(std::memory_order_acquire));
}

void uwp_memory_set_log(void (*callback)(const char*)) {
    logger.store(callback, std::memory_order_release);
}

int uwp_memory_init(uint64_t size, uint64_t direct, uintptr_t canonical, const char* path,
                    int enabled) {
    return Guard([&] {
        if (MemoryState().load(std::memory_order_acquire)) {
            SetLastError(ERROR_ALREADY_INITIALIZED);
            return false;
        }
        auto current = std::make_shared<Core::Uwp::GuestMemory>(size, direct, canonical);
        const char* configured = std::getenv("SHADPS4_UWP_SWAP_FILE");
        if (configured && *configured)
            path = configured;
        const char* flag = std::getenv("SHADPS4_UWP_SWAP");
        enabled = enabled && !(flag && *flag == '0');
        if (enabled) {
            const auto length =
                MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, nullptr, 0);
            if (!length)
                return false;
            std::vector<wchar_t> wide(length);
            if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide.data(),
                                     length) ||
                !current->OpenStore(wide.data()))
                return false;
        }
        MemoryState().store(std::move(current), std::memory_order_release);
        return true;
    });
}

void uwp_memory_start(void) {
    Guard([] {
        auto current = Current();
        if (!current)
            return false;
        current->Start();
        return true;
    });
}

void uwp_memory_stop(void) {
    Guard([] {
        if (auto current = MemoryState().load(std::memory_order_acquire))
            current->Stop();
        return true;
    });
}

void uwp_memory_shutdown(void) {
    Guard([] {
        if (auto current = MemoryState().exchange(nullptr, std::memory_order_acq_rel))
            current->Stop();
        return true;
    });
}

int uwp_memory_map(uintptr_t address, uint64_t size, uint64_t physical, int protection, int) {
    return Guard([&] {
        auto current = Current();
        return current && current->Map(address, size, physical, protection);
    });
}

int uwp_memory_unmap(uintptr_t address, uint64_t size) {
    return Guard([&] {
        auto current = Current();
        return current && current->Unmap(address, size);
    });
}

int uwp_memory_protect(uintptr_t address, uint64_t size, int protection) {
    return Guard([&] {
        auto current = Current();
        return current && current->ProtectRange(address, size, protection);
    });
}

int uwp_memory_fault(uintptr_t address, int access, int, int* failed) {
    if (!failed)
        return 0;
    bool failure{};
    const auto result = Guard([&] {
        auto current = Current();
        if (!current)
            return false;
        try {
            return current->Fault(address, access, failure);
        } catch (...) {
            failure = true;
            throw;
        }
    });
    *failed = failure;
    return result;
}

int uwp_memory_acquire(uintptr_t address, uint64_t size, int populate, void** output) {
    if (!output) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    *output = nullptr;
    const auto result = Guard([&] {
        auto current = Current();
        if (!current)
            return false;
        auto lease = std::make_unique<MemoryLease>();
        lease->owner = current;
        lease->pins = current->Acquire(address, size, populate != 0);
        *output = lease.release();
        return true;
    });
    if (!result) {
        const auto error = GetLastError();
        if (const auto log = logger.load(std::memory_order_acquire)) {
            char message[256]{};
            std::snprintf(message, sizeof(message),
                          "[KytyMemory] acquire_failed address=0x%llx size=0x%llx "
                          "populate=%d error=%lu",
                          static_cast<uint64_t>(address), size, populate, error);
            log(message);
        }
        uwp_memory_report();
        SetLastError(error);
    }
    return result;
}

void uwp_memory_release(void* opaque) {
    std::unique_ptr<MemoryLease> lease{static_cast<MemoryLease*>(opaque)};
    if (!lease)
        return;
    Guard([&] {
        if (auto current = lease->owner.lock())
            current->Release(lease->pins);
        return true;
    });
}

void uwp_memory_free_lease(void* opaque) {
    delete static_cast<MemoryLease*>(opaque);
}

int uwp_memory_copy(void* destination, const void* source, uint64_t size) {
    return Guard([&] {
        auto current = Current();
        return current && current->Copy(destination, source, size);
    });
}

int uwp_memory_discard(uint64_t physical, uint64_t size) {
    return Guard([&] {
        auto current = Current();
        return current && current->Discard(physical, size);
    });
}

uint64_t uwp_memory_reclaim(uint64_t bytes, int urgent) {
    uint64_t reclaimed{};
    Guard([&] {
        auto current = Current();
        if (!current)
            return false;
        reclaimed = current->Reclaim(bytes, urgent != 0);
        return true;
    });
    return reclaimed;
}

void uwp_memory_report(void) {
    Guard([] {
        if (auto current = MemoryState().load(std::memory_order_acquire))
            current->Report(logger.load(std::memory_order_acquire));
        return true;
    });
}

} // extern "C"
