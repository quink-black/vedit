#ifndef VEDIT_UTIL_PLATFORM_COMPAT_H
#define VEDIT_UTIL_PLATFORM_COMPAT_H

// Cross-platform compatibility layer for POSIX APIs that behave differently
// on Linux, macOS, and Windows.
//
// Design principle: prefer C++ standard library (C++17) where possible:
//   - <chrono> instead of clock_gettime / gettimeofday
//   - std::this_thread::sleep_for instead of nanosleep / usleep
//   - std::filesystem instead of stat / lstat / opendir
//
// This header fills in the gaps for APIs with no standard alternative,
// such as pthread_setname_np which has different signatures on each OS.

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <process.h>     // _getpid
#else
  #include <pthread.h>
  #include <unistd.h>      // getpid
#endif

namespace vedit {

// ---- Thread naming ----
// macOS:  pthread_setname_np(name)             -- single argument
// Linux:  pthread_setname_np(thread, name)      -- two arguments
// Windows: SetThreadDescription (10+) or best-effort
//
// Best-effort: failure is silent since thread naming is non-critical.
inline void set_thread_name(const std::string& name) {
#ifdef _WIN32
    // SetThreadDescription is available on Windows 10 1607+ and is
    // declared by <windows.h> when _WIN32_WINNT >= 0x0A00 (set by the
    // CMake build). Best-effort: ignore the HRESULT.
    std::wstring wide(name.begin(), name.end());
    (void)SetThreadDescription(GetCurrentThread(), wide.c_str());
#elif defined(__APPLE__)
    // macOS: single-argument form
    pthread_setname_np(name.c_str());
#else
    // Linux and other POSIX: two-argument form
    pthread_setname_np(pthread_self(), name.c_str());
#endif
}

// ---- Sleep ----
// Thin wrapper around std::this_thread::sleep_for for project consistency.
// Replaces platform-specific nanosleep (POSIX) and Sleep (Win32).
inline void sleep_for(std::chrono::nanoseconds duration) {
    std::this_thread::sleep_for(duration);
}

// ---- Clock / Time ----
// C++17 <chrono> replaces clock_gettime and gettimeofday.
// Convenience aliases for commonly used clocks:

inline std::chrono::steady_clock::time_point steady_now() {
    return std::chrono::steady_clock::now();
}

inline std::chrono::system_clock::time_point system_now() {
    return std::chrono::system_clock::now();
}

// ---- Process ID ----
// Windows uses _getpid (declared in <process.h>); POSIX uses getpid
// (declared in <unistd.h>). Both return int.
inline int current_pid() {
#ifdef _WIN32
    return ::_getpid();
#else
    return ::getpid();
#endif
}

inline std::string pid_string() {
    return std::to_string(current_pid());
}

// ---- Temp directory and default socket path ----
// Centralized so the socket path construction lives in one place. The
// previous call sites hardcoded "/tmp/vedit-<pid>.sock" in main.cpp and
// rpc_methods.cpp; std::filesystem::temp_directory_path() returns /tmp on
// Linux and the per-user temp dir on Windows/macOS, which keeps the Linux
// path identical while giving Windows a valid filesystem location for the
// AF_UNIX socket.
inline std::filesystem::path temp_dir() {
    std::error_code ec;
    auto p = std::filesystem::temp_directory_path(ec);
    if (ec) {
        // Should not happen in practice; fall back to "." so callers do
        // not throw out of a diagnostic helper.
        return std::filesystem::current_path(ec);
    }
    return p;
}

inline std::string default_socket_path() {
    return (temp_dir() / ("vedit-" + pid_string() + ".sock")).string();
}

// ---- File system ----
// C++17 std::filesystem replaces stat, lstat, opendir, glob, etc.
// No wrappers needed -- use std::filesystem directly in call sites.

// ---- Unix domain sockets ----
// AF_UNIX is available on Linux, macOS, and Windows 10 1803+ (Win10
// April 2018 Update). Asio's local::stream_protocol compiles on Windows
// when _WIN32_WINNT >= 0x0A00 and ASIO_HAS_LOCAL_SOCKETS is defined.
// Call sites use default_socket_path() (above) for a file-system-based
// socket path that works on every platform.

} // namespace vedit

#endif
