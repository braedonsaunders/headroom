// Parameter-mapping regressions found by the unit fidelity audit (docs/UNIT_AUDIT.md, tools/unit_audit):
// controls that were routed to the wrong DSP parameter, or not at all, by the keyword lookup of the shared
// archetype classes.
//   25000 EQ Shelving / 26000 EQ Parametric / 32000 EQ Graphic: explicit band layouts (StudioEQ.hpp)
//   154000 Lexicon 224 / 155000 Lexicon 480L: predelay is a predelay (not the size), Mid sets the decay,
//          Hi Cut the tail brightness
//   27000 6G15: High Damp darkens the wet tail; Spring Length is stored only
//   127000 / 126000 / 118000 IR reverbs: the untouched Mute switch (schema default 0.5) is off
//   two-position switches: the state the UI shows (IComponent::switchState / switchStateFor) is the one the DSP
//          plays, at 0, 1 and the untouched midpoint 0.5 (read as off by most units, as on by some)
#undef NDEBUG
#include "../src/core/AudioBlock.hpp"
#include "../src/core/IComponent.hpp"
#include <cmath>
#include <complex>
#include <cstdio>
#include <map>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

namespace {
int g_failures = 0;
void check(bool ok, const std::string& what) {
    std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what.c_str());
    if (!ok) ++g_failures;
}
constexpr double kSr = 48000.0;
using Params = std::map<std::string, float>;

std::unique_ptr<rig::IComponent> make(int32_t table, const Params& p) {
    auto c = rig::createComponentByTableId(table);
    c->prepare(kSr, 256);
    for (const auto& s : c->schema().params) c->setParameter(s.id, s.default_value); // as a freshly added unit
    c->setParameter("Pwr", 1.0f);
    for (const auto& [k, v] : p) c->setParameter(k, v);
    c->processControlRate();
    c->reset();
    return c;
}

std::vector<float> impulse(rig::IComponent& c, std::size_t n) {
    std::vector<float> x(n, 0.0f), yl(n), yr(n);
    x[0] = 0.5f;
    for (std::size_t p = 0; p + 256 <= n; p += 256) {
        c.processControlRate();
        const float* ins[2] = {x.data() + p, x.data() + p};
        float* outs[2] = {yl.data() + p, yr.data() + p};
        rig::AudioBlock b{};
        b.in = ins; b.out = outs; b.num_channels = 2; b.num_frames = 256; b.sample_rate = kSr;
        c.processAudio(b);
    }
    return yl;
}

double respDb(const std::vector<float>& h, double f) {
    std::complex<double> acc{};
    const double w = 2.0 * std::numbers::pi * f / kSr;
    for (std::size_t i = 0; i < h.size(); ++i) acc += static_cast<double>(h[i]) * 2.0 * std::polar(1.0, -w * static_cast<double>(i));
    return 20.0 * std::log10(std::max(1e-12, std::abs(acc)));
}
double eqDb(int32_t t, const Params& p, double f) { auto c = make(t, p); return respDb(impulse(*c, 16384), f); }

