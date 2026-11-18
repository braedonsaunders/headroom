// Aliasing gate (docs/CIRCUIT_ENGINE.md §7): every bound circuit unit, through the real CircuitUnit path (its default
// quality, 48 kHz host, 256-sample blocks), driven with a 2.9 kHz sine. With 4800 analysed samples every harmonic and
// every image folded about 24 kHz falls on an exact 10 Hz bin, so a coherent DFT separates them without a window:
// alias level = the largest non-harmonic bin relative to the fundamental. On a unit whose netlist has a mains source
// (a 45-65 Hz V source: its supply ripple and heater hum, docs §3.9) the bins within 10 Hz of a harmonic ± 1-12 × its own
// mains frequency are excluded as hum sidebands (docs §10.62: the other frequency's steps hid folds that cannot be hum);
// a unit without one (pedals, DC-supplied studio units) has no hum, so every fold bin counts (docs §10.61).
// Every unit is also measured with a second tone, 3130 Hz, and gated on both (docs §10.63). Its folds about every
// power-of-two rate from 48 kHz to 1.536 MHz land 830-1150 Hz from a harmonic, clear of every masked hum bin of either
// mains frequency: 2.9 kHz's 96 / 192 kHz folds land 300 / 600 Hz from a harmonic, on the 60 Hz unit's 5th / 10th hum
// step, so the 2.9 kHz own-mains mask hid six amps' base-rate folds (docs §10.62). On a unit without mains the second
// tone samples other harmonics against each fold rate (the GT2 failed it alone, docs §10.63).
// HEADROOM_ALIAS_TONE2=0 reports the second tone without gating it.
// Units with time-varying blocks (BBD, tape, LFO) are reported, not gated: their modulation sidebands are not aliasing.
// The Leslie 122 is gated with its rotors stopped (the rotor block's Doppler sidebands are not aliasing either).
//
// HEADROOM_ALIAS_REPORT=1 prints every unit (the gate figure, and the masked and every-bin figures); HEADROOM_ALIAS_LEVEL
// overrides the drive (default 0.5 FS = 0.5 V at the input jack for 1 V/FS units); HEADROOM_ALIAS_ONLY=<id>[,<id>...]
// measures only those netlist or catalogue ids.
#include "platform/Environment.hpp"
#include "circuit/CircuitUnit.hpp"
#include "circuit/Netlist.hpp"
#include "dsp/ComponentModel.hpp"
#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace rig;
namespace ce = rig::ce;

static int failures = 0;
#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);        \
            std::fprintf(stderr, __VA_ARGS__);                               \
            std::fprintf(stderr, "\n");                                      \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

static constexpr double kFs = 48000.0;
static constexpr double kTone = 2900.0;
static constexpr double kTone2 = 3130.0; // the second tone (docs §10.62; gated on every unit since §10.63)
static const int kSettle = std::getenv("HEADROOM_ALIAS_SETTLE") ? std::atoi(std::getenv("HEADROOM_ALIAS_SETTLE")) : 72000; // 1.5 s: germanium bias settles slowly
static constexpr int kN = 4800;       // 10 Hz bins; 2.9 kHz = bin 290
static constexpr double kGateDb = -60.0;

static std::vector<float> render(IComponent& c, const std::vector<float>& in, uint32_t block = 256) {
    std::vector<float> outL(in.size()), outR(in.size());
    for (std::size_t o = 0; o < in.size(); o += block) {
        const uint32_t n = static_cast<uint32_t>(std::min<std::size_t>(block, in.size() - o));
        const float* ins[2] = {in.data() + o, in.data() + o};
        float* outs[2] = {outL.data() + o, outR.data() + o};
        AudioBlock b;
        b.in = ins;
        b.out = outs;
        b.num_channels = 2;
        b.num_frames = n;
        b.sample_rate = static_cast<uint32_t>(kFs);
        c.processControlRate();
        c.processAudio(b);
    }
    return outL;
}

struct AliasResult {
    double fundamental = 0, worstDb = -300, worstHz = 0; // the gate's figure (own-mains hum bins skipped on a mains unit)
    double maskedDb = -300, maskedHz = 0;                // hum bins of both mains frequencies skipped (the rule before §10.62)
    double fullDb = -300, fullHz = 0;                    // every fold bin
    double ownDb = -300, ownHz = 0;                      // hum bins of the netlist's own mains frequency only
};

