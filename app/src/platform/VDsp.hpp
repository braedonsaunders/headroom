#pragma once
// Portable subset of Apple's vDSP, so DSP code written against Accelerate builds on Windows and Linux.
//
//   * macOS: includes <Accelerate/Accelerate.h> and the engine keeps using Apple's implementation unchanged.
//   * elsewhere: the same names (vDSP_fft_zrip, vDSP_ctoz, vDSP_mmul, DSPSplitComplex, FFTSetup, ...) are
//     provided by the header-only implementations in rig::vdsp below, with vDSP's exact data layout and scaling:
//       - vDSP_fft_zrip forward on N real samples (packed even/odd by vDSP_ctoz) returns 2× the DFT for bins
//         1..N/2−1, with 2·X[0] in realp[0] and 2·X[N/2] in imagp[0]; the inverse is the unnormalised sum, so a
//         forward + inverse round trip scales by 2N — callers already divide by that.
//       - one setup created for log2n serves every transform up to that size.
//   * rig::vdsp is always compiled, so tests/test_vdsp_portable.cpp can check it against Accelerate on a Mac.
//
// Real-time contract: after vDSP_create_fftsetup, no function here allocates, locks or makes syscalls.
// Only the strides the engine uses are optimised; other strides take the generic (still correct) path.
// The FFT is a split-radix-2 kernel with per-stage contiguous twiddles: correct and allocation-free but slower
// than vDSP/pffft. docs/PORTING.md tracks swapping in pffft (BSD) behind this same interface if profiling asks.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <new>
#include <vector>

#if defined(__APPLE__) && !defined(RIG_PORTABLE_VDSP)
#include <Accelerate/Accelerate.h>
#define RIG_VDSP_ACCELERATE 1
#endif

namespace rig::vdsp {

using vDSP_Length = unsigned long;
using vDSP_Stride = long;
using FFTRadix = int;
using FFTDirection = int;
inline constexpr FFTRadix kFFTRadix2 = 0;
inline constexpr FFTDirection kFFTDirection_Forward = +1;
inline constexpr FFTDirection kFFTDirection_Inverse = -1;

struct DSPComplex { float real, imag; };
struct DSPSplitComplex { float* realp; float* imagp; };

// Split-complex buffers for Accelerate's vDSP_fft_zrip. On Apple Silicon it computes transforms of 512 points and up
// with another kernel (other rounding, about -125 dB) unless both arrays are 64-byte aligned; every 64-byte-aligned
// address gives the same bits. On a plain stack array or std::vector (16-byte aligned) a transform depended on where
// the buffer happened to be: the main thread's stack offset (which moves with the size of the process environment),
// the heap layout, the calling thread. So every array handed to vDSP_fft_zrip is alignas(kFftAlign) or an FftVector.
inline constexpr std::size_t kFftAlign = 64;
template <class T> struct FftAllocator {
    using value_type = T;
    FftAllocator() noexcept = default;
    template <class U> FftAllocator(const FftAllocator<U>&) noexcept {}
    T* allocate(std::size_t n) { return static_cast<T*>(::operator new(n * sizeof(T), std::align_val_t{kFftAlign})); }
    void deallocate(T* p, std::size_t) noexcept { ::operator delete(p, std::align_val_t{kFftAlign}); }
    template <class U> bool operator==(const FftAllocator<U>&) const noexcept { return true; }
};
template <class T> using FftVector = std::vector<T, FftAllocator<T>>;

// Twiddles for every radix-2 stage up to `max_n` (stage length L uses L/2 entries at offset L/2 − 1), so each
// butterfly pass reads its twiddles contiguously. Stage L = N also serves the real-FFT split step.
struct OpaqueFFTSetup {
    unsigned log2max = 0;
    float* wr = nullptr; // cos(−2πj/L)
    float* wi = nullptr; // sin(−2πj/L)
};
using FFTSetup = OpaqueFFTSetup*;

inline FFTSetup vDSP_create_fftsetup(vDSP_Length log2n, FFTRadix) {
    if (log2n < 1 || log2n > 24) return nullptr;
    auto* s = new (std::nothrow) OpaqueFFTSetup;
    if (!s) return nullptr;
    const std::size_t n = std::size_t(1) << log2n;
    s->log2max = static_cast<unsigned>(log2n);
    s->wr = new (std::nothrow) float[n];
    s->wi = new (std::nothrow) float[n];
    if (!s->wr || !s->wi) { delete[] s->wr; delete[] s->wi; delete s; return nullptr; }
    for (std::size_t L = 2; L <= n; L <<= 1) {
        const std::size_t off = L / 2 - 1;
        for (std::size_t j = 0; j < L / 2; ++j) {
            const double a = -2.0 * 3.14159265358979323846 * double(j) / double(L);
            s->wr[off + j] = static_cast<float>(std::cos(a));
            s->wi[off + j] = static_cast<float>(std::sin(a));
        }
    }
    return s;
}

inline void vDSP_destroy_fftsetup(FFTSetup s) {
    if (!s) return;
    delete[] s->wr;
    delete[] s->wi;
    delete s;
}

namespace detail {
// In-place unnormalised complex FFT of m = 2^log2m points (split layout). sign = −1 forward, +1 inverse.
inline void complexFFT(const OpaqueFFTSetup& s, float* re, float* im, unsigned log2m, int sign) noexcept {
    const std::size_t m = std::size_t(1) << log2m;
    for (std::size_t i = 1, j = 0; i < m; ++i) { // bit reversal
        std::size_t bit = m >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            const float tr = re[i]; re[i] = re[j]; re[j] = tr;
            const float ti = im[i]; im[i] = im[j]; im[j] = ti;
        }
    }
    const float sg = sign < 0 ? 1.0f : -1.0f; // tables hold e^{−iθ}; the inverse conjugates them
    for (std::size_t L = 2; L <= m; L <<= 1) {
        const std::size_t h = L / 2;
        const float* wr = s.wr + (h - 1);
        const float* wi = s.wi + (h - 1);
        for (std::size_t b = 0; b < m; b += L) {
            float* ar = re + b; float* ai = im + b;
            float* br = ar + h; float* bi = ai + h;
            for (std::size_t j = 0; j < h; ++j) {
                const float cr = wr[j], ci = sg * wi[j];
                const float xr = br[j] * cr - bi[j] * ci;
                const float xi = br[j] * ci + bi[j] * cr;
                br[j] = ar[j] - xr; bi[j] = ai[j] - xi;
                ar[j] += xr;        ai[j] += xi;
            }
        }
    }
}
} // namespace detail

