// Test helper (loaded with DYLD_INSERT_LIBRARIES on macOS, LD_PRELOAD on Linux): counts malloc / calloc / realloc /
// free made by one chosen thread, so a test can prove a code path (including Apple framework calls) never touches
// the heap.
//   rtguard_begin() — start counting on the calling thread;  rtguard_end() — stop, return the count.
//   rtguard_locks() — (macOS) pthread mutex lock / trylock calls made by that thread since rtguard_begin().
#ifdef __APPLE__
#include <atomic>
#include <cstdlib>
#include <pthread.h>

static std::atomic<long> g_count{0};
static std::atomic<long> g_locks{0};
static std::atomic<pthread_t> g_thread{nullptr};

static inline void note() {
    const pthread_t t = g_thread.load(std::memory_order_relaxed);
    if (t && pthread_equal(pthread_self(), t)) g_count.fetch_add(1, std::memory_order_relaxed);
}

extern "C" {
static void* rt_malloc(size_t n) { note(); return malloc(n); }
static void* rt_calloc(size_t a, size_t b) { note(); return calloc(a, b); }
static void* rt_realloc(void* p, size_t n) { note(); return realloc(p, n); }
static void rt_free(void* p) { if (p) note(); free(p); }
static int rt_mutex_lock(pthread_mutex_t* m) {
    const pthread_t t = g_thread.load(std::memory_order_relaxed);
    if (t && pthread_equal(pthread_self(), t)) g_locks.fetch_add(1, std::memory_order_relaxed);
    return pthread_mutex_lock(m);
}
static int rt_mutex_trylock(pthread_mutex_t* m) {
    const pthread_t t = g_thread.load(std::memory_order_relaxed);
    if (t && pthread_equal(pthread_self(), t)) g_locks.fetch_add(1, std::memory_order_relaxed);
    return pthread_mutex_trylock(m);
}
__attribute__((visibility("default"))) void rtguard_begin() { g_count.store(0); g_locks.store(0); g_thread.store(pthread_self()); }
__attribute__((visibility("default"))) long rtguard_end() { g_thread.store(nullptr); return g_count.exchange(0); }
__attribute__((visibility("default"))) long rtguard_locks() { return g_locks.exchange(0); }
}

#define RT_INTERPOSE(rep, orig)                                                                                  \
    __attribute__((used)) static const struct { const void* r; const void* o; } rt_ip_##orig                    \
        __attribute__((section("__DATA,__interpose"))) = {(const void*)(unsigned long)&rep, (const void*)(unsigned long)&orig};
RT_INTERPOSE(rt_malloc, malloc)
RT_INTERPOSE(rt_calloc, calloc)
RT_INTERPOSE(rt_realloc, realloc)
RT_INTERPOSE(rt_free, free)
RT_INTERPOSE(rt_mutex_lock, pthread_mutex_lock)
RT_INTERPOSE(rt_mutex_trylock, pthread_mutex_trylock)
#else
// Linux (glibc): the preloaded library defines the allocator entry points itself and forwards to glibc's
// internal ones (__libc_*), so no dlsym bootstrapping (which can itself allocate) is needed.
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <malloc.h>
#include <pthread.h>
#include <stdlib.h>

extern "C" {
void* __libc_malloc(size_t);
void* __libc_calloc(size_t, size_t);
void* __libc_realloc(void*, size_t);
void __libc_free(void*);
void* __libc_memalign(size_t, size_t);
}

static std::atomic<long> g_count{0};
static std::atomic<uintptr_t> g_thread{0};

static inline void note() {
    const uintptr_t t = g_thread.load(std::memory_order_relaxed);
    if (t && static_cast<uintptr_t>(pthread_self()) == t) g_count.fetch_add(1, std::memory_order_relaxed);
}

extern "C" {
__attribute__((visibility("default"))) void* malloc(size_t n) noexcept { note(); return __libc_malloc(n); }
__attribute__((visibility("default"))) void* calloc(size_t a, size_t b) noexcept { note(); return __libc_calloc(a, b); }
__attribute__((visibility("default"))) void* realloc(void* p, size_t n) noexcept { note(); return __libc_realloc(p, n); }
__attribute__((visibility("default"))) void free(void* p) noexcept { if (p) note(); __libc_free(p); }
__attribute__((visibility("default"))) void* memalign(size_t a, size_t n) noexcept { note(); return __libc_memalign(a, n); }
__attribute__((visibility("default"))) void* aligned_alloc(size_t a, size_t n) noexcept { note(); return __libc_memalign(a, n); }
__attribute__((visibility("default"))) int posix_memalign(void** out, size_t a, size_t n) noexcept {
    note();
    void* p = __libc_memalign(a, n);
    if (!p) return ENOMEM;
    *out = p;
    return 0;
}
__attribute__((visibility("default"))) void rtguard_begin() { g_count.store(0); g_thread.store(static_cast<uintptr_t>(pthread_self())); }
__attribute__((visibility("default"))) long rtguard_end() { g_thread.store(0); return g_count.exchange(0); }
}
#endif
