#pragma once
// Analog-modelled guitar amplifier & drive-pedal building blocks.
//
//  * HalfbandIIR / Oversampler   — polyphase elliptic half-band (de Soras design) up/down sampling
//  * Svf                         — Cytomic trapezoidal SVF (LP/HP/BP/bell/shelves), stable at 4x rates
//  * OnePole                     — TPT one-pole (LP/HP/shelf)
//  * TriodeStage                 — common-cathode 12AX7 stage: coupled grid RC state, partial cathode bypass,
//                                  grid-conduction / cutoff asymmetry, Miller LP
//  * FmvToneStack                — Yeh/Smith analytic Fender/Marshall/Vox passive tone stack (3rd order)
//  * PowerAmp                    — phase inverter + push-pull transconductance pentode pair, supply sag,
//                                  bi-directional reactive speaker impedance load inside the NFB loop
//
// Units: 1.0 full-scale ≈ 1 V at the guitar jack. Triode grid conduction knee is at |u| ≈ 1.
#include "circuit/TubeDevice.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

namespace rig::amp {

inline constexpr float kPi = std::numbers::pi_v<float>;

inline float fastTanh(float x) noexcept {
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

inline float dbToLin(float db) noexcept { return std::pow(10.0f, db * 0.05f); }

// Audio-taper potentiometer (≈ 10% resistance at half rotation).
inline float audioTaper(float p) noexcept {
    p = std::clamp(p, 0.0f, 1.0f);
    constexpr float a = 4.2f;
    return (std::exp(a * p) - 1.0f) / (std::exp(a) - 1.0f);
}

// ───────────────────────────── Half-band polyphase IIR ─────────────────────────────
// Coefficients computed with Laurent de Soras' elliptic design (hiir), so the
// oversampler gets >100 dB image rejection with a narrow transition band.
class HalfbandDesign {
public:
    static std::vector<double> compute(int nbr_coefs, double transition) {
        double k = 0.0, q = 0.0;
        computeTransitionParam(transition, k, q);
        const int order = nbr_coefs * 2 + 1;
        std::vector<double> c(static_cast<std::size_t>(nbr_coefs));
        for (int i = 0; i < nbr_coefs; ++i) c[static_cast<std::size_t>(i)] = computeCoef(i, k, q, order);
        return c;
    }

private:
    static void computeTransitionParam(double transition, double& k, double& q) {
        k = std::tan((1.0 - transition * 2.0) * std::numbers::pi / 4.0);
        k *= k;
        const double kksqrt = std::pow(1.0 - k * k, 0.25);
        const double e = 0.5 * (1.0 - kksqrt) / (1.0 + kksqrt);
        const double e2 = e * e;
        const double e4 = e2 * e2;
        q = e * (1.0 + e4 * (2.0 + e4 * (15.0 + 150.0 * e4)));
    }
    static double accNum(double q, int order, int c) {
        double acc = 0.0, term = 0.0;
        int i = 0, j = 1;
        do {
            term = std::pow(q, static_cast<double>(i * (i + 1))) *
                   std::sin(static_cast<double>((i * 2 + 1) * c) * std::numbers::pi / order) * j;
            acc += term;
            j = -j;
            ++i;
        } while (std::abs(term) > 1e-100 && i < 64);
        return acc;
    }
    static double accDen(double q, int order, int c) {
        double acc = 0.0, term = 0.0;
        int i = 1, j = -1;
        do {
            term = std::pow(q, static_cast<double>(i * i)) *
                   std::cos(static_cast<double>(i * 2 * c) * std::numbers::pi / order) * j;
            acc += term;
            j = -j;
            ++i;
        } while (std::abs(term) > 1e-100 && i < 64);
        return acc;
    }
    static double computeCoef(int index, double k, double q, int order) {
        const int c = index + 1;
        const double num = accNum(q, order, c) * std::pow(q, 0.25);
        const double den = accDen(q, order, c) + 0.5;
        const double ww = num / den;
        const double wwsq = ww * ww;
        const double x = std::sqrt((1.0 - wwsq * k) * (1.0 - wwsq / k)) / (1.0 + wwsq);
        return (1.0 - x) / (1.0 + x);
    }
};

template <int NC>
class HalfbandIIR {
public:
    void setCoefs(const std::vector<double>& c) noexcept {
        for (int i = 0; i < NC; ++i) coef_[static_cast<std::size_t>(i)] = static_cast<float>(c[static_cast<std::size_t>(i)]);
        reset();
    }
    void reset() noexcept {
        x_.fill(0.0f);
        y_.fill(0.0f);
    }
    // 1 -> 2 samples
    void up(float in, float& o0, float& o1) noexcept {
        float even = in, odd = in;
        for (int i = 0; i < NC; i += 2) {
            even = section(even, i);
            if (i + 1 < NC) odd = section(odd, i + 1);
        }
        o0 = even;
        o1 = odd;
    }
    // 2 -> 1 samples
    float down(float i0, float i1) noexcept {
        float a = i1, b = i0;
        for (int i = 0; i < NC; i += 2) {
            a = section(a, i);
            if (i + 1 < NC) b = section(b, i + 1);
        }
        return 0.5f * (a + b);
    }

private:
    float section(float in, int i) noexcept {
        const auto u = static_cast<std::size_t>(i);
        const float out = (in - y_[u]) * coef_[u] + x_[u];
        x_[u] = in;
        y_[u] = out;
        return out;
    }
    std::array<float, NC> coef_{};
    std::array<float, NC> x_{};
    std::array<float, NC> y_{};
};

// Up to 8x oversampler (cascade of half-band stages; first stage steepest).
class Oversampler {
public:
    void prepare(int factor) {
        factor_ = (factor >= 8) ? 8 : (factor >= 4 ? 4 : (factor >= 2 ? 2 : 1));
        static const std::vector<double> c1 = HalfbandDesign::compute(10, 0.035);
        static const std::vector<double> c2 = HalfbandDesign::compute(6, 0.20);
        static const std::vector<double> c3 = HalfbandDesign::compute(4, 0.30);
        for (auto& s : up1_) s.setCoefs(c1);
        for (auto& s : dn1_) s.setCoefs(c1);
        for (auto& s : up2_) s.setCoefs(c2);
        for (auto& s : dn2_) s.setCoefs(c2);
        for (auto& s : up3_) s.setCoefs(c3);
        for (auto& s : dn3_) s.setCoefs(c3);
    }
    void reset() {
        for (int c = 0; c < 2; ++c) {
            up1_[c].reset(); dn1_[c].reset();
            up2_[c].reset(); dn2_[c].reset();
            up3_[c].reset(); dn3_[c].reset();
        }
    }
    int factor() const noexcept { return factor_; }

    // Writes `factor()` samples into `out`.
    void up(int ch, float x, float* out) noexcept {
        if (factor_ == 1) { out[0] = x; return; }
        float a, b;
        up1_[ch].up(x, a, b);
        if (factor_ == 2) { out[0] = a; out[1] = b; return; }
        float a0, a1, b0, b1;
        up2_[ch].up(a, a0, a1);
        up2_[ch].up(b, b0, b1);
        if (factor_ == 4) { out[0] = a0; out[1] = a1; out[2] = b0; out[3] = b1; return; }
        up3_[ch].up(a0, out[0], out[1]);
        up3_[ch].up(a1, out[2], out[3]);
        up3_[ch].up(b0, out[4], out[5]);
        up3_[ch].up(b1, out[6], out[7]);
    }
    float down(int ch, const float* in) noexcept {
        if (factor_ == 1) return in[0];
        if (factor_ == 2) return dn1_[ch].down(in[0], in[1]);
        if (factor_ == 4) {
            const float a = dn2_[ch].down(in[0], in[1]);
            const float b = dn2_[ch].down(in[2], in[3]);
            return dn1_[ch].down(a, b);
        }
        const float a0 = dn3_[ch].down(in[0], in[1]);
        const float a1 = dn3_[ch].down(in[2], in[3]);
        const float b0 = dn3_[ch].down(in[4], in[5]);
        const float b1 = dn3_[ch].down(in[6], in[7]);
        return dn1_[ch].down(dn2_[ch].down(a0, a1), dn2_[ch].down(b0, b1));
    }

private:
    int factor_ = 1;
    std::array<HalfbandIIR<10>, 2> up1_{}, dn1_{};
    std::array<HalfbandIIR<6>, 2> up2_{}, dn2_{};
    std::array<HalfbandIIR<4>, 2> up3_{}, dn3_{};
};

// ───────────────────────────── Linear filters ─────────────────────────────
class OnePole {
public:
    void set(double sr, float hz) noexcept {
        const float f = std::clamp(hz, 1.0f, static_cast<float>(sr * 0.45));
        const float wc = std::tan(kPi * f / static_cast<float>(sr));
        g_ = wc / (1.0f + wc);
        hz_ = f;
    }
    void adoptRuntimeStateFrom(const OnePole& previous) noexcept { z_ = previous.z_; }
    float hz() const noexcept { return hz_; } // corner actually in use (after clamping)
    void reset() noexcept { z_ = 0.0f; }
    float lp(float x) noexcept {
        const float v = (x - z_) * g_;
        const float y = v + z_;
        z_ = y + v;
        return y;
    }
    float hp(float x) noexcept { return x - lp(x); }

private:
    float g_ = 0.1f;
    float z_ = 0.0f;
    float hz_ = 0.0f;
};

class Svf {
public:
    enum class Type { LowPass, HighPass, BandPass, Bell, LowShelf, HighShelf };

    void set(Type t, double sr, float hz, float q, float gain_db = 0.0f) noexcept {
        const float f = std::clamp(hz, 5.0f, static_cast<float>(sr * 0.45));
        type_ = t; hz_ = f; q_ = q; gain_db_ = gain_db;
        const float A = std::pow(10.0f, gain_db / 40.0f);
        float g = std::tan(kPi * f / static_cast<float>(sr));
        float k = 1.0f / std::max(0.05f, q);
        m0_ = 0.0f; m1_ = 0.0f; m2_ = 0.0f;
        switch (t) {
            case Type::LowPass: m2_ = 1.0f; break;
            case Type::HighPass: m0_ = 1.0f; m1_ = -k; m2_ = -1.0f; break;
            case Type::BandPass: m1_ = 1.0f; break;
            case Type::Bell:
                k = 1.0f / (std::max(0.05f, q) * A);
                m0_ = 1.0f; m1_ = k * (A * A - 1.0f); m2_ = 0.0f;
                break;
            case Type::LowShelf:
                g /= std::sqrt(A);
                m0_ = 1.0f; m1_ = k * (A - 1.0f); m2_ = A * A - 1.0f;
                break;
            case Type::HighShelf:
                g *= std::sqrt(A);
                m0_ = A * A; m1_ = k * (1.0f - A) * A; m2_ = 1.0f - A * A;
                break;
        }
        a1_ = 1.0f / (1.0f + g * (g + k));
        a2_ = g * a1_;
        a3_ = g * a2_;
    }
    void reset() noexcept { ic1_ = ic2_ = 0.0f; }
    void adoptRuntimeStateFrom(const Svf& previous) noexcept { ic1_ = previous.ic1_; ic2_ = previous.ic2_; }
    // Settings actually in use (for model export).
    Type type() const noexcept { return type_; }
    float hz() const noexcept { return hz_; }
    float q() const noexcept { return q_; }
    float gainDb() const noexcept { return gain_db_; }
    float process(float v0) noexcept {
        const float v3 = v0 - ic2_;
        const float v1 = a1_ * ic1_ + a2_ * v3;
        const float v2 = ic2_ + a2_ * ic1_ + a3_ * v3;
        ic1_ = 2.0f * v1 - ic1_;
        ic2_ = 2.0f * v2 - ic2_;
        return m0_ * v0 + m1_ * v1 + m2_ * v2;
    }

private:
    float a1_ = 0.0f, a2_ = 0.0f, a3_ = 0.0f;
    float m0_ = 1.0f, m1_ = 0.0f, m2_ = 0.0f;
    float ic1_ = 0.0f, ic2_ = 0.0f;
    Type type_ = Type::LowPass;
    float hz_ = 0.0f, q_ = 0.7f, gain_db_ = 0.0f;
};

// ───────────────────────────── Triode gain stage ─────────────────────────────
// Static transfer curves of a real common-cathode 12AX7 stage, solved offline from the Koren triode
// equations (B+ 250 V, 100 k plate load, grid current through a 68 k source, cathode bias network),
// normalised so the small-signal slope is 1 and the grid-conduction knee sits at u ≈ 1.
// Three operating points: hot bypassed (1.5 k), colder bypassed (2.7 k) and an unbypassed 10 k
// "cold clipper" (strongly asymmetric, as used in JCM800 / Soldano / 5150 preamps).
class TriodeCurve {
public:
    static constexpr int kN = 2048;
    static constexpr float kRange = 12.0f; // u in [-kRange, kRange]

    float f(float u) const noexcept {
        const float x = std::clamp((u + kRange) * inv_h_, 0.0f, static_cast<float>(kN - 1) - 1e-3f);
        const int i = static_cast<int>(x);
        const float t = x - static_cast<float>(i);
        return y_[static_cast<std::size_t>(i)] + t * (y_[static_cast<std::size_t>(i) + 1] - y_[static_cast<std::size_t>(i)]);
    }
    // Exact antiderivative of the piecewise-linear curve (for first-order ADAA).
    float F(float u) const noexcept {
        if (u <= -kRange) return F_[0] + y_[0] * (u + kRange);
        if (u >= kRange) return F_[kN - 1] + y_[kN - 1] * (u - kRange);
        const float x = (u + kRange) * inv_h_;
        const int i = std::min(static_cast<int>(x), kN - 2);
        const float t = x - static_cast<float>(i);
        const float y0 = y_[static_cast<std::size_t>(i)], y1 = y_[static_cast<std::size_t>(i) + 1];
        return F_[static_cast<std::size_t>(i)] + h_ * (y0 * t + 0.5f * (y1 - y0) * t * t);
    }

    static const TriodeCurve& get(int variant) {
        static const std::array<TriodeCurve, 3> bank = {TriodeCurve(1500.0, true), TriodeCurve(2700.0, true), TriodeCurve(10000.0, false)};
        return bank[static_cast<std::size_t>(std::clamp(variant, 0, 2))];
    }

private:
    TriodeCurve(double rk, bool bypassed) {
        constexpr double B = 250.0, Rp = 100e3, Rs = 68e3;
        // Koren 12AX7: mu 100, ex 1.4, kg1 1060, kp 600, kvb 300
        auto koren = [](double vgk, double vpk) {
            if (vpk <= 0.0) return 0.0;
            const double e1 = (vpk / 600.0) * std::log1p(std::exp(600.0 * (1.0 / 100.0 + vgk / std::sqrt(300.0 + vpk * vpk))));
            return e1 > 0.0 ? 2.0 * std::pow(e1, 1.4) / 1060.0 : 0.0;
        };
        // Grid conduction: the law shared with circuit::TubeDevice.
        auto gridI = [](double vgk) { return circuit::GridLaw::current(vgk); };
        // Plate voltage for a given grid & cathode voltage (bisection on the load line).
        auto plate = [&](double vg, double vk) {
            double lo = vk, hi = B;
            for (int it = 0; it < 44; ++it) {
                const double vp = 0.5 * (lo + hi);
                const double ip = koren(vg - vk, vp - vk);
                if (B - ip * Rp > vp) lo = vp; else hi = vp;
            }
            return 0.5 * (lo + hi);
        };
        // Effective grid voltage when grid current flows through the source impedance.
        auto gridV = [&](double vin, double vk) {
            double lo = std::min(vin, vk - 5.0), hi = vin;
            if (vin <= vk - 0.5) return vin;
            for (int it = 0; it < 44; ++it) {
                const double vg = 0.5 * (lo + hi);
                if (vg + Rs * gridI(vg - vk) > vin) hi = vg; else lo = vg;
            }
            return 0.5 * (lo + hi);
        };
        // Cathode voltage consistent with the plate current it carries (monotonic → bisection).
        auto solveVk = [&](double vin) {
            double lo = 0.0, hi = 25.0;
            for (int it = 0; it < 48; ++it) {
                const double vk = 0.5 * (lo + hi);
                const double vg = gridV(vin, vk);
                const double vp = plate(vg, vk);
                if (koren(vg - vk, vp - vk) * rk > vk) lo = vk; else hi = vk;
            }
            return 0.5 * (lo + hi);
        };
        const double vk0 = solveVk(0.0); // DC operating point
        const double vp0 = plate(0.0, vk0);
        auto out = [&](double vin) {
            if (bypassed) return plate(gridV(vin, vk0), vk0);
            const double vk = solveVk(vin); // unbypassed cathode: local current feedback
            return plate(gridV(vin, vk), vk);
        };
        const double vref = std::max(0.3, vk0); // u = 1 ↔ grid at the cathode (conduction knee)
        const double d = 0.01 * vref;
        const double gain = (out(-d) - out(d)) / (2.0 * d); // inverting stage: positive number
        h_ = 2.0f * kRange / static_cast<float>(kN - 1);
        inv_h_ = 1.0f / h_;
        for (int i = 0; i < kN; ++i) {
            const double u = -kRange + static_cast<double>(i) * h_;
            y_[static_cast<std::size_t>(i)] = static_cast<float>((vp0 - out(u * vref)) / (gain * vref));
        }
        F_[0] = 0.0f;
        for (int i = 1; i < kN; ++i) F_[static_cast<std::size_t>(i)] = F_[static_cast<std::size_t>(i) - 1] + 0.5f * h_ * (y_[static_cast<std::size_t>(i) - 1] + y_[static_cast<std::size_t>(i)]);
    }

    std::array<float, kN> y_{};
    std::array<float, kN> F_{};
    float h_ = 0.0f, inv_h_ = 1.0f;
};

// Shared 12AX7 common-cathode solution for stages whose cathode voltage is a *state* (Rk ∥ Ck):
//   ip(vgk, vk) — plate current on the 100 k / 250 V load line for a given grid-cathode and cathode voltage,
//   g(d)        — grid-cathode voltage reached when the source (68 k) drives d = vin − vk volts into a
//                 conducting grid,
//   ig(d)       — physical grid conduction current (A) flowing into the grid at drive d = vin − vk.
// Per sample a stage evaluates ip & ig from these tables and integrates both Ck·dvk/dt = ip − vk/Rk
// and the coupled input capacitor Cc·dvc/dt = ig − vc/(Rg+Rs), so partial bypass, blocking distortion
// and bias recovery all emerge from the RC circuit states.
class CathodeTables {
public:
    static constexpr int kNg = 512, kNk = 160, kNd = 2048;
    static constexpr float kGlo = -9.0f, kGhi = 1.5f, kKhi = 10.0f, kDlo = -60.0f, kDhi = 60.0f;
    static constexpr double kB = 250.0, kRp = 100e3, kRs = 68e3;

    static const CathodeTables& get() {
        static const CathodeTables t;
        return t;
    }
    float ip(float vgk, float vk) const noexcept {
        const float x = std::clamp((vgk - kGlo) * gi_, 0.0f, kNg - 1.001f), y = std::clamp(vk * ki_, 0.0f, kNk - 1.001f);
        const int i = static_cast<int>(x), j = static_cast<int>(y);
        const float fx = x - static_cast<float>(i), fy = y - static_cast<float>(j);
        const float* r0 = &ip_[static_cast<std::size_t>(j) * kNg];
        const float* r1 = r0 + kNg;
        const float a = r0[i] + fx * (r0[i + 1] - r0[i]), b = r1[i] + fx * (r1[i + 1] - r1[i]);
        return a + fy * (b - a);
    }
    float grid(float d) const noexcept {
        const float x = std::clamp((d - kDlo) * di_, 0.0f, kNd - 1.001f);
        const int i = static_cast<int>(x);
        const float f = x - static_cast<float>(i);
        return g_[static_cast<std::size_t>(i)] + f * (g_[static_cast<std::size_t>(i) + 1] - g_[static_cast<std::size_t>(i)]);
    }
    // Physical grid current Ig (Amperes) when driving d = vin - vk through Rs = 68 kOhm.
    float gridCurrent(float d) const noexcept {
        const float vgk = grid(d);
        return std::max(0.0f, (d - vgk) * static_cast<float>(1.0 / kRs));
    }

private:
    CathodeTables() {
        auto koren = [](double vgk, double vpk) {
            if (vpk <= 0.0) return 0.0;
            const double e1 = (vpk / 600.0) * std::log1p(std::exp(600.0 * (1.0 / 100.0 + vgk / std::sqrt(300.0 + vpk * vpk))));
            return e1 > 0.0 ? 2.0 * std::pow(e1, 1.4) / 1060.0 : 0.0;
        };
        gi_ = (kNg - 1) / (kGhi - kGlo);
        ki_ = (kNk - 1) / kKhi;
        di_ = (kNd - 1) / (kDhi - kDlo);
        ip_.resize(static_cast<std::size_t>(kNg) * kNk);
        for (int j = 0; j < kNk; ++j) {
            const double vk = static_cast<double>(j) / ki_;
            for (int i = 0; i < kNg; ++i) {
                const double vgk = kGlo + static_cast<double>(i) / gi_;
                double lo = vk, hi = kB;
                for (int it = 0; it < 40; ++it) {
                    const double vp = 0.5 * (lo + hi);
                    if (kB - koren(vgk, vp - vk) * kRp > vp) lo = vp; else hi = vp;
                }
                const double vp = 0.5 * (lo + hi);
                ip_[static_cast<std::size_t>(j) * kNg + i] = static_cast<float>((kB - vp) / kRp);
            }
        }
        auto gridI = [](double e) { return circuit::GridLaw::current(e); }; // shared with circuit::TubeDevice
        g_.resize(kNd);
        for (int i = 0; i < kNd; ++i) {
            const double d = kDlo + static_cast<double>(i) / di_;
            double lo = std::min(d, -10.0), hi = d;
            if (d > -0.5) {
                for (int it = 0; it < 50; ++it) {
                    const double e = 0.5 * (lo + hi);
                    if (e + kRs * gridI(e) > d) hi = e; else lo = e;
                }
                g_[static_cast<std::size_t>(i)] = static_cast<float>(0.5 * (lo + hi));
            } else {
                g_[static_cast<std::size_t>(i)] = static_cast<float>(d);
            }
        }
    }
    std::vector<float> ip_, g_;
    float gi_ = 1.0f, ki_ = 1.0f, di_ = 1.0f;
};

struct StageSpec {
    float gain = 1.0f;       // drive into this stage's grid (normalised)
    float hp_hz = 20.0f;     // input coupling capacitor
    float lp_hz = 12000.0f;  // Miller / plate load roll-off
    float bias = 0.0f;       // operating point: >= -0.08 hot, to -0.3 colder, below that cold clipper
    float cath_hz = 0.0f;    // partial cathode bypass: gain reduced below this frequency
    float cath_db = 0.0f;    // amount of low-frequency gain reduction (negative dB)
    float lim_pos = 1.0f;    // (analytic stages only) positive knee
    float lim_neg = 1.6f;    // (analytic stages only) negative knee
    float blocking = 0.35f;  // grid-current bias shift amount
};

class TriodeStage {
public:
    // Coupling network into every stage's grid: Cc from the previous plate, Rg = 1 MΩ grid leak to
    // the bias reference, and the conducting grid-cathode diode (circuit::GridLaw). Cc is sized so
    // the linear corner 1/(2π·Rg·Cc) is the voicing's hp_hz.
    static constexpr float kRg = 1.0e6f;
    // Grid-cathode volts per normalised unit for the static-curve (cold clipper) and analytic
    // (cathode follower / solid-state) stages, whose transfer is expressed in normalised units.
    static constexpr float kStaticVoltsPerUnit = 1.5f;
    static constexpr float kAnalyticVoltsPerUnit = 1.0f;
    // `blocking` = 0.35 (the default voicing) charges Cc with exactly the physical grid current.
    static constexpr float kUnityBlocking = 0.35f;
    static constexpr float kFollowerChargeFraction = 0.25f;

    void prepare(double sr, const StageSpec& s) noexcept {
        spec_ = s;
        lp_.set(sr, s.lp_hz);
        if (s.cath_hz > 0.0f && std::abs(s.cath_db) > 0.01f) {
            cath_lp_.set(sr, s.cath_hz);
            cath_k_ = dbToLin(s.cath_db);
        } else {
            cath_k_ = 1.0f;
        }
        // Cathode followers and solid-state stages keep the analytic soft clipper; real triode gain
        // stages use the Koren-derived curves.
        analytic_ = s.lim_neg >= 2.4f || s.lim_pos >= 2.0f;
        curve_ = &TriodeCurve::get(curveVariantFor(s.bias));
        // Bypassed-cathode gain stages run the dynamic circuit (Rk ∥ Ck as a state); the unbypassed
        // cold-clipper and followers keep their static curves.
        dynamic_ = !analytic_ && s.bias >= -0.3f;
        dt_ = 1.0f / static_cast<float>(sr);
        // 1-node coupled-grid state vc_ (the coupling capacitor's voltage). Forward-Euler on Cc is
        // stable while dt/(Cc·Rs) < 2, i.e. for corners up to ≈4 kHz at 192 kHz: clamp to 2 kHz.
        const float fc_cap = std::clamp(s.hp_hz, 1.0f, 2000.0f);
        cc_ = 1.0f / (2.0f * kPi * kRg * fc_cap);
        leak_ = dt_ / (kRg * cc_);       // per-sample discharge fraction through Rg
        charge_ = dt_ / cc_;             // volts per ampere per sample
        ig_scale_ = std::max(0.0f, s.blocking) / kUnityBlocking;
        // Followers are DC-coupled to the previous plate in most amps: their grid current mostly
        // loads that plate (part of the analytic curve) and only a small share charges a network.
        if (analytic_) ig_scale_ *= kFollowerChargeFraction;
        if (dynamic_) {
            const auto& T = CathodeTables::get();
            rk_ = (s.bias >= -0.08f) ? 1500.0f : 2700.0f;
            // Partial bypass corner from the voicing (fully bypassed when none given).
            const float corner = (s.cath_hz > 0.0f && std::abs(s.cath_db) > 0.01f) ? s.cath_hz : 4.0f;
            ck_ = 1.0f / (2.0f * kPi * rk_ * corner);
            // DC operating point and small-signal (bypassed) gain for normalisation.
            float vk = 1.0f;
            for (int it = 0; it < 400; ++it) vk += 0.05f * (T.ip(T.grid(-vk), vk) * rk_ - vk);
            vk0_ = vk;
            ip0_ = T.ip(T.grid(-vk0_), vk0_);
            const float dv = 0.01f * vk0_;
            const float gm = (T.ip(T.grid(dv - vk0_), vk0_) - T.ip(T.grid(-dv - vk0_), vk0_)) / (2.0f * dv);
            gnorm_ = 1.0f / std::max(1e-6f, gm * static_cast<float>(CathodeTables::kRp) * vk0_);
            cath_k_ = 1.0f; // the circuit provides the low-frequency gain reduction itself
        }
        inv_lp_ = 1.0f / std::max(0.05f, s.lim_pos);
        inv_ln_ = 1.0f / std::max(0.05f, s.lim_neg);
        const float fs = static_cast<float>(sr);
        bloom_ = 1.0f - std::exp(-1.0f / (0.035f * fs)); // cathode R·C ≈ 35 ms
        gain_ = s.gain;
        reset();
    }
    void reset() noexcept {
        lp_.reset();
        cath_lp_.reset();
        vc_ = 0.0f;
        cath_dc_ = 0.0f;
        vk_ = vk0_;
        u1_ = 0.0f;
        F1_ = curve_ ? curve_->F(0.0f) : 0.0f;
    }
    void adoptRuntimeStateFrom(const TriodeStage& previous) noexcept {
        if (analytic_ != previous.analytic_ || dynamic_ != previous.dynamic_) return;
        lp_.adoptRuntimeStateFrom(previous.lp_);
        cath_lp_.adoptRuntimeStateFrom(previous.cath_lp_);
        vc_ = previous.vc_; cath_dc_ = previous.cath_dc_; vk_ = previous.vk_;
        u1_ = previous.u1_; F1_ = curve_ ? curve_->F(u1_) : previous.F1_;
    }
    void setGain(float g) noexcept { gain_ = g; }

    // ── Introspection (component model export / analysis; never called on the audio path) ──
    const StageSpec& spec() const noexcept { return spec_; }
    bool isAnalytic() const noexcept { return analytic_; }   // cathode follower / solid-state soft clipper
    bool isDynamic() const noexcept { return dynamic_; }     // Koren tables with Rk ∥ Ck cathode state
    int curveVariant() const noexcept { return curveVariantFor(spec_.bias); }
    static int curveVariantFor(float bias) noexcept { return bias >= -0.08f ? 0 : (bias >= -0.3f ? 1 : 2); }
    float cathodeR() const noexcept { return rk_; }
    float cathodeC() const noexcept { return ck_; }
    float couplingC() const noexcept { return cc_; }
    float quiescentVk() const noexcept { return vk0_; }
    float quiescentIp() const noexcept { return ip0_; }
    // Coupling-capacitor voltage in grid-drive volts (positive = grid pushed negative): the blocking bias.
    float couplingVolts() const noexcept {
        return dynamic_ ? vc_ : vc_ * (analytic_ ? kAnalyticVoltsPerUnit : kStaticVoltsPerUnit);
    }
    // Static transfer y(u) at the quiescent operating point (no blocking charge, before the inverting
    // output and Miller low-pass): the exact nonlinearity process() applies for this stage.
    float staticTransfer(float u) const noexcept {
        if (analytic_) {
            u += spec_.bias;
            if (u >= 0.0f) {
                const float r = u * inv_lp_;
                return u / std::sqrt(1.0f + r * r);
            }
            return spec_.lim_neg * fastTanh(u * inv_ln_);
        }
        if (dynamic_) {
            const auto& T = CathodeTables::get();
            const float vgk = T.grid(u * vk0_ - vk0_);
            return (T.ip(vgk, vk0_) - ip0_) * static_cast<float>(CathodeTables::kRp) * gnorm_;
        }
        return curve_ ? curve_->f(u) : u;
    }

    // One sample. The coupling capacitor is the stage's only input high-pass: the grid sees
    // vg = drive − vc − Rs·ic and Cc·dvc/dt = vg/Rg + Ig, so the linear corner, blocking distortion
    // (Cc charging through the conducting grid on hard peaks) and bias recovery (discharge through Rg,
    // τ = Rg·Cc) all come from this single state (cf. circuit::PowerCircuit::coupledGrid).
    float process(float x) noexcept {
        const auto& T = CathodeTables::get();
        if (dynamic_) {
            // Volts: u = 1 ↔ grid at the quiescent cathode voltage.
            const float drive_v = gain_ * x * vk0_ - vc_ - vk_; // open-circuit grid-cathode drive
            const float vgk = T.grid(drive_v);                   // after the 68 k source drop
            const float ig = T.gridCurrent(drive_v);
            const float ip = T.ip(vgk, vk_);
            // Cathode bypass capacitor Ck: Ck * dvk/dt = ip + ig - vk/Rk
            vk_ += dt_ / ck_ * ((ip + ig) - vk_ / rk_);
            vk_ = std::clamp(vk_, 0.0f, CathodeTables::kKhi);
            const float vg = vgk + vk_; // grid voltage to the bias reference (0 at rest)
            vc_ += leak_ * vg + charge_ * ig_scale_ * ig;
            vc_ = std::clamp(vc_, -20.0f * vk0_, 20.0f * vk0_);
            const float y = (ip - ip0_) * static_cast<float>(CathodeTables::kRp) * gnorm_; // plate swing, normalised
            return -lp_.lp(y);
        }
        // Normalised stages: vg is the AC-coupled grid signal in units of the stage's knee voltage.
        float vg = gain_ * x - vc_;
        float g = vg;
        if (cath_k_ != 1.0f) g += (cath_k_ - 1.0f) * cath_lp_.lp(vg);
        float y;
        if (analytic_) {
            const float u = g + spec_.bias;
            if (u >= 0.0f) {
                const float r = u * inv_lp_;
                y = u / std::sqrt(1.0f + r * r);
            } else {
                y = spec_.lim_neg * fastTanh(u * inv_ln_);
            }
            const float ig = T.gridCurrent((u - 1.0f) * kAnalyticVoltsPerUnit); // knee at u = 1, as for the other stages
            vc_ += leak_ * vg + charge_ * ig_scale_ * ig * (1.0f / kAnalyticVoltsPerUnit);
            vc_ = std::clamp(vc_, -20.0f, 20.0f);
            return -lp_.lp(y);
        }
        // Cold-clipper / static-curve stage with the coupled grid state + cathode bloom:
        const float u = g - 0.25f * cath_dc_;
        // First-order antiderivative anti-aliasing on top of the oversampling.
        const float F = curve_->F(u);
        const float du = u - u1_;
        y = (std::abs(du) > 1e-4f) ? (F - F1_) / du : curve_->f(0.5f * (u + u1_));
        u1_ = u;
        F1_ = F;
        const float ig = T.gridCurrent((u - 1.0f) * kStaticVoltsPerUnit); // knee at u = 1
        vc_ += leak_ * vg + charge_ * ig_scale_ * ig * (1.0f / kStaticVoltsPerUnit);
        vc_ = std::clamp(vc_, -20.0f, 20.0f);
        cath_dc_ += (y - cath_dc_) * bloom_;
        return -lp_.lp(y); // common-cathode stage inverts
    }

private:
    StageSpec spec_{};
    const TriodeCurve* curve_ = nullptr;
    bool analytic_ = false;
    OnePole lp_{}, cath_lp_{};
    float cath_k_ = 1.0f;
    float inv_lp_ = 1.0f, inv_ln_ = 0.6f;
    float gain_ = 1.0f;
    float vc_ = 0.0f, cath_dc_ = 0.0f;
    float u1_ = 0.0f, F1_ = 0.0f;
    float bloom_ = 0.001f;
    bool dynamic_ = false;
    float rk_ = 1500.0f, ck_ = 1e-5f, cc_ = 2.2e-8f, leak_ = 1e-4f, charge_ = 1.0f, ig_scale_ = 1.0f;
    float dt_ = 1.0f / 192000.0f, vk_ = 1.0f, vk0_ = 1.0f, ip0_ = 0.0f, gnorm_ = 1.0f;
};

// ───────────────────────────── Passive tone stack ─────────────────────────────
struct ToneStackValues {
    double C1, C2, C3, R1, R2, R3, R4;
};

namespace stacks {
inline constexpr ToneStackValues kFender{250e-12, 20e-9, 20e-9, 250e3, 1e6, 25e3, 56e3};
inline constexpr ToneStackValues kMarshall{470e-12, 22e-9, 22e-9, 220e3, 1e6, 22e3, 33e3};
inline constexpr ToneStackValues kModern{500e-12, 22e-9, 22e-9, 250e3, 1e6, 25e3, 47e3};   // Soldano/5150/Mesa-style
inline constexpr ToneStackValues kHiwatt{470e-12, 22e-9, 22e-9, 220e3, 1e6, 47e3, 22e3};   // mid-forward
inline constexpr ToneStackValues kVox{100e-12, 22e-9, 22e-9, 1e6, 1e6, 47e3, 33e3};
inline constexpr ToneStackValues kBass{330e-12, 22e-9, 47e-9, 250e3, 1e6, 25e3, 33e3};
} // namespace stacks

class FmvToneStack {
public:
    void prepare(double sr, const ToneStackValues& v) {
        sr_ = sr;
        vals_ = v;
        // Normalise so that the "all knobs at noon" response peaks at 0 dB in the guitar band.
        computeAnalog(0.5, 0.5, 0.5);
        double peak = 1e-9;
        for (double f = 80.0; f < 6000.0; f *= 1.06) peak = std::max(peak, magnitudeAt(f));
        makeup_ = 1.0 / peak;
        update(0.5f, 0.5f, 0.5f);
        reset();
    }
    void reset() noexcept { z_.fill(0.0); }
    void adoptRuntimeStateFrom(const FmvToneStack& previous) noexcept {
        z_ = previous.z_; cb_ = previous.cb_; cm_ = previous.cm_; ct_ = previous.ct_;
        tick_ = previous.tick_;
        updateNow(cb_, cm_, ct_);
    }

    // Control thread: set target knob positions; the audio thread glides there (see process()).
    void setTarget(float bass, float mid, float treble) noexcept {
        tb_ = bass;
        tm_ = mid;
        tt_ = treble;
    }

    void update(float bass, float mid, float treble) {
        cb_ = tb_ = bass;
        cm_ = tm_ = mid;
        ct_ = tt_ = treble;
        updateNow(bass, mid, treble);
    }

    void updateNow(float bass, float mid, float treble) {
        double l, m, t;
        potPositions(bass, mid, treble, l, m, t);
        computeAnalog(l, m, t);
        bilinear();
    }

    // Knob (0..1) → pot wiper fraction. Bass/treble pots are audio taper, mid is linear.
    static void potPositions(float bass, float mid, float treble, double& l, double& m, double& t) noexcept {
        l = std::clamp(static_cast<double>(audioTaper(bass)) * 0.98 + 0.02 * bass, 1e-3, 1.0);
        m = std::clamp(static_cast<double>(mid), 1e-3, 1.0);
        t = std::clamp(0.6 * treble + 0.4 * static_cast<double>(audioTaper(treble)), 1e-3, 1.0);
    }
    // Analysis only (model export): |H(j2πf)| of the analog prototype at the given knob positions,
    // including the noon-normalising makeup gain. Overwrites the analog coefficients, so call it on a
    // private instance, never on one that is processing audio.
    double analysisMagnitude(double f, float bass, float mid, float treble) {
        double l, m, t;
        potPositions(bass, mid, treble, l, m, t);
        computeAnalog(l, m, t);
        return magnitudeAt(f) * makeup_;
    }
    const ToneStackValues& values() const noexcept { return vals_; }

    float process(float xf) noexcept {
        if (++tick_ >= 64) {
            tick_ = 0;
            if (std::abs(tb_ - cb_) + std::abs(tm_ - cm_) + std::abs(tt_ - ct_) > 1e-4f) {
                cb_ += (tb_ - cb_) * 0.2f;
                cm_ += (tm_ - cm_) * 0.2f;
                ct_ += (tt_ - ct_) * 0.2f;
                updateNow(cb_, cm_, ct_);
            }
        }
        const double x = xf;
        const double y = B_[0] * x + z_[0];
        z_[0] = B_[1] * x - A_[1] * y + z_[1];
        z_[1] = B_[2] * x - A_[2] * y + z_[2];
        z_[2] = B_[3] * x - A_[3] * y;
        return static_cast<float>(y * makeup_);
    }

private:
    void computeAnalog(double l, double m, double t) {
        const double C1 = vals_.C1, C2 = vals_.C2, C3 = vals_.C3;
        const double R1 = vals_.R1, R2 = vals_.R2, R3 = vals_.R3, R4 = vals_.R4;
        b_[0] = 0.0;
        b_[1] = t * C1 * R1 + m * C3 * R3 + l * (C1 * R2 + C2 * R2) + (C1 * R3 + C2 * R3);
        b_[2] = t * (C1 * C2 * R1 * R4 + C1 * C3 * R1 * R4) - m * m * (C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
                m * (C1 * C3 * R1 * R3 + C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
                l * (C1 * C2 * R1 * R2 + C1 * C2 * R2 * R4 + C1 * C3 * R2 * R4) +
                l * m * (C1 * C3 * R2 * R3 + C2 * C3 * R2 * R3) +
                (C1 * C2 * R1 * R3 + C1 * C2 * R3 * R4 + C1 * C3 * R3 * R4);
        b_[3] = l * m * (C1 * C2 * C3 * R1 * R2 * R3 + C1 * C2 * C3 * R2 * R3 * R4) -
                m * m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4) +
                m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4) +
                t * C1 * C2 * C3 * R1 * R3 * R4 - t * m * C1 * C2 * C3 * R1 * R3 * R4 +
                t * l * C1 * C2 * C3 * R1 * R2 * R4;
        a_[0] = 1.0;
        a_[1] = (C1 * R1 + C1 * R3 + C2 * R3 + C2 * R4 + C3 * R4) + m * C3 * R3 + l * (C1 * R2 + C2 * R2);
        a_[2] = m * (C1 * C3 * R1 * R3 - C2 * C3 * R3 * R4 + C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
                l * m * (C1 * C3 * R2 * R3 + C2 * C3 * R2 * R3) - m * m * (C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
                l * (C1 * C2 * R2 * R4 + C1 * C2 * R1 * R2 + C1 * C3 * R2 * R4 + C2 * C3 * R2 * R4) +
                (C1 * C2 * R1 * R4 + C1 * C3 * R1 * R4 + C1 * C2 * R3 * R4 + C1 * C2 * R1 * R3 + C1 * C3 * R3 * R4 +
                 C2 * C3 * R3 * R4);
        a_[3] = l * m * (C1 * C2 * C3 * R1 * R2 * R3 + C1 * C2 * C3 * R2 * R3 * R4) -
                m * m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4) +
                m * (C1 * C2 * C3 * R3 * R3 * R4 + C1 * C2 * C3 * R1 * R3 * R3 - C1 * C2 * C3 * R1 * R3 * R4) +
                l * C1 * C2 * C3 * R1 * R2 * R4 + C1 * C2 * C3 * R1 * R3 * R4;
    }

    double magnitudeAt(double f) const {
        const double w = 2.0 * std::numbers::pi * f;
        // s = jw
        const double nr = b_[0] - b_[2] * w * w, nim = b_[1] * w - b_[3] * w * w * w;
        const double dr = a_[0] - a_[2] * w * w, di = a_[1] * w - a_[3] * w * w * w;
        return std::sqrt((nr * nr + nim * nim) / (dr * dr + di * di));
    }

    void bilinear() {
        const double c = 2.0 * sr_;
        static constexpr double P[4][4] = {{1, 3, 3, 1}, {1, 1, -1, -1}, {1, -1, -1, 1}, {1, -3, 3, -1}};
        double B[4] = {0, 0, 0, 0}, A[4] = {0, 0, 0, 0};
        double ck = 1.0;
        for (int k = 0; k < 4; ++k) {
            for (int n = 0; n < 4; ++n) {
                B[n] += b_[k] * ck * P[k][n];
                A[n] += a_[k] * ck * P[k][n];
            }
            ck *= c;
        }
        for (int n = 0; n < 4; ++n) {
            B_[n] = B[n] / A[0];
            A_[n] = A[n] / A[0];
        }
    }

    double sr_ = 192000.0;
    ToneStackValues vals_ = stacks::kMarshall;
    double b_[4] = {0, 0, 0, 0};
    double a_[4] = {1, 0, 0, 0};
    double B_[4] = {1, 0, 0, 0};
    double A_[4] = {1, 0, 0, 0};
    std::array<double, 3> z_{};
    double makeup_ = 1.0;
    float tb_ = 0.5f, tm_ = 0.5f, tt_ = 0.5f, cb_ = 0.5f, cm_ = 0.5f, ct_ = 0.5f;
    int tick_ = 0;
};

// ───────────────────────────── Power amplifier ─────────────────────────────
struct PowerSpec {
    float drive = 1.0f;        // PI drive at master = 1
    float sag = 0.25f;         // supply sag depth
    float asym = 0.06f;        // push-pull imbalance (even harmonics)
    float knee = 1.0f;         // output stage clip knee
    float presence_hz = 3500.0f;
    float presence_db = 7.0f;  // max boost from presence (NFB)
    float depth_hz = 95.0f;
    float depth_db = 7.0f;     // max boost from resonance/depth
    float ot_hp_hz = 55.0f;    // output transformer low corner
    float ot_lp_hz = 11000.0f; // output transformer high corner
    bool class_a = false;      // single-ended-ish / no NFB character (Vox, Tweed)
    float damping = 0.55f;     // output damping (NFB): 1 = stiff solid-state, 0 = no feedback
    float spk_fs = 90.0f;      // speaker/cab resonance the amp is driving
};

// Bi-directional transconductance power amplifier coupled to a nonlinear reactive speaker load:
//   1. The push-pull pentode/tetrode pair acts as a high-impedance transconductance (current-source)
//      generator with finite plate conductance g_p, driving the output transformer and speaker load.
//   2. The speaker load is a physical 2-port state-space / TPT network inside the closed-loop NFB path:
//        * Motional parallel RLC resonator at f_s(x_cone) with excursion-stiffened suspension compliance
//          (cone displacement x_cone shifts f_s upward under heavy low-end excursion) and back-EMF velocity.
//        * Voice-coil inductance L_e(i_spk) with eddy-current loss and current-dependent core saturation.
//        * Thermal voice-coil resistance R_dc(T) heated by true instantaneous electrical power v_spk * i_spk.
//   3. Global negative feedback (fb_) is tapped from the secondary speaker terminal voltage v_spk across
//      this reactive load (shaped by Presence and Depth in the feedback loop), so speaker back-EMF and
//      inductive impedance rise bi-directionally modulate phase-inverter drive, output tube saturation,
//      and screen/plate B+ supply sag.
class PowerAmp {
public:
    void prepare(double sr, const PowerSpec& s) {
        sr_ = sr;
        spec_ = s;
        ot_hp_.set(Svf::Type::HighPass, sr, s.ot_hp_hz, 0.6f);
        ot_lp_.set(Svf::Type::LowPass, sr, s.ot_lp_hz, 0.6f);
        updateSpeakerImpedanceStates();
        pi_hp_.set(sr, 18.0f);
        spk_lf_.set(sr, 160.0f);
        heat_k_ = 1.0f - std::exp(-1.0f / (0.9f * static_cast<float>(sr)));
        env_att_ = 1.0f - std::exp(-1.0f / (0.004f * static_cast<float>(sr)));
        env_rel_ = 1.0f - std::exp(-1.0f / (0.140f * static_cast<float>(sr)));
        // Global negative feedback (speaker secondary terminal → phase inverter), as in the real circuits:
        // presence / resonance act on the feedback signal from the reactive speaker load.
        beta_ = feedbackBeta(spec_);
        tgt_ = Ctl{0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
        cur_ = tgt_;
        recompute();
        reset();
    }
    void reset() {
        ot_hp_.reset();
        ot_lp_.reset();
        spk_res_.reset();
        spk_hi_.reset();
        pres_.reset();
        depth_.reset();
        fb_pres_.reset();
        fb_depth_.reset();
        pi_hp_.reset();
        spk_lf_.reset();
        mot_v_ = 0.0f;
        mot_i_ = 0.0f;
        cone_x_ = 0.0f;
        le_i_ = 0.0f;
        ot_core_flux_ = 0.0f;
        heat_ = 0.0f;
        env_ = 0.0f;
        fb_ = 0.0f;
        tick_ = 0;
    }

    void adoptRuntimeStateFrom(const PowerAmp& previous) noexcept {
        ot_hp_.adoptRuntimeStateFrom(previous.ot_hp_); ot_lp_.adoptRuntimeStateFrom(previous.ot_lp_);
        pres_.adoptRuntimeStateFrom(previous.pres_); depth_.adoptRuntimeStateFrom(previous.depth_);
        fb_pres_.adoptRuntimeStateFrom(previous.fb_pres_); fb_depth_.adoptRuntimeStateFrom(previous.fb_depth_);
        spk_res_.adoptRuntimeStateFrom(previous.spk_res_); spk_hi_.adoptRuntimeStateFrom(previous.spk_hi_);
        pi_hp_.adoptRuntimeStateFrom(previous.pi_hp_); spk_lf_.adoptRuntimeStateFrom(previous.spk_lf_);
        mot_v_ = previous.mot_v_; mot_i_ = previous.mot_i_; cone_x_ = previous.cone_x_;
        le_i_ = previous.le_i_; ot_core_flux_ = previous.ot_core_flux_;
        heat_ = previous.heat_; env_ = previous.env_; fb_ = previous.fb_;
        cab_fs_override_ = previous.cab_fs_override_;
        cab_q_override_ = previous.cab_q_override_;
        cab_le_override_ = previous.cab_le_override_;
        cur_ = previous.cur_; tick_ = previous.tick_;
        updateSpeakerImpedanceStates();
        recompute();
    }

    // Dynamic coupling from downstream CabinetUnit / ConvolutionCabinetComponent:
    // updates the speaker load's fundamental resonance fs_hz, Q factor, and voice-coil inductance scale.
    void setSpeakerLoadProfile(float fs_hz, float q_res = 1.4f, float le_scale = 1.0f) noexcept {
        const float clamped_fs = (fs_hz > 25.0f && fs_hz < 250.0f) ? fs_hz : 0.0f;
        const float clamped_q = std::clamp(q_res, 0.5f, 4.0f);
        const float clamped_le = std::clamp(le_scale, 0.2f, 2.5f);
        if (std::abs(clamped_fs - cab_fs_override_) > 0.2f ||
            std::abs(clamped_q - cab_q_override_) > 0.02f ||
            std::abs(clamped_le - cab_le_override_) > 0.02f) {
            cab_fs_override_ = clamped_fs;
            cab_q_override_ = clamped_q;
            cab_le_override_ = clamped_le;
            updateSpeakerImpedanceStates();
        }
    }

    float effectiveSpeakerFs() const noexcept {
        return cab_fs_override_ > 25.0f ? cab_fs_override_ : spec_.spk_fs;
    }

    // All controls normalised 0..1. Safe to call from the control thread: the audio side glides to the
    // new values (≈ 20 ms) and recomputes its filters at control rate, so knob moves never click.
    void setControls(float master, float presence, float depth, float sag, float vac, float bias) {
        tgt_ = Ctl{master, presence, depth, sag, vac, bias};
    }
    float sagEnvelope() const noexcept { return env_ * sag_; }

    float process(float x) noexcept {
        if (++tick_ >= 32) {
            tick_ = 0;
            glide();
        }
        float u;
        if (beta_ > 0.0f) {
            // Closed-loop NFB tapped directly from the reactive speaker terminal voltage fb_
            // (PI + transconductance output tubes + output transformer + reactive speaker impedance load).
            float f = fb_pres_.process(fb_);
            f = fb_depth_.process(f);
            u = pi_hp_.hp(x) * drive_ * (1.0f + beta_ * drive_) - beta_ * drive_ * f;
        } else {
            x = depth_.process(x);
            x = pres_.process(x);
            u = pi_hp_.hp(x) * drive_;
        }
        // Supply sag lowers screen/plate headroom while the output stage is drawing current into the load.
        const float h = headroom_ * (1.0f - sag_ * env_);
        const float v = phaseInverter(u) / h;

        // Push-pull output stage acts as a transconductance (current) source. Its source impedance
        // relative to the speaker is the voicing's fixed `damping`; when the stage clips, the NFB loop
        // loses gain, so the speaker's resonance and inductive rise return ("bloom").
        const float i_short = outputStage(v, asym_, spec_.class_a) * h;

        // Output transformer primary-to-secondary bandlimiting + core flux saturation at very low frequencies:
        float i_sec = ot_hp_.process(i_short);
        i_sec = ot_lp_.process(i_sec);
        ot_core_flux_ = 0.9985f * ot_core_flux_ + dt_ * i_sec;
        const float flux_sat = 1.0f / std::sqrt(1.0f + 0.08f * ot_core_flux_ * ot_core_flux_);
        i_sec *= flux_sat;

        // ── Bi-directional transconductance → reactive speaker impedance solver ──
        // Coupling strength `und` reflects how much the tube output stage behaves as a current source
        // (high source impedance = strong speaker impedance voltage division) vs voltage source (stiff damping).
        const float und = 1.0f - std::clamp(spec_.damping, 0.0f, 1.0f);

        // 1. Cone excursion state x_cone modulates the suspension stiffness (shifts f_s upward at high excursion):
        const float lf_exc = spk_lf_.lp(mot_v_);
        cone_x_ = 0.995f * cone_x_ + 0.005f * (lf_exc * lf_exc);
        const float stiffness_mult = 1.0f + 0.28f * std::min(1.5f, cone_x_);
        const float g_mot = mot_g0_ * std::sqrt(stiffness_mult);

        // 2. Trapezoidal (TPT) solve of the parallel motional RLC resonator (cone velocity back-EMF `mot_v_`
        //    and suspension compliance current `mot_i_`) coupled to the tube output conductance `mot_damping_`:
        const float denom_mot = 1.0f + g_mot * (g_mot + mot_damping_);
        const float v_mot = (mot_damping_ * (i_sec - mot_i_) + g_mot * mot_v_) / denom_mot;
        const float i_c_step = g_mot * v_mot;
        mot_v_ = v_mot + i_c_step - mot_i_; // trapezoidal integrator state update
        mot_i_ += 2.0f * g_mot * v_mot;
        mot_i_ *= 0.9998f; // mechanical suspension creep prevention

        // 3. Nonlinear voice-coil inductance L_e(i) (eddy-current loss + permeable pole-piece saturation):
        const float le_sat = 1.0f / std::sqrt(1.0f + 0.16f * i_sec * i_sec);
        const float g_le = le_g0_ / le_sat;
        const float v_le = (i_sec - le_i_) / (1.0f + g_le);
        le_i_ += 2.0f * g_le * v_le / (1.0f + g_le);

        // 4. Combine resistive voice-coil drop, motional back-EMF voltage, and inductive HF rise into
        //    the physical speaker terminal voltage v_spk:
        const float r_thermal = 1.0f / std::sqrt(1.0f + 0.35f * heat_);
        float v_spk = i_sec * r_thermal + und * (mot_gain_ * v_mot + le_gain_ * v_le);

        // Also pass through the calibrated cabinet impedance shaping filters so small-signal frequency
        // response matches the expected voicing curve while carrying the dynamic motional/inductive states:
        v_spk = spk_res_.process(v_spk);
        v_spk = spk_hi_.process(v_spk);

        // Cone spider/surround excursion soft-limiting on the lowest octave:
        const float lf = spk_lf_.lp(v_spk);
        v_spk = v_spk - lf + 1.6f * fastTanh(lf * (1.0f / 1.6f));

        // 5. True reactive power & current draw feed back into supply sag and voice-coil heating:
        //    Tube current demand increases when driving out-of-phase reactive speaker back-EMF!
        const float i_actual = i_sec - 0.25f * und * v_mot;
        const float level = std::min(1.0f, (0.45f * std::abs(v) + 0.20f * std::abs(i_actual)));
        env_ += (level - env_) * ((level > env_) ? env_att_ : env_rel_);

        const float p_spk = std::abs(v_spk * i_actual);
        heat_ += (p_spk - heat_) * heat_k_;

        // 6. Close the global NFB loop from the actual speaker terminal voltage!
        fb_ = v_spk;
        return v_spk;
    }

    // Long-tailed-pair phase inverter: soft, nearly symmetric.
    static float phaseInverter(float u) noexcept { return 2.2f * fastTanh(u * (1.0f / 2.2f)); }
    // Push-pull pair (v = PI output / headroom): each side clips; slight imbalance adds even harmonics.
    static float outputStage(float v, float asym, bool class_a) noexcept {
        float y = fastTanh(v + asym) - fastTanh(asym);
        if (class_a) y = 0.7f * y + 0.3f * fastTanh(1.6f * v);
        return y;
    }
    // Control-derived operating values (what recompute() sets), for model export / analysis.
    struct Derived {
        float drive, sag, headroom, asym, presence_db, depth_db;
    };
    static Derived derive(const PowerSpec& s, float master, float presence, float depth, float sag, float vac, float bias) noexcept {
        Derived d{};
        d.drive = s.drive * (0.15f + 2.4f * audioTaper(master));
        d.presence_db = (presence - 0.35f) * s.presence_db * 1.4f;
        d.depth_db = (depth - 0.35f) * s.depth_db * 1.4f;
        d.sag = s.sag * (0.4f + 1.2f * std::clamp(sag, 0.0f, 1.0f));
        d.headroom = s.knee * (0.80f + 0.40f * std::clamp(vac, 0.0f, 1.0f));
        d.asym = s.asym * (0.5f + 1.5f * std::clamp(1.0f - bias, 0.0f, 1.0f));
        return d;
    }
    static float feedbackBeta(const PowerSpec& s) noexcept { return s.class_a ? 0.0f : std::clamp(0.75f * s.damping, 0.0f, 0.6f); }

private:
    struct Ctl {
        float master, presence, depth, sag, vac, bias;
    };
    void updateSpeakerImpedanceStates() noexcept {
        const float und = 1.0f - std::clamp(spec_.damping, 0.0f, 1.0f);
        const float fs = effectiveSpeakerFs();
        const float q = cab_q_override_ > 0.0f ? cab_q_override_ : 1.4f;
        const float le_scale = cab_le_override_ > 0.0f ? cab_le_override_ : 1.0f;
        dt_ = 1.0f / static_cast<float>(std::max(8000.0, sr_));
        mot_g0_ = std::tan(kPi * std::clamp(fs, 30.0f, 250.0f) / static_cast<float>(std::max(8000.0, sr_)));
        mot_damping_ = 1.0f / std::max(0.3f, q);
        mot_gain_ = 0.22f;
        le_g0_ = std::tan(kPi * (2800.0f / le_scale) / static_cast<float>(std::max(8000.0, sr_)));
        le_gain_ = 0.14f * le_scale;
        spk_res_.set(Svf::Type::Bell, sr_, fs, q, 1.0f + 7.0f * und);
        spk_hi_.set(Svf::Type::HighShelf, sr_, 2800.0f / std::sqrt(le_scale), 0.5f, (0.5f + 4.5f * und) * le_scale);
    }
    void glide() noexcept {
        constexpr float k = 0.18f; // per 32-sample tick → ≈ 20 ms at 192 kHz
        bool moved = false;
        auto step = [&](float& c, float t) {
            if (std::abs(t - c) > 1e-4f) {
                c += (t - c) * k;
                moved = true;
            } else c = t;
        };
        const Ctl t = tgt_;
        step(cur_.master, t.master);
        step(cur_.presence, t.presence);
        step(cur_.depth, t.depth);
        step(cur_.sag, t.sag);
        step(cur_.vac, t.vac);
        step(cur_.bias, t.bias);
        if (moved) recompute();
    }
    void recompute() noexcept {
        const Derived d = derive(spec_, cur_.master, cur_.presence, cur_.depth, cur_.sag, cur_.vac, cur_.bias);
        drive_ = d.drive;
        const float pres_db = d.presence_db;
        const float depth_db = d.depth_db;
        pres_.set(Svf::Type::HighShelf, sr_, spec_.presence_hz, 0.6f, pres_db);
        depth_.set(Svf::Type::Bell, sr_, spec_.depth_hz, 0.9f, depth_db);
        // In the feedback path the same controls *remove* feedback in their band (→ boost at the output).
        fb_pres_.set(Svf::Type::HighShelf, sr_, spec_.presence_hz, 0.6f, -std::max(0.0f, pres_db + 3.0f) * 1.3f);
        fb_depth_.set(Svf::Type::Bell, sr_, spec_.depth_hz, 0.9f, -std::max(0.0f, depth_db + 3.0f) * 1.3f);
        sag_ = d.sag;
        headroom_ = d.headroom;
        asym_ = d.asym;
    }

    double sr_ = 192000.0;
    float dt_ = 1.0f / 192000.0f;
    PowerSpec spec_{};
    Svf ot_hp_{}, ot_lp_{}, pres_{}, depth_{}, fb_pres_{}, fb_depth_{}, spk_res_{}, spk_hi_{};
    OnePole pi_hp_{}, spk_lf_{};
    // Bi-directional reactive speaker states (motional RLC + nonlinear Le + OT core flux)
    float mot_g0_ = 0.0015f, mot_damping_ = 0.7f, mot_gain_ = 0.22f;
    float le_g0_ = 0.045f, le_gain_ = 0.14f;
    float mot_v_ = 0.0f, mot_i_ = 0.0f, cone_x_ = 0.0f, le_i_ = 0.0f, ot_core_flux_ = 0.0f;
    float cab_fs_override_ = 0.0f, cab_q_override_ = 1.4f, cab_le_override_ = 1.0f;
    float heat_ = 0.0f, heat_k_ = 0.0f;
    Ctl tgt_{}, cur_{};
    float beta_ = 0.0f, fb_ = 0.0f;
    int tick_ = 0;
    float drive_ = 1.0f, sag_ = 0.2f, headroom_ = 1.0f, asym_ = 0.05f;
    float env_ = 0.0f, env_att_ = 0.01f, env_rel_ = 0.001f;
};

// ───────────────────────────── Deterministic calibration signal ─────────────────────────────
// A synthetic plucked-string DI performance (power chords, palm mutes, single-note lead, open chords)
// used to level-match amps and whole presets. Karplus-Strong with pick noise and body coloration.
inline std::vector<float> makeCalibrationDI(double sr, float seconds = 4.0f) {
    const std::size_t n = static_cast<std::size_t>(sr * seconds);
    std::vector<float> out(n, 0.0f);
    uint32_t rng = 0x1234567u;
    auto rnd = [&rng]() {
        rng = rng * 1664525u + 1013904223u;
        return static_cast<float>((rng >> 8) & 0xFFFF) / 32768.0f - 1.0f;
    };
    struct Note { float t; float hz; float dur; float vel; };
    std::vector<Note> notes;
    const float E2 = 82.41f, A2 = 110.0f, D3 = 146.83f, G3 = 196.0f;
    auto power = [&](float t, float root, float dur, float vel) {
        notes.push_back({t, root, dur, vel});
        notes.push_back({t + 0.004f, root * 1.4983f, dur, vel * 0.9f});
        notes.push_back({t + 0.008f, root * 2.0f, dur, vel * 0.8f});
    };
    // Palm-muted chugs
    for (int i = 0; i < 8; ++i) power(0.05f + i * 0.125f, E2, 0.11f, 0.75f);
    power(1.05f, G3 / 2.0f * 1.0f, 0.45f, 0.95f);
    power(1.55f, A2, 0.45f, 0.95f);
    // Single-note lead line
    const float lead[] = {329.6f, 392.0f, 440.0f, 493.9f, 440.0f, 392.0f, 587.3f};
    for (int i = 0; i < 7; ++i) notes.push_back({2.05f + i * 0.17f, lead[i], (i == 6) ? 0.7f : 0.16f, 0.8f});
    // Open chord strum
    const float chord[] = {E2, 123.47f, 164.81f, 207.65f, 246.94f, 329.63f};
    for (int i = 0; i < 6; ++i) notes.push_back({3.25f + i * 0.012f, chord[i], 0.7f, 0.7f});
    (void)D3;

    for (const auto& nt : notes) {
        const std::size_t start = static_cast<std::size_t>(nt.t * sr);
        const std::size_t len = static_cast<std::size_t>(nt.dur * sr);
        const std::size_t period = std::max<std::size_t>(2, static_cast<std::size_t>(sr / nt.hz));
        std::vector<float> line(period);
        // Pluck excitation: dark filtered noise burst (pick + string)
        float lp1 = 0.0f, lp2 = 0.0f;
        for (auto& v : line) {
            lp1 += 0.25f * (rnd() - lp1);
            lp2 += 0.25f * (lp1 - lp2);
            v = lp2 * 3.0f;
        }
        std::size_t idx = 0;
        const float damp = (nt.dur < 0.15f) ? 0.982f : 0.9975f; // palm mute decays fast
        float loop_lp = 0.0f;
        for (std::size_t i = 0; i < len && start + i < n; ++i) {
            const float cur = line[idx];
            const float nxt = line[(idx + 1) % period];
            loop_lp += 0.6f * (0.5f * (cur + nxt) - loop_lp);
            line[idx] = damp * loop_lp;
            idx = (idx + 1) % period;
            const float rel = (i + 512 > len) ? static_cast<float>(len - i) / 512.0f : 1.0f;
            out[start + i] += cur * nt.vel * rel;
        }
    }
    // Humbucker pickup: resonant low-pass around 3 kHz, plus cable/input high-pass.
    {
        Svf pu, hp;
        pu.set(Svf::Type::LowPass, sr, 3000.0f, 1.3f);
        hp.set(Svf::Type::HighPass, sr, 60.0f, 0.7f);
        for (auto& v : out) v = hp.process(pu.process(v));
    }
    // Normalise to a hot, realistic DI level: overall RMS ≈ -19 dBFS.
    double e = 0.0;
    for (float v : out) e += static_cast<double>(v) * v;
    if (e > 0.0) {
        const float rms = static_cast<float>(std::sqrt(e / static_cast<double>(out.size())));
        const float g = 0.112f / rms;
        for (auto& v : out) v = std::clamp(v * g, -0.95f, 0.95f);
    }
    return out;
}

// Program loudness meter used for preset level matching: mean square of the mid signal after an 80 Hz
// high-pass. (Unlike full K-weighting this doesn't penalise the upper-mid density of saturated amps, so
// high-gain rigs land as loud as cleans — which is how players judge "equal".)
class LoudnessMeter {
public:
    void prepare(double sr) {
        shelf_.set(Svf::Type::HighShelf, sr, 1500.0f, 0.707f, 0.0f);
        hp_.set(Svf::Type::HighPass, sr, 80.0f, 0.6f);
        acc_ = 0.0;
        count_ = 0;
    }
    void add(float l, float r) {
        const float m = 0.5f * (l + r);
        const float s = hp_.process(shelf_.process(m));
        acc_ += static_cast<double>(s) * s;
        ++count_;
    }
    float meanSquareDb() const {
        if (count_ == 0 || acc_ <= 0.0) return -120.0f;
        return 10.0f * std::log10(static_cast<float>(acc_ / static_cast<double>(count_)));
    }

private:
    Svf shelf_{}, hp_{};
    double acc_ = 0.0;
    std::size_t count_ = 0;
};

} // namespace rig::amp
