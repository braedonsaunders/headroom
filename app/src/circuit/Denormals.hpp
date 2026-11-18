#pragma once
// Scoped flush-to-zero / denormals-are-zero for the audio thread (docs/CIRCUIT_ENGINE.md §3.8).
// Restores the caller's floating-point mode on exit, so it is safe inside any host callback.
#include <cstdint>
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace rig::ce {

class ScopedFlushDenormals {
public:
    ScopedFlushDenormals() noexcept {
#if defined(__aarch64__)
        std::uint64_t v;
        asm volatile("mrs %0, fpcr" : "=r"(v));
        saved_ = v;
        v |= (1ull << 24); // FZ
        asm volatile("msr fpcr, %0" ::"r"(v));
#elif defined(__x86_64__) || defined(_M_X64)
        saved_ = _mm_getcsr();
        _mm_setcsr(static_cast<unsigned>(saved_) | 0x8040u); // FTZ | DAZ
#endif
    }
    ~ScopedFlushDenormals() {
#if defined(__aarch64__)
        const std::uint64_t v = saved_;
        asm volatile("msr fpcr, %0" ::"r"(v));
#elif defined(__x86_64__) || defined(_M_X64)
        _mm_setcsr(static_cast<unsigned>(saved_));
#endif
    }
    ScopedFlushDenormals(const ScopedFlushDenormals&) = delete;
    ScopedFlushDenormals& operator=(const ScopedFlushDenormals&) = delete;

private:
    std::uint64_t saved_ = 0;
};

} // namespace rig::ce
