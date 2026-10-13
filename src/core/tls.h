// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstring>
#include "common/types.h"
#ifdef SHADPS4_UWP
#include <exception>
#include "uwp/session.h"
namespace Libraries::Kernel {
bool IsGuestThread();
[[noreturn]] void ExitGuestThread();
}
#endif
#ifdef _WIN32
#include <malloc.h>
#endif

namespace Xbyak {
class CodeGenerator;
}

namespace Libraries::Fiber {
struct OrbisFiberContext;
}

namespace Core {

union DtvEntry {
    std::size_t counter;
    u8* pointer;
};

struct Tcb {
    Tcb* tcb_self;
    DtvEntry* tcb_dtv;
    void* tcb_thread;
    void* tcb_spare[2];
    u64 tcb_canary;
    ::Libraries::Fiber::OrbisFiberContext* tcb_fiber;
};

#ifdef _WIN32
/// Gets the thread local storage key for the TCB block.
u32 GetTcbKey();
#endif

/// Sets the data pointer to the TCB block.
void SetTcbBase(void* image_address);

/// Retrieves Tcb structure for the calling thread.
Tcb* GetTcbBase();

/// Makes sure TLS is initialized for the thread before entering guest.
void InitializeTLS();

template <auto f>
struct HostCallWrapperImpl;

template <class ReturnType, class... Args, PS4_SYSV_ABI ReturnType (*func)(Args...)>
struct HostCallWrapperImpl<func> {
    static ReturnType PS4_SYSV_ABI wrap(Args... args) {
#ifdef SHADPS4_UWP
        if (Core::Uwp::StopRequested() && Libraries::Kernel::IsGuestThread())
            Libraries::Kernel::ExitGuestThread();
        try {
            return func(args...);
        } catch (const std::exception& error) {
            Core::Uwp::FailGuestSession(error.what());
        } catch (...) {
            Core::Uwp::FailGuestSession("Unhandled host exception in a guest HLE call");
        }
        Libraries::Kernel::ExitGuestThread();
#else
        return func(args...);
#endif
    }
};

#define HOST_CALL(func) (Core::HostCallWrapperImpl<func>::wrap)

} // namespace Core
