// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <windows.h>

namespace Core::Uwp::MemoryDiagnostics {

struct Operation {
    const char* name{};
    const char* stage{};
    DWORD thread{};
    uint64_t started{};
    uint64_t changed{};
    uint64_t address{};
    uint64_t value{};
    DWORD error{};
};

struct Registry {
    std::mutex mutex;
    std::array<Operation, 128> active{};
    Operation last_private{};
    uint64_t completed{};
    uint64_t dropped{};
};

inline Registry& State() {
    static Registry state;
    return state;
}

class Scope {
public:
    Scope(const char* name, uint64_t address = 0, uint64_t value = 0, bool privatization = false)
        : privatization{privatization} {
        const auto error = GetLastError();
        auto& state = State();
        {
            std::lock_guard lock{state.mutex};
            for (auto& candidate : state.active) {
                if (!candidate.name) {
                    operation = &candidate;
                    const auto now = GetTickCount64();
                    candidate = {name, "enter", GetCurrentThreadId(), now, now, address, value};
                    break;
                }
            }
            if (!operation)
                ++state.dropped;
        }
        SetLastError(error);
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    ~Scope() {
        const auto error = GetLastError();
        if (operation) {
            auto& state = State();
            std::lock_guard lock{state.mutex};
            if (privatization) {
                state.last_private = *operation;
                state.last_private.changed = GetTickCount64();
                state.last_private.error = error;
            }
            ++state.completed;
            *operation = {};
        }
        SetLastError(error);
    }

    void Stage(const char* name, uint64_t address = 0) {
        const auto error = GetLastError();
        if (operation) {
            std::lock_guard lock{State().mutex};
            operation->stage = name;
            if (address)
                operation->address = address;
            operation->changed = GetTickCount64();
            operation->error = error;
        }
        SetLastError(error);
    }

private:
    Operation* operation{};
    bool privatization{};
};

inline void Report(void (*log)(const char*)) {
    if (!log)
        return;
    std::array<Operation, 129> operations{};
    uint64_t completed{}, dropped{};
    {
        auto& state = State();
        std::lock_guard lock{state.mutex};
        std::copy(state.active.begin(), state.active.end(), operations.begin());
        operations.back() = state.last_private;
        completed = state.completed;
        dropped = state.dropped;
    }
    const auto now = GetTickCount64();
    char message[512];
    std::snprintf(message, sizeof(message), "[MemoryWatch] heartbeat completed=%llu dropped=%llu",
                  completed, dropped);
    log(message);
    for (size_t index = 0; index < operations.size(); ++index) {
        const auto& op = operations[index];
        const bool last = index == operations.size() - 1;
        if (!op.name || (!last && now - op.started < 5000))
            continue;
        std::snprintf(message, sizeof(message),
                      "[MemoryWatch] status=%s operation=%s stage=%s tid=%lu age_ms=%llu "
                      "stage_age_ms=%llu address=0x%llx value=%llu win32=%lu",
                      last ? "last_privatization" : "pending", op.name, op.stage, op.thread,
                      now - op.started, now - op.changed, op.address, op.value, op.error);
        log(message);
    }
}

} // namespace Core::Uwp::MemoryDiagnostics
