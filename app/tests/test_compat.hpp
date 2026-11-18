#pragma once
// POSIX shims so the tests also build with MSVC. On macOS/Linux every helper makes exactly the call the tests
// always made (posix_memalign, setenv, getpid, mkdir, clock_gettime).
#include <cstddef>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <direct.h>
#include <malloc.h>
#include <process.h>
#include <windows.h>
#include <filesystem>
#else
#include <ctime>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace rig_test {

// Aligned allocation for replacement operator new(size, align_val_t). Pair with alignedFree: on Windows the
// block comes from _aligned_malloc and must not reach free(); on POSIX it's posix_memalign + free.
inline void* alignedAlloc(std::size_t align, std::size_t n) {
#ifdef _WIN32
    return _aligned_malloc(n ? n : 1, align);
#else
    void* p = nullptr;
    return posix_memalign(&p, align, n ? n : 1) == 0 ? p : nullptr;
#endif
}
#ifdef _WIN32
inline constexpr bool kAlignedNeedsOwnFree = true;
inline void alignedFree(void* p) { _aligned_free(p); }
#else
inline constexpr bool kAlignedNeedsOwnFree = false; // free() handles posix_memalign blocks
inline void alignedFree(void* p) { std::free(p); }
#endif

inline int setEnv(const char* key, const char* value) {
#ifdef _WIN32
    return _putenv_s(key, value);
#else
    return setenv(key, value, 1);
#endif
}

inline int processId() {
#ifdef _WIN32
    return _getpid();
#else
    return static_cast<int>(::getpid());
#endif
}

inline int makeDir(const std::string& path) {
#ifdef _WIN32
    return _mkdir(path.c_str());
#else
    return ::mkdir(path.c_str(), 0755);
#endif
}

// "/tmp/<name>" on POSIX (as before); the user temp folder on Windows.
inline std::string tmpPath(const std::string& name) {
#ifdef _WIN32
    return (std::filesystem::temp_directory_path() / name).string();
#else
    return "/tmp/" + name;
#endif
}

// CPU time consumed by the calling thread, seconds. False when the clock isn't available.
inline bool threadCpuSeconds(double& out) {
#ifdef _WIN32
    FILETIME c, e, k, u;
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return false;
    auto t = [](const FILETIME& f) { return (static_cast<unsigned long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    out = 1e-7 * static_cast<double>(t(k) + t(u));
    return true;
#else
    timespec ts{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return false;
    out = double(ts.tv_sec) + 1e-9 * double(ts.tv_nsec);
    return true;
#endif
}

} // namespace rig_test
