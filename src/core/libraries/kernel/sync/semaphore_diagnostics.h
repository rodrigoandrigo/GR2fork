// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#ifdef _WIN64
#include <atomic>
#include <windows.h>
#include "common/logging/log.h"

namespace Libraries::Kernel::SemaphoreDiagnostics {

struct Counters {
    std::atomic<uint64_t> created{}, closed{}, live{}, peak{};
    std::atomic<uint64_t> create_failures{}, close_failures{}, wait_failures{}, release_failures{};
    std::atomic<uint64_t> waiters{}, timeouts{};
};

inline Counters counters;

struct Waiting {
    Waiting() { ++counters.waiters; }
    ~Waiting() { --counters.waiters; }
};

inline void Report(const char* operation, const void* object, HANDLE handle, int64_t maximum,
                   int32_t initial, DWORD error, DWORD result = 0) noexcept {
    APP_MEMORY_INFORMATION memory{};
    const bool available = GetProcessInformation(GetCurrentProcess(), ProcessAppMemoryInfo,
                                                 &memory, sizeof(memory));
    try {
        LOG_WARNING(Kernel,
                    "[SemaphoreDiag] operation={} object={} handle={} tid={} max={} initial={} "
                    "win32={} wait_result={:#x} created={} closed={} live={} peak={} "
                    "create_failures={} close_failures={} wait_failures={} release_failures={} "
                    "waiters={} timeouts={} commit_available={} private_commit={} "
                    "total_commit={} memory_info={}",
                    operation, object, handle, GetCurrentThreadId(), maximum, initial, error, result,
                    counters.created.load(), counters.closed.load(), counters.live.load(),
                    counters.peak.load(), counters.create_failures.load(),
                    counters.close_failures.load(), counters.wait_failures.load(),
                    counters.release_failures.load(), counters.waiters.load(),
                    counters.timeouts.load(), memory.AvailableCommit,
                    memory.PrivateCommitUsage, memory.TotalCommitUsage, available);
    } catch (...) {
    }
    SetLastError(error);
}

inline void Created(const void* object, HANDLE handle, int64_t maximum, int32_t initial) noexcept {
    const auto error = GetLastError();
    if (handle) {
        const auto count = counters.live.fetch_add(1) + 1;
        auto peak = counters.peak.load();
        while (peak < count && !counters.peak.compare_exchange_weak(peak, count)) {}
        const auto total = counters.created.fetch_add(1) + 1;
        if (total == 1 || total % 1024 == 0)
            Report("created", object, handle, maximum, initial, 0);
    } else {
        ++counters.create_failures;
        Report("CreateSemaphore.failed", object, handle, maximum, initial, error);
        void* stack[8]{};
        const auto count = CaptureStackBackTrace(0, 8, stack, nullptr);
        for (USHORT index = 0; index < count; ++index) {
            HMODULE module{};
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(stack[index]), &module);
            try {
                LOG_ERROR(Kernel,
                          "[SemaphoreDiag] stack_index={} address={} module_base={} offset={:#x}",
                          index, stack[index], static_cast<void*>(module),
                          reinterpret_cast<uintptr_t>(stack[index]) - reinterpret_cast<uintptr_t>(module));
            } catch (...) {
            }
        }
    }
    SetLastError(error);
}

} // namespace Libraries::Kernel::SemaphoreDiagnostics
#endif
