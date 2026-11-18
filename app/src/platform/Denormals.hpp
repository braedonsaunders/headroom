#pragma once
// Flush-to-zero for the audio thread. On x86 a subnormal float costs ~100× a normal one, so decaying IIR
// filters, reverb tails and feedback delays can spike CPU when the input goes silent; Apple Silicon handles
// subnormals at speed, which is why the macOS build never needed this. Construct one at the top of every
// device callback (it restores the previous mode on exit, so it is safe inside a plugin host too).
//
//   x86/x64: MXCSR FTZ (bit 15) + DAZ (bit 6).   AArch64: FPCR.FZ (bit 24).   Elsewhere: no-op.
//
//   ScopedNoDenormals     x86 and AArch64 — for the portable device callbacks (AudioIOMiniaudio).
//   ScopedNoDenormalsX86  x86 only — for engine paths shared with the macOS build (renderHost, worker and library
//                         threads), so Apple Silicon numerics stay exactly as they always were while Intel Macs,
//                         Windows and Linux on x86 get FTZ/DAZ.
#include <cstdint>
#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86_FP)
#include <xmmintrin.h>
#define RIG_DENORMALS_X86 1
#endif

namespace rig::platform {

template <bool kArm>
class BasicScopedNoDenormals {
public:
    BasicScopedNoDenormals() noexcept {
#if defined(RIG_DENORMALS_X86)
        prev_ = _mm_getcsr();
        _mm_setcsr(static_cast<unsigned>(prev_) | 0x8040u);
#elif defined(__aarch64__) && !defined(_MSC_VER)
        if (!kArm) return;
        uint64_t v;
        __asm__ __volatile__("mrs %0, fpcr" : "=r"(v));
        prev_ = v;
        v |= (uint64_t(1) << 24);
        __asm__ __volatile__("msr fpcr, %0" : : "r"(v));
#endif
    }
    ~BasicScopedNoDenormals() {
#if defined(RIG_DENORMALS_X86)
        _mm_setcsr(static_cast<unsigned>(prev_));
#elif defined(__aarch64__) && !defined(_MSC_VER)
        if (!kArm) return;
        const uint64_t v = prev_;
        __asm__ __volatile__("msr fpcr, %0" : : "r"(v));
#endif
    }
    BasicScopedNoDenormals(const BasicScopedNoDenormals&) = delete;
    BasicScopedNoDenormals& operator=(const BasicScopedNoDenormals&) = delete;

private:
    uint64_t prev_ = 0;
};

using ScopedNoDenormals = BasicScopedNoDenormals<true>;
using ScopedNoDenormalsX86 = BasicScopedNoDenormals<false>;

} // namespace rig::platform