// Real FFT on 2^log2n samples held as n/2 split-complex values (even samples in realp, odd in imagp).
inline void vDSP_fft_zrip(FFTSetup setup, const DSPSplitComplex* c, vDSP_Stride stride, vDSP_Length log2n,
                          FFTDirection dir) noexcept {
    if (!setup || !c || log2n < 1 || log2n > setup->log2max || stride != 1) return;
    const unsigned lg = static_cast<unsigned>(log2n);
    const std::size_t n = std::size_t(1) << lg, m = n / 2;
    float* re = c->realp;
    float* im = c->imagp;
    // W^k = e^{−2πik/N}, k < N/2: stage-N slice of the twiddle table.
    const float* wr = setup->wr + (m - 1);
    const float* wi = setup->wi + (m - 1);
    if (dir == kFFTDirection_Forward) {
        if (m > 1) detail::complexFFT(*setup, re, im, lg - 1, -1);
        // 2·X[k] = (Z[k] + Z*[m−k]) − i·W^k·(Z[k] − Z*[m−k]), done pairwise (k, m−k) in place.
        const float z0r = re[0], z0i = im[0];
        re[0] = 2.0f * (z0r + z0i);
        im[0] = 2.0f * (z0r - z0i);
        for (std::size_t k = 1; k <= m / 2; ++k) {
            const std::size_t q = m - k;
            const float ar = re[k], ai = im[k], br = re[q], bi = im[q];
            // E = Z[k] + conj(Z[q]), D = Z[k] − conj(Z[q])
            const float er = ar + br, ei = ai - bi, dr = ar - br, di = ai + bi;
            // −i·W·D = −i·(wr + i·wi)(dr + i·di) = (wr·di + wi·dr) + i·(wi·di − wr·dr)
            const float cr = wr[k], ci = wi[k];
            re[k] = er + (cr * di + ci * dr);
            im[k] = ei + (ci * di - cr * dr);
            if (q != k) {
                // bin q: E' = Z[q] + conj(Z[k]) = conj(E), D' = Z[q] − conj(Z[k]) = −conj(D); W^q = −conj(W^k)
                const float er2 = er, ei2 = -ei, dr2 = -dr, di2 = di;
                const float cr2 = -cr, ci2 = ci;
                re[q] = er2 + (cr2 * di2 + ci2 * dr2);
                im[q] = ei2 + (ci2 * di2 - cr2 * dr2);
            }
        }
    } else {
        // Z[k] = (F[k] + F*[m−k]) + i·W^{−k}·(F[k] − F*[m−k]); Z[0] = (Y0 + YN/2) + i·(Y0 − YN/2)
        const float y0 = re[0], ym = im[0];
        re[0] = y0 + ym;
        im[0] = y0 - ym;
        for (std::size_t k = 1; k <= m / 2; ++k) {
            const std::size_t q = m - k;
            const float ar = re[k], ai = im[k], br = re[q], bi = im[q];
            const float er = ar + br, ei = ai - bi, dr = ar - br, di = ai + bi;
            // i·conj(W)·D = i·(wr − i·wi)(dr + i·di) = (wi·dr − wr·di) + i·(wr·dr + wi·di)
            const float cr = wr[k], ci = wi[k];
            re[k] = er + (ci * dr - cr * di);
            im[k] = ei + (cr * dr + ci * di);
            if (q != k) {
                const float er2 = er, ei2 = -ei, dr2 = -dr, di2 = di;
                const float cr2 = -cr, ci2 = ci;
                re[q] = er2 + (ci2 * dr2 - cr2 * di2);
                im[q] = ei2 + (cr2 * dr2 + ci2 * di2);
            }
        }
        if (m > 1) detail::complexFFT(*setup, re, im, lg - 1, +1);
    }
}

