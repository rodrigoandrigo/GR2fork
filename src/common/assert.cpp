// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <fmt/format.h>
#include "common/arch.h"
#include "common/assert.h"
#include "core/signals.h"
#include "emulator.h"

#if defined(ARCH_X86_64)
#define Crash() __asm__ __volatile__("int $3")
#elif defined(ARCH_ARM64)
#define Crash() __asm__ __volatile__("brk 0")
#else
#error "Missing Crash() implementation for target CPU architecture."
#endif

void assert_fail_impl() {
#ifdef SHADPS4_UWP
    throw std::runtime_error("Embedded core assertion failed");
#else
    Core::Signals::Instance()->RemoveHandlers();
    Common::Singleton<Core::Emulator>::Instance()->Shutdown();
    Crash();
#endif
}

void assert_fail_msg(const char* file, int line, const char* func, fmt::string_view format,
                     fmt::format_args args) {
#ifdef SHADPS4_UWP
    Common::Log::Flush();
    throw std::runtime_error(
        fmt::format("{}:{} {}: {}", file, line, func, fmt::vformat(format, args)));
#else
    assert_fail_impl();
#endif
}

[[noreturn]] void unreachable_impl() {
    assert_fail_impl();
    throw std::runtime_error("Unreachable code");
}

void assert_fail_debug_msg(const char* msg) {
    LOG_CRITICAL(Debug, "Assertion Failed!\n{}", msg);
    assert_fail_msg(__FILE__, __LINE__, __func__, "Assertion Failed!\n{}",
                    fmt::make_format_args(msg));
}
