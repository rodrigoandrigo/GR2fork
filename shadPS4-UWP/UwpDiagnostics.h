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
inline LONG WINAPI Unhandled(EXCEPTION_POINTERS *exception) noexcept {
  if (!reporting.test_and_set()) {
    if (exception && exception->ExceptionRecord) {
      const auto &record = *exception->ExceptionRecord;
      Write(crash_file, "unhandled_seh code/pc/fault/access",
            record.ExceptionCode,
            reinterpret_cast<uintptr_t>(record.ExceptionAddress),
            record.NumberParameters > 1 ? record.ExceptionInformation[1] : 0,
            record.NumberParameters > 0 ? record.ExceptionInformation[0] : 0);
#if defined(_M_X64) || defined(__x86_64__)
      if (exception->ContextRecord)
        Write(crash_file, "context rip/rsp", exception->ContextRecord->Rip,
              exception->ContextRecord->Rsp);
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
  } catch (...) {
    OutputDebugStringW(L"Could not initialize persistent UWP diagnostics\n");
  }
}
void Memory(const char *stage) noexcept;
} // namespace UwpDiagnostics