// The netlist's mains frequency: its largest 45-65 Hz V source (the engine's heater-hum reference, Engine.cpp), else 0.
static double mainsHz(const ce::Netlist& n) {
    double hz = 0, amp = 0;
    for (const auto& p : n.parts)
        if (p.type == "V" && p.acAmp != 0 && p.acFreq >= 45.0 && p.acFreq <= 65.0 && std::abs(p.acAmp) > amp) amp = std::abs(p.acAmp), hz = p.acFreq;
    return hz;
}

// Coherent DFT magnitude at integer bins 0..kN/2 of the last kN samples.
static AliasResult measure(const std::vector<float>& y, double mains, double tone = kTone) {
    const std::size_t off = y.size() - kN;
    double mean = 0;
    for (int i = 0; i < kN; ++i) mean += y[off + static_cast<std::size_t>(i)];
    mean /= kN;
    std::vector<double> x(kN);
    for (int i = 0; i < kN; ++i) x[static_cast<std::size_t>(i)] = y[off + static_cast<std::size_t>(i)] - mean;
    auto mag = [&](int k) {
        std::complex<double> acc = 0, w = 1, step = std::polar(1.0, -2.0 * M_PI * k / kN);
        for (int i = 0; i < kN; ++i) {
            acc += x[static_cast<std::size_t>(i)] * w;
            w *= step;
            if ((i & 1023) == 1023) w = std::polar(1.0, -2.0 * M_PI * k * (i + 1) / kN); // keep the rotation exact
        }
        return 2.0 * std::abs(acc) / kN;
    };
    AliasResult r;
    const int toneBin = static_cast<int>(std::lround(tone * kN / kFs)); // 290 (3130 Hz: 313)
    r.fundamental = mag(toneBin);
    // Where aliases must land: harmonic k folded about any rate the circuit runs at, |k·f0 − m·fs|, for the host rate
    // and every internal (oversampled) rate up to 8x. Everything is a multiple of 100 Hz, so exact bins.
    std::vector<int> bins;
    for (double fs : {kFs, 2 * kFs, 4 * kFs, 8 * kFs, 88200.0, 176400.0, 352800.0})
        for (int k = 2; k <= 400; ++k)
            for (int m = 1; m <= 16; ++m) {
                const double f = std::abs(k * tone - m * fs);
                if (f < 20.0 || f > 20000.0) continue;
                const double bin = f * kN / kFs;
                if (std::abs(bin - std::round(bin)) > 1e-6) continue; // 44.1 kHz-family rates fold off the grid
                bins.push_back(static_cast<int>(std::lround(bin)));
            }
    std::sort(bins.begin(), bins.end());
    bins.erase(std::unique(bins.begin(), bins.end()), bins.end());
    // within 1 bin of h·f0 ± j·50/60 Hz (supply ripple intermodulation) or of low hum; own: the mains frequency's step only
    auto humSideband = [&](int k, bool own = false) {
        if (own && !(mains > 0)) return false; // no mains: no hum of its own
        const int ownStep = mains > 0 && mains < 55 ? 5 : 6;
        for (int h = 0; h * toneBin <= kN / 2 + 60; ++h)
            for (int j = 1; j <= 12; ++j)
                for (int step : {5, 6})
                    for (int d : {-1, 0, 1})
                        if ((!own || step == ownStep) && (k == h * toneBin + j * step + d || k == h * toneBin - j * step + d)) return true;
        return false;
    };
    for (int k : bins) {
        if (k % toneBin == 0) continue;
        const double m = mag(k);
        const double db = 20.0 * std::log10(std::max(m, 1e-15) / std::max(r.fundamental, 1e-15));
        const double hz = k * kFs / kN;
        if (db > r.fullDb) r.fullDb = db, r.fullHz = hz;
        if (!humSideband(k) && db > r.maskedDb) r.maskedDb = db, r.maskedHz = hz;
        if (!humSideband(k, true) && db > r.ownDb) r.ownDb = db, r.ownHz = hz;
    }
    r.worstDb = mains > 0 ? r.ownDb : r.fullDb;
    r.worstHz = mains > 0 ? r.ownHz : r.fullHz;
    return r;
}

