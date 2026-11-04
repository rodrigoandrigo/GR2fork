// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "mutex.h"

#include <atomic>
#include "common/assert.h"
#include "common/logging/log.h"

namespace Libraries::Kernel {

#ifdef _WIN64
namespace {
std::atomic<u64> created{}, closed{}, create_failures{}, close_failures{}, live{}, peak{};
std::atomic<u64> native_created{}, native_closed{}, reused{};
struct MutexCache {
    SRWLOCK lock = SRWLOCK_INIT;
    std::array<HANDLE, 128> handles{};
    size_t count{};
    ~MutexCache() {
        for (size_t index = 0; index < count; ++index)
            CloseHandle(handles[index]);
    }
};

MutexCache& Cache() {
    static MutexCache cache;
    return cache;
}

void ReportMutex(const char* operation, const void* object, HANDLE handle, DWORD error) {
    APP_MEMORY_INFORMATION memory{};
    const bool available = GetProcessInformation(GetCurrentProcess(), ProcessAppMemoryInfo,
                                                  &memory, sizeof(memory));
    try {
        LOG_WARNING(Kernel, "[MutexDiag] operation={} object={} handle={} tid={} win32={} "
                        "created={} closed={} live={} peak={} create_failures={} close_failures={} "
                        "commit_available={} private_commit={} total_commit={} memory_info={} "
                        "native_created={} native_closed={} reused={}",
                operation, object, handle, GetCurrentThreadId(), error, created.load(), closed.load(),
                live.load(), peak.load(), create_failures.load(), close_failures.load(),
                memory.AvailableCommit, memory.PrivateCommitUsage, memory.TotalCommitUsage,
                    available, native_created.load(), native_closed.load(), reused.load());
    } catch (...) {
    }
    SetLastError(error);
}
}
#endif

TimedMutex::TimedMutex() {
#ifdef _WIN64
    auto& cache = Cache();
    AcquireSRWLockExclusive(&cache.lock);
    mtx = cache.count ? cache.handles[--cache.count] : nullptr;
    ReleaseSRWLockExclusive(&cache.lock);
    if (mtx) {
        ++reused;
    } else {
        mtx = CreateMutex(nullptr, false, nullptr);
        if (mtx)
            ++native_created;
    }
    const auto error = GetLastError();
    if (mtx) {
        const auto count = live.fetch_add(1) + 1;
        auto maximum = peak.load();
        while (maximum < count && !peak.compare_exchange_weak(maximum, count)) {}
        const auto total = created.fetch_add(1) + 1;
        if (total == 1 || total % 1024 == 0)
            ReportMutex("created", this, mtx, 0);
    } else {
        ++create_failures;
        ReportMutex("CreateMutex.failed", this, mtx, error);
        void* stack[8]{};
        const auto count = CaptureStackBackTrace(0, 8, stack, nullptr);
        for (USHORT index = 0; index < count; ++index) {
            HMODULE module{};
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(stack[index]), &module);
            try {
                LOG_ERROR(Kernel, "[MutexDiag] stack_index={} address={} module_base={} offset={:#x}",
                      index, stack[index], static_cast<void*>(module),
                          reinterpret_cast<uintptr_t>(stack[index]) - reinterpret_cast<uintptr_t>(module));
            } catch (...) {
            }
        }
    }
    SetLastError(error);
    ASSERT(mtx);
#endif
}

TimedMutex::~TimedMutex() {
#ifdef _WIN64
    const auto previous = GetLastError();
    bool retained{};
    if (!held.load()) {
        auto& cache = Cache();
        AcquireSRWLockExclusive(&cache.lock);
        if (cache.count < cache.handles.size()) {
            cache.handles[cache.count++] = mtx;
            retained = true;
        }
        ReleaseSRWLockExclusive(&cache.lock);
    }
    if (retained || CloseHandle(mtx)) {
        if (!retained)
            ++native_closed;
        ++closed;
        --live;
    } else {
        const auto error = GetLastError();
        ++close_failures;
        ReportMutex("CloseHandle.failed", this, mtx, error);
    }
    SetLastError(previous);
#endif
}

void TimedMutex::lock() {
#ifdef _WIN64
    for (;;) {
        u64 res = WaitForSingleObjectEx(mtx, INFINITE, true);
        if (res == WAIT_OBJECT_0) {
            ++held;
            return;
        }
    }
#else
    mtx.lock();
#endif
}

bool TimedMutex::try_lock() {
#ifdef _WIN64
    if (WaitForSingleObjectEx(mtx, 0, true) != WAIT_OBJECT_0)
        return false;
    ++held;
    return true;
#else
    return mtx.try_lock();
#endif
}

void TimedMutex::unlock() {
#ifdef _WIN64
    if (ReleaseMutex(mtx))
        --held;
#else
    mtx.unlock();
#endif
}

} // namespace Libraries::Kernel
