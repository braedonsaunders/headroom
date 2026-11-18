// Factory SSL 82E242 response against ngspice, plus the legacy E/G algorithm used by the A/B tool.
#undef NDEBUG
#include "../src/core/AudioBlock.hpp"
#include "../src/core/IComponent.hpp"
#include "../src/dsp/StudioEQ.hpp"
#include "../src/dsp/ComponentModel.hpp"
#include "json.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <complex>
#include <cstdio>
#include <map>
#include <memory>
#include <numbers>
#include <random>
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

std::unique_ptr<rig::IComponent> makeLegacy(int32_t table, const Params& p) {
    auto c = std::make_unique<rig::dsp::studio_eq::StudioEQComponent>(*rig::catalog::findByTableId(table));
    for (const auto& s : c->schema().params) c->setParameter(s.id, s.default_value);
    c->setParameter("Pwr", 1.0f);
    for (const auto& [k, v] : p) c->setParameter(k, v);
    c->prepare(kSr, 256);
    c->reset();
    return c;
}

std::vector<float> impulse(rig::IComponent& c, std::size_t n = 32768, float amplitude = 1.0f) {
    std::vector<float> x(n, 0.0f), yl(n), yr(n);
    x[0] = amplitude;
    for (std::size_t p = 0; p < n; p += 256) {
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
    for (std::size_t i = 0; i < h.size(); ++i) acc += static_cast<double>(h[i]) * std::polar(1.0, -w * static_cast<double>(i));
    return 20.0 * std::log10(std::max(1e-12, std::abs(acc)));
}

double at(int32_t table, const Params& p, double f) { auto c = makeLegacy(table, p); return respDb(impulse(*c), f); }

// Frequency of the largest |response| on a fine log grid (for peak tracking).
double peakHz(int32_t table, const Params& p, double lo, double hi) {
    auto c = makeLegacy(table, p);
    const auto h = impulse(*c);
    double best = lo, bv = -1e9;
    for (double f = lo; f <= hi; f *= 1.01) {
        const double v = std::fabs(respDb(h, f));
        if (v > bv) { bv = v; best = f; }
    }
    return best;
}
bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
} // namespace

