#pragma once
// CMOS logic cells (docs/CIRCUIT_ENGINE.md §10.14): the 4013 D flip-flop, the 4011 NAND gate and a buffered inverter.
//
// A cell reads its input pins (node voltages) once per sample and compares them with the switching threshold
// VSS + vth·(VDD − VSS). A threshold crossing is located between samples by cubic interpolation of the last four
// samples, so its time is not quantised to the sample grid. Each output then changes at the crossing time plus the
// supply-dependent propagation delay. The output reaches the circuit as a normalised drive λ (0 = low, 1 = high) on a
// known node, which the output stage (Devices.hpp CmosOut) turns into a rail-to-rail MOS driver. Each change is
// written into λ as a minimum-phase band-limited step (minBLEP, Brandt 2001): the step's in-band timing is exact and its
// aliasing is below the table's stopband, with no latency beyond the step's own minimum-phase rise.
//
// Allocation-free on the audio thread: the table is shared and built on the control thread (Circuit::compile), and the
// event rings are fixed arrays.
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <memory>
#include <vector>

namespace rig::ce {

// Minimum-phase band-limited unit step, tabulated `over` points per sample over `zc` samples.
struct MinBlep {
    int zc = 32, over = 64;
    std::vector<double> s;  // step value on the fine grid (size zc·over + 1), 0 at the edge, 1 at the end
    std::vector<double> ds; // its slope per fine-grid point
    double delay = 0.0;     // DC group delay (centroid of the impulse), samples: the constant latency every edge carries
    // Value x samples after the edge: 0 for x < 0, 1 for x ≥ zc; cubic Hermite in between.
    double step(double x) const noexcept {
        if (x <= 0.0) return 0.0;
        const double u = x * over;
        const int n = zc * over;
        if (u >= n) return 1.0;
        const int j = static_cast<int>(u);
        const double t = u - j, t2 = t * t, t3 = t2 * t;
        return (2 * t3 - 3 * t2 + 1) * s[static_cast<std::size_t>(j)] + (t3 - 2 * t2 + t) * ds[static_cast<std::size_t>(j)] +
               (-2 * t3 + 3 * t2) * s[static_cast<std::size_t>(j + 1)] + (t3 - t2) * ds[static_cast<std::size_t>(j + 1)];
    }
};
namespace logic_detail {
inline void fft(std::vector<std::complex<double>>& a, bool inverse) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double ang = 2.0 * M_PI / static_cast<double>(len) * (inverse ? 1.0 : -1.0);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inverse)
        for (auto& x : a) x /= static_cast<double>(n);
}
inline double besselI0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 60; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1e-17 * sum) break;
    }
    return sum;
}
} // namespace logic_detail

// Kaiser-windowed sinc (cut-off 0.45·fs, β 12, 32 samples), made minimum phase through the real cepstrum (FFT 16384,
// so the cepstrum's time aliasing is negligible), integrated to a step. Built once per process on the control thread
// (Circuit::compile); never call it from the audio thread.
inline std::shared_ptr<const MinBlep> buildMinBlep(int zc = 32, int over = 64, double cutoff = 0.45, double beta = 12.0) {
    using cd = std::complex<double>;
    auto m = std::make_shared<MinBlep>();
    m->zc = zc;
    m->over = over;
    const int L = zc * over;
    std::size_t nfft = 1;
    while (nfft < static_cast<std::size_t>(8 * L)) nfft <<= 1;
    std::vector<cd> a(nfft, cd(0.0));
    const double i0b = logic_detail::besselI0(beta);
    for (int k = 0; k <= L; ++k) {
        const double t = (k - 0.5 * L) / over; // samples from the centre
        const double x = 2.0 * cutoff * t;
        const double sinc = std::abs(x) < 1e-12 ? 1.0 : std::sin(M_PI * x) / (M_PI * x);
        const double r = 2.0 * (k - 0.5 * L) / L;
        const double w = logic_detail::besselI0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0b;
        a[static_cast<std::size_t>(k)] = cd(2.0 * cutoff * sinc * w, 0.0);
    }
    logic_detail::fft(a, false);
    double peak = 0;
    for (const auto& x : a) peak = std::max(peak, std::abs(x));
    for (auto& x : a) x = cd(std::log(std::max(std::abs(x), 1e-16 * peak)), 0.0); // log magnitude
    logic_detail::fft(a, true);                                                    // real cepstrum
    for (std::size_t k = 1; k < nfft / 2; ++k) a[k] *= 2.0;                          // fold onto the causal half
    for (std::size_t k = nfft / 2 + 1; k < nfft; ++k) a[k] = 0.0;
    logic_detail::fft(a, false);
    for (auto& x : a) x = std::exp(x);
    logic_detail::fft(a, true); // minimum-phase impulse response
    std::vector<double> h(static_cast<std::size_t>(L + 1));
    for (int k = 0; k <= L; ++k) h[static_cast<std::size_t>(k)] = a[static_cast<std::size_t>(k)].real();
    m->s.assign(static_cast<std::size_t>(L + 1), 0.0);
    m->ds.assign(static_cast<std::size_t>(L + 1), 0.0);
    double acc = 0;
    for (int k = 1; k <= L; ++k) {
        acc += 0.5 * (h[static_cast<std::size_t>(k - 1)] + h[static_cast<std::size_t>(k)]); // trapezoidal integral
        m->s[static_cast<std::size_t>(k)] = acc;
    }
    const double total = acc;
    for (int k = 0; k <= L; ++k) {
        m->s[static_cast<std::size_t>(k)] /= total;
        m->ds[static_cast<std::size_t>(k)] = h[static_cast<std::size_t>(k)] / total; // slope per fine-grid point
    }
    m->s[static_cast<std::size_t>(L)] = 1.0;
    double mom = 0;
    for (int k = 0; k <= L; ++k) mom += k * h[static_cast<std::size_t>(k)];
    m->delay = mom / (total * over); // Σ k·h / Σ h is in fine-grid points; / over → samples
    return m;
}
inline std::shared_ptr<const MinBlep> sharedMinBlep() {
    static const std::shared_ptr<const MinBlep> table = buildMinBlep(); // control thread only (see above)
    return table;
}