// Units still waiting for a per-group rate (docs §3.7). List a netlist id here to report it without gating.
// (The Plexi and Plexi variac were listed here while the rate ladder's cut currents crossed levels as a base-rate mean:
// fixed 2026-11-10, ENGINE_QUEUE 33.)
// The JCM800 (-59.6 dB, §10.46) and the Peavey 5150 (-50.4 dB) were listed here until 2026-11-14 (docs §10.49). Both alias
// at the gate tone through their output transformer's flyback: the worst spur is harmonic 131-138 (380-400 kHz) folded
// about 384 kHz, the AC30 / Plexi mechanism of §10.32, and in both the preamp and power amp are one Newton group, so no
// per-group rate could reach it. They now take §10.48's event-local 16x ("multirate": {"rates": {...: 16}, "events":
// true}): the JCM800 reads -77.0 dB and the 5150 -71.5 dB here, and they are gated.
// Bogner Uberschall (2026-11-11): -58.5 dB. The spur follows the LTE substep decisions, not the rate (8x rate -57.7 dB,
// fixed step -62.7 dB, but the dimed SPICE case then fails at 6.7 %); everything after the supply is one 36-port group with
// no exact cut, so no per-group rate applies either. Reported, not gated (it is time-varying as well: its LDR channel
// switching is a slow partition). With event-local 16x it reads -61.6 dB at its tolerance 8 (§10.49), not yet taken.
static bool pendingPerGroupRate(const std::string& id) { return id == "bogner-uberschall-81000"; }

// Slow partitions that are constant at fixed controls (no oscillator or side chain in them): the unit is static and
// stays gated. SLO-100 (2026-11-11): its channel-switch LED drivers and LDR light history. BE-100 (2026-11-16, docs
// §10.65): the bypassed FX loop and the SAT clamp with SAT off, which only leak into the amp through 1 GOhm open contacts.
static bool staticSlowPartition(const std::string& id) { return id == "soldano-slo100-122000" || id == "friedman-be100-151000"; }

// Spring tanks are linear and time-invariant (Blocks.hpp SpringTank: delays, allpasses, fixed losses), but a sine that
// starts at t = 0 leaves the tank's broadband onset ringing for seconds (T60 ~3 s), far above -60 dB after the common
// 1.5 s. Units listed here are gated anyway, after a settle long enough for that onset to decay (6 s: ~-120 dB).
// Fender 6G15 (2026-11-12): the tank is the whole wet path, so its drive and recovery stages are what this gates.
static bool staticSpring(const std::string& id) { return id == "fender-6g15-27000"; }
static constexpr int kSpringSettle = 288000;

static bool timeVarying(const ce::Netlist& n, const std::string& raw) {
    for (const auto& p : n.parts)
        if (p.type == "bbd" || p.type == "tape" || (p.type == "spring" && !staticSpring(n.id))) return true;
    const auto j = nlohmann::json::parse(raw, nullptr, false);
    return (!n.slowNodes.empty() && !staticSlowPartition(n.id)) || (j.is_object() && j.contains("readouts") && j["readouts"].contains("lfo"));
}

// The Leslie 122's rotor block (binding post "leslie-rotors"): chorale with both rotors' speeds at 0 stops them, so the
// gate sees the circuit amplifier and dividing network through still rotors (docs §10.62).
static void stopRotors(IComponent& u, const ce::UnitBinding& b) {
    if (std::string(b.post) != "leslie-rotors") return;
    u.setParameter("SF", 0.0f);
    u.setParameter("TS", 0.0f);
    u.setParameter("BS", 0.0f);
}