// Reverb impulse metrics: onset of the wet part (ms) and T60 from the -5..-25 dB Schroeder slope (s).
struct Tail { double onset_ms = -1, t60 = 0, hf_db = -200, wet_db = -200; };
Tail tail(int32_t t, const Params& p) {
    auto c = make(t, p);
    const auto y = impulse(*c, static_cast<std::size_t>(2.5 * kSr));
    std::vector<double> fe;
    for (std::size_t s = 0; s + 48 <= y.size(); s += 48) {
        double e = 0;
        for (std::size_t i = s; i < s + 48; ++i) e += static_cast<double>(y[i]) * y[i];
        fe.push_back(e);
    }
    Tail r;
    double mx = 0, tot = 0;
    for (std::size_t i = 3; i < fe.size(); ++i) { mx = std::max(mx, fe[i]); tot += fe[i]; }
    r.wet_db = 10 * std::log10(std::max(tot, 1e-20));
    if (mx <= 1e-14) return r;
    for (std::size_t i = 3; i < fe.size(); ++i) if (fe[i] >= 0.1 * mx) { r.onset_ms = static_cast<double>(i); break; }
    std::vector<double> edc(fe.size(), 0.0);
    double acc = 0;
    for (std::size_t i = fe.size(); i-- > 3;) { acc += fe[i]; edc[i] = acc; }
    double t5 = -1, t25 = -1;
    for (std::size_t i = 3; i < fe.size(); ++i) {
        const double l = 10 * std::log10(edc[i] / edc[3]);
        if (t5 < 0 && l <= -5) t5 = static_cast<double>(i);
        if (t25 < 0 && l <= -25) { t25 = static_cast<double>(i); break; }
    }
    if (t5 >= 0 && t25 > t5) r.t60 = 3.0 * (t25 - t5) / 1000.0;
    // HF content of the late tail (200 ms .. 1.2 s), 4-8 kHz vs 250-500 Hz
    std::vector<float> late(y.begin() + static_cast<std::ptrdiff_t>(0.2 * kSr), y.begin() + static_cast<std::ptrdiff_t>(1.2 * kSr));
    auto band = [&](double lo, double hi) {
        double e = 0;
        for (double f = lo; f <= hi; f *= 1.06) { const double d = respDb(late, f); e += std::pow(10.0, d / 10.0); }
        return 10 * std::log10(e);
    };
    r.hf_db = band(4000, 8000) - band(250, 500);
    return r;
}
// Steady two-tone wet response: compare 5 kHz and 375 Hz after the tank has settled.
double springBrightness(float damp) {
    auto c = make(27000, {{"Wet", 1.0f}, {"L", damp}});
    constexpr std::size_t n = 96000;
    std::vector<float> x(n), yl(n), yr(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kSr;
        x[i] = static_cast<float>(0.01 * (std::sin(2 * std::numbers::pi * 375 * t) + std::sin(2 * std::numbers::pi * 5000 * t)));
    }
    for (std::size_t p = 0; p + 256 <= n; p += 256) {
        c->processControlRate();
        const float* ins[2] = {x.data() + p, x.data() + p};
        float* outs[2] = {yl.data() + p, yr.data() + p};
        rig::AudioBlock b{};
        b.in = ins; b.out = outs; b.num_channels = 2; b.num_frames = 256; b.sample_rate = kSr;
        c->processAudio(b);
    }
    const std::vector<float> steady(yl.begin() + 48000, yl.begin() + 96000);
    return respDb(steady, 5000) - respDb(steady, 375);
}
// A chord-ish stereo burst through a fresh unit with one parameter set (renders both channels).
std::vector<float> burst(int32_t table, const std::string& id, float v, int* state) {
    auto c = make(table, {{id, v}});
    if (state) *state = c->switchState(id);
    const std::size_t n = 36000;
    std::vector<float> xl(n), xr(n), yl(n), yr(n), y;
    uint32_t seed = 7;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kSr, env = std::exp(-3.0 * t) * 0.4;
        const double s = std::sin(2 * std::numbers::pi * 110 * t) + std::sin(2 * std::numbers::pi * 164.8 * t) + 0.3 * std::sin(2 * std::numbers::pi * 660 * t);
        seed = seed * 1664525u + 1013904223u;
        const float noise = (static_cast<float>(seed >> 8) / 16777216.0f - 0.5f) * 0.02f;
        xl[i] = static_cast<float>(env * s / 2.3) + noise;
        xr[i] = static_cast<float>(env * s / 2.3 * 0.8) - noise;
    }
    for (std::size_t p = 0; p + 256 <= n; p += 256) {
        c->processControlRate();
        const float* ins[2] = {xl.data() + p, xr.data() + p};
        float* outs[2] = {yl.data() + p, yr.data() + p};
        rig::AudioBlock b{};
        b.in = ins; b.out = outs; b.num_channels = 2; b.num_frames = 256; b.sample_rate = kSr;
        c->processAudio(b);
    }
    y = yl;
    y.insert(y.end(), yr.begin(), yr.end());
    return y;
}
double nullDb(const std::vector<float>& a, const std::vector<float>& b) {
    double e = 0, r = 1e-30;
    for (std::size_t i = 0; i < a.size(); ++i) { e += (a[i] - b[i]) * (a[i] - b[i]); r += a[i] * a[i] + b[i] * b[i]; }
    return e <= 0 ? -400.0 : 10 * std::log10(e / r);
}
} // namespace

