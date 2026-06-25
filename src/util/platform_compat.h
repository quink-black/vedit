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
#include <string>
#include <thread>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#else
  #include <pthread.h>
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
    // SetThreadDescription requires Windows 10 version 1607+.
    // Not critical if unavailable; skip on older Windows.
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

// ---- File system ----
// C++17 std::filesystem replaces stat, lstat, opendir, glob, etc.
// No wrappers needed -- use std::filesystem directly in call sites.

// ---- Unix domain sockets ----
// Linux supports abstract socket namespace (name starts with '\0').
// macOS and Windows use file-system-based sockets only.
// Call sites should use file-based paths (e.g. /tmp/vedit-<pid>.sock)
// for maximum portability. No wrapper needed here.

} // namespace vedit

#endif
