#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <windows.h>

namespace UwpDiagnostics {
inline HANDLE session_file{INVALID_HANDLE_VALUE};
inline HANDLE crash_file{INVALID_HANDLE_VALUE};
inline LPTOP_LEVEL_EXCEPTION_FILTER previous_filter{};
inline std::terminate_handler previous_terminate{};
inline std::atomic_flag reporting = ATOMIC_FLAG_INIT;
inline std::atomic<uint64_t> sequence{};
inline std::atomic<const char*> ui_phase{"startup"};
inline std::atomic<const char*> core_phase{"startup"};
inline std::atomic<uintptr_t> ui_object{};
inline std::atomic<uintptr_t> core_object{};

inline void Append(char *&cursor, const char *value) noexcept {
  for (unsigned i = 0; value && value[i] && i < 512; ++i)
    *cursor++ = value[i] == '\n' || value[i] == '\r' ? ' ' : value[i];
}
inline void Number(char *&cursor, uint64_t value) noexcept {
  Append(cursor, "0x");
  for (int shift = 60; shift >= 0; shift -= 4)
    *cursor++ = "0123456789abcdef"[(value >> shift) & 15];
}
inline void Write(HANDLE file, const char *event, uint64_t a = 0,
                  uint64_t b = 0, uint64_t c = 0, uint64_t d = 0) noexcept {
  if (file == INVALID_HANDLE_VALUE)
    return;
  char buffer[1024];
  char *cursor = buffer;
  FILETIME time{};
  GetSystemTimeAsFileTime(&time);
  Append(cursor, "time=");
  Number(cursor, (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime);
  Append(cursor, " tid=");
  Number(cursor, GetCurrentThreadId());
  Append(cursor, " seq=");
  Number(cursor, sequence.fetch_add(1, std::memory_order_relaxed));
  Append(cursor, " event=");
  Append(cursor, event);
  Append(cursor, " a=");
  Number(cursor, a);
  Append(cursor, " b=");
  Number(cursor, b);
  Append(cursor, " c=");
  Number(cursor, c);
  Append(cursor, " d=");
  Number(cursor, d);
  *cursor++ = '\r';
  *cursor++ = '\n';
  DWORD written{};
  WriteFile(file, buffer, static_cast<DWORD>(cursor - buffer), &written,
            nullptr);
  FlushFileBuffers(file);
}
inline void Record(const char *event, uint64_t a = 0, uint64_t b = 0) noexcept {
  Write(session_file, event, a, b);
}
inline void UiStage(const char* stage, const void* object, uint64_t a = 0,
                    uint64_t b = 0) noexcept {
  ui_object.store(reinterpret_cast<uintptr_t>(object), std::memory_order_relaxed);
  ui_phase.store(stage, std::memory_order_release);
  Write(session_file, stage, reinterpret_cast<uintptr_t>(object), a, b);
}
inline void CoreStage(const char* stage, const void* object, uint64_t a = 0,
                      uint64_t b = 0) noexcept {
  core_object.store(reinterpret_cast<uintptr_t>(object), std::memory_order_relaxed);
  core_phase.store(stage, std::memory_order_release);
  Write(session_file, stage, reinterpret_cast<uintptr_t>(object), a, b);
}
inline void Text(HANDLE file, const char* stage, const wchar_t* message) noexcept {
  if (!message)
    return;
  char utf8[768]{};
  const auto length = static_cast<int>(wcsnlen_s(message, 160));
  const auto count = WideCharToMultiByte(CP_UTF8, 0, message, length, utf8,
                                        sizeof(utf8) - 1, nullptr, nullptr);
  if (count <= 0)
    return;
  utf8[count] = 0;
  char event[2048]{};
  auto* cursor = event;
  Append(cursor, stage);
  *cursor++ = ' ';
  Append(cursor, utf8);
  *cursor = 0;
  Write(file, event);
}
#if defined(_M_X64) || defined(__x86_64__)
inline bool ReadWord(uintptr_t address, uint64_t* value) noexcept {
  MEMORY_BASIC_INFORMATION info{};
  if (!VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) ||
      info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
      reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize - address < sizeof(*value))
    return false;
  __try {
    *value = *reinterpret_cast<const uint64_t*>(address);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
inline bool UnwindFrame(CONTEXT* context) noexcept {
  __try {
    DWORD64 image{};
    const auto function = RtlLookupFunctionEntry(context->Rip, &image, nullptr);
    if (function) {
      void* handler{};
      DWORD64 frame{};
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, context->Rip, function, context, &handler,
                       &frame, nullptr);
    } else {
      MEMORY_BASIC_INFORMATION info{};
      if (!VirtualQuery(reinterpret_cast<void*>(context->Rsp), &info, sizeof(info)) ||
          info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return false;
      if (!ReadWord(context->Rsp, &context->Rip))
        return false;
      context->Rsp += sizeof(DWORD64);
    }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
inline void TraceContext(const CONTEXT* original) noexcept {
  CONTEXT context = *original;
  for (unsigned index = 0; index < 32 && context.Rip; ++index) {
    MEMORY_BASIC_INFORMATION info{};
    const bool queried = VirtualQuery(reinterpret_cast<void*>(context.Rip), &info, sizeof(info)) != 0;
    const auto base = queried ? reinterpret_cast<uintptr_t>(info.AllocationBase) : 0;
    Write(crash_file, "stack_frame index/pc/base/rva", index, context.Rip, base,
          base ? context.Rip - base : 0);
    wchar_t path[MAX_PATH]{};
    if (base && GetModuleFileNameW(reinterpret_cast<HMODULE>(base), path, MAX_PATH))
      Text(crash_file, "stack_module", path);
    const auto previous = context.Rip;
    const auto stack = context.Rsp;
    if (!UnwindFrame(&context) || context.Rsp <= stack || context.Rip == previous) {
      Write(crash_file, "stack_unwind_stopped index/pc/sp", index, context.Rip, context.Rsp);
      break;
    }
  }
}
#endif
inline LONG WINAPI Unhandled(EXCEPTION_POINTERS *exception) noexcept {
  if (!reporting.test_and_set()) {
    Write(crash_file, ui_phase.load(std::memory_order_acquire), ui_object.load());
    Write(crash_file, core_phase.load(std::memory_order_acquire), core_object.load());
    Write(crash_file, "previous_exception_filter", reinterpret_cast<uintptr_t>(previous_filter));
    if (exception && exception->ExceptionRecord) {
      const auto &record = *exception->ExceptionRecord;
      Write(crash_file, "unhandled_seh code/pc/fault/access",
            record.ExceptionCode,
            reinterpret_cast<uintptr_t>(record.ExceptionAddress),
            record.NumberParameters > 1 ? record.ExceptionInformation[1] : 0,
            record.NumberParameters > 0 ? record.ExceptionInformation[0] : 0);
      MEMORY_BASIC_INFORMATION module_info{};
      if (VirtualQuery(record.ExceptionAddress, &module_info, sizeof(module_info))) {
        const auto base = reinterpret_cast<uintptr_t>(module_info.AllocationBase);
        Write(crash_file, "fault_module base/rva", base,
              reinterpret_cast<uintptr_t>(record.ExceptionAddress) - base);
        wchar_t module_path[MAX_PATH]{};
        if (GetModuleFileNameW(static_cast<HMODULE>(module_info.AllocationBase), module_path,
                               MAX_PATH))
          Text(crash_file, "fault_module", module_path);
      }
#if defined(_M_X64) || defined(__x86_64__)
      if (exception->ContextRecord) {
        Write(crash_file, "context rip/rsp", exception->ContextRecord->Rip,
              exception->ContextRecord->Rsp);
        const auto& context = *exception->ContextRecord;
        Write(crash_file, "context rax/rbx/rcx/rdx", context.Rax, context.Rbx, context.Rcx, context.Rdx);
        Write(crash_file, "context rsi/rdi/rbp/eflags", context.Rsi, context.Rdi, context.Rbp, context.EFlags);
        Write(crash_file, "context r8/r9/r10/r11", context.R8, context.R9, context.R10, context.R11);
        Write(crash_file, "context r12/r13/r14/r15", context.R12, context.R13, context.R14, context.R15);
        for (unsigned index = 0; index < 4; ++index) {
          const auto address = context.Rip + index * sizeof(uint64_t);
          uint64_t bytes{};
          const bool read = ReadWord(address, &bytes);
          Write(crash_file, "instruction_bytes address/little_endian_word/valid", address, bytes, read);
        }
        TraceContext(exception->ContextRecord);
      }
#endif
    }
  }
  return previous_filter ? previous_filter(exception)
                         : EXCEPTION_CONTINUE_SEARCH;
}
inline void Terminate() noexcept {
  Write(crash_file, "std_terminate");
  if (previous_terminate)
    previous_terminate();
  std::abort();
}
inline HANDLE OpenFile(const std::filesystem::path &path) {
  std::error_code error;
  if (std::filesystem::file_size(path, error) > 4 * 1024 * 1024 && !error) {
    auto previous = path;
    previous += L".previous";
    std::filesystem::remove(previous, error);
    std::filesystem::rename(path, previous, error);
  }
  CREATEFILE2_EXTENDED_PARAMETERS parameters{};
  parameters.dwSize = sizeof(parameters);
  parameters.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
  parameters.dwFileFlags = FILE_FLAG_WRITE_THROUGH;
  auto file =
      CreateFile2(path.c_str(), GENERIC_WRITE,
                  FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_ALWAYS, &parameters);
  if (file == INVALID_HANDLE_VALUE) {
    parameters.dwFileFlags = 0;
    file = CreateFile2(path.c_str(), GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_ALWAYS,
                       &parameters);
  }
  if (file != INVALID_HANDLE_VALUE) {
    LARGE_INTEGER end{};
    SetFilePointerEx(file, end, nullptr, FILE_END);
  }
  return file;
}
inline void Initialize(const std::wstring &root) noexcept {
  try {
    const auto directory = std::filesystem::path(root) / L"log";
    std::filesystem::create_directories(directory);
    session_file = OpenFile(directory / L"uwp-session.log");
    crash_file = OpenFile(directory / L"uwp-crash.log");
    previous_filter = SetUnhandledExceptionFilter(Unhandled);
    previous_terminate = std::set_terminate(Terminate);
    Record("app_started pid/exe_base", GetCurrentProcessId(),
           reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)));
    Record("diagnostics_revision memory_rootcause_v4");
  } catch (...) {
    OutputDebugStringW(L"Could not initialize persistent UWP diagnostics\n");
  }
}
void Memory(const char *stage) noexcept;
} // namespace UwpDiagnostics
