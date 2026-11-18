#pragma once
// Studio EQ units with explicit, per-table band layouts. Currently 121000 SSL 4000 E/G Channel EQ.
//
// The SSL used to share the generic 4-band ClassicEQComponent, whose "first matching id" lookup crossed its
// controls (H0 = HF Gain drove the low shelf, H1 = HF Freq a mid gain, H3 = HMF Gain the high shelf) and ignored
// the frequency, Q, bell/shelf, HPF/LPF and E/G controls.
//
// The layout follows the hardware console spec (HPF 30-600 Hz, LPF 50 kHz-3 kHz, both off fully left; G/E series) and
// the illustrated face's curve model (app/ui/assets/units/index.json, drawn by unit-art.js `curvePoints`), so
// the sound matches the curve the player sees: RBJ analog prototypes for bells and shelves (same Q convention
// as unit-art.js bandDb), Butterworth high/low passes. Realised as prewarped bilinear biquads, with
// magnitude-matched designs where those track the analog curve better (the LPF, and high or wide bells).
#include "../core/IComponent.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <numbers>

namespace rig::dsp::studio_eq {

enum class BandType : uint8_t { Bell, LowShelf, HighShelf, LowPass, HighPass };

struct Band {
    BandType type = BandType::Bell;
    float f = 1000.0f;   // Hz
    float g_db = 0.0f;   // bells / shelves
    float q = 0.707f;
    int order = 12;      // dB/oct for LowPass / HighPass (6, 12 or 18)
    bool on = true;
};

struct Layout {
    std::array<Band, 8> bands{};
    int count = 0;
    float out_db = 0.0f;
    void add(const Band& b) noexcept { if (count < static_cast<int>(bands.size())) bands[static_cast<std::size_t>(count++)] = b; }
};

// The SSL plus the shelving / parametric / graphic EQs (25000 / 26000 / 32000). Those three used to share
// ClassicEQComponent, whose first-matching-id lookup crossed or dropped their controls (EQ Shelving: Hi Gain drove a
// ~280 Hz bell, both Freq knobs dead; EQ Parametric: Freq/Q dead; EQ Graphic: bands 2/4/6/8, range and span
// dead) — see docs/UNIT_AUDIT.md. A new table needs a layoutFor() case.
inline bool isStudioEqTable(int32_t t) noexcept { return t == 121000 || t == 25000 || t == 26000 || t == 32000; }

inline float logMap(float n, float lo, float hi) noexcept { return lo * std::pow(hi / lo, std::clamp(n, 0.0f, 1.0f)); }
inline float linMap(float n, float lo, float hi) noexcept { return lo + (hi - lo) * std::clamp(n, 0.0f, 1.0f); }

// Builds the band layout of `table` from normalized (0..1) parameter values. `n(id, fallback)` returns the
// parameter's normalized value. Switches are "on" above 0.5 (an untouched switch default of 0.5 = off).
template <class Norm>
bool layoutFor(int32_t table, Norm&& n, Layout& L) {
    L = Layout{};
    switch (table) {
        case 121000: { // SSL 4000 E/G channel strip EQ
            // Spec: HPF 30-600 Hz and LPF 50 kHz-3 kHz, each switched off fully left. Console slopes:
            // HP 18 dB/oct, LP 12 dB/oct. Band ranges and Q law (4.0 fully left -> 0.4) as on the face.
            // G/E (M12, G above 0.5): G = steeper LF/HF shelves and proportional-Q mids (narrower as the gain
            // rises, wider at small boosts/cuts); E = softer shelves and constant Q.
            const float hp = n("H12", 0.0f), lp = n("L13", 0.0f);
            const bool g_mode = n("M12", 1.0f) > 0.5f;
            const float shelf_q = g_mode ? 0.9f : 0.6f;
            auto midQ = [&](float q, float gain_db) {
                return g_mode ? q * (0.4f + 0.6f * std::min(1.0f, std::fabs(gain_db) / 15.0f)) : q;
            };
            L.add({BandType::HighPass, logMap(hp, 30.0f, 600.0f), 0.0f, 0.7071f, 18, hp * 60.0f >= 0.5f});
            L.add({n("L11", 0.5f) > 0.5f ? BandType::Bell : BandType::LowShelf, logMap(n("L10", 0.5f), 30.0f, 450.0f),
                   linMap(n("L9", 0.5f), -15.0f, 15.0f), shelf_q, 12, true});
            const float lmf_g = linMap(n("L6", 0.5f), -15.0f, 15.0f);
            L.add({BandType::Bell, logMap(n("L7", 0.5f), 200.0f, 2500.0f), lmf_g, midQ(logMap(n("L8", 0.5f), 4.0f, 0.4f), lmf_g), 12, true});
            const float hmf_g = linMap(n("H3", 0.5f), -15.0f, 15.0f);
            L.add({BandType::Bell, logMap(n("H4", 0.5f), 600.0f, 7000.0f), hmf_g, midQ(logMap(n("H5", 0.5f), 4.0f, 0.4f), hmf_g), 12, true});
            L.add({n("H2", 0.5f) > 0.5f ? BandType::Bell : BandType::HighShelf, logMap(n("H1", 0.5f), 1500.0f, 16000.0f),
                   linMap(n("H0", 0.5f), -15.0f, 15.0f), shelf_q, 12, true});
            L.add({BandType::LowPass, logMap(lp, 50000.0f, 3000.0f), 0.0f, 0.7071f, 12, lp * 60.0f >= 0.5f});
            L.out_db = linMap(n("O14", 0.5f), -18.0f, 18.0f);
            return true;
        }
        // Ranges below follow the illustrated faces (app/ui/assets/units/index.json curve specs) so the sound
        // matches the drawn curve.
        case 25000: { // EQ Shelving: low shelf 35-500 Hz, high shelf 1.5-16 kHz, +-18 dB, Output +-18 dB
            L.add({BandType::LowShelf, logMap(n("F1", 0.5f), 35.0f, 500.0f), linMap(n("G1", 0.5f), -18.0f, 18.0f), 0.7f, 12, true});
            L.add({BandType::HighShelf, logMap(n("F2", 0.5f), 1500.0f, 16000.0f), linMap(n("G2", 0.5f), -18.0f, 18.0f), 0.7f, 12, true});
            L.out_db = linMap(n("Vol", 0.5f), -18.0f, 18.0f);
            return true;
        }
        case 26000: { // EQ Parametric: bell 1 20-2500 Hz, bell 2 200 Hz-20 kHz, +-18 dB, Q 0.2-12, Output +-18 dB
            L.add({BandType::Bell, logMap(n("F1", 0.5f), 20.0f, 2500.0f), linMap(n("G1", 0.5f), -18.0f, 18.0f), logMap(n("Q1", 0.5f), 0.2f, 12.0f), 12, true});
            L.add({BandType::Bell, logMap(n("F2", 0.5f), 200.0f, 20000.0f), linMap(n("G2", 0.5f), -18.0f, 18.0f), logMap(n("Q2", 0.5f), 0.2f, 12.0f), 12, true});
            L.out_db = linMap(n("Vol", 0.5f), -18.0f, 18.0f);
            return true;
        }
        case 32000: { // EQ Graphic: 8 bells (Q 1.4) log-spaced from Min Freq (20-200 Hz) to Max Freq (4-20 kHz),
                      // band gain +-Gain Range (6-24 dB). Output keeps the previous +-4 dB (the face shows no scale).
            const float fmin = logMap(n("mi", 0.5f), 20.0f, 200.0f), fmax = logMap(n("ma", 0.5f), 4000.0f, 20000.0f);
            const float range = linMap(n("r", 0.5f), 6.0f, 24.0f);
            static constexpr const char* kBand[8] = {"b1", "b2", "b3", "b4", "b5", "b6", "b7", "b8"};
            for (int k = 0; k < 8; ++k) {
                const float f = fmin * std::pow(fmax / fmin, static_cast<float>(k) / 7.0f);
                L.add({BandType::Bell, f, (n(kBand[k], 0.5f) - 0.5f) * 2.0f * range, 1.4f, 12, true});
            }
            L.out_db = linMap(n("V", 0.5f), -4.0f, 4.0f);
            return true;
        }
        default: return false;
    }
}

// Analog magnitude (dB) of one band at f: the same prototypes as unit-art.js bandDb, except that the
// high/low passes are exact Butterworth of the band's order.
inline double analogDb(const Band& b, double f) noexcept {
    if (!b.on) return 0.0;
    const double w = f / b.f, w2 = w * w, A = std::pow(10.0, b.g_db / 40.0), sA = std::sqrt(A), q = b.q;
    auto mag = [&](double a, double bb, double c, double d, double e, double h) {
        return std::hypot(a - c * w2, bb * w) / std::hypot(d - h * w2, e * w);
    };
    double m = 1.0;
    switch (b.type) {
        case BandType::Bell: m = mag(1, A / q, 1, 1, 1 / (A * q), 1); break;
        case BandType::LowShelf: m = mag(A * A, A * sA / q, A, 1, sA / q, A); break;
        case BandType::HighShelf: m = mag(A, A * sA / q, A * A, A, sA / q, 1); break;
        case BandType::LowPass: return -10.0 * std::log10(1.0 + std::pow(w, 2.0 * b.order / 6.0));
        case BandType::HighPass: return -10.0 * std::log10(1.0 + std::pow(w, -2.0 * b.order / 6.0));
    }
    return 20.0 * std::log10(std::max(1e-9, m));
}

inline double analogDb(const Layout& L, double f) noexcept {
    double db = L.out_db;
    for (int i = 0; i < L.count; ++i) db += analogDb(L.bands[static_cast<std::size_t>(i)], f);
    return db;
}

// One biquad section (transposed direct form II, double state: the HP sits at ~18 Hz).
struct Section {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;
    void reset() noexcept { z1 = z2 = 0; }
    float process(float xf) noexcept {
        const double x = xf, y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return static_cast<float>(y);
    }
    void set(double B0, double B1, double B2, double A0, double A1, double A2) noexcept {
        b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0; a1 = A1 / A0; a2 = A2 / A0;
    }
};

inline double sectionDb(const Section& s, double sr, double f) noexcept {
    const std::complex<double> z = std::polar(1.0, -2.0 * std::numbers::pi * f / sr);
    return 20.0 * std::log10(std::max(1e-12, std::abs((s.b0 + s.b1 * z + s.b2 * z * z) / (1.0 + s.a1 * z + s.a2 * z * z))));
}
// Largest deviation from the analog band, 20 Hz-20 kHz (or Nyquist), on a 1/3-octave grid. Errors above 8 kHz
// count a third: when a band near Nyquist can't be matched everywhere, keep the audible range right.
inline double maxError(const Section& s, const Band& b, double sr) noexcept {
    double worst = 0.0;
    for (double f = 20.0; f <= std::min(20000.0, sr * 0.49); f *= 1.26)
        worst = std::max(worst, std::fabs(sectionDb(s, sr, f) - analogDb(b, f)) * (f > 8000.0 ? 0.33 : 1.0));
    return worst;
}
// Matched peaking EQ after M. Vicanek, "Matched Second Order Digital Filters" (2016).
inline bool matchedBell(const Band& b, double sr, double w0, double A, double q, Section& out) noexcept {
    auto ha2 = [&](double f) { const double w = f / b.f, w2 = w * w; return ((1 - w2) * (1 - w2) + (A / q * w) * (A / q * w)) / ((1 - w2) * (1 - w2) + (w / (A * q)) * (w / (A * q))); };
    const double z = 1.0 / (2.0 * A * q), a2 = std::exp(-2.0 * z * w0);
    const double a1 = z <= 1.0 ? -2.0 * std::exp(-z * w0) * std::cos(std::sqrt(1.0 - z * z) * w0)
                               : -2.0 * std::exp(-z * w0) * std::cosh(std::sqrt(z * z - 1.0) * w0);
    const double A0 = (1 + a1 + a2) * (1 + a1 + a2), A1 = (1 - a1 + a2) * (1 - a1 + a2), A2 = -4 * a2;
    const double s2 = std::sin(w0 / 2) * std::sin(w0 / 2), p0 = 1 - s2, p1 = s2, p2 = 4 * p0 * p1;
    if (p2 < 1e-9) return false;
    const double B0 = A0, B1 = A1 * ha2(sr / 2), R = (A0 * p0 + A1 * p1 + A2 * p2) * ha2(b.f);
    const double B2 = (R - B0 * p0 - B1 * p1) / p2, W = 0.5 * (std::sqrt(B0) + std::sqrt(B1)), disc = W * W + B2;
    if (!(disc >= 0.0)) return false;
    const double b0 = 0.5 * (W + std::sqrt(disc));
    if (!(b0 > 1e-12)) return false;
    out.set(b0, 0.5 * (std::sqrt(B0) - std::sqrt(B1)), -B2 / (4.0 * b0), 1.0, a1, a2);
    return std::isfinite(out.b0) && std::isfinite(out.b2);
}

// Realises `b` as up to two sections. Returns the number of sections written.
inline int design(const Band& b, double sr, Section* out) noexcept {
    if (!b.on) return 0;
    const double f = std::clamp(static_cast<double>(b.f), 10.0, sr * 0.49);
    const double w0 = 2.0 * std::numbers::pi * f / sr, cs = std::cos(w0), sn = std::sin(w0);
    const double A = std::pow(10.0, b.g_db / 40.0);
    auto biquadLP = [&](double q, Section& s) { const double al = sn / (2 * q); s.set((1 - cs) / 2, 1 - cs, (1 - cs) / 2, 1 + al, -2 * cs, 1 - al); };
    auto biquadHP = [&](double q, Section& s) { const double al = sn / (2 * q); s.set((1 + cs) / 2, -(1 + cs), (1 + cs) / 2, 1 + al, -2 * cs, 1 - al); };
    // Magnitude-matched 2nd-order low pass (M. Vicanek, "Matched Second Order Digital Filters", 2016): impulse-
    // invariant poles, zeros fitted to the analog magnitude at DC and at the corner. Unlike the bilinear LP it
    // does not dive to -inf at Nyquist, so a 12-22 kHz console LPF stays within ~1 dB of the analog curve.
    auto matchedLP = [&](double q, Section& s) {
        const double z = 1.0 / (2.0 * q);
        const double a1 = z <= 1.0 ? -2.0 * std::exp(-z * w0) * std::cos(std::sqrt(1.0 - z * z) * w0)
                                   : -2.0 * std::exp(-z * w0) * std::cosh(std::sqrt(z * z - 1.0) * w0);
        const double a2 = std::exp(-2.0 * z * w0);
        const double A0 = (1 + a1 + a2) * (1 + a1 + a2), A1 = (1 - a1 + a2) * (1 - a1 + a2), A2 = -4 * a2;
        const double s2 = std::sin(w0 / 2) * std::sin(w0 / 2), p0 = 1 - s2, p1 = s2, p2 = 4 * p0 * p1;
        const double R1 = (A0 * p0 + A1 * p1 + A2 * p2) * q * q;
        const double B0 = A0, B1 = std::max(0.0, (R1 - B0 * p0) / p1);
        const double b0 = 0.5 * (std::sqrt(B0) + std::sqrt(B1)), b1 = std::sqrt(B0) - b0;
        s.set(b0, b1, 0, 1, a1, a2);
    };
    auto onePole = [&](bool hp, Section& s) { // bilinear, prewarped
        const double k = std::tan(w0 / 2);
        if (hp) s.set(1, -1, 0, 1 + k, k - 1, 0);
        else s.set(k, k, 0, 1 + k, k - 1, 0);
    };
    switch (b.type) {
        case BandType::Bell: {
            const double q = std::max(0.05f, b.q), al = sn / (2 * q);
            out[0].set(1 + al * A, -2 * cs, 1 - al * A, 1 + al / A, -2 * cs, 1 - al / A);
            // High or wide bells cramp under the bilinear transform. Also fit a magnitude-matched design
            // (impulse-invariant poles, numerator matched to the analog curve at DC, Nyquist and f0) and keep
            // whichever of the two follows the analog curve more closely up to 20 kHz.
            if (f > 1500.0 || q < 0.5) {
                Section m;
                if (matchedBell(b, sr, w0, A, q, m) && maxError(m, b, sr) < maxError(out[0], b, sr)) out[0] = m;
            }
            return 1;
        }
        case BandType::LowShelf: {
            const double al = sn / (2 * std::max(0.05f, b.q)), t = 2 * std::sqrt(A) * al;
            out[0].set(A * ((A + 1) - (A - 1) * cs + t), 2 * A * ((A - 1) - (A + 1) * cs), A * ((A + 1) - (A - 1) * cs - t),
                       (A + 1) + (A - 1) * cs + t, -2 * ((A - 1) + (A + 1) * cs), (A + 1) + (A - 1) * cs - t);
            return 1;
        }
        case BandType::HighShelf: {
            const double al = sn / (2 * std::max(0.05f, b.q)), t = 2 * std::sqrt(A) * al;
            out[0].set(A * ((A + 1) + (A - 1) * cs + t), -2 * A * ((A - 1) + (A + 1) * cs), A * ((A + 1) + (A - 1) * cs - t),
                       (A + 1) - (A - 1) * cs + t, 2 * ((A - 1) - (A + 1) * cs), (A + 1) - (A - 1) * cs - t);
            return 1;
        }
        case BandType::LowPass:
        case BandType::HighPass: {
            const bool hp = b.type == BandType::HighPass;
            if (b.order <= 6) { onePole(hp, out[0]); return 1; }
            if (b.order <= 12) { hp ? biquadHP(0.70710678, out[0]) : matchedLP(0.70710678, out[0]); return 1; }
            onePole(hp, out[0]); // 3rd-order Butterworth = 1st order + 2nd order with Q = 1
            hp ? biquadHP(1.0, out[1]) : biquadLP(1.0, out[1]);
            return 2;
        }
    }
    return 0;
}

class StudioEQComponent final : public SchemaComponentBase {
public:
    using SchemaComponentBase::SchemaComponentBase;

