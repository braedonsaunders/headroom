// Portable vDSP subset (src/platform/VDsp.hpp): checks layout and scaling of the real FFT against a
// double-precision DFT on every platform, and against Accelerate itself on macOS, plus the vector/matrix helpers.
#undef NDEBUG
#include "../src/platform/VDsp.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace pv = rig::vdsp;

namespace {

int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("  FAIL: " __VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

#ifdef RIG_VDSP_ACCELERATE
double maxAbs(const std::vector<float>& v) { double m = 1e-12; for (float x : v) m = std::max(m, double(std::fabs(x))); return m; }
#endif

// Reference: what vDSP_fft_zrip forward returns for x (2·DFT, DC in re[0], Nyquist in im[0]).
void refForward(const std::vector<float>& x, std::vector<double>& re, std::vector<double>& im) {
    const std::size_t n = x.size(), m = n / 2;
    re.assign(m, 0.0); im.assign(m, 0.0);
    for (std::size_t k = 0; k <= m; ++k) {
        double sr = 0, si = 0;
        for (std::size_t t = 0; t < n; ++t) {
            const double a = -2.0 * M_PI * double(k * t % n) / double(n);
            sr += x[t] * std::cos(a); si += x[t] * std::sin(a);
        }
        if (k == 0) re[0] = 2 * sr;
        else if (k == m) im[0] = 2 * sr;
        else { re[k] = 2 * sr; im[k] = 2 * si; }
    }
}

// Reference inverse: y[t] = Σ over the Hermitian-completed packed spectrum of e^{+2πikt/N} (no 1/N).
std::vector<double> refInverse(const std::vector<float>& re, const std::vector<float>& im) {
    const std::size_t m = re.size(), n = 2 * m;
    std::vector<double> y(n, 0.0);
    for (std::size_t t = 0; t < n; ++t) {
        double s = re[0] + im[0] * ((t & 1) ? -1.0 : 1.0);
        for (std::size_t k = 1; k < m; ++k) {
            const double a = 2.0 * M_PI * double(k * t % n) / double(n);
            s += 2.0 * (re[k] * std::cos(a) - im[k] * std::sin(a));
        }
        y[t] = s;
    }
    return y;
}

void testFFT() {
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    const unsigned kMax = 13;
    pv::FFTSetup ps = pv::vDSP_create_fftsetup(kMax, pv::kFFTRadix2);
    assert(ps);
#ifdef RIG_VDSP_ACCELERATE
    FFTSetup as = vDSP_create_fftsetup(kMax, kFFTRadix2);
#endif
    for (unsigned lg = 1; lg <= kMax; ++lg) {
        const std::size_t n = std::size_t(1) << lg, m = n / 2;
        std::vector<float> x(n);
        for (auto& v : x) v = u(rng);
        // forward
        std::vector<float> re(m), im(m);
        pv::DSPSplitComplex sc{re.data(), im.data()};
        pv::vDSP_ctoz(reinterpret_cast<const pv::DSPComplex*>(x.data()), 2, &sc, 1, m);
        pv::vDSP_fft_zrip(ps, &sc, 1, lg, pv::kFFTDirection_Forward);
        if (lg <= 11) { // O(N²) reference
            std::vector<double> rr, ri;
            refForward(x, rr, ri);
            double err = 0, mag = 1e-9;
            for (std::size_t k = 0; k < m; ++k) {
                err = std::max({err, std::fabs(rr[k] - re[k]), std::fabs(ri[k] - im[k])});
                mag = std::max({mag, std::fabs(rr[k]), std::fabs(ri[k])});
            }
            CHECK(err / mag < 2e-5, "forward N=%zu vs DFT: rel err %.3g", n, err / mag);
        }
#ifdef RIG_VDSP_ACCELERATE
        {
            std::vector<float> ar(m), ai(m);
            DSPSplitComplex asc{ar.data(), ai.data()};
            vDSP_ctoz(reinterpret_cast<const DSPComplex*>(x.data()), 2, &asc, 1, m);
            vDSP_fft_zrip(as, &asc, 1, lg, kFFTDirection_Forward);
            double err = 0;
            for (std::size_t k = 0; k < m; ++k) err = std::max({err, double(std::fabs(ar[k] - re[k])), double(std::fabs(ai[k] - im[k]))});
            const double mag = std::max(maxAbs(ar), maxAbs(ai));
            CHECK(err / mag < 2e-5, "forward N=%zu vs Accelerate: rel err %.3g", n, err / mag);
        }
#endif
        // inverse of an arbitrary packed spectrum
        std::vector<float> fr(m), fi(m);
        for (std::size_t k = 0; k < m; ++k) { fr[k] = u(rng); fi[k] = u(rng); }
        std::vector<float> ir = fr, ii = fi;
        pv::DSPSplitComplex isc{ir.data(), ii.data()};
        pv::vDSP_fft_zrip(ps, &isc, 1, lg, pv::kFFTDirection_Inverse);
        std::vector<float> y(n);
        pv::vDSP_ztoc(&isc, 1, reinterpret_cast<pv::DSPComplex*>(y.data()), 2, m);
        if (lg <= 11) {
            const auto ry = refInverse(fr, fi);
            double err = 0, mag = 1e-9;
            for (std::size_t t = 0; t < n; ++t) { err = std::max(err, std::fabs(ry[t] - y[t])); mag = std::max(mag, std::fabs(ry[t])); }
            CHECK(err / mag < 2e-5, "inverse N=%zu vs reference: rel err %.3g", n, err / mag);
        }
#ifdef RIG_VDSP_ACCELERATE
        {
            std::vector<float> ar = fr, ai = fi, ay(n);
            DSPSplitComplex asc{ar.data(), ai.data()};
            vDSP_fft_zrip(as, &asc, 1, lg, kFFTDirection_Inverse);
            vDSP_ztoc(&asc, 1, reinterpret_cast<DSPComplex*>(ay.data()), 2, m);
            double err = 0;
            for (std::size_t t = 0; t < n; ++t) err = std::max(err, double(std::fabs(ay[t] - y[t])));
            CHECK(err / maxAbs(ay) < 2e-5, "inverse N=%zu vs Accelerate: rel err %.3g", n, err / maxAbs(ay));
        }
#endif
        // round trip scales by 2N
        std::vector<float> rt(n);
        pv::vDSP_fft_zrip(ps, &sc, 1, lg, pv::kFFTDirection_Inverse);
        pv::vDSP_ztoc(&sc, 1, reinterpret_cast<pv::DSPComplex*>(rt.data()), 2, m);
        double err = 0;
        for (std::size_t t = 0; t < n; ++t) err = std::max(err, std::fabs(rt[t] / (2.0 * n) - x[t]));
        CHECK(err < 1e-5, "round trip N=%zu: err %.3g", n, err);
    }
    pv::vDSP_destroy_fftsetup(ps);
#ifdef RIG_VDSP_ACCELERATE
    vDSP_destroy_fftsetup(as);
#endif
    std::printf("[%s] real FFT N=2..8192: layout, 2x forward / unnormalised inverse scaling%s\n", g_fail ? "FAIL" : "PASS",
#ifdef RIG_VDSP_ACCELERATE
                ", identical to Accelerate"
#else
                ""
#endif
    );
}

void testVector() {
    const int before = g_fail;
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    const unsigned M = 7, N = 13, P = 29;
    std::vector<float> A(M * P), B(P * N), C(M * N), T(P * M);
    for (auto& v : A) v = u(rng);
    for (auto& v : B) v = u(rng);
    pv::vDSP_mmul(A.data(), 1, B.data(), 1, C.data(), 1, M, N, P);
    double err = 0;
    for (unsigned i = 0; i < M; ++i)
        for (unsigned j = 0; j < N; ++j) {
            double s = 0;
            for (unsigned k = 0; k < P; ++k) s += double(A[i * P + k]) * B[k * N + j];
            err = std::max(err, std::fabs(s - C[i * N + j]));
        }
    CHECK(err < 1e-5, "mmul err %.3g", err);
    pv::vDSP_mtrans(A.data(), 1, T.data(), 1, P, M); // T (P×M) = Aᵀ
    bool tr = true;
    for (unsigned i = 0; i < M; ++i) for (unsigned k = 0; k < P; ++k) tr = tr && T[k * M + i] == A[i * P + k];
    CHECK(tr, "mtrans");
    std::vector<float> a(37), b(37), c(37), d(37), e(37);
    for (auto& v : a) v = u(rng);
    for (auto& v : b) v = u(rng);
    for (auto& v : c) v = u(rng);
    float dot = 0, ref = 0, sq = 0, rsq = 0, ds = 0, rds = 0;
    for (std::size_t i = 0; i < a.size(); ++i) { ref += a[i] * b[i]; rsq += a[i] * a[i]; rds += (a[i] - b[i]) * (a[i] - b[i]); }
    pv::vDSP_dotpr(a.data(), 1, b.data(), 1, &dot, a.size());
    pv::vDSP_svesq(a.data(), 1, &sq, a.size());
    pv::vDSP_distancesq(a.data(), 1, b.data(), 1, &ds, a.size());
    CHECK(std::fabs(dot - ref) < 1e-5 && std::fabs(sq - rsq) < 1e-5 && std::fabs(ds - rds) < 1e-5, "dotpr/svesq/distancesq");
    const float s = 0.37f;
    pv::vDSP_vsma(a.data(), 1, &s, c.data(), 1, d.data(), 1, a.size());
    pv::vDSP_vsmul(a.data(), 1, &s, e.data(), 1, a.size());
    bool ok = true;
    for (std::size_t i = 0; i < a.size(); ++i) ok = ok && std::fabs(d[i] - (a[i] * s + c[i])) < 1e-6f && std::fabs(e[i] - a[i] * s) < 1e-6f;
    pv::vDSP_vadd(a.data(), 1, b.data(), 1, d.data(), 1, a.size());
    pv::vDSP_vsadd(a.data(), 1, &s, e.data(), 1, a.size());
    for (std::size_t i = 0; i < a.size(); ++i) ok = ok && d[i] == a[i] + b[i] && e[i] == a[i] + s;
    // zvma: D = A·B + C (complex)
    std::vector<float> xr(9), xi(9), hr(9), hi(9), yr(9), yi(9), zr(9), zi(9);
    for (int i = 0; i < 9; ++i) { xr[i] = u(rng); xi[i] = u(rng); hr[i] = u(rng); hi[i] = u(rng); yr[i] = u(rng); yi[i] = u(rng); }
    pv::DSPSplitComplex X{xr.data(), xi.data()}, H{hr.data(), hi.data()}, Y{yr.data(), yi.data()}, Z{zr.data(), zi.data()};
    pv::vDSP_zvma(&X, 1, &H, 1, &Y, 1, &Z, 1, 9);
    for (int i = 0; i < 9; ++i)
        ok = ok && std::fabs(zr[i] - (xr[i] * hr[i] - xi[i] * hi[i] + yr[i])) < 1e-6f && std::fabs(zi[i] - (xr[i] * hi[i] + xi[i] * hr[i] + yi[i])) < 1e-6f;
    CHECK(ok, "vsma/vsmul/vadd/vsadd/zvma");
    std::printf("[%s] mmul, mtrans, dotpr, svesq, distancesq, vadd, vsadd, vsmul, vsma, zvma\n", g_fail == before ? "PASS" : "FAIL");
}

} // namespace

int main() {
    testFFT();
    testVector();
    return g_fail ? 1 : 0;
}