struct LogicCell {
    enum Type : std::uint8_t { Dff, Nand, Inv, Jkff };
    static constexpr int kMaxIn = 5;
    static constexpr int kMaxEvents = 24;
    struct Event {
        double t;  // sample time of the output change (sample index, fractional)
        double dv; // change of λ
    };
    struct Out {
        int pin = -1;        // the output pin node (driven by the CmosOut stage)
        int node = -1;       // λ known node
        bool level = false;  // logical value after every scheduled event
        double base = 0.0;   // λ with every retired event applied
        Event ev[kMaxEvents];
        int nev = 0;
    };
    Type type = Inv;
    int part = -1;
    int nin = 1;
    int in[kMaxIn] = {}; // dff: d, clk, set, reset; jkff: j, k, clk, set, reset; nand: a, b; inv: a
    int vdd = 0, vss = 0;
    int nout = 1;
    Out out[2];
    double vth = 0.5;                   // switching threshold, fraction of VDD − VSS
    double tpd[3] = {125e-9, 60e-9, 45e-9}; // propagation delay at 5 / 10 / 15 V supply (s)
    double fs = 96000.0;
    bool blep = true; // false: λ steps at the first sample after the event (sample-quantised, for comparison only)
    bool late = false; // evaluated after the sample (one-sample delay) instead of before its first reader
    double q0 = 0.0;   // flip-flop power-up state
    const MinBlep* table = nullptr;
    // Logic nets: an input pin wired straight to another cell's output pin takes that output's events (time and level)
    // instead of re-detecting the band-limited edge on the pin, so chained stages keep exact timing.
    static constexpr int kMaxNet = 8, kMaxSubs = 8;
    struct NetEv {
        double t;
        bool level;
    };
    struct Sub {
        LogicCell* cell;
        int k;
    };
    bool fromNet[kMaxIn] = {};
    Sub subs[2][kMaxSubs] = {};
    int nsub[2] = {0, 0};
    NetEv netq[kMaxIn][kMaxNet] = {};
    int nq[kMaxIn] = {};
    // runtime
    double hist[kMaxIn][4] = {}; // per input: v − threshold at samples n−3 … n (index 3 = newest)
    int nh = 0;
    bool lvl[kMaxIn] = {};
    bool q = false;
    double n = 0.0; // current sample index