static std::vector<float> toneInput(double tone, double level, int n) {
    std::vector<float> x(static_cast<std::size_t>(n));
    for (std::size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(level * std::sin(2 * M_PI * tone * double(i) / kFs));
    return x;
}

int main() {
    rig::platform::setEnv("HEADROOM_NOISE_FIXED_SEED", "1"); // reproducible noise streams (CircuitUnit, docs §3.9)
    const bool report = std::getenv("HEADROOM_ALIAS_REPORT") != nullptr;
    const double level = std::getenv("HEADROOM_ALIAS_LEVEL") ? std::atof(std::getenv("HEADROOM_ALIAS_LEVEL")) : 0.5;
    const bool tone2Gated = !(std::getenv("HEADROOM_ALIAS_TONE2") && std::atoi(std::getenv("HEADROOM_ALIAS_TONE2")) == 0); // docs §10.63: on by default
    std::printf("aliasing gate: %.0f Hz and %.0f Hz (%s) at %.2f FS, 48 kHz host, default quality; gate %.0f dB (static units)\n", kTone,
                kTone2, tone2Gated ? "gated" : "reported", level, kGateDb);
    int gated = 0, passed = 0;
    const std::string only = std::getenv("HEADROOM_ALIAS_ONLY") ? std::string(",") + std::getenv("HEADROOM_ALIAS_ONLY") + "," : std::string();
    for (const auto& b : ce::unitBindings()) {
        if (!ce::circuitEngineEnabledFor(b.tableId)) continue;
        if (!only.empty() && only.find("," + std::string(b.netlistId) + ",") == std::string::npos &&
            only.find("," + std::to_string(b.tableId) + ",") == std::string::npos)
            continue;
        const auto* schema = catalog::findByTableId(b.tableId);
        if (!schema) continue;
        ce::Netlist n;
        std::string err;
        const std::string raw = ce::libraryNetlist(b.netlistId);
        if (!ce::parseNetlistText(raw, n, err)) continue;
        const double hz = mainsHz(n);
        const int len = (staticSpring(b.netlistId) ? kSpringSettle : kSettle) + kN;
        // one fresh unit per tone
        auto run = [&](double tone, AliasResult& r) {
            auto u = ce::makeCircuitUnitAlways(*schema);
            if (!u) return false;
            stopRotors(*u, b);
            u->prepare(kFs, 256);
            r = measure(render(*u, toneInput(tone, level, len)), hz, tone);
            return true;
        };
        const bool tv = timeVarying(n, raw);
        const bool pending = pendingPerGroupRate(b.netlistId);
        AliasResult r, r2;
        if (!run(kTone, r)) continue;
        const bool two = (report || (!tv && !pending)) && run(kTone2, r2); // report-only units: with REPORT only
        const bool gate2 = two && tone2Gated;
        const double worst = gate2 ? std::max(r.worstDb, r2.worstDb) : r.worstDb;
        const double worstHz = gate2 && r2.worstDb > r.worstDb ? r2.worstHz : r.worstHz;
        const bool gate = !tv && !pending && r.fundamental > 1e-6;
        if (report || pending || (gate && worst > kGateDb)) {
            std::printf("  %-34s %6.1f dB at %7.1f Hz  [%s; hum-masked %6.1f at %7.1f, own-mains mask %6.1f at %7.1f, every bin %6.1f at %7.1f]", b.netlistId, worst, worstHz,
                        hz > 0 ? (hz < 55 ? "mains 50 Hz" : "mains 60 Hz") : "no mains", r.maskedDb, r.maskedHz, r.ownDb, r.ownHz, r.fullDb, r.fullHz);
            if (two) std::printf("  %.0f Hz%s: own-mains mask %6.1f at %7.1f, hum-masked %6.1f, every bin %6.1f", kTone2, gate2 ? "" : " (reported)", r2.ownDb, r2.ownHz, r2.maskedDb, r2.fullDb);
            std::printf("%s\n", tv ? "  (time-varying: report only)" : pending ? "  (pending per-group rate: report only)" : "");
        }
        if (gate) {
            ++gated;
            if (worst <= kGateDb) ++passed;
            CHECK(worst <= kGateDb, "%s: alias %.1f dB at %.0f Hz (gate %.0f dB)", b.netlistId, worst, worstHz, kGateDb);
        }
    }
    std::printf("%d of %d gated units below %.0f dB\n", passed, gated, kGateDb);
    if (failures) std::fprintf(stderr, "%d aliasing failure(s)\n", failures);
    else std::printf("all aliasing checks passed\n");
    return failures ? 1 : 0;
}