int main() {
    using namespace rig::dsp::studio_eq;
    const int32_t SSL = 121000;
    // Neutral init: filters out, every gain at 0 dB.
    const Params flat{{"H12", 0.0f}, {"L13", 0.0f}};
    auto with = [&](Params extra) { Params p = flat; for (auto& kv : extra) p[kv.first] = kv.second; return p; };

    // ── neutral settings are flat ──
    {
        auto c = makeLegacy(SSL, flat);
        const auto h = impulse(*c);
        double worst = 0;
        for (double f : {20.0, 60.0, 200.0, 1000.0, 5000.0, 12000.0, 18000.0}) worst = std::max(worst, std::fabs(respDb(h, f)));
        check(worst < 0.05, "SSL at defaults (filters out, gains centred) is flat (worst " + std::to_string(worst) + " dB)");
    }
    // ── each gain drives its own band (the old engine crossed HF Gain onto the low shelf) ──
    {
        const double lo = at(SSL, with({{"H0", 1.0f}}), 60.0), hi = at(SSL, with({{"H0", 1.0f}}), 16000.0);
        check(near(lo, 0.0, 0.2) && hi > 13.0, "HF Gain max lifts the highs (+" + std::to_string(hi) + " dB @16k), not the lows (" + std::to_string(lo) + ")");
        const double lf_lo = at(SSL, with({{"L9", 1.0f}}), 40.0), lf_hi = at(SSL, with({{"L9", 1.0f}}), 5000.0);
        check(lf_lo > 13.0 && near(lf_hi, 0.0, 0.2), "LF Gain max lifts the lows (+" + std::to_string(lf_lo) + " dB @40), not the highs");
        const double lmf = at(SSL, with({{"L6", 1.0f}}), 200.0 * std::sqrt(12.5));
        check(near(lmf, 15.0, 0.3), "LMF Gain max = +15 dB at the LMF centre (" + std::to_string(lmf) + ")");
        const double hmf = at(SSL, with({{"H3", 0.0f}}), 600.0 * std::sqrt(7000.0 / 600.0));
        check(near(hmf, -15.0, 0.3), "HMF Gain min = -15 dB at the HMF centre (" + std::to_string(hmf) + ")");
        const double lf_cut = at(SSL, with({{"L9", 0.0f}}), 25.0);
        check(lf_cut < -12.0, "LF Gain min cuts the lows (" + std::to_string(lf_cut) + ")");
    }
    // ── frequency knobs move their band across the face's ranges ──
    {
        const double l0 = peakHz(SSL, with({{"L6", 1.0f}, {"L7", 0.0f}}), 100, 5000), l1 = peakHz(SSL, with({{"L6", 1.0f}, {"L7", 1.0f}}), 100, 5000);
        check(near(l0, 200, 12) && near(l1, 2500, 150), "LMF Freq sweeps 200 Hz -> 2.5 kHz (" + std::to_string(l0) + ", " + std::to_string(l1) + ")");
        const double h0 = peakHz(SSL, with({{"H3", 1.0f}, {"H4", 0.0f}}), 300, 12000), h1 = peakHz(SSL, with({{"H3", 1.0f}, {"H4", 1.0f}}), 300, 12000);
        check(near(h0, 600, 35) && near(h1, 7000, 450), "HMF Freq sweeps 600 Hz -> 7 kHz (" + std::to_string(h0) + ", " + std::to_string(h1) + ")");
        // shelf corner: the half-gain point of the HF shelf follows HF Freq
        const double lowF = at(SSL, with({{"H0", 1.0f}, {"H1", 0.0f}}), 1500.0), highF = at(SSL, with({{"H0", 1.0f}, {"H1", 1.0f}}), 1500.0);
        check(lowF > highF + 6.0, "HF Freq moves the shelf (1.5 kHz: " + std::to_string(lowF) + " vs " + std::to_string(highF) + " dB)");
        const double lfA = at(SSL, with({{"L9", 1.0f}, {"L10", 0.0f}}), 200.0), lfB = at(SSL, with({{"L9", 1.0f}, {"L10", 1.0f}}), 200.0);
        check(lfB > lfA + 6.0, "LF Freq moves the shelf (200 Hz: " + std::to_string(lfA) + " vs " + std::to_string(lfB) + " dB)");
    }
    // ── Q knobs (face: 4.0 fully left -> 0.4 fully right) ──
    {
        const double fc = 200.0 * std::sqrt(12.5), off = fc * 2.0;
        const double narrow = at(SSL, with({{"L6", 1.0f}, {"L8", 0.0f}}), off), wide = at(SSL, with({{"L6", 1.0f}, {"L8", 1.0f}}), off);
        check(narrow < 3.0 && wide > 9.0, "LMF Q: an octave off the centre, Q 4 = +" + std::to_string(narrow) + " dB, Q 0.4 = +" + std::to_string(wide));
        const double fh = 600.0 * std::sqrt(7000.0 / 600.0);
        const double hn = at(SSL, with({{"H3", 1.0f}, {"H5", 0.0f}}), fh * 2), hw = at(SSL, with({{"H3", 1.0f}, {"H5", 1.0f}}), fh * 2);
        check(hn < 3.0 && hw > 9.0, "HMF Q acts (" + std::to_string(hn) + " vs " + std::to_string(hw) + ")");
    }
    // ── bell / shelf switches (0.5 = default switch = shelf) ──
    {
        const double shelf = at(SSL, with({{"H0", 1.0f}, {"H1", 0.3f}, {"H2", 0.5f}}), 18000.0);
        const double bell = at(SSL, with({{"H0", 1.0f}, {"H1", 0.3f}, {"H2", 1.0f}}), 18000.0);
        check(shelf > 13.0 && bell < shelf - 4.0, "HF Bell: shelf keeps rising (" + std::to_string(shelf) + "), bell falls back (" + std::to_string(bell) + ")");
        const double lshelf = at(SSL, with({{"L9", 1.0f}, {"L10", 0.6f}, {"L11", 0.5f}}), 25.0);
        const double lbell = at(SSL, with({{"L9", 1.0f}, {"L10", 0.6f}, {"L11", 1.0f}}), 25.0);
        check(lshelf > 13.0 && lbell < lshelf - 4.0, "LF Bell: shelf (" + std::to_string(lshelf) + ") vs bell (" + std::to_string(lbell) + ")");
    }
    // ── HP / LP filters ──
    {
        check(near(at(SSL, with({{"H12", 0.0f}}), 20.0), 0.0, 0.05), "HP Freq fully left = filter out");
        // SSL 4000 channel filters: HPF 30-600 Hz, LPF 50 kHz-3 kHz, each off when fully left.
        const double c3 = at(SSL, with({{"H12", 1.0f}}), 600.0), oct = at(SSL, with({{"H12", 1.0f}}), 300.0);
        check(near(c3, -3.0, 0.3) && near(oct, -18.1, 1.0), "HP at 600 Hz, 18 dB/oct (" + std::to_string(c3) + " @600, " + std::to_string(oct) + " @300)");
        const double h_lo3 = at(SSL, with({{"H12", 0.02f}}), 30.0 * std::pow(20.0, 0.02));
        check(near(h_lo3, -3.0, 0.4), "HP just past off sits at ~30 Hz (" + std::to_string(h_lo3) + " dB at its corner)");
        check(near(at(SSL, with({{"L13", 0.0f}}), 16000.0), 0.0, 0.05), "LP Freq fully left = filter out");
        const double l3 = at(SSL, with({{"L13", 1.0f}}), 3000.0), loct = at(SSL, with({{"L13", 1.0f}}), 6000.0);
        check(near(l3, -3.0, 0.3) && near(loct, -12.3, 1.0), "LP at 3 kHz, 12 dB/oct (" + std::to_string(l3) + " @3k, " + std::to_string(loct) + " @6k)");
    }
    // ── G / E series (M12; default = G): G = proportional-Q mids and steeper shelves ──
    {
        const double fc = 200.0 * std::sqrt(12.5), off = fc * 1.6;
        const double g_small = at(SSL, with({{"M12", 1.0f}, {"L6", 0.6f}}), off) / 3.0, e_small = at(SSL, with({{"M12", 0.0f}, {"L6", 0.6f}}), off) / 3.0;
        const double g_big = at(SSL, with({{"M12", 1.0f}, {"L6", 1.0f}}), off) / 15.0, e_big = at(SSL, with({{"M12", 0.0f}, {"L6", 1.0f}}), off) / 15.0;
        check(g_small > e_small + 0.15 && near(g_big, e_big, 0.02),
              "G mode widens small LMF boosts (share of peak gain off-centre: G " + std::to_string(g_small) + " vs E " + std::to_string(e_small) +
                  "), same width at full boost");
        // steeper G shelf: closer to full gain an octave above the corner, and the G overshoot dips below 0 dB under it
        const double fs = 1500.0 * std::pow(16000.0 / 1500.0, 0.3);
        const double gs = at(SSL, with({{"M12", 1.0f}, {"H0", 1.0f}, {"H1", 0.3f}}), fs * 2.0), es = at(SSL, with({{"M12", 0.0f}, {"H0", 1.0f}, {"H1", 0.3f}}), fs * 2.0);
        check(gs > es + 0.5, "G mode HF shelf is steeper than E (" + std::to_string(gs) + " vs " + std::to_string(es) + " dB an octave above the corner)");
    }
    // ── output ──
    check(near(at(SSL, with({{"O14", 1.0f}}), 1000.0), 18.0, 0.05) && near(at(SSL, with({{"O14", 0.0f}}), 1000.0), -18.0, 0.05), "Output spans -18..+18 dB");

    // ── random settings: engine == the legacy analog prototype (analog prototypes), below the bilinear-cramping region ──
    {
        std::mt19937 rng(11);
        std::uniform_real_distribution<float> u(0.0f, 1.0f);
        double worst = 0, worst_lo = 0, sum = 0;
        int cnt = 0;
        for (int t = 0; t < 40; ++t) {
            Params p;
            for (const char* id : {"H0", "H1", "H3", "H4", "H5", "L6", "L7", "L8", "L9", "L10", "H12", "L13", "O14"}) p[id] = u(rng);
            p["H2"] = u(rng) > 0.5f ? 1.0f : 0.0f;
            p["L11"] = u(rng) > 0.5f ? 1.0f : 0.0f;
            p["M12"] = u(rng) > 0.5f ? 1.0f : 0.0f;
            Layout L;
            layoutFor(SSL, [&](const char* id, float fb) { auto it = p.find(id); return it == p.end() ? fb : it->second; }, L);
            auto c = makeLegacy(SSL, p);
            const auto h = impulse(*c);
            for (double f = 30.0; f <= 5000.0; f *= 1.25) {
                const double e = std::fabs(respDb(h, f) - analogDb(L, f));
                worst = std::max(worst, e);
                if (f <= 2000.0) worst_lo = std::max(worst_lo, e);
                sum += e;
                ++cnt;
            }
        }
        // Bilinear biquads cramp toward Nyquist, so wide 7 kHz bells / 16 kHz shelves read a little low in the top octave.
        check(worst_lo < 0.5 && worst < 1.5 && sum / cnt < 0.15,
              "engine matches the legacy analog prototype over 40 random settings: worst " + std::to_string(worst_lo) + " dB to 2 kHz, " +
                  std::to_string(worst) + " dB to 5 kHz, mean " + std::to_string(sum / cnt) + " dB");
    }

    // Measure the factory component via the same public response API used by the face.
    // Reference curves come from the independent ngspice AC fixtures in test_circuit_engine.
    const auto app = std::filesystem::path(__FILE__).parent_path().parent_path();
    const auto spec = nlohmann::json::parse(std::ifstream(app / "ui/assets/units/index.json"));
    const auto& displays = spec.at("units").at("121000").at("displays");
    check(std::any_of(displays.begin(), displays.end(), [](const auto& d) {
        return d.value("type", "") == "curve" && d.value("model", "") == "measured";
    }), "SSL face draws the measured response");
    const auto measure = [&](const Params& p) {
        const auto curves = rig::model::curves(SSL, nlohmann::json(p).dump(), "", kSr, "response", 0.25f, 330.0f);
        return nlohmann::json::parse(curves).at("response");
    };
    const auto checkSpice = [&](const char* fixture, const Params& p) {
        std::ifstream ref(app / "tests/fixtures/circuit" / (std::string(fixture) + ".ac.txt"));
        std::string header;
        std::getline(ref, header);
        std::vector<double> freqs, gains;
        double f, gain;
        while (ref >> f >> gain) { freqs.push_back(f); gains.push_back(gain); }
        check(freqs.size() == 61, std::string("complete ngspice fixture: ") + fixture);
        if (freqs.size() != 61) return;
        const auto r = measure(p);
        double worst = 0, worstHz = 0;
        size_t compared = 0;
        for (size_t i = 0; i < r.at("f").size(); ++i) {
            f = r.at("f")[i];
            if (f < freqs.front() || f > 5000.0) continue;
            const auto upper = std::upper_bound(freqs.begin(), freqs.end(), f) - freqs.begin();
            const auto lower = upper - 1;
            const double frac = std::log(f / freqs[lower]) / std::log(freqs[upper] / freqs[lower]);
            const double expected = std::lerp(gains[lower], gains[upper], frac);
            const double db = r.at("db")[i].get<double>() + r.at("peak_db").get<double>();
            if (!std::isfinite(db)) { check(false, "nonfinite measured gain"); return; }
            if (std::abs(db - expected) > worst) { worst = std::abs(db - expected); worstHz = f; }
            ++compared;
        }
        // Includes interpolation between the SPICE file's 20 points per decade
        // and the host-rate render through the rack oversampler.
        check(compared > 70 && worst < 0.6, std::string(fixture) + " factory magnitude 20 Hz–5 kHz, worst " + std::to_string(worst) + " dB at " + std::to_string(worstHz) + " Hz");
    };
    checkSpice("ssleq_ac", {});
    checkSpice("ssleq_ac_bells", {{"H0", 1}, {"H2", 1}, {"H3", 0}, {"H5", 0}, {"L6", 1}, {"L8", 0}, {"L9", 0}, {"L11", 1}});
    checkSpice("ssleq_ac_filt", {{"hpf_in", 1}, {"lpf_in", 1}, {"H12", 0.7f}, {"L13", 0.6f}, {"H0", 0.8f}, {"L9", 0.2f}});
    const auto hp = measure({{"H12", 1.0f}, {"L13", 1.0f}, {"hpf_in", 1.0f}});
    const auto lp = measure({{"H12", 1.0f}, {"L13", 1.0f}, {"lpf_in", 1.0f}});
    const auto dbAt = [](const nlohmann::json& response, double hz) {
        const auto fs = response.at("f").get<std::vector<double>>();
        const auto i = std::lower_bound(fs.begin(), fs.end(), hz) - fs.begin();
        return response.at("db").at(i).get<double>() + response.at("peak_db").get<double>();
    };
    check(dbAt(hp, 60) < -35 && std::abs(dbAt(hp, 12000)) < 1.2, "HP In engages only the high-pass filter");
    check(dbAt(lp, 12000) < -18 && std::abs(dbAt(lp, 60)) < 0.2, "LP In engages only the low-pass filter");
    const auto& controls = spec.at("units").at("121000").at("controls");
    for (const char* name : {"HP In", "LP In"}) {
        check(std::any_of(controls.begin(), controls.end(), [&](const auto& c) {
            return c.value("p", "") == name && c.value("t", "") == "switch";
        }), std::string("face switch bound to ") + name);
    }
    // Compare the displayed absolute gain with audio rendered through the public rack factory.
    // Exercise all four independent filter combinations and the individual band controls.
    std::mt19937 rng(23);
    std::uniform_real_distribution<float> knob(0.0f, 1.0f);
    double worst = 0;
    for (int t = 0; t < 12; ++t) {
        Params p;
        for (const char* id : {"H0","H1","H3","H4","H5","L6","L7","L8","L9","L10","H12","L13"})
            p[id] = knob(rng);
        p["O14"] = knob(rng);
        p["H2"] = t % 3 == 0;
        p["L11"] = t % 3 == 1;
        p["hpf_in"] = t % 2;
        p["lpf_in"] = (t / 2) % 2;
        auto rack = rig::createComponentByTableId(SSL);
        for (const auto& [id, v] : p) rack->setParameter(id, v);
        rack->prepare(kSr, 256);
        rack->reset();
        const auto background = impulse(*rack, 16384, 0.0f);
        rack->reset();
        auto h = impulse(*rack, 16384, 0.01f);
        for (size_t i = 0; i < h.size(); ++i) h[i] -= background[i];
        const auto r = measure(p);
        for (size_t i = 2; i < r.at("f").size(); i += 4) {
            const double actual = respDb(h, r.at("f")[i]) + 40.0; // compensate the -40 dBFS impulse
            const double shown = r.at("db")[i].get<double>() + r.at("peak_db").get<double>();
            if (!std::isfinite(actual) || !std::isfinite(shown)) { check(false, "nonfinite rack response"); break; }
            worst = std::max(worst, std::abs(actual - shown));
        }
    }
    check(worst < 0.8, "measured face versus rack audio at 12 settings, worst " + std::to_string(worst) + " dB");
    std::printf("%s\n", g_failures ? "FAILED" : "all studio EQ checks passed");
    return g_failures ? 1 : 0;
}