// Interleaved complex → split complex. `ic` counts floats (2 = contiguous), as in vDSP.
inline void vDSP_ctoz(const DSPComplex* c, vDSP_Stride ic, const DSPSplitComplex* z, vDSP_Stride iz, vDSP_Length n) noexcept {
    const vDSP_Stride step = ic / 2;
    for (vDSP_Length i = 0; i < n; ++i) {
        z->realp[i * iz] = c[i * step].real;
        z->imagp[i * iz] = c[i * step].imag;
    }
}

inline void vDSP_ztoc(const DSPSplitComplex* z, vDSP_Stride iz, DSPComplex* c, vDSP_Stride ic, vDSP_Length n) noexcept {
    const vDSP_Stride step = ic / 2;
    for (vDSP_Length i = 0; i < n; ++i) {
        c[i * step].real = z->realp[i * iz];
        c[i * step].imag = z->imagp[i * iz];
    }
}

// D = A·B + C (complex, split).
inline void vDSP_zvma(const DSPSplitComplex* a, vDSP_Stride ia, const DSPSplitComplex* b, vDSP_Stride ib,
                      const DSPSplitComplex* c, vDSP_Stride icc, const DSPSplitComplex* d, vDSP_Stride id, vDSP_Length n) noexcept {
    if (ia == 1 && ib == 1 && icc == 1 && id == 1) {
        const float *ar = a->realp, *ai = a->imagp, *br = b->realp, *bi = b->imagp, *cr = c->realp, *ci = c->imagp;
        float *dr = d->realp, *di = d->imagp;
        for (vDSP_Length i = 0; i < n; ++i) {
            const float r = ar[i] * br[i] - ai[i] * bi[i] + cr[i];
            const float m = ar[i] * bi[i] + ai[i] * br[i] + ci[i];
            dr[i] = r;
            di[i] = m;
        }
        return;
    }
    for (vDSP_Length i = 0; i < n; ++i) {
        const float xr = a->realp[i * ia], xi = a->imagp[i * ia], yr = b->realp[i * ib], yi = b->imagp[i * ib];
        const float r = xr * yr - xi * yi + c->realp[i * icc];
        const float m = xr * yi + xi * yr + c->imagp[i * icc];
        d->realp[i * id] = r;
        d->imagp[i * id] = m;
    }
}