int main() {
    // ---------------- 25000 EQ Shelving
    {
        const double lo_boost_100 = eqDb(25000, {{"G1", 1.0f}}, 60.0), lo_boost_6k = eqDb(25000, {{"G1", 1.0f}}, 8000.0);
        check(lo_boost_100 > 14.0 && std::fabs(lo_boost_6k) < 1.0, "25000 Lo Gain max boosts the lows (+" + std::to_string(lo_boost_100) + " dB @60 Hz), not the highs");
        const double hi_boost_10k = eqDb(25000, {{"G2", 1.0f}}, 12000.0), hi_boost_300 = eqDb(25000, {{"G2", 1.0f}}, 300.0);
        check(hi_boost_10k > 14.0 && std::fabs(hi_boost_300) < 1.0, "25000 Hi Gain max boosts the highs (+" + std::to_string(hi_boost_10k) + " dB @12 kHz), not ~300 Hz");
        const double f1a = eqDb(25000, {{"G1", 1.0f}, {"F1", 0.0f}}, 150.0), f1b = eqDb(25000, {{"G1", 1.0f}, {"F1", 1.0f}}, 150.0);
        check(f1b - f1a > 8.0, "25000 Lo Freq moves the low-shelf corner");
        const double f2a = eqDb(25000, {{"G2", 1.0f}, {"F2", 0.0f}}, 3000.0), f2b = eqDb(25000, {{"G2", 1.0f}, {"F2", 1.0f}}, 3000.0);
        check(f2a - f2b > 8.0, "25000 Hi Freq moves the high-shelf corner");
        check(std::fabs(eqDb(25000, {}, 1000.0)) < 0.2, "25000 flat at defaults");
    }
    // ---------------- 26000 EQ Parametric
    {
        const double at_f = eqDb(26000, {{"G1", 1.0f}, {"F1", 0.5f}}, 223.6); // logMap(0.5, 20, 2500) = 223.6 Hz
        check(std::fabs(at_f - 18.0) < 1.0, "26000 Gain 1 max = +18 dB bell at Freq 1 (" + std::to_string(at_f) + ")");
        const double moved = eqDb(26000, {{"G1", 1.0f}, {"F1", 0.0f}}, 223.6);
        check(moved < 6.0, "26000 Freq 1 moves the bell");
        const double wide = eqDb(26000, {{"G1", 1.0f}, {"Q1", 0.0f}}, 600.0), narrow = eqDb(26000, {{"G1", 1.0f}, {"Q1", 1.0f}}, 600.0);
        check(wide - narrow > 6.0, "26000 Q 1 narrows the bell");
        const double b2 = eqDb(26000, {{"G2", 0.0f}}, 2000.0); // logMap(0.5, 200, 20000) = 2 kHz
        check(std::fabs(b2 + 18.0) < 1.0, "26000 Gain 2 min = -18 dB bell at Freq 2");
    }
    // ---------------- 32000 EQ Graphic: band k peaks at its own centre
    {
        const float fmin = 20.0f * std::sqrt(10.0f), fmax = 4000.0f * std::sqrt(5.0f);
        bool all_own = true;
        for (int k = 0; k < 8; ++k) {
            const std::string id = "b" + std::to_string(k + 1);
            int best = -1;
            double best_db = -1e9;
            for (int j = 0; j < 8; ++j) {
                const double f = fmin * std::pow(fmax / fmin, j / 7.0);
                const double d = eqDb(32000, {{id, 1.0f}}, f);
                if (d > best_db) { best_db = d; best = j; }
            }
            if (best != k || best_db < 12.0) all_own = false;
            std::printf("    band %d: peak at centre %d, %.1f dB\n", k + 1, best + 1, best_db);
        }
        check(all_own, "32000 every band boosts its own centre (+15 dB at the default Gain Range)");
        const double r_lo = eqDb(32000, {{"b5", 1.0f}, {"r", 0.0f}}, fmin * std::pow(fmax / fmin, 4 / 7.0));
        const double r_hi = eqDb(32000, {{"b5", 1.0f}, {"r", 1.0f}}, fmin * std::pow(fmax / fmin, 4 / 7.0));
        check(r_lo > 4.0 && r_lo < 8.0 && r_hi > 20.0, "32000 Gain Range scales the bands (6 -> 24 dB)");
    }
    // ---------------- 154000 Lexicon 224: Predelay is a predelay, Mid is the decay, Hi Cut the brightness
    {
        const Tail p0 = tail(154000, {{"P0", 0.0f}}), p1 = tail(154000, {{"P0", 1.0f}});
        std::printf("    224 predelay 0/1: onset %.0f / %.0f ms, T60 %.2f / %.2f s\n", p0.onset_ms, p1.onset_ms, p0.t60, p1.t60);
        check(p1.onset_ms - p0.onset_ms > 120.0 && std::fabs(p1.t60 - p0.t60) < 0.25 * std::max(p0.t60, p1.t60),
              "154000 Predelay delays the onset (~152 ms) and leaves the decay alone");
        const Tail m0 = tail(154000, {{"M5", 0.0f}}), m1 = tail(154000, {{"M5", 1.0f}});
        std::printf("    224 Mid 0/1: T60 %.2f / %.2f s\n", m0.t60, m1.t60);
        check(m1.t60 > 1.8 * m0.t60, "154000 Mid sets the reverb time");
        const Tail h0 = tail(154000, {{"H7", 0.0f}}), h1 = tail(154000, {{"H7", 1.0f}});
        check(h1.hf_db - h0.hf_db > 3.0, "154000 Hi Cut fader up = brighter tail");
    }
    // ---------------- 155000 Lexicon 480L
    {
        const Tail p0 = tail(155000, {{"P0", 0.0f}}), p1 = tail(155000, {{"P0", 1.0f}});
        check(p1.onset_ms - p0.onset_ms > 150.0, "155000 Predelay delays the onset");
        const Tail m0 = tail(155000, {{"M4", 0.0f}}), m1 = tail(155000, {{"M4", 1.0f}});
        check(m1.t60 > 1.8 * m0.t60, "155000 Mid sets the reverb time");
        const Tail h0 = tail(155000, {{"H5", 0.0f}}), h1 = tail(155000, {{"H5", 1.0f}});
        check(h1.hf_db - h0.hf_db > 3.0, "155000 Hi Cut fader up = brighter tail");
    }
    // ---------------- 27000 6G15: Tone affects the wet return; legacy controls are stored only
    {
        const double l0 = springBrightness(0.0f), l1 = springBrightness(1.0f);
        check(l0 - l1 > 3.0, "27000 High Damp darkens the wet return (" + std::to_string(l0 - l1) + " dB)");
        const Tail h0 = tail(27000, {{"H", 0.0f}}), h1 = tail(27000, {{"H", 1.0f}});
        check(std::fabs(h0.hf_db - h1.hf_db) < 1.0, "27000 Low Damp does not damp the highs");
        auto s = make(27000, {{"SpringLength", 0.0f}});
        const bool stored0 = s->getParameter("SpringLength") == 0.0f;
        s->setParameter("SpringLength", 1.0f);
        const bool stored1 = s->getParameter("SpringLength") == 1.0f;
        check(stored0 && stored1 && nullDb(burst(27000, "SpringLength", 0.0f, nullptr), burst(27000, "SpringLength", 1.0f, nullptr)) < -100.0,
              "27000 Spring Length is stored without affecting audio");
    }
    // ---------------- IR reverbs are audible as freshly added units (Mute switch at its 0.5 default = off)
    for (int32_t t : {127000, 126000, 118000}) {
        const Tail d = tail(t, {});
        check(d.wet_db > -60.0, std::to_string(t) + " produces a reverb tail at schema defaults (" + std::to_string(d.wet_db) + " dB)");
    }
    {
        const char* mute = "M3";
        const Tail m = tail(127000, {{mute, 1.0f}});
        check(m.wet_db < -100.0, "127000 Mute on silences it");
    }
    // ---------------- Switches: the UI shows what the DSP plays (behavioural `> 0.5` / `>= 0.5` units, circuit switches,
    // two-way selectors and an inverted parameter range). Each switch is audible: 0.5 must null against the side the
    // engine reports, both from the live instance and from the catalogue answer (no instance).
    {
        const std::vector<std::pair<int32_t, std::string>> cases = {
            {28000, "Sy"}, {98000, "s"}, {100000, "m1"}, {118000, "M12"}, {127000, "M3"}, {148000, "preDSync"},
            {150000, "boost"}, {156000, "V"}, {177000, "ultra low"},        // behavioural
            {30000, "RvOn"}, {81000, "c"}, {172000, "reverb on"}, {78000, "dm"}}; // circuit: inverted range, selector, switches
        for (const auto& [t, id] : cases) {
            int s0 = -1, sh = -1, s1 = -1;
            const auto a = burst(t, id, 0.0f, &s0), h = burst(t, id, 0.5f, &sh), b = burst(t, id, 1.0f, &s1);
            const double dab = nullDb(a, b), dha = nullDb(h, a), dhb = nullDb(h, b);
            const int played = dha < dhb ? 0 : 1;
            const int listed = rig::switchStateFor(t, id, 0.5f);
            std::printf("    %d %s: 0 vs 1 %.1f dB, 0.5 vs 0 %.1f / vs 1 %.1f dB -> plays %s; shows %d/%d/%d, catalogue %d\n", t, id.c_str(), dab,
                        dha, dhb, played ? "on" : "off", s0, sh, s1, listed);
            check(dab > -40.0 && std::fabs(dha - dhb) > 10.0, std::to_string(t) + " " + id + " is an audible switch with a clear midpoint");
            check(s0 == 0 && s1 == 1 && sh == played && listed == played,
                  std::to_string(t) + " " + id + " shows the state the DSP plays at 0 / 0.5 / 1");
        }
    }
    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