    // Same-structure state hand-off (Circuit::adoptStateFrom): runtime state only, never the wiring.
    void adoptRuntime(const LogicCell& o) noexcept {
        std::copy(&o.hist[0][0], &o.hist[0][0] + kMaxIn * 4, &hist[0][0]);
        nh = o.nh;
        std::copy(o.lvl, o.lvl + kMaxIn, lvl);
        q = o.q;
        n = o.n;
        for (int j = 0; j < 2; ++j) {
            out[j].level = o.out[j].level;
            out[j].base = o.out[j].base;
            out[j].nev = o.out[j].nev;
            std::copy(o.out[j].ev, o.out[j].ev + kMaxEvents, out[j].ev);
        }
        for (int k = 0; k < kMaxIn; ++k) {
            nq[k] = o.nq[k];
            std::copy(o.netq[k], o.netq[k] + kMaxNet, netq[k]);
        }
    }
    double threshold(const double* V) const noexcept { return V[vss] + vth * (V[vdd] - V[vss]); }
    double delaySamples(const double* V) const noexcept {
        const double vs = std::clamp(V[vdd] - V[vss], 3.0, 20.0);
        // log-log interpolation between the datasheet's 5, 10 and 15 V points (extrapolated outside)
        const double x = std::log(vs);
        const double x0 = std::log(5.0), x1 = std::log(10.0), x2 = std::log(15.0);
        double y;
        if (x < x1) y = std::log(tpd[0]) + (std::log(tpd[1]) - std::log(tpd[0])) * (x - x0) / (x1 - x0);
        else y = std::log(tpd[1]) + (std::log(tpd[2]) - std::log(tpd[1])) * (x - x1) / (x2 - x1);
        return std::exp(y) * fs;
    }
    // Logical outputs for the current input levels and state.
    void evaluate(bool clkRise, bool& o0, bool& o1) noexcept {
        switch (type) {
            case Inv:
                o0 = !lvl[0];
                o1 = false;
                return;
            case Nand:
                o0 = !(lvl[0] && lvl[1]);
                o1 = false;
                return;
            case Jkff: {
                // CD4027B truth table: set / reset as the 4013 (both high: Q = Q̄ = 1); on the clock's rising edge
                // Q ← J·Q̄ + K̄·Q (J = K = 1 toggles)
                const bool s = lvl[3], r = lvl[4];
                if (s && !r) q = true;
                else if (r && !s) q = false;
                else if (!s && !r && clkRise) q = (lvl[0] && !q) || (!lvl[1] && q);
                o0 = s ? true : (r ? false : q);
                o1 = r ? true : (s ? false : !q);
                return;
            }
            case Dff: {
                const bool s = lvl[2], r = lvl[3];
                if (s && !r) q = true;
                else if (r && !s) q = false;
                else if (!s && !r && clkRise) q = lvl[0];
                o0 = s ? true : (r ? false : q);
                o1 = r ? true : (s ? false : !q);
                return;
            }
        }
    }
    // Power-up / DC: levels straight from the node voltages, no events.
    void reset(const double* V) noexcept {
        const double th = threshold(V);
        for (int k = 0; k < nin; ++k) {
            lvl[k] = V[in[k]] > th;
            for (int h = 0; h < 4; ++h) hist[k][h] = V[in[k]] - th;
        }
        nh = 1;
        for (int k = 0; k < kMaxIn; ++k) nq[k] = 0;
        q = q0 > 0.5;
        n = 0.0;
        bool o0, o1;
        evaluate(false, o0, o1);
        const bool o[2] = {o0, o1};
        for (int j = 0; j < nout; ++j) {
            out[j].level = o[j];
            out[j].base = o[j] ? 1.0 : 0.0;
            out[j].nev = 0;
        }
    }
    // DC settling pass: re-read the levels (no events) and report whether an output changed.
    bool settleDc(double* V) noexcept {
        const double th = threshold(V);
        for (int k = 0; k < nin; ++k) {
            lvl[k] = V[in[k]] > th;
            for (int h = 0; h < 4; ++h) hist[k][h] = V[in[k]] - th;
        }
        bool o0, o1;
        evaluate(false, o0, o1);
        const bool o[2] = {o0, o1};
        bool changed = false;
        for (int j = 0; j < nout; ++j) {
            changed |= o[j] != out[j].level;
            out[j].level = o[j];
            out[j].base = o[j] ? 1.0 : 0.0;
            out[j].nev = 0;
            V[out[j].node] = out[j].base;
        }
        return changed;
    }
    // Crossing time of input k within the last sample, as a fraction in (−1, 0] (0 = this sample).
    double crossing(int k) const noexcept {
        const double* d = hist[k];
        const double d0 = d[3], d1 = d[2];
        double t = d1 != d0 ? -1.0 + d1 / (d1 - d0) : 0.0; // linear estimate
        t = std::clamp(t, -1.0, 0.0);
        if (nh < 4) return t;
        // cubic Lagrange through t = −3, −2, −1, 0, refined by Newton on (−1, 0]
        const double a = d[0], b = d[1];
        auto P = [&](double x, double& dp) {
            const double l0 = -(x + 2) * (x + 1) * x / 6.0, l1 = (x + 3) * (x + 1) * x / 2.0, l2 = -(x + 3) * (x + 2) * x / 2.0,
                         l3 = (x + 3) * (x + 2) * (x + 1) / 6.0;
            const double e0 = -((x + 1) * x + (x + 2) * x + (x + 2) * (x + 1)) / 6.0,
                         e1 = ((x + 1) * x + (x + 3) * x + (x + 3) * (x + 1)) / 2.0,
                         e2 = -((x + 2) * x + (x + 3) * x + (x + 3) * (x + 2)) / 2.0,
                         e3 = ((x + 2) * (x + 1) + (x + 3) * (x + 1) + (x + 3) * (x + 2)) / 6.0;
            dp = a * e0 + b * e1 + d1 * e2 + d0 * e3;
            return a * l0 + b * l1 + d1 * l2 + d0 * l3;
        };
        double x = t;
        for (int it = 0; it < 6; ++it) {
            double dp;
            const double p = P(x, dp);
            if (!(std::abs(dp) > 1e-30)) break;
            const double nx = std::clamp(x - p / dp, -1.0, 0.0);
            if (std::abs(nx - x) < 1e-13) {
                x = nx;
                break;
            }
            x = nx;
        }
        return x;
    }
    void schedule(int j, double t, bool level) noexcept {
        Out& o = out[j];
        if (level == o.level) return;
        o.level = level;
        for (int s = 0; s < nsub[j]; ++s) {
            LogicCell& c = *subs[j][s].cell;
            const int k = subs[j][s].k;
            if (c.nq[k] == kMaxNet) { // drop the oldest pending edge (only an output toggling every sample fills it)
                for (int m = 1; m < c.nq[k]; ++m) c.netq[k][m - 1] = c.netq[k][m];
                --c.nq[k];
            }
            c.netq[k][c.nq[k]++] = {t, level};
        }
        const double dv = level ? 1.0 : -1.0;
        if (o.nev == kMaxEvents) { // ring full: retire the oldest (only reached by inputs toggling every sample)
            o.base += o.ev[0].dv;
            for (int k = 1; k < o.nev; ++k) o.ev[k - 1] = o.ev[k];
            --o.nev;
        }
        o.ev[o.nev++] = {t, dv};
    }
    // One sample: read the inputs, place the crossings, run the logic at each crossing time in order, schedule the
    // output changes, and write λ for this sample (or, when `late`, for the next one: a one-sample delay).
    void step(double* V) noexcept {
        n += 1.0;
        const double th = threshold(V);
        struct InEv {
            double t;
            int k;
            bool level;
        } evs[kMaxIn * (kMaxNet + 1)];
        int ne = 0;
        for (int k = 0; k < nin; ++k) {
            double* d = hist[k];
            d[0] = d[1], d[1] = d[2], d[2] = d[3];
            d[3] = V[in[k]] - th;
            if (fromNet[k]) { // edges from the driving cell, up to this sample's time
                int used = 0;
                while (used < nq[k] && netq[k][used].t <= n) {
                    evs[ne++] = {netq[k][used].t, k, netq[k][used].level};
                    ++used;
                }
                for (int m = used; m < nq[k]; ++m) netq[k][m - used] = netq[k][m];
                nq[k] -= used;
                continue;
            }
            const bool now = d[3] > 0.0;
            if (now != lvl[k]) evs[ne++] = {n + crossing(k), k, now};
        }
        nh = std::min(nh + 1, 4);
        std::sort(evs, evs + ne, [](const InEv& x, const InEv& y) { return x.t < y.t; });
        const double dly = delaySamples(V);
        for (int e = 0; e < ne; ++e) {
            const int k = evs[e].k;
            if (lvl[k] == evs[e].level) continue;
            lvl[k] = evs[e].level;
            bool o0, o1;
            evaluate((type == Dff && k == 1 && lvl[1]) || (type == Jkff && k == 2 && lvl[2]), o0, o1);
            schedule(0, evs[e].t + dly, o0);
            if (nout > 1) schedule(1, evs[e].t + dly, o1);
        }
        for (int j = 0; j < nout; ++j) {
            Out& o = out[j];
            double v = o.base;
            int keep = 0;
            for (int k = 0; k < o.nev; ++k) {
                const double x = n - o.ev[k].t;
                if (x >= table->zc) {
                    o.base += o.ev[k].dv;
                    v += o.ev[k].dv;
                    continue;
                }
                v += o.ev[k].dv * (blep ? table->step(x) : (x >= 0.0 ? 1.0 : 0.0));
                o.ev[keep++] = o.ev[k];
            }
            o.nev = keep;
            V[o.node] = v;
        }
    }
};

} // namespace rig::ce