inline void vDSP_dotpr(const float* a, vDSP_Stride ia, const float* b, vDSP_Stride ib, float* c, vDSP_Length n) noexcept {
    if (ia == 1 && ib == 1) { // four partial sums: lets the compiler vectorise without -ffast-math
        float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        vDSP_Length i = 0;
        for (; i + 4 <= n; i += 4) {
            acc[0] += a[i] * b[i];
            acc[1] += a[i + 1] * b[i + 1];
            acc[2] += a[i + 2] * b[i + 2];
            acc[3] += a[i + 3] * b[i + 3];
        }
        float s = (acc[0] + acc[1]) + (acc[2] + acc[3]);
        for (; i < n; ++i) s += a[i] * b[i];
        *c = s;
        return;
    }
    float s = 0.0f;
    for (vDSP_Length i = 0; i < n; ++i) s += a[i * ia] * b[i * ib];
    *c = s;
}

// C (M×N) = A (M×P) · B (P×N), row-major. i-k-j order keeps the inner loop contiguous (auto-vectorised).
inline void vDSP_mmul(const float* a, vDSP_Stride ia, const float* b, vDSP_Stride ib, float* c, vDSP_Stride icc,
                      vDSP_Length M, vDSP_Length N, vDSP_Length P) noexcept {
    if (ia == 1 && ib == 1 && icc == 1) {
        for (vDSP_Length i = 0; i < M; ++i) {
            float* ci = c + i * N;
            for (vDSP_Length j = 0; j < N; ++j) ci[j] = 0.0f;
            const float* ai = a + i * P;
            for (vDSP_Length k = 0; k < P; ++k) {
                const float s = ai[k];
                const float* bk = b + k * N;
                for (vDSP_Length j = 0; j < N; ++j) ci[j] += s * bk[j];
            }
        }
        return;
    }
    for (vDSP_Length i = 0; i < M; ++i)
        for (vDSP_Length j = 0; j < N; ++j) {
            float s = 0.0f;
            for (vDSP_Length k = 0; k < P; ++k) s += a[(i * P + k) * ia] * b[(k * N + j) * ib];
            c[(i * N + j) * icc] = s;
        }
}

// C (M×N) = transpose of A (N×M).
inline void vDSP_mtrans(const float* a, vDSP_Stride ia, float* c, vDSP_Stride icc, vDSP_Length M, vDSP_Length N) noexcept {
    for (vDSP_Length i = 0; i < M; ++i)
        for (vDSP_Length j = 0; j < N; ++j) c[(i * N + j) * icc] = a[(j * M + i) * ia];
}

inline void vDSP_vadd(const float* a, vDSP_Stride ia, const float* b, vDSP_Stride ib, float* c, vDSP_Stride icc, vDSP_Length n) noexcept {
    for (vDSP_Length i = 0; i < n; ++i) c[i * icc] = a[i * ia] + b[i * ib];
}
inline void vDSP_vsadd(const float* a, vDSP_Stride ia, const float* b, float* c, vDSP_Stride icc, vDSP_Length n) noexcept {
    const float s = *b;
    for (vDSP_Length i = 0; i < n; ++i) c[i * icc] = a[i * ia] + s;
}
inline void vDSP_vsmul(const float* a, vDSP_Stride ia, const float* b, float* c, vDSP_Stride icc, vDSP_Length n) noexcept {
    const float s = *b;
    for (vDSP_Length i = 0; i < n; ++i) c[i * icc] = a[i * ia] * s;
}
// D = A·b + C
inline void vDSP_vsma(const float* a, vDSP_Stride ia, const float* b, const float* c, vDSP_Stride icc, float* d,
                      vDSP_Stride id, vDSP_Length n) noexcept {
    const float s = *b;
    for (vDSP_Length i = 0; i < n; ++i) d[i * id] = a[i * ia] * s + c[i * icc];
}
inline void vDSP_svesq(const float* a, vDSP_Stride ia, float* c, vDSP_Length n) noexcept {
    float s = 0.0f;
    for (vDSP_Length i = 0; i < n; ++i) s += a[i * ia] * a[i * ia];
    *c = s;
}
inline void vDSP_distancesq(const float* a, vDSP_Stride ia, const float* b, vDSP_Stride ib, float* c, vDSP_Length n) noexcept {
    float s = 0.0f;
    for (vDSP_Length i = 0; i < n; ++i) { const float d = a[i * ia] - b[i * ib]; s += d * d; }
    *c = s;
}

} // namespace rig::vdsp

#ifndef RIG_VDSP_ACCELERATE
// Off Apple platforms the engine's vDSP calls resolve to the portable implementations.
using namespace rig::vdsp;
#endif