    // Series pipelining (RackGraph): both channels run the same sections on the same state.
    bool monoPreserving() const noexcept override { return true; }

    void adoptRuntimeStateFrom(IComponent& previous) noexcept override {
        auto* old = dynamic_cast<StudioEQComponent*>(&previous);
        if (!old || tableId() != old->tableId() || host_sr_ != old->host_sr_) return;
        for (int c = 0; c < 2; ++c)
            for (std::size_t i = 0; i < kSlots; ++i) {
                sec_[c][i].z1 = old->sec_[c][i].z1;
                sec_[c][i].z2 = old->sec_[c][i].z2;
            }
    }

    const Layout& layout() const noexcept { return layout_; }

protected:
    void onPrepare(double, uint32_t) override {
        have_ = false;
        onProcessControlRate();
        onReset();
    }
    void onReset() override {
        for (auto& ch : sec_) for (auto& s : ch) s.reset();
    }
    void onProcessControlRate() override {
        std::array<float, 24> vals{};
        std::size_t nv = 0;
        auto n = [&](const char* id, float fallback) {
            float v = fallback;
            for (const auto& p : schema_.params) {
                if (p.id != id) continue;
                float lo = 0.0f, hi = 1.0f;
                getParamEffectiveRange(p, lo, hi);
                v = std::clamp((getParamOr(p.id, p.default_value) - lo) / std::max(1e-6f, hi - lo), 0.0f, 1.0f);
                break;
            }
            if (nv < vals.size()) vals[nv++] = v;
            return v;
        };
        Layout L;
        layoutFor(schema_.table_id, n, L);
        if (have_ && vals == last_vals_) return; // unchanged: keep the coefficients
        have_ = true;
        last_vals_ = vals;
        layout_ = L;
        // Band i owns sections 2i and 2i+1, so switching one band in or out never shifts another band's state.
        for (int i = 0; i < L.count && i < static_cast<int>(kBands); ++i) {
            std::array<Section, 2> fresh{};
            used_[static_cast<std::size_t>(i)] = design(L.bands[static_cast<std::size_t>(i)], host_sr_, fresh.data());
            for (auto& ch : sec_)
                for (std::size_t j = 0; j < 2; ++j) {
                    Section& s = ch[static_cast<std::size_t>(2 * i) + j];
                    const double z1 = s.z1, z2 = s.z2;
                    s = fresh[j];
                    s.z1 = z1;
                    s.z2 = z2;
                }
        }
        bands_ = std::min(L.count, static_cast<int>(kBands));
        out_gain_ = static_cast<float>(std::pow(10.0, L.out_db / 20.0));
    }
    void onProcessAudio(AudioBlock& block) override {
        for (uint32_t c = 0; c < block.num_channels; ++c) {
            const float* in = block.in[c];
            float* out = block.out[c];
            if (!in || !out) continue;
            auto& ch = sec_[c & 1];
            for (uint32_t i = 0; i < block.num_frames; ++i) {
                float s = in[i];
                for (int b = 0; b < bands_; ++b)
                    for (int j = 0; j < used_[static_cast<std::size_t>(b)]; ++j) s = ch[static_cast<std::size_t>(2 * b + j)].process(s);
                out[i] = s * out_gain_;
            }
        }
    }

private:
    static constexpr std::size_t kBands = 8, kSlots = 2 * kBands;
    std::array<std::array<Section, kSlots>, 2> sec_{};
    std::array<int, kBands> used_{};
    std::array<float, 24> last_vals_{};
    bool have_ = false;
    Layout layout_{};
    int bands_ = 0;
    float out_gain_ = 1.0f;
};

} // namespace rig::dsp::studio_eq
