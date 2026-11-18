// circuit_cli: developer tool for the Headroom circuit engine (docs/CIRCUIT_ENGINE.md).
//   circuit_cli info   <netlist>                         groups, solvers, DC operating point
//   circuit_cli spice  <netlist> [opts] [--input SPEC] [--tran "step stop"]   ngspice deck on stdout
//   circuit_cli render <netlist> [opts] --rate R (--sine f amp secs | --wav in.wav [--gain g] | --csv in.csv) --out out.csv|out.wav [--probe n]...
//                      io.inputR circuits: [--sineR f amp | --wavR in.wav | --csvR in.csv] (default: the left signal);
//                      io.outputR circuits add the right output as the CSV column after the left one
//   circuit_cli bench  <netlist> [opts] [--rate R] [--seconds s] [--real]   --real (with --table): also the DI and hard-material
//                      cost and 64-frame block times through the unit path (realBench; GR_LESSONS.md §4 P0)
//   circuit_cli alias    <netlist> [--table ID] [--ctl k=v]...   the aliasing gate's figure (tests/test_aliasing.cpp), unit path
//                        CE_ALIAS_IDEAL=1: the any-rate figure instead (the tone at the circuit's rate, every non-harmonic
//                        10 Hz bin 20 Hz-20 kHz of the circuit-rate output: a perfect decimator; docs §10.48)
//   circuit_cli autorate <netlist> [--table ID] [--ctl k=v]...   per-group rate choice by that figure ("multirate.rates")
//   circuit_cli autocut  <netlist> [--table ID] [--gate test_circuit_engine] [--wav di.wav] [--top N] [--minports P]
//                        [--try "node:LOAD,LOAD;node:LOAD"] [--full]   signal cuts that cost nothing measurable (docs §10.27)
//                      both default to the CircuitUnit's path at a 48 kHz host: its oversampling (netlist factor, minRate),
//                      450 ms settle, the netlist's rates (alias); --table <catalogue id> sets the controls where the unit's
//                      default parameters put them (as the gate does); --rate / --settle / --ctl / CE_LEVELS override
// opts: --ctl id=value (0..1), --solver dk|wdf|auto, --nomerge
//       --settle ms   the start state of mains-fed supplies (CompileOptions::settleMs; default 600, the unit's 450 for
//                     alias / autorate / --table): 0 = the DC operating point (what the SPICE .op gate compares; a
//                     transformer-fed supply has every rail at 0 V there); > 0 = the running state: rectifiers fed
//                     straight from mains sources run at least this long, until the rails' per-period means stop
//                     moving (at most 2 s, or the value if longer); a supply behind a power transformer starts at its
//                     periodic steady state (settlePeriodic; any value > 0)
//       --cold ms     a cold start instead (CompileOptions::coldMs, ngspice `.tran ... uic`): everything at 0 V and 0 A,
//                     then this many ms of the mains (a real duration) before the render
// spice: --ic-settled writes the engine's running state (--settle) as the deck's .ic / inductor IC= (add `uic`)
#include "platform/Environment.hpp"
#include "circuit/CircuitOversampler.hpp"
#include "generated/ComponentCatalog.hpp"
#include "circuit/Engine.hpp"
#include "circuit/LaneBlock.hpp"
#include "circuit/Denormals.hpp"
#include "WavIO.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <complex>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#endif
#ifdef __APPLE__
#include <libproc.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

using namespace rig::ce;

namespace {
std::string slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}
struct Args {
    std::vector<std::string> pos;
    std::vector<std::pair<std::string, double>> ctl;
    std::string solver, input, tran, out, wav, csvIn, wavR, csvInR;
    std::vector<std::string> probes;
    double rate = 96000, sineF = 0, sineA = 0, secs = 1, gain = 1, benchSecs = 10, settleMs = 600, sineRF = 0, sineRA = 0, coldMs = -1;
    bool sineR = false;
    bool merge = true;
    int table = 0;                           // --table: the catalogue unit whose default parameters set the controls
    bool rateSet = false, settleSet = false; // --rate / --settle given (alias and autorate otherwise use the unit's)
    bool exportAllNodes = false;             // every node probe-able (spice --ic-settled)
    bool unitLike = false;                   // --table: compile as the live unit does (noise, hum, BBD noise on; minimal exports)
    // autocut
    std::string gate;                        // --gate <test_circuit_engine>: run the netlist's SPICE cases per candidate
    std::vector<std::string> tries;          // --try "node:LOAD,LOAD[;node:LOAD...]": evaluate these cut sets only
    int minPorts = 6, top = 12;              // --minports: groups considered; --top: candidates measured per run
    bool full = false;                       // --full: measure every criterion even after one fails
    bool real = false;                       // bench --real: DI and hard material through the unit path (realBench)
};
Args parse(int argc, char** argv) {
    Args a;
    for (int i = 2; i < argc; ++i) {
        std::string s = argv[i];
        auto next = [&]() { return std::string(i + 1 < argc ? argv[++i] : ""); };
        if (s == "--ctl") {
            auto kv = next();
            auto eq = kv.find('=');
            a.ctl.push_back({kv.substr(0, eq), std::stod(kv.substr(eq + 1))});
        } else if (s == "--solver") a.solver = next();
        else if (s == "--input") a.input = next();
        else if (s == "--tran") a.tran = next();
        else if (s == "--out") a.out = next();
        else if (s == "--wav") a.wav = next();
        else if (s == "--csv") a.csvIn = next();
        else if (s == "--wavR") a.wavR = next();
        else if (s == "--csvR") a.csvInR = next();
        else if (s == "--sineR") {
            a.sineR = true;
            a.sineRF = std::stod(next());
            a.sineRA = std::stod(next());
        }
        else if (s == "--gain") a.gain = std::stod(next());
        else if (s == "--rate") a.rate = std::stod(next()), a.rateSet = true;
        else if (s == "--table") a.table = std::stoi(next());
        else if (s == "--probe") a.probes.push_back(next());
        else if (s == "--seconds") a.benchSecs = std::stod(next());
        else if (s == "--nomerge") a.merge = false;
        else if (s == "--gate") a.gate = next();
        else if (s == "--try") a.tries.push_back(next());
        else if (s == "--minports") a.minPorts = std::stoi(next());
        else if (s == "--top") a.top = std::stoi(next());
        else if (s == "--full") a.full = true;
        else if (s == "--real") a.real = true;
        else if (s == "--settle") a.settleMs = std::stod(next()), a.settleSet = true; // mains-fed start state (see the header)
        else if (s == "--cold") a.coldMs = std::stod(next());                          // cold start + this many ms of mains
        else if (s == "--sine") {
            a.sineF = std::stod(next());
            a.sineA = std::stod(next());
            a.secs = std::stod(next());
        } else a.pos.push_back(s);
    }
    return a;
}
bool load(const Args& a, Netlist& n) {
    std::string err;
    if (a.pos.empty()) {
        std::fprintf(stderr, "missing netlist\n");
        return false;
    }
    const std::string text = slurp(a.pos[0]);
    if (!parseNetlistText(text, n, err)) {
        std::fprintf(stderr, "netlist: %s\n", err.c_str());
        return false;
    }
    // Authoring lint on every command: unit errors and mains-node collisions pass the numeric gates silently.
    for (const auto& w : lintNetlist(nlohmann::json::parse(text, nullptr, false), n)) std::fprintf(stderr, "lint: %s\n", w.c_str());
    return true;
}
std::vector<double> ctlVector(const Netlist& n, const Args& a) {
    std::vector<double> v;
    for (const auto& c : n.controls) v.push_back(c.def);
    for (const auto& [k, x] : a.ctl) {
        const int i = n.controlIndex(k);
        if (i < 0) std::fprintf(stderr, "unknown control %s\n", k.c_str());
        else v[static_cast<std::size_t>(i)] = x;
    }
    return v;
}
bool build(const Args& a, const Netlist& n, Circuit& c) {
    CompileOptions o;
    o.sampleRate = a.rate;
    o.solver = a.solver;
    o.probes = a.probes;
    o.mergeDiodePairs = a.merge;
    o.settleMs = a.settleMs;
    o.coldMs = a.coldMs;
    o.exportAllNodes = a.exportAllNodes || (std::getenv("CE_BENCH") == nullptr && !a.unitLike);
    if (std::getenv("CE_NOLAZY")) o.lazyClamps = false;
    // CE_NOISE / CE_HUM: 1 on, 0 off (any other value also on); unset: the unit's default under --table (on), else off
    auto envOn = [](const char* name, bool dflt) {
        const char* v = std::getenv(name);
        return v ? std::string_view(v) != "0" : dflt;
    };
    o.noise = envOn("CE_NOISE", a.unitLike); // thermal + shot noise (CompileOptions::noise)
    o.hum = envOn("CE_HUM", a.unitLike);     // heater-cathode hum (CompileOptions::hum)
    if (a.unitLike) o.bbdNoise = o.noise;    // the unit sets all three from one switch (CircuitUnit: bbdNoise_)
    if (const char* sd = std::getenv("CE_NOISE_SEED")) o.noiseSeed = std::strtoull(sd, nullptr, 10);
    if (const char* m = std::getenv("CE_MAXIT")) o.maxIterations = std::atoi(m);
    o.multirate = std::getenv("CE_MULTIRATE") != nullptr; // rate = 2x host; render/bench feed host-rate samples
    if (const char* lv = std::getenv("CE_LEVELS")) { // rate ladder: "REF=level,REF=level" (CompileOptions::partLevels)
        std::istringstream s(lv);
        std::string tok;
        while (std::getline(s, tok, ','))
            if (const auto eq = tok.find('='); eq != std::string::npos) o.partLevels.push_back({tok.substr(0, eq), std::stoi(tok.substr(eq + 1))});
        while (48000.0 * (1 << (o.ladderBaseStage + 1)) <= a.rate + 1.0) ++o.ladderBaseStage; // converter stages as a 48 kHz unit's
    }
    std::string err;
    if (!c.compile(n, o, err)) {
        std::fprintf(stderr, "compile: %s\n", err.c_str());
        return false;
    }
    const auto v = ctlVector(n, a);
    for (std::size_t i = 0; i < v.size(); ++i) c.setControlImmediate(static_cast<int>(i), v[i]);
    if (!a.ctl.empty() && !c.reset()) std::fprintf(stderr, "warning: DC did not converge\n");
    return true;
}
// --table: the controls where the unit puts them at its defaults (CircuitUnit::bindControls / controlFromParams):
// the first of a control's parameter aliases the unit has, mapped from its paramMin..paramMax; else the netlist
// default; switches snap at 0.5. --ctl arguments still win. (unitPath, and spice --table so a deck matches the unit.)
void tableControls(Args& a, const Netlist& n) {
    if (a.table > 0) {
        const rig::catalog::ComponentSchema* schema = nullptr; // the main catalogue (header only; no studio/extra units)
        for (const auto& cs : rig::catalog::kAllComponents)
            if (cs.table_id == a.table) schema = &cs;
        if (!schema) std::fprintf(stderr, "no catalogue unit %d\n", a.table);
        for (const auto& c : n.controls) {
            if (!schema || std::any_of(a.ctl.begin(), a.ctl.end(), [&](const auto& kv) { return kv.first == c.id; })) continue;
            double x = c.def;
            for (const auto& pid : c.params) {
                const auto it = std::find_if(schema->params.begin(), schema->params.end(), [&](const auto& ps) { return ps.id == pid; });
                if (it == schema->params.end()) continue;
                x = std::clamp((static_cast<double>(static_cast<float>(it->default_value)) - c.paramMin) / (c.paramMax - c.paramMin), 0.0, 1.0);
                break;
            }
            if (c.kind == "switch") x = x > 0.5 ? 1.0 : 0.0;
            a.ctl.push_back({c.id, x});
        }
    }
}
// Aliasing as tests/test_aliasing.cpp measures it, through the unit's path: 48 kHz host, a float half-band oversampler of
// rate/48000 around the circuit, a 2.9 kHz tone at CE_ALIAS_LEVEL (0.5) FS, the worst non-harmonic bin of 4800 samples
// after 1.5 s. Returns that bin in dB re the tone; worstHz and the CPU share (circuit + oversampler, thread time) on the side.
// The CircuitUnit's choices at a 48 kHz host (CircuitUnit::onPrepare / rebuild, HQ default): oversampling factor from the
// netlist's factor and minRate, the 450 ms mains settle, and the netlist's per-group rates as ladder levels. Applied by
// alias / autorate unless --rate / --settle / CE_LEVELS say otherwise, so their numbers are the aliasing gate's.
void unitPath(Args& a, const Netlist& n, bool levelsFromNetlist) {
    if (!a.rateSet) {
        int f = std::max(1, n.oversampling);
        while (48000.0 * f < n.minRate - 1.0 && f < CircuitOversampler::kMaxFactor) f *= 2;
        a.rate = 48000.0 * f;
    }
    if (!a.settleSet) a.settleMs = 450.0;
    a.unitLike = true; // noise, hum and BBD noise on and only the needed nodes exported, as the unit (CE_NOISE=0 to compare)
    if (!std::getenv("CE_LTE") && !(n.lteTol > 0)) rig::platform::setEnv("CE_LTE", std::to_string(rig::ce::kLiveLteTol).c_str()); // the unit's tolerance (a netlist "lte.tol" replaces it, as in the unit)
    if (!std::getenv("CE_LTEPORTS")) rig::platform::setEnv("CE_LTEPORTS", std::to_string(rig::ce::kLiveLteMaxPorts).c_str()); // ...and group limit
    tableControls(a, n);
    if (levelsFromNetlist && !std::getenv("CE_LEVELS")) {
        const int f = static_cast<int>(std::lround(a.rate / 48000.0));
        std::string spec;
        for (const auto& [ref, want] : n.partRates) {
            int lv = 0;
            while ((f << (lv + 1)) <= want + 1e-9 && (f << (lv + 1)) <= (n.rateEvents ? 2 : 1) * CircuitOversampler::kMaxFactor) ++lv; // events: one level more (CircuitUnit, docs §10.62)
            if (lv > 0) spec += (spec.empty() ? "" : ",") + ref + "=" + std::to_string(lv);
        }
        if (!spec.empty()) rig::platform::setEnv("CE_LEVELS", spec.c_str());
    }
}
double threadSecs();
// The aliasing gate's hum rule (tests/test_aliasing.cpp, docs §10.61 / §10.62): on a netlist with a mains source (a 45-65 Hz
// V source; the largest one's frequency) bins near a harmonic ± 1-12 × that frequency are skipped as hum sidebands;
// otherwise every bin counts. Returns the hum steps to skip, in 10 Hz bins (0: none). CE_ALIAS_HUMMASK=1 skips both the 50
// and 60 Hz steps on every netlist (the rule before §10.61), 2 both on mains netlists (§10.61), 0 none.
// CE_ALIAS_TONE=<Hz> replaces the 2.9 kHz tone (a multiple of 10 Hz; the gate's second tone for mains units is 3130).
int aliasHumSteps(const Netlist& n) {
    double hz = 0, amp = 0;
    for (const auto& p : n.parts)
        if (p.type == "V" && p.acAmp != 0 && p.acFreq >= 45.0 && p.acFreq <= 65.0 && std::abs(p.acAmp) > amp) amp = std::abs(p.acAmp), hz = p.acFreq;
    if (const char* e = std::getenv("CE_ALIAS_HUMMASK")) {
        const int m = std::atoi(e);
        return m == 1 || (m == 2 && hz > 0) ? 56 : 0;
    }
    return hz > 0 ? (hz < 55 ? 5 : 6) : 0;
}
bool aliasHumAt(int k, int toneBin, int nHalf, int steps) { // within a bin of h·tone ± j·step, j = 1..12 (steps 56: both)
    if (!steps) return false;
    for (int h = 0; h * toneBin <= nHalf + 60; ++h)
        for (int j = 1; j <= 12; ++j)
            for (int st : {5, 6})
                for (int d : {-1, 0, 1})
                    if ((steps == 56 || st == steps) && (k == h * toneBin + j * st + d || k == h * toneBin - j * st + d)) return true;
    return false;
}
double aliasTone() { return std::getenv("CE_ALIAS_TONE") ? std::atof(std::getenv("CE_ALIAS_TONE")) : 2900.0; }
// CE_ALIAS_IDEAL=1 (docs §10.48; the shoot-out's any-rate metric): the same 2.9 kHz tone generated at the
// circuit's own rate and fed straight in (no host oversampler), 1.5 s settle, then a coherent DFT of 0.1 s of the
// circuit-rate output: every 10 Hz bin from 20 Hz to 20 kHz that is not a harmonic or a hum bin (the gate's hum rule)
// counts, i.e. a perfect decimator after the circuit. The gate's bin set covers folds about power-of-two rates only.
double aliasIdeal(const Args& a, const Netlist& n, Circuit& c, double& worstHz, double& secs) {
    const double R = a.rate, tone = aliasTone(), level = std::getenv("CE_ALIAS_LEVEL") ? std::atof(std::getenv("CE_ALIAS_LEVEL")) : 0.5;
    const int kN = static_cast<int>(std::lround(R / 10.0)), settle = static_cast<int>(std::lround(1.5 * R));
    const double inV = n.io.inputVoltsPerFS;
    std::vector<double> y(static_cast<std::size_t>(kN));
    const double t0 = threadSecs();
    {
        const ScopedFlushDenormals ftz;
        for (int i = 0; i < settle + kN; ++i) {
            const double v = c.process(level * inV * std::sin(2 * M_PI * tone * (i % kN) / R));
            if (i >= settle) y[static_cast<std::size_t>(i - settle)] = v;
        }
    }
    secs = threadSecs() - t0;
    if (const char* dump = std::getenv("CE_ALIAS_DUMP")) {
        std::ofstream f(dump);
        for (double v : y) f << v << "\n";
    }
    double mean = 0;
    for (double v : y) mean += v;
    mean /= kN;
    for (double& v : y) v -= mean;
    auto mag = [&](int k) {
        std::complex<double> acc = 0, w = 1, step = std::polar(1.0, -2.0 * M_PI * k / kN);
        for (int i = 0; i < kN; ++i) {
            acc += y[static_cast<std::size_t>(i)] * w;
            w *= step;
            if ((i & 1023) == 1023) w = std::polar(1.0, -2.0 * M_PI * static_cast<double>(k) * (i + 1) / kN);
        }
        return 2.0 * std::abs(acc) / kN;
    };
    const int toneBin = static_cast<int>(std::lround(tone / 10.0));
    const int steps = aliasHumSteps(n);
    auto hum = [&](int k) { return aliasHumAt(k, toneBin, 2000, steps); };
    const double fund = mag(toneBin);
    double worst = -300;
    worstHz = 0;
    for (int k = 2; k <= 2000; ++k) {
        if (k % toneBin == 0 || hum(k)) continue;
        const double db = 20.0 * std::log10(std::max(mag(k), 1e-15) / std::max(fund, 1e-15));
        if (db > worst) worst = db, worstHz = k * 10.0;
    }
    return worst;
}
double aliasMeasure(const Args& a, const Netlist& n, Circuit& c, double& worstHz, double& secs, int& factor) {
    const double host = 48000.0, tone = aliasTone(), level = std::getenv("CE_ALIAS_LEVEL") ? std::atof(std::getenv("CE_ALIAS_LEVEL")) : 0.5;
    factor = static_cast<int>(std::lround(a.rate / host));
    if (std::getenv("CE_ALIAS_IDEAL") && std::atoi(std::getenv("CE_ALIAS_IDEAL")) > 0) return aliasIdeal(a, n, c, worstHz, secs);
    const int kN = 4800, settle = 72000;
    CircuitOversampler os;
    os.prepare(factor);
    os.reset();
    const double inV = n.io.inputVoltsPerFS, outS = 1.0 / n.io.outputVoltsPerFS;
    std::vector<double> di; // --wav: a 48 kHz DI instead of the tone (null tests)
    if (!a.wav.empty()) {
        double r = host;
        ce_tools::readWav(a.wav, di, r);
    }
    std::vector<double> y;
    const double t0 = threadSecs();
    for (int i = 0; i < settle + kN; ++i) {
        float up[CircuitOversampler::kMaxFactor];
        const double xin = di.empty() ? level * std::sin(2 * M_PI * tone * i / host) : di[static_cast<std::size_t>(i) % di.size()];
        os.up(0, static_cast<float>(xin), up);
        for (int k = 0; k < os.factor(); ++k) up[k] = static_cast<float>(c.process(static_cast<double>(up[k]) * inV) * outS);
        y.push_back(os.down(0, up));
    }
    secs = threadSecs() - t0; // thread CPU time: the machine's load does not inflate it
    if (const char* dump = std::getenv("CE_ALIAS_DUMP")) { // host-rate output, one value per line (null tests)
        std::ofstream f(dump);
        for (double v : y) f << v << "\n";
    }
    std::vector<double> x(static_cast<std::size_t>(kN));
    double mean = 0;
    for (int i = 0; i < kN; ++i) mean += y[static_cast<std::size_t>(settle + i)];
    mean /= kN;
    for (int i = 0; i < kN; ++i) x[static_cast<std::size_t>(i)] = y[static_cast<std::size_t>(settle + i)] - mean;
    auto mag = [&](int k) { // test_aliasing's DFT, operation for operation
        std::complex<double> acc = 0, w = 1, step = std::polar(1.0, -2.0 * M_PI * k / kN);
        for (int i = 0; i < kN; ++i) {
            acc += x[static_cast<std::size_t>(i)] * w;
            w *= step;
            if ((i & 1023) == 1023) w = std::polar(1.0, -2.0 * M_PI * k * (i + 1) / kN);
        }
        return 2.0 * std::abs(acc) / kN;
    };
    const int toneBin = static_cast<int>(std::lround(tone * kN / host));
    const double fund = mag(toneBin);
    std::vector<int> bins;
    for (double fs : {host, 2 * host, 4 * host, 8 * host, 88200.0, 176400.0, 352800.0}) // the gate's bin set (test_aliasing)
        for (int k = 2; k <= 400; ++k)
            for (int m = 1; m <= 16; ++m) {
                const double f = std::abs(k * tone - m * fs);
                if (f < 20.0 || f > 20000.0) continue;
                const double bin = f * kN / host;
                if (std::abs(bin - std::round(bin)) > 1e-6) continue;
                bins.push_back(static_cast<int>(std::lround(bin)));
            }
    std::sort(bins.begin(), bins.end());
    bins.erase(std::unique(bins.begin(), bins.end()), bins.end());
    const int steps = aliasHumSteps(n);
    auto hum = [&](int k) { return aliasHumAt(k, toneBin, kN / 2, steps); };
    double worst = -300;
    worstHz = 0;
    for (int k : bins) {
        if (k % toneBin == 0 || hum(k)) continue;
        const double db = 20.0 * std::log10(std::max(mag(k), 1e-15) / std::max(fund, 1e-15));
        if (db > worst) worst = db, worstHz = k * host / kN;
    }
    return worst;
}
// ───────────────────────────── autocut (docs/CIRCUIT_ENGINE.md §10.27) ─────────────────────────────
// Finds signal cuts that make a netlist cheaper without measurable loss: enumerate the nodes that split a large group
// conductively, rank them by the small-signal impedance seen into the node, then measure each (and greedy
// combinations) against the uncut circuit on the unit's path and keep the set that is cheapest under the rule below.
std::uint64_t instructionsNow() {
#ifdef __APPLE__
    rusage_info_v4 ri{};
    if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&ri)) != 0) return 0;
    return ri.ri_instructions;
#else
    return 0;
#endif
}
double threadSecs() {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0;
    auto ticks = [](FILETIME t) { return (std::uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    return (ticks(kernel) + ticks(user)) * 1e-7;
#else
    timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return t.tv_sec + 1e-9 * t.tv_nsec;
#endif
}
struct CutSpec {
    std::string node;
    std::vector<std::string> loads;
    std::string label() const {
        std::string s = node + ":";
        for (std::size_t i = 0; i < loads.size(); ++i) s += (i ? "," : "") + loads[i];
        return s;
    }
};
nlohmann::json withCuts(const nlohmann::json& j0, const std::vector<CutSpec>& cuts) {
    nlohmann::json j = j0;
    if (!j.contains("cuts")) j["cuts"] = nlohmann::json::array();
    for (const auto& c : cuts) j["cuts"].push_back({{"node", c.node}, {"loads", c.loads}});
    return j;
}
bool parseVariant(const nlohmann::json& j, Netlist& n) {
    std::string err;
    if (!parseNetlistText(j.dump(), n, err)) {
        std::fprintf(stderr, "variant: %s\n", err.c_str());
        return false;
    }
    return true;
}
// Structure-only compile: the DC operating point at the unit's rate and controls, every node exported, no settle.
bool lightCompile(const Args& a, const Netlist& n, Circuit& c, std::string* why = nullptr) {
    CompileOptions o;
    o.sampleRate = a.rate;
    o.exportAllNodes = true;
    o.settleMs = 0;
    std::string err;
    if (!c.compile(n, o, err)) {
        if (why) *why = err;
        return false;
    }
    const auto v = ctlVector(n, a);
    for (std::size_t i = 0; i < v.size(); ++i) c.setControlImmediate(static_cast<int>(i), v[i]);
    if (!c.reset()) {
        if (why) *why = "DC did not converge";
        return false;
    }
    return true;
}
// One run through the unit's path (48 kHz host, the unit's oversampler around the circuit): host samples in, out.
struct UnitRun {
    std::vector<double> y, yR;
    double instr = 0, secs = 0;
    std::uint64_t failures = 0, ptc = 0;
    bool finite = true;
    double maxInstrPerSample = 0; // > 0: give up (slow = true) once a candidate runs this far above budget
    bool slow = false;
};
void unitRender(Circuit& c, const Netlist& n, double rate, CircuitOversampler& os, const std::vector<double>& x, UnitRun& r) {
    const double inV = n.io.inputVoltsPerFS, outS = 1.0 / n.io.outputVoltsPerFS;
    const bool stereo = c.hasOutputR();
    (void)rate;
    const auto f0 = c.diagnostics().failures, p0 = c.diagnostics().ptcRescues;
    const std::uint64_t i0 = instructionsNow();
    const double t0 = threadSecs();
    for (double xin : x) {
        if (r.maxInstrPerSample > 0 && (r.y.size() % 4800) == 4799 &&
            static_cast<double>(instructionsNow() - i0) > r.maxInstrPerSample * static_cast<double>(r.y.size() + 1)) {
            r.slow = true; // a cut that sends Newton into rescues on every sample: stop rather than spend minutes on it
            break;
        }
        float up[CircuitOversampler::kMaxFactor], upR[CircuitOversampler::kMaxFactor];
        os.up(0, static_cast<float>(xin), up);
        for (int k = 0; k < os.factor(); ++k) {
            if (c.hasInputR()) c.setInputR(static_cast<double>(up[k]) * inV);
            const double v = c.process(static_cast<double>(up[k]) * inV);
            up[k] = static_cast<float>(v * outS);
            if (stereo) upR[k] = static_cast<float>(c.outputR() * outS);
            if (!std::isfinite(v)) r.finite = false;
        }
        r.y.push_back(os.down(0, up));
        if (stereo) r.yR.push_back(os.down(1, upR));
    }
    r.secs += threadSecs() - t0;
    r.instr += static_cast<double>(instructionsNow() - i0);
    r.failures += c.diagnostics().failures - f0;
    r.ptc += c.diagnostics().ptcRescues - p0;
}
struct Null {
    double rmsDb = -300, peakDb = -300;
};
Null nullOf(const std::vector<double>& y, const std::vector<double>& y0, std::size_t from = 0, std::size_t to = SIZE_MAX) {
    to = std::min({to, y.size(), y0.size()});
    double e = 0, r = 0, ep = 0, rp = 0;
    for (std::size_t i = from; i < to; ++i) {
        const double d = y[i] - y0[i];
        e += d * d, r += y0[i] * y0[i];
        ep = std::max(ep, std::abs(d)), rp = std::max(rp, std::abs(y0[i]));
    }
    Null nl;
    nl.rmsDb = 10.0 * std::log10(std::max(e, 1e-300) / std::max(r, 1e-300));
    nl.peakDb = 20.0 * std::log10(std::max(ep, 1e-150) / std::max(rp, 1e-150));
    if (!std::isfinite(nl.rmsDb)) nl.rmsDb = 300;
    if (!std::isfinite(nl.peakDb)) nl.peakDb = 300;
    return nl;
}
struct GateFig {
    bool ran = false, pass = false;
    std::map<std::string, double> nrmse; // "case node" -> %
    std::map<std::string, double> ac;    // "case" -> worst dB (below 5 kHz), "case hi" -> 5-10 kHz
};
// The netlist's own SPICE cases (test_circuit_engine with CE_GATE_FILE / CE_GATE_LIBDIR): exactly the gate's figures.
GateFig runGate(const std::string& bin, const std::string& libDir, const std::string& file) {
    GateFig g;
    if (bin.empty()) return g;
#ifdef _WIN32
    const std::string cmd = "set \"CE_GATE_FILE=" + file + "\" && set \"CE_GATE_LIBDIR=" + libDir + "\" && \"" + bin + "\" 2>&1";
    FILE* p = _popen(cmd.c_str(), "r");
#else
    const std::string cmd = "CE_GATE_FILE='" + file + "' CE_GATE_LIBDIR='" + libDir + "' nice -n 10 '" + bin + "' 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
#endif
    if (!p) return g;
    char line[1024];
    bool allPassed = false;
    while (std::fgets(line, sizeof line, p)) {
        std::string s(line);
        if (s.find("all circuit engine checks passed") != std::string::npos) allPassed = true;
        char id[128], node[128];
        double v = 0, v2 = 0;
        if (std::sscanf(line, " %127s %127s NRMSE %lf", id, node, &v) == 3) g.nrmse[std::string(id) + " " + node] = v;
        else if (std::sscanf(line, " %127s AC: worst |Δ| %lf dB (20 Hz-5 kHz), %lf", id, &v, &v2) == 3) {
            g.ac[id] = v;
            g.ac[std::string(id) + " hi"] = v2;
        }
    }
    g.ran = true;
#ifdef _WIN32
    g.pass = _pclose(p) == 0 && allPassed;
#else
    g.pass = pclose(p) == 0 && allPassed;
#endif
    return g;
}
struct Metrics {
    bool built = false;
    std::string why;            // first failed criterion (empty: passes)
    Null di, sine, dimed;       // nulls against the uncut circuit
    Null modeWorst;             // the worst switch / selector position's DI null
    std::string modeWorstAt;
    double idleGrowth = 0;      // last idle window's difference over the smallest earlier one
    double idleLastDb = -300;   // that window, dBFS rms
    std::uint64_t failures = 0, ptc = 0;
    double alias = 0, aliasHz = 0;
    double instrDi = 0, instrTone = 0; // per audio-second, unit path, noise on
    double cpuDi = 0;                  // % of one core (thread CPU), same run
    double idleProbe = 0;              // instructions per internal sample of 50 ms idle from the DC point (light compile)
    double instrNullPerSample = 0;
    double perSine = 0, perIdle = 0, perDim = 0; // the uncut's instructions per host sample of each run     // the noise-off DI run's instructions per host sample (the uncut's budget reference)
    GateFig gate;
    double gateWorst = -1e9;    // largest NRMSE increase over uncut, percentage points
    std::string gateWorstAt;
    double acWorst = -1e9;      // largest AC error increase over uncut, dB
    int maxPorts = 0;           // the largest group's port count
    std::string groups;         // port counts of the groups, largest first
};
struct Signals {
    std::vector<double> di, sine, idle, settleGap, dimSine, cpuDi;
};
std::string portSummary(const Circuit& c, int& maxPorts) {
    std::vector<int> v;
    for (const auto& g : c.groups())
        if (g.nn > 0) v.push_back(g.nn);
    std::sort(v.rbegin(), v.rend());
    maxPorts = v.empty() ? 0 : v.front();
    std::string s;
    for (std::size_t i = 0; i < v.size() && i < 8; ++i) s += (i ? "+" : "") + std::to_string(v[i]);
    if (v.size() > 8) s += "+...";
    return s;
}

int autocut(Args a, const Netlist& n0) {
    const std::string path = a.pos[0];
    const nlohmann::json j0 = nlohmann::json::parse(slurp(path), nullptr, false);
    if (j0.is_discarded()) return 1;
    const std::string file = path.substr(path.find_last_of('/') + 1);
    const std::string libDir = path.find('/') == std::string::npos ? "." : path.substr(0, path.find_last_of('/'));
    unitPath(a, n0, true); // the unit's rate, LTE, controls (--table) and ladder levels
    const double host = 48000.0;
    const int factor = static_cast<int>(std::lround(a.rate / host));
    const char* dimEnv = std::getenv("CE_AUTOCUT_DIMED"); // "0": skip the dimed sine
    const bool doDimed = !(dimEnv && std::string_view(dimEnv) == "0");
    // ── signals (host rate): the DI, a hard sine at the unit's controls, idle (stability), the same sine dimed ──
    Signals sg;
    {
        std::string di = a.wav;
        if (di.empty()) di = libDir + "/../../../samples/di/Lead DI.wav"; // app/samples from app/src/circuit/library
        std::vector<double> w;
        double r = host;
        if (!ce_tools::readWav(di, w, r) || w.size() < 4 * static_cast<std::size_t>(r)) {
            std::fprintf(stderr, "cannot read the DI %s\n", di.c_str());
            return 1;
        }
        const auto off = static_cast<std::size_t>(2.0 * r), len = static_cast<std::size_t>((a.benchSecs > 0 && a.benchSecs < 30 ? a.benchSecs : 6.0) * host);
        for (std::size_t i = 0; i < len; ++i) sg.di.push_back(w[(off + i) % w.size()]); // a 44.1 kHz file plays at 48 kHz
        for (std::size_t i = 0; i < static_cast<std::size_t>(4.0 * host); ++i) sg.cpuDi.push_back(w[(off + 6 * static_cast<std::size_t>(r) + i) % w.size()]);
    }
    const double sineLevel = std::getenv("CE_AUTOCUT_SINE") ? std::atof(std::getenv("CE_AUTOCUT_SINE")) : 0.5;
    for (int i = 0; i < static_cast<int>(host); ++i) sg.sine.push_back(sineLevel * std::sin(2 * M_PI * 220.0 * i / host));
    sg.idle.assign(static_cast<std::size_t>(10 * host), 0.0);
    sg.settleGap.assign(static_cast<std::size_t>(0.5 * host), 0.0);
    sg.dimSine = sg.sine;
    std::vector<std::pair<std::string, double>> dimCtl;
    for (const auto& c : n0.controls)
        if (c.kind == "continuous" && !c.legacyOnly && !c.mod) dimCtl.push_back({c.id, 1.0});

    // ── the measurement of one netlist (uncut or cut) ──
    std::vector<double> refDi, refSine, refIdle, refDim; // the uncut outputs
    // Every other position of each panel switch and selector (one at a time, the rest at the unit's settings): a cut that
    // is stiff in one mode can be a live node in another (a relay that un-bypasses a cathode). 2 s of the DI each.
    std::vector<std::pair<std::string, double>> modes;
    for (const auto& c : n0.controls) {
        if (c.legacyOnly || c.mod || (c.kind != "switch" && c.kind != "selector")) continue;
        const int k = c.kind == "switch" ? 2 : std::max<int>(2, static_cast<int>(c.options.size()));
        double cur = c.def;
        for (const auto& [id, v] : a.ctl)
            if (id == c.id) cur = v;
        for (int i = 0; i < k; ++i) {
            const double v = static_cast<double>(i) / (k - 1);
            if (std::abs(v - cur) > 1e-6) modes.push_back({c.id, v});
        }
    }
    const std::vector<double> modeDi(sg.di.begin(), sg.di.begin() + static_cast<long>(std::min<std::size_t>(sg.di.size(), 2 * static_cast<std::size_t>(host))));
    std::vector<std::vector<double>> refMode(modes.size());
    std::vector<std::uint64_t> refModeFail(modes.size(), 0);
    bool modesBaseDone = false;
    Metrics base;
    auto measure = [&](const Netlist& n, bool isBase, const std::string& gateDir) {
        Metrics m;
        Circuit lc;
        if (!lightCompile(a, n, lc, &m.why)) return m;
        m.groups = portSummary(lc, m.maxPorts);
        m.built = true;
        // idle work probe (no settle): a cut that leaves Newton in rescues on every sample would otherwise spend
        // minutes in the unit compile's mains settle, which nothing can interrupt
        {
            const std::uint64_t p0 = lc.diagnostics().ptcRescues, i0 = instructionsNow();
            const int N = static_cast<int>(a.rate / 20.0);
            int done = 0;
            for (; done < N; ++done) {
                lc.process(0.0);
                if (!isBase && (done & 255) == 255 &&
                    (lc.diagnostics().ptcRescues - p0 > static_cast<std::uint64_t>(N / 10) ||
                     static_cast<double>(instructionsNow() - i0) > 8.0 * base.idleProbe * (done + 1)))
                    break; // already hopeless
            }
            m.idleProbe = static_cast<double>(instructionsNow() - i0) / (done + (done < N ? 1 : 0));
            const std::uint64_t ptc = lc.diagnostics().ptcRescues - p0;
            if (!isBase && (m.idleProbe > 8.0 * base.idleProbe || ptc > static_cast<std::uint64_t>(N / 10))) {
                m.why = "idle work " + std::to_string(static_cast<int>(m.idleProbe / std::max(base.idleProbe, 1.0))) + "x the uncut's, " +
                        std::to_string(ptc) + " PTC rescues (Newton trouble)";
                return m;
            }
        }
        auto fail = [&](const std::string& w) {
            if (m.why.empty()) m.why = w;
            return !a.full;
        };
        // noise off (the null compares the circuits, not two noise realisations); hum stays as in the unit
        rig::platform::setEnv("CE_NOISE", "0");
        Circuit c;
        Args aa = a;
        if (!build(aa, n, c)) {
            m.why = "compile";
            return m;
        }
        rig::platform::unsetEnv("CE_NOISE");
        CircuitOversampler os;
        os.prepare(factor);
        os.reset();
        UnitRun rd;
        if (!isBase) rd.maxInstrPerSample = 4.0 * base.instrNullPerSample;
        unitRender(c, n, a.rate, os, sg.di, rd);
        m.failures += rd.failures, m.ptc += rd.ptc;
        if (rd.slow) {
            m.why = "over 4x the uncut's work (Newton trouble)";
            return m;
        }
        if (isBase) refDi = rd.y, m.instrNullPerSample = rd.instr / static_cast<double>(std::max<std::size_t>(rd.y.size(), 1));
        else {
            m.di = nullOf(rd.y, refDi);
            if (!rd.finite && fail("output not finite")) return m;
            if (m.di.rmsDb > -60.0 && fail("DI null")) return m;
            if (m.di.peakDb > -40.0 && fail("DI null peak")) return m;
        }
        // each run's budget: 8x the uncut's own work per sample on the same signal (the DI's for the switch positions)
        auto budgetOf = [&](double perSample) { return isBase ? 0.0 : 8.0 * std::max(perSample, base.instrNullPerSample); };
        const double budget = budgetOf(base.instrNullPerSample);
        auto tooSlow = [&](const UnitRun& r) {
            if (r.slow) m.why = "over 8x the uncut's work on the same signal (Newton trouble)";
            return r.slow;
        };
        UnitRun rs;
        rs.maxInstrPerSample = budgetOf(base.perSine);
        unitRender(c, n, a.rate, os, sg.sine, rs);
        if (tooSlow(rs)) return m;
        UnitRun ri;
        ri.maxInstrPerSample = budgetOf(base.perIdle);
        unitRender(c, n, a.rate, os, sg.idle, ri);
        if (tooSlow(ri)) return m;
        m.failures += rs.failures + ri.failures, m.ptc += rs.ptc + ri.ptc;
        if (isBase) {
            refSine = rs.y, refIdle = ri.y;
            m.perSine = rs.instr / static_cast<double>(std::max<std::size_t>(rs.y.size(), 1));
            m.perIdle = ri.instr / static_cast<double>(std::max<std::size_t>(ri.y.size(), 1));
        }
        else {
            m.sine = nullOf(rs.y, refSine);
            if (m.sine.rmsDb > -50.0 && fail("sine null")) return m;
            // stability: the difference from the uncut circuit, 1 s windows of the idle tail, must not grow
            const auto W = static_cast<std::size_t>(host);
            std::vector<double> win;
            for (std::size_t w0 = 0; w0 + W <= ri.y.size(); w0 += W) {
                double e = 0;
                for (std::size_t i = w0; i < w0 + W; ++i) e += (ri.y[i] - refIdle[i]) * (ri.y[i] - refIdle[i]);
                win.push_back(std::sqrt(e / static_cast<double>(W)));
            }
            const double last = win.back();
            double lo = 1e300;
            for (std::size_t i = 0; i + 1 < win.size(); ++i) lo = std::min(lo, win[i]);
            m.idleLastDb = 20.0 * std::log10(std::max(last, 1e-15));
            m.idleGrowth = last / std::max(lo, 1e-15);
            if (!ri.finite && fail("idle output not finite")) return m;
            if (last > 1e-7 && m.idleGrowth > 1.5 && fail("idle difference grows")) return m; // floor: -140 dBFS
        }
        if (doDimed) {
            for (const auto& [id, v] : dimCtl) c.setControlImmediate(n.controlIndex(id), v);
            UnitRun rg, rdm;
            rg.maxInstrPerSample = rdm.maxInstrPerSample = budgetOf(base.perDim);
            unitRender(c, n, a.rate, os, sg.settleGap, rg);
            unitRender(c, n, a.rate, os, sg.dimSine, rdm);
            if (tooSlow(rg) || tooSlow(rdm)) return m;
            m.failures += rg.failures + rdm.failures, m.ptc += rg.ptc + rdm.ptc;
            if (isBase) refDim = rdm.y, m.perDim = (rg.instr + rdm.instr) / static_cast<double>(std::max<std::size_t>(rg.y.size() + rdm.y.size(), 1));
            else {
                m.dimed = nullOf(rdm.y, refDim);
                if (m.dimed.rmsDb > -50.0 && fail("dimed sine null")) return m;
            }
        }
        if (!isBase && (m.failures > base.failures || m.ptc > base.ptc) && fail("new Newton failures")) return m;
        // CPU and aliasing: the unit as it runs (noise on), the aliasing gate's tone, then a DI
        {
            Circuit cn;
            if (!build(aa, n, cn)) {
                m.why = "compile";
                return m;
            }
            double secs = 0;
            int f = 1;
            aa.wav.clear(); // aliasMeasure plays a.wav instead of its tone when set
            const std::uint64_t i0 = instructionsNow();
            m.alias = aliasMeasure(aa, n, cn, m.aliasHz, secs, f);
            m.instrTone = static_cast<double>(instructionsNow() - i0) / (76800.0 / host);
            CircuitOversampler os2;
            os2.prepare(factor);
            os2.reset();
            UnitRun rc;
            unitRender(cn, n, a.rate, os2, sg.cpuDi, rc);
            m.instrDi = rc.instr / (static_cast<double>(sg.cpuDi.size()) / host);
            m.cpuDi = 100.0 * rc.secs / (static_cast<double>(sg.cpuDi.size()) / host);
        }
        if (!isBase) {
            const bool aliasOk = m.alias <= base.alias + 0.5 || m.alias <= -70.0;
            if (!aliasOk && fail("aliasing")) return m;
            if (m.instrDi > 0.97 * base.instrDi && fail("saves < 3 % CPU")) return m;
        }
        if (!a.gate.empty() && !isBase) {
            if (!base.gate.ran) base.gate = runGate(a.gate, libDir, file); // the uncut figures, on first use
            m.gate = runGate(a.gate, gateDir, file);
            {
                for (const auto& [k, v] : m.gate.nrmse) {
                    const auto it = base.gate.nrmse.find(k);
                    const double d = it == base.gate.nrmse.end() ? 1e9 : v - it->second;
                    if (d > m.gateWorst) m.gateWorst = d, m.gateWorstAt = k;
                }
                for (const auto& [k, v] : m.gate.ac) {
                    const auto it = base.gate.ac.find(k);
                    m.acWorst = std::max(m.acWorst, it == base.gate.ac.end() ? 1e9 : v - it->second);
                }
                if (!m.gate.pass && fail("SPICE gate fails")) return m;
                if (m.gate.nrmse.size() != base.gate.nrmse.size() && fail("SPICE gate cases differ")) return m;
                if (m.gateWorst > 0.01 && fail("SPICE NRMSE +" + std::to_string(m.gateWorst) + " pp at " + m.gateWorstAt)) return m;
                if (m.acWorst > 0.01 && fail("SPICE AC error grows")) return m;
            }
        }
        // every other switch / selector position (last: a compile per position). The uncut references are made on first use.
        auto modeRun = [&](const Netlist& nm, std::size_t mi, UnitRun& rm) {
            Args am = a;
            bool set = false;
            for (auto& [id, v] : am.ctl)
                if (id == modes[mi].first) v = modes[mi].second, set = true;
            if (!set) am.ctl.push_back(modes[mi]);
            rig::platform::setEnv("CE_NOISE", "0");
            Circuit cm;
            const bool ok = build(am, nm, cm);
            rig::platform::unsetEnv("CE_NOISE");
            if (!ok) return false;
            CircuitOversampler osm;
            osm.prepare(factor);
            osm.reset();
            unitRender(cm, nm, a.rate, osm, modeDi, rm);
            return true;
        };
        if (!isBase && !modesBaseDone) {
            for (std::size_t mi = 0; mi < modes.size(); ++mi) {
                UnitRun rm;
                modeRun(n0, mi, rm);
                refMode[mi] = rm.y;
                refModeFail[mi] = rm.failures + rm.ptc;
            }
            modesBaseDone = true;
        }
        for (std::size_t mi = 0; mi < modes.size() && !isBase; ++mi) {
            UnitRun rm;
            rm.maxInstrPerSample = budget;
            if (!modeRun(n, mi, rm)) {
                m.why = "compile (mode)";
                return m;
            }
            if (tooSlow(rm)) return m;
            const Null nm = nullOf(rm.y, refMode[mi]);
            if (nm.rmsDb > m.modeWorst.rmsDb) m.modeWorst = nm, m.modeWorstAt = modes[mi].first + "=" + std::to_string(modes[mi].second).substr(0, 4);
            if ((nm.rmsDb > -60.0 || nm.peakDb > -40.0 || !rm.finite || rm.failures + rm.ptc > refModeFail[mi]) && fail("DI null at " + m.modeWorstAt)) return m;
        }
        return m;
    };

    // ── 1. the uncut circuit ──
    Circuit c0;
    {
        std::string why;
        if (!lightCompile(a, n0, c0, &why)) {
            std::fprintf(stderr, "%s: %s\n", n0.id.c_str(), why.c_str());
            return 1;
        }
    }
    std::printf("autocut %s (%s): unit path x%d (%.0f Hz), LTE tol %s, %zu groups\n", n0.id.c_str(), file.c_str(), factor, a.rate,
                std::getenv("CE_LTE") ? std::getenv("CE_LTE") : "-", c0.groups().size());

    // ── 2. candidates: nodes whose removal splits a large group's parts into conductively separate sides ──
    std::set<std::string> cutNames(n0.cuts.begin(), n0.cuts.end());
    struct Cand {
        CutSpec cut;
        int group = 0, ports = 0;
        std::string split; // port counts after
        double z[3] = {0, 0, 0};
        double zmax = 0;
        bool loop = false;
        Metrics m;
    };
    std::vector<Cand> cands;
    auto structural = [&](const std::vector<CutSpec>& set, std::string& split, bool& loop, std::string& why, const Group* orig) {
        Netlist nv;
        if (!parseVariant(withCuts(j0, set), nv)) {
            why = "parse";
            return false;
        }
        Circuit cv;
        if (!lightCompile(a, nv, cv, &why)) return false;
        int mp = 0;
        split = portSummary(cv, mp);
        loop = false;
        const auto& G = cv.groups();
        // group of each node / part
        std::vector<int> gOfNode(cv.nodeNames().size(), -1);
        for (std::size_t gi = 0; gi < G.size(); ++gi)
            for (int u : G[gi].unknowns) gOfNode[static_cast<std::size_t>(u)] = static_cast<int>(gi);
        std::map<int, int> gOfPart;
        for (std::size_t gi = 0; gi < G.size(); ++gi)
            for (int p : G[gi].parts) gOfPart.emplace(p, static_cast<int>(gi));
        // upstream closure: X reads a node solved by Y
        std::vector<std::set<int>> up(G.size());
        for (std::size_t gi = 0; gi < G.size(); ++gi)
            for (int in : G[gi].inputs)
                if (gOfNode[static_cast<std::size_t>(in)] >= 0 && gOfNode[static_cast<std::size_t>(in)] != static_cast<int>(gi)) up[gi].insert(gOfNode[static_cast<std::size_t>(in)]);
        auto upstream = [&](int g0) {
            std::set<int> seen;
            std::vector<int> st = {g0};
            while (!st.empty()) {
                const int g = st.back();
                st.pop_back();
                for (int u : up[static_cast<std::size_t>(g)])
                    if (seen.insert(u).second) st.push_back(u);
            }
            return seen;
        };
        for (const auto& c : set) {
            const int id = cv.node(c.node);
            if (id <= 0 || gOfNode[static_cast<std::size_t>(id)] < 0) {
                why = "cut node not solved";
                return false;
            }
            const int owner = gOfNode[static_cast<std::size_t>(id)];
            const auto ups = upstream(owner);
            std::set<int> loadGroups;
            for (const auto& l : c.loads)
                for (std::size_t pi = 0; pi < nv.parts.size(); ++pi)
                    if (nv.parts[pi].ref == l && gOfPart.count(static_cast<int>(pi))) loadGroups.insert(gOfPart[static_cast<int>(pi)]);
            loadGroups.erase(owner);
            if (loadGroups.empty()) {
                why = "loads stay in the owner's group";
                return false;
            }
            for (int lg : loadGroups)
                if (ups.count(lg)) loop = true; // the owner depends on its own loads: the lag sits inside a feedback loop
        }
        if (orig) { // the original group must split into smaller Newton problems (moving linear parts out saves little)
            std::set<int> parts(orig->parts.begin(), orig->parts.end()), gs;
            for (int p : parts)
                if (gOfPart.count(p)) gs.insert(gOfPart[p]);
            int largest = 0;
            for (int g : gs) largest = std::max(largest, G[static_cast<std::size_t>(g)].nn);
            if (gs.size() < 2 || largest > orig->nn - 2) {
                why = "group does not split";
                return false;
            }
        }
        return true;
    };

    if (a.tries.empty()) {
        for (std::size_t gi = 0; gi < c0.groups().size(); ++gi) {
            const auto& g = c0.groups()[gi];
            if (g.nn < a.minPorts) continue;
            std::set<int> unk(g.unknowns.begin(), g.unknowns.end());
            std::vector<int> parts(g.parts.begin(), g.parts.end());
            std::sort(parts.begin(), parts.end());
            parts.erase(std::unique(parts.begin(), parts.end()), parts.end());
            // the group's named nodes
            std::set<std::string> nodes;
            for (int p : parts)
                for (const auto& nm : n0.parts[static_cast<std::size_t>(p)].nodes) {
                    const int id = c0.node(nm);
                    // subcircuit internals ("SPK.x1") are left alone: their part refs are not the netlist's own
                    if (id > 0 && unk.count(id) && !cutNames.count(nm) && !n0.cutLoads.count(nm) && nm != n0.io.output && nm.find('.') == std::string::npos)
                        nodes.insert(nm);
                }
            int tried = 0, kept = 0;
            for (const auto& x : nodes) {
                // union-find over the group's parts along shared solved nodes other than x
                std::map<int, int> uf;
                for (int p : parts) uf[p] = p;
                std::function<int(int)> find = [&](int v) { return uf[v] == v ? v : uf[v] = find(uf[v]); };
                std::map<std::string, int> firstAt;
                for (int p : parts)
                    for (const auto& nm : n0.parts[static_cast<std::size_t>(p)].nodes) {
                        if (nm == x || cutNames.count(nm)) continue;
                        const int id = c0.node(nm);
                        if (id <= 0 || !unk.count(id)) continue;
                        auto it = firstAt.find(nm);
                        if (it == firstAt.end()) firstAt[nm] = p;
                        else uf[find(p)] = find(it->second);
                    }
                // sides: components of the parts touching x; a part whose other pins are all rails/ground stays with the owner
                std::map<int, std::vector<std::string>> sides;
                for (int p : parts) {
                    const auto& pn = n0.parts[static_cast<std::size_t>(p)].nodes;
                    if (std::find(pn.begin(), pn.end(), x) == pn.end()) continue;
                    bool shunt = true;
                    for (const auto& nm : pn) {
                        const int id = c0.node(nm);
                        if (nm != x && id > 0 && unk.count(id) && !cutNames.count(nm)) shunt = false;
                    }
                    if (shunt) continue;
                    sides[find(p)].push_back(n0.parts[static_cast<std::size_t>(p)].ref);
                }
                if (sides.size() < 2) continue;
                std::vector<std::vector<std::string>> opts;
                for (const auto& [root, refs] : sides) {
                    opts.push_back(refs); // these parts as the loads (the rest owns x)
                    if (sides.size() > 2) {
                        std::vector<std::string> rest;
                        for (const auto& [r2, refs2] : sides)
                            if (r2 != root) rest.insert(rest.end(), refs2.begin(), refs2.end());
                        opts.push_back(rest); // ...or everything but these
                    }
                }
                // The owner must keep the device that drives x: an option whose loads hold an active device on an output
                // terminal at x (plate, cathode, collector, emitter, drain, source, op-amp output) is dropped when another
                // option has none (a cathode follower's own cathode as a lagged load is never the cheap side).
                auto drivesX = [&](const std::vector<std::string>& refs) {
                    int k = 0;
                    for (const auto& r : refs) {
                        const Part* pp = n0.part(r);
                        if (!pp) continue;
                        int ctl1 = -1, ctl2 = -1; // control (input) pins
                        if (pp->type == "triode" || pp->type == "npn" || pp->type == "pnp" || pp->type == "njf" || pp->type == "pjf") ctl1 = 1;
                        else if (pp->type == "pentode") ctl1 = 2;
                        else if (pp->type == "opamp") ctl1 = 0, ctl2 = 1;
                        else continue;
                        for (std::size_t q = 0; q < pp->nodes.size(); ++q)
                            if (pp->nodes[q] == x && static_cast<int>(q) != ctl1 && static_cast<int>(q) != ctl2 && !(pp->type == "opamp" && q >= 3)) ++k;
                    }
                    return k;
                };
                bool anyClean = false;
                for (const auto& o : opts) anyClean |= drivesX(o) == 0;
                if (anyClean) opts.erase(std::remove_if(opts.begin(), opts.end(), [&](const auto& o) { return drivesX(o) > 0; }), opts.end());
                for (auto& loads : opts) {
                    std::sort(loads.begin(), loads.end());
                    Cand cd;
                    cd.cut = {x, loads};
                    cd.group = static_cast<int>(gi);
                    cd.ports = g.nn;
                    std::string why;
                    ++tried;
                    if (!structural({cd.cut}, cd.split, cd.loop, why, &g)) continue;
                    ++kept;
                    cands.push_back(cd);
                }
            }
            std::printf("group %zu (%d ports, %zu unknowns): %zu nodes, %d cut options compiled, %d split it\n", gi, g.nn, g.unknowns.size(), nodes.size(), tried, kept);
        }
    } else {
        for (const auto& t : a.tries) {
            std::istringstream s(t);
            std::string one;
            while (std::getline(s, one, ';')) {
                Cand cd;
                const auto colon = one.find(':');
                cd.cut.node = one.substr(0, colon);
                std::istringstream ls(colon == std::string::npos ? "" : one.substr(colon + 1));
                std::string l;
                while (std::getline(ls, l, ',')) cd.cut.loads.push_back(l);
                std::string why;
                if (!structural({cd.cut}, cd.split, cd.loop, why, nullptr)) {
                    std::printf("--try %s: %s\n", cd.cut.label().c_str(), why.c_str());
                    continue;
                }
                cands.push_back(cd);
            }
        }
    }

    // ── 3. rank by the impedance seen into the node: the engine's own small-signal response at the operating point,
    //       with and without a probe current gm·v(in) injected there (Z = ΔH / gm) ──
    const std::vector<double> zf = {100.0, 1000.0, 10000.0};
    std::map<std::string, std::array<double, 3>> zOf;
    for (auto& cd : cands) {
        if (!zOf.count(cd.cut.node)) {
            std::array<double, 3> z{-1, -1, -1};
            nlohmann::json jz = j0;
            const double gm = 1e-3;
            jz["parts"].push_back({{"ref", "AUTOCUT_ZPROBE"}, {"type", "vccs"}, {"nodes", {cd.cut.node, "0", n0.io.input, "0"}}, {"gain", gm}});
            Netlist nz;
            Circuit cz;
            if (parseVariant(jz, nz) && lightCompile(a, nz, cz)) {
                const auto h0 = c0.smallSignalResponse(zf, c0.node(cd.cut.node));
                const auto h1 = cz.smallSignalResponse(zf, cz.node(cd.cut.node));
                for (std::size_t i = 0; i < 3; ++i) z[i] = std::abs(h1[i] - h0[i]) / gm;
            }
            zOf[cd.cut.node] = z;
        }
        const auto& z = zOf[cd.cut.node];
        for (int i = 0; i < 3; ++i) cd.z[i] = z[static_cast<std::size_t>(i)];
        cd.zmax = std::max({z[0], z[1], z[2]});
        if (cd.zmax < 0) cd.zmax = 1e30;
    }
    std::stable_sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) {
        if (x.loop != y.loop) return !x.loop;
        return x.zmax < y.zmax;
    });
    std::printf("\ncandidates (lowest impedance first; loop = the owner depends on its loads, i.e. inside a feedback loop):\n");
    std::printf("| # | cut node : loads | group ports -> after | |Z| 100 Hz / 1 kHz / 10 kHz (ohm) | loop |\n|---|---|---|---|---|\n");
    for (std::size_t i = 0; i < cands.size(); ++i) {
        const auto& cd = cands[i];
        std::printf("| %zu | %s | %d -> %s | %.3g / %.3g / %.3g | %s |\n", i + 1, cd.cut.label().c_str(), cd.ports, cd.split.c_str(), cd.z[0], cd.z[1], cd.z[2],
                    cd.loop ? "yes" : "");
    }
    std::fflush(stdout);
    if (cands.empty() || a.top <= 0) {
        std::printf("\nselected for %s: no cut (%s)\n", n0.id.c_str(), cands.empty() ? "no candidate splits a group" : "--top 0: candidates only");
        return 0;
    }

    // ── 4. measure: the uncut circuit, then each candidate (top N outside loops), then greedy combinations ──
    const std::string scratch = std::string(std::getenv("CE_AUTOCUT_TMP") ? std::getenv("CE_AUTOCUT_TMP") : "/tmp/autocut") + "/" + n0.id;
    std::system(("mkdir -p '" + scratch + "'").c_str());
    auto writeVariant = [&](const std::vector<CutSpec>& set) {
        std::ofstream(scratch + "/" + file) << withCuts(j0, set).dump(1);
        return scratch;
    };
    base = measure(n0, true, libDir);
    if (!base.built) {
        std::fprintf(stderr, "uncut circuit: %s\n", base.why.c_str());
        return 1;
    }
    std::printf("\nuncut: groups %s; %.2f G instr per audio-second on the DI (%.1f %% of one core), gate tone %.2f G; alias %.1f dB; Newton failures %llu, PTC %llu",
                base.groups.c_str(), base.instrDi / 1e9, base.cpuDi, base.instrTone / 1e9, base.alias, static_cast<unsigned long long>(base.failures),
                static_cast<unsigned long long>(base.ptc));
    if (base.gate.ran) std::printf("; SPICE cases %zu NRMSE + %zu AC, %s", base.gate.nrmse.size(), base.gate.ac.size() / 2, base.gate.pass ? "pass" : "FAIL");
    std::printf("\n");
    auto row = [&](const std::string& what, const Metrics& m) {
        auto f1 = [](double v) { // unmeasured stages print as "-"
            char b[32];
            if (v <= -299.0) return std::string("-");
            std::snprintf(b, sizeof b, "%.1f", v);
            return std::string(b);
        };
        char cpu[160] = "- | -", al[48] = "-", gt[160] = "not run", idle[64] = "-";
        if (m.instrDi > 0)
            std::snprintf(cpu, sizeof cpu, "%.2f (%+.1f %%) | %.2f (%+.1f %%)", m.instrDi / 1e9, 100.0 * (m.instrDi / base.instrDi - 1.0), m.instrTone / 1e9,
                          100.0 * (m.instrTone / base.instrTone - 1.0));
        if (m.instrDi > 0) std::snprintf(al, sizeof al, "%.1f (%+.1f)", m.alias, m.alias - base.alias);
        if (m.gate.ran) std::snprintf(gt, sizeof gt, "%+.3f pp (%s)%s", m.gateWorst, m.gateWorstAt.c_str(), m.gate.pass ? "" : ", FAILS");
        if (m.idleLastDb > -299.0) std::snprintf(idle, sizeof idle, "%.1f / x%.2f", m.idleLastDb, m.idleGrowth);
        std::printf("| %s | %s | %s / %s | %s / %s / %s | %s | %llu / %llu | %s | %s | %s | %s |\n", what.c_str(), m.groups.c_str(), f1(m.di.rmsDb).c_str(), f1(m.di.peakDb).c_str(),
                    f1(m.sine.rmsDb).c_str(), f1(m.dimed.rmsDb).c_str(), f1(m.modeWorst.rmsDb).c_str(), idle, static_cast<unsigned long long>(m.failures), static_cast<unsigned long long>(m.ptc), cpu, al, gt,
                    m.why.empty() ? "**pass**" : m.why.c_str());
        std::fflush(stdout);
    };
    std::printf("\n| cut set | groups (ports) | DI null rms / peak dB | sine / dimed / worst-mode null dB | idle diff dBFS / growth | Newton fail / PTC | DI G instr/s | tone G instr/s | alias dB | SPICE worst ΔNRMSE | verdict |\n");
    std::printf("|---|---|---|---|---|---|---|---|---|---|---|\n");
    std::vector<std::size_t> passing;
    int measured = 0;
    for (std::size_t i = 0; i < cands.size(); ++i) {
        auto& cd = cands[i];
        if (cd.loop && a.tries.empty()) continue; // the rule: never inside a feedback loop (--try measures it anyway)
        if (measured >= a.top) break;
        ++measured;
        Netlist nv;
        parseVariant(withCuts(j0, {cd.cut}), nv);
        cd.m = measure(nv, false, writeVariant({cd.cut}));
        if (cd.loop && cd.m.why.empty()) cd.m.why = "inside a feedback loop (measured only)";
        row(cd.cut.label(), cd.m);
        if (cd.m.why.empty()) passing.push_back(i);
    }
    // greedy: the biggest single saving first, then add each further passing cut if the whole set still passes and saves more
    std::sort(passing.begin(), passing.end(), [&](std::size_t x, std::size_t y) { return cands[x].m.instrDi < cands[y].m.instrDi; });
    std::vector<CutSpec> chosen;
    Metrics best = base;
    for (std::size_t k = 0; k < passing.size(); ++k) {
        const auto& cd = cands[passing[k]];
        if (std::any_of(chosen.begin(), chosen.end(), [&](const CutSpec& c) { return c.node == cd.cut.node; })) continue;
        auto set = chosen;
        set.push_back(cd.cut);
        Metrics m;
        if (chosen.empty()) m = cd.m;
        else {
            std::string split, why;
            bool loop = false;
            if (!structural(set, split, loop, why, nullptr) || loop) continue;
            Netlist nv;
            parseVariant(withCuts(j0, set), nv);
            m = measure(nv, false, writeVariant(set));
            std::string lbl;
            for (const auto& c : set) lbl += (lbl.empty() ? "" : " + ") + c.label();
            row(lbl, m);
            if (!m.why.empty() || m.instrDi > 0.98 * best.instrDi) continue;
        }
        chosen = set;
        best = m;
    }
    // ── 5. result ──
    std::printf("\nselected for %s: ", n0.id.c_str());
    if (chosen.empty()) std::printf("no cut (none passes the rule)\n");
    else {
        nlohmann::json js = nlohmann::json::array();
        for (const auto& c : chosen) js.push_back({{"node", c.node}, {"loads", c.loads}});
        std::printf("%s\n", js.dump().c_str());
        std::printf("summary | %s | %s -> %s | DI %.2f -> %.2f G instr/s (%+.1f %%) | tone %.2f -> %.2f G (%+.1f %%) | DI null %.1f dB rms / %.1f peak | sine %.1f / dimed %.1f | worst mode %.1f (%s) | alias %.1f -> %.1f dB | SPICE worst %+.4f pp |\n",
                    n0.id.c_str(), base.groups.c_str(), best.groups.c_str(), base.instrDi / 1e9, best.instrDi / 1e9, 100.0 * (best.instrDi / base.instrDi - 1.0),
                    base.instrTone / 1e9, best.instrTone / 1e9, 100.0 * (best.instrTone / base.instrTone - 1.0), best.di.rmsDb, best.di.peakDb, best.sine.rmsDb,
                    best.dimed.rmsDb, best.modeWorst.rmsDb, best.modeWorstAt.c_str(), base.alias, best.alias, best.gateWorst > -1e8 ? best.gateWorst : 0.0);
    }
    return 0;
}
// ───────────────────────────── bench --real (GR_LESSONS.md §4 P0) ─────────────────────────────
// 48 kHz mono DI excerpts and the same excerpts through a deterministic fuzz pre-stage.
// CE_REAL=synthetic selects the legacy DI and synthetic hard suite. Timings use each
// block's minimum over CE_REALREPS runs; the combined p99 is the budget worst case.
struct RealMaterial {
    std::string name;
    std::vector<double> x;                                    // host rate, FS
    std::vector<std::pair<std::string, std::size_t>> segs;   // name, first host sample
};
std::vector<RealMaterial> realMaterials(const Args& a) {
    const double host = 48000.0;
    std::vector<RealMaterial> m;
    const char* sel = std::getenv("CE_REAL");
    const bool synthetic = sel && std::string_view(sel) == "synthetic";
    if (!synthetic) {
        RealMaterial real{"real", {}, {}}, driven{"driven", {}, {}};
        const std::string p = a.pos[0];
        const std::string dir = p.find('/') == std::string::npos ? "." : p.substr(0, p.find_last_of('/'));
        for (const char* file : {"Lead DI.wav", "Rock DI.wav", "Metal DI.wav", "Clean Strumming.wav"}) {
            const std::string path = dir + "/../../../samples/di/" + file;
            std::vector<double> w;
            double rate = host;
            if (!ce_tools::readWav(path, w, rate) || w.empty() || rate <= 0) {
                std::fprintf(stderr, "cannot read DI %s\n", path.c_str());
                return {};
            }
            const std::size_t frames = static_cast<std::size_t>(w.size() * host / rate);
            std::vector<float> pcm(frames);
            float peak = 0;
            for (std::size_t i = 0; i < frames; ++i) {
                const double src = i * rate / host;
                const auto i0 = std::min(static_cast<std::size_t>(src), w.size() - 1), i1 = std::min(i0 + 1, w.size() - 1);
                const float a0 = static_cast<float>(w[i0]), a1 = static_cast<float>(w[i1]);
                pcm[i] = a0 + static_cast<float>(src - i0) * (a1 - a0);
                peak = std::max(peak, std::abs(pcm[i]));
            }
            const float gain = peak > 1e-4f ? 0.35f / peak : 1.0f;
            const std::size_t off = std::min(static_cast<std::size_t>(2 * host), frames);
            const std::size_t end = std::min(off + static_cast<std::size_t>(6 * host), frames);
            if (end == off) { std::fprintf(stderr, "DI too short: %s\n", path.c_str()); return {}; }
            real.segs.push_back({file, real.x.size()});
            driven.segs.push_back({file, driven.x.size()});
            double lp = 0;
            const double alpha = 1.0 - std::exp(-2.0 * M_PI * 6000.0 / host);
            for (std::size_t i = off; i < end; ++i) {
                const double x = std::lround(std::clamp(pcm[i] * gain, -1.0f, 1.0f) * 32767.0f) / 32767.0;
                real.x.push_back(x);
                lp += alpha * (std::tanh(12.0 * x) - lp);
                driven.x.push_back(lp);
            }
        }
        m.push_back(std::move(real));
        m.push_back(std::move(driven));
    }
    if (synthetic) {
        RealMaterial di{"di", {}, {}};
        std::string path = a.wav;
        if (path.empty()) {
            const std::string p = a.pos[0];
            const std::string dir = p.find('/') == std::string::npos ? "." : p.substr(0, p.find_last_of('/'));
            path = dir + "/../../../samples/di/Lead DI.wav"; // app/samples from app/src/circuit/library
        }
        std::vector<double> w;
        double r = host;
        if (!ce_tools::readWav(path, w, r) || w.size() < 9 * static_cast<std::size_t>(r)) std::fprintf(stderr, "cannot read the DI %s\n", path.c_str());
        else {
            const auto off = static_cast<std::size_t>(2.0 * r), len = static_cast<std::size_t>(6.0 * host);
            for (std::size_t i = 0; i < len; ++i) di.x.push_back(w[off + i]);
            di.segs.push_back({"di", 0});
            m.push_back(std::move(di));
        }
    }
    if (synthetic) {
        RealMaterial h{"hard", {}, {}};
        auto fadeAdd = [&](const std::string& name, std::size_t n, double fadeMs, const std::function<double(std::size_t)>& f) {
            h.segs.push_back({name, h.x.size()});
            const std::size_t nf = static_cast<std::size_t>(fadeMs * 1e-3 * host);
            for (std::size_t i = 0; i < n; ++i) {
                double g = 1;
                if (nf > 0 && i < nf) g = 0.5 - 0.5 * std::cos(M_PI * static_cast<double>(i) / static_cast<double>(nf));
                if (nf > 0 && n - 1 - i < nf) g = 0.5 - 0.5 * std::cos(M_PI * static_cast<double>(n - 1 - i) / static_cast<double>(nf));
                h.x.push_back(g * f(i));
            }
        };
        const double m6 = std::pow(10.0, -6.0 / 20.0), m12 = std::pow(10.0, -12.0 / 20.0);
        for (double f : {2500.0, 3183.0, 4999.0, 7001.0, 9973.0, 14013.0})
            fadeAdd("hf" + std::to_string(static_cast<int>(f)), static_cast<std::size_t>(0.5 * host), 5.0,
                    [&](std::size_t i) { return m6 * std::sin(2 * M_PI * f * static_cast<double>(i) / host); });
        const double T = 2.0;
        fadeAdd("sweep", static_cast<std::size_t>(T * host), 5.0, [&](std::size_t i) {
            const double t = static_cast<double>(i) / host;
            return m12 * std::sin(2 * M_PI * (20000.0 / (2 * T)) * t * t);
        });
        fadeAdd("burst82", static_cast<std::size_t>(1.0 * host), 2.0, [&](std::size_t i) { return std::sin(2 * M_PI * 82.4 * static_cast<double>(i) / host); });
        fadeAdd("gap1", static_cast<std::size_t>(0.25 * host), 0.0, [](std::size_t) { return 0.0; });
        fadeAdd("burst110", static_cast<std::size_t>(0.5 * host), 2.0, [&](std::size_t i) { return std::sin(2 * M_PI * 110.0 * static_cast<double>(i) / host); });
        fadeAdd("gap2", static_cast<std::size_t>(0.25 * host), 0.0, [](std::size_t) { return 0.0; });
        m.push_back(std::move(h));
    }
    return m;
}
int realBench(const Args& a, const Netlist& n) {
    const double host = 48000.0;
    const int factor = static_cast<int>(std::lround(a.rate / host));
    const int kBlock = 64;
    const double inV = n.io.inputVoltsPerFS, outS = 1.0 / n.io.outputVoltsPerFS;
    const int reps = std::max(1, std::getenv("CE_REALREPS") ? std::atoi(std::getenv("CE_REALREPS")) : 3);
    const double deadline = 1e6 * kBlock / host;
    const auto materials = realMaterials(a);
    if (materials.empty()) return 1;
    std::vector<double> combined, combinedI;
    for (const auto& mat : materials) {
        const std::size_t N = mat.x.size(), nb = (N + kBlock - 1) / kBlock;
        std::vector<double> best(nb, 1e30), y(N);
        // CE_BLOCKINSTR=1 (macOS): retired instructions per 64-frame block in the first run, a load-independent p99 / mean
        const bool blockInstr = std::getenv("CE_BLOCKINSTR") != nullptr;
        std::vector<double> bi(blockInstr ? nb : 0);
        double bestTotal = 1e30, its = 0, lte = 0, subs = 0, homs = 0, fails = 0, instr = 1e300;
        std::uint64_t hash = 0;
        for (int r = 0; r < reps; ++r) {
            Circuit c;
            if (!build(a, n, c)) return 1;
            const bool lanes = c.lanesPlanned(), trueIn = c.hasInputR();
            if (lanes) (void)rig::RtPool::instance();
            CircuitOversampler os;
            os.prepare(factor);
            os.reset();
            const Diagnostics d0 = c.diagnostics();
            std::vector<double> bt(nb);
            const std::uint64_t i0 = instructionsNow(); // macOS: retired instructions (load-independent A/B); 0 elsewhere
            const auto t0 = std::chrono::steady_clock::now();
            {
                const ScopedFlushDenormals ftz;
                for (std::size_t b = 0; b < nb; ++b) {
                    const std::uint64_t bi0 = blockInstr && r == 0 ? instructionsNow() : 0;
                    const auto b0 = std::chrono::steady_clock::now();
                    {
                        const LaneBlock lb(c, lanes);
                        const std::size_t e = std::min(N, (b + 1) * kBlock);
                        for (std::size_t i = b * kBlock; i < e; ++i) {
                            float up[CircuitOversampler::kMaxFactor];
                            os.up(0, static_cast<float>(mat.x[i]), up);
                            for (int k = 0; k < os.factor(); ++k) {
                                if (trueIn) c.setInputR(static_cast<double>(up[k]) * inV); // a mono host feeds both inputs (CircuitUnit)
                                up[k] = static_cast<float>(c.process(static_cast<double>(up[k]) * inV) * outS);
                            }
                            y[i] = os.down(0, up);
                        }
                    }
                    bt[b] = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - b0).count();
                    if (blockInstr && r == 0) bi[b] = static_cast<double>(instructionsNow() - bi0);
                }
            }
            const double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            instr = std::min(instr, static_cast<double>(instructionsNow() - i0));
            bestTotal = std::min(bestTotal, total);
            for (std::size_t b = 0; b < nb; ++b) best[b] = std::min(best[b], bt[b]);
            const Diagnostics& d1 = c.diagnostics();
            const double S = static_cast<double>(d1.samples - d0.samples);
            its = static_cast<double>(d1.newtonIterations - d0.newtonIterations) / S;
            lte = static_cast<double>(d1.lteSubsteps - d0.lteSubsteps) / S;
            subs = static_cast<double>(d1.substeps - d0.substeps) / S;
            homs = static_cast<double>(d1.homotopies - d0.homotopies);
            fails = static_cast<double>(d1.failures - d0.failures);
            if (std::getenv("CE_PROFILE") && r == reps - 1) { // per group, the last run (CE_PROFILE also times each group)
                std::printf("real %s groups:\n%s", mat.name.c_str(), c.describeGroups().c_str());
                const auto& t = c.groupTimesNs();
                for (std::size_t g = 0; g < t.size(); ++g)
                    std::printf("  group %zu: %.1f ns/sample, Newton %.1f\n", g, t[g] / static_cast<double>(d1.samples), c.newtonTimesNs()[g] / static_cast<double>(d1.samples));
            }
            hash = 1469598103934665603ull;
            for (double v : y) {
                unsigned char bb[sizeof(double)];
                std::memcpy(bb, &v, sizeof bb);
                for (unsigned char q : bb) hash = (hash ^ q) * 1099511628211ull;
            }
        }
        if (const char* dump = std::getenv("CE_BENCH_DUMP")) {
            std::ofstream f(std::string(dump) + mat.name + ".f64", std::ios::binary);
            f.write(reinterpret_cast<const char*>(y.data()), static_cast<std::streamsize>(y.size() * sizeof(double)));
        }
        double sumBest = 0;
        for (double x : best) sumBest += x;
        const double secs = static_cast<double>(N) / host, pct = 100.0 * 1e-6 * sumBest / secs;
        combined.insert(combined.end(), best.begin(), best.end());
        std::vector<double> s = best;
        std::sort(s.begin(), s.end());
        auto q = [&](double p) { return s[std::min(s.size() - 1, static_cast<std::size_t>(p * static_cast<double>(s.size())))]; };
        std::size_t over = 0;
        for (double x : s) over += x > 0.6 * deadline;
        std::printf("real %s: %.3f %% of one core (x%d, %.1f s, per-block min of %d runs; single %.3f %%); newton %.2f it/sample; lte %.4f events/sample; halvings %.4f/sample; homotopies %.0f; failures %.0f; "
                    "block64 p50 %.1f us, p99 %.1f us, max %.1f us (%.1f / %.1f / %.1f %% of the %.1f us period); %zu of %zu blocks over 60 %%; hash %016llx; %.3f G instructions/s\n",
                    mat.name.c_str(), pct, factor, secs, reps, 100.0 * bestTotal / secs, its, lte, subs, homs, fails, q(0.5), q(0.99), s.back(), 100.0 * q(0.5) / deadline,
                    100.0 * q(0.99) / deadline, 100.0 * s.back() / deadline, deadline, over, s.size(), static_cast<unsigned long long>(hash), instr / secs * 1e-9);
        if (blockInstr && !bi.empty()) {
            combinedI.insert(combinedI.end(), bi.begin(), bi.end());
            std::vector<double> si = bi;
            std::sort(si.begin(), si.end());
            double mean = 0;
            for (double x : bi) mean += x;
            mean /= static_cast<double>(bi.size());
            auto qi = [&](double p) { return si[std::min(si.size() - 1, static_cast<std::size_t>(p * static_cast<double>(si.size())))]; };
            std::printf("real %s block64 instructions: mean %.0f, p50 %.0f, p99 %.0f, max %.0f; p99 / mean %.3f, max / mean %.3f\n", mat.name.c_str(), mean, qi(0.5), qi(0.99),
                        si.back(), qi(0.99) / mean, si.back() / mean);
        }
        if (mat.segs.size() > 1) { // per segment: the block-time sum over the segment's audio time
            std::printf("real %s segments:", mat.name.c_str());
            for (std::size_t k = 0; k < mat.segs.size(); ++k) {
                const std::size_t s0 = mat.segs[k].second / kBlock, s1 = (k + 1 < mat.segs.size() ? mat.segs[k + 1].second : N) / kBlock;
                double t = 0;
                for (std::size_t b = s0; b < s1 && b < nb; ++b) t += best[b];
                std::printf(" %s %.1f", mat.segs[k].first.c_str(), s1 > s0 ? 100.0 * t / (1e6 * static_cast<double>((s1 - s0) * kBlock) / host) : 0.0);
            }
            std::printf(" (%% of one core)\n");
        }
    }
    std::sort(combined.begin(), combined.end());
    const double p99 = combined[std::min(combined.size() - 1, static_cast<std::size_t>(0.99 * combined.size()))];
    std::printf("real combined block64: p99 %.1f us, max %.1f us (%.2f / %.2f %% of period)\n",
                p99, combined.back(), 100.0 * p99 / deadline, 100.0 * combined.back() / deadline);
    {
        double mean = 0;
        for (double x : combined) mean += x;
        mean /= static_cast<double>(combined.size());
        std::printf("real combined block64 time: mean %.1f us, p99 / mean %.3f\n", mean, p99 / mean);
    }
    if (!combinedI.empty()) {
        std::sort(combinedI.begin(), combinedI.end());
        double mean = 0;
        for (double x : combinedI) mean += x;
        mean /= static_cast<double>(combinedI.size());
        const double p = combinedI[std::min(combinedI.size() - 1, static_cast<std::size_t>(0.99 * combinedI.size()))];
        std::printf("real combined block64 instructions: mean %.0f, p99 %.0f, max %.0f; p99 / mean %.3f, max / mean %.3f\n", mean, p, combinedI.back(), p / mean,
                    combinedI.back() / mean);
    }
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: circuit_cli info|spice|render|bench <netlist> ...\n");
        return 2;
    }
    const std::string cmd = argv[1];
    Args a = parse(argc, argv);
    Netlist n;
    if (cmd == "import") {
        SpiceImportReport rep;
        std::string err;
        if (!importSpice(slurp(a.pos.at(0)), n, rep, err)) {
            std::fprintf(stderr, "import: %s\n", err.c_str());
            return 1;
        }
        for (const auto& w : rep.warnings) std::fprintf(stderr, "warning: %s\n", w.c_str());
        std::cout << toJson(n).dump(2) << "\n";
        return 0;
    }
    if (!load(a, n)) return 1;
    if (cmd == "autocut") return autocut(a, n);
    if (cmd == "spice") {
        SpiceExportOptions o;
        tableControls(a, n); // --table: the unit's default control positions
        o.controls = ctlVector(n, a);
        o.inputSource = a.input.empty() ? "DC 0" : a.input;
        o.logicRate = a.rate; // CMOS logic: the engine's band-limited edges carry a constant latency at this rate
        if (!a.tran.empty()) o.analysis = ".tran " + a.tran;
        // Seed ngspice with the engine's own DC solution (ENGINE_QUEUE items 3, 32): the op-amp swing nodes and sources,
        // and every netlist node the deck has (exportSpice drops nodes it does not export). An ideal op-amp with diode
        // feedback, or a latch/Darlington bias loop, has several DC solutions and a plain .op can land on another one;
        // a .nodeset only seeds the first DC phase, so ngspice's answer still satisfies its own equations.
        // --no-hints exports without any seeds.
        if (std::find(a.pos.begin(), a.pos.end(), std::string("--no-hints")) == a.pos.end()) {
            Circuit hc;
            CompileOptions ho;
            ho.sampleRate = 96000.0;
            ho.exportAllNodes = true;
            std::string herr;
            if (hc.compile(n, ho, herr)) {
                for (std::size_t i = 0; i < o.controls.size() && static_cast<int>(i) < hc.controlCount(); ++i) hc.setControlImmediate(static_cast<int>(i), o.controls[i]);
                if (hc.reset()) {
                    const auto& names = hc.nodeNames();
                    for (std::size_t i = 0; i < names.size(); ++i)
                        if ((names[i].size() > 2 && names[i].compare(names[i].size() - 2, 2, "#x") == 0) || names[i].find("#src1") != std::string::npos ||
                            (i > 0 && names[i].find('#') == std::string::npos))
                            o.nodeset[names[i]] = hc.nodeVoltage(static_cast<int>(i));
                }
            }
        }
        // --ic-settled: the engine's running state (--settle, default 600 ms: a transformer-fed supply's periodic steady
        // state) as the deck's initial state, every node voltage as .ic and every inductor current as IC= (the SPICE gate's
        // "settled" start, tests/test_circuit_engine.cpp); give the .tran a `uic`.
        if (std::find(a.pos.begin(), a.pos.end(), std::string("--ic-settled")) != a.pos.end()) {
            Circuit sc;
            Args sa = a;
            sa.exportAllNodes = true;
            if (build(sa, n, sc) && sc.reset()) {
                for (std::size_t i = 1; i < sc.nodeNames().size(); ++i) o.ic[sc.nodeNames()[i]] = sc.nodeVoltage(static_cast<int>(i));
                for (const auto& [k, v] : sc.inductorCurrents()) o.icL[k] = v;
                o.nodeset.clear();
            }
        }
        std::cout << exportSpice(n, o);
        return 0;
    }
    if (cmd == "alias" || cmd == "autorate") unitPath(a, n, cmd == "alias");
    else if ((cmd == "render" || cmd == "bench") && a.table > 0) unitPath(a, n, true); // the unit's rate, LTE, controls and levels
    Circuit c;
    if (!build(a, n, c)) return 1;
    if (cmd == "info") {
        std::printf("%s\n", c.describeGroups().c_str());
        for (const auto& w : c.warnings()) std::printf("note: %s\n", w.c_str());
        std::printf("DC operating point:\n");
        for (std::size_t i = 1; i < c.nodeNames().size(); ++i)
            if (c.isExported(static_cast<int>(i))) std::printf("  %-12s %10.5f V\n", c.nodeNames()[i].c_str(), c.nodeVoltage(static_cast<int>(i)));
        return 0;
    }
    if (cmd == "alias") {
        double worstHz = 0, secs = 0;
        int factor = 1;
        const double worst = aliasMeasure(a, n, c, worstHz, secs, factor);
        const double host = 48000.0;
        const int kN = 4800, settle = 72000;
        std::printf("alias %.1f dB at %.0f Hz  (base x%d, %.1f %% of one core: circuit + oversampler, thread CPU time)\n", worst, worstHz, factor,
                    100.0 * secs / ((settle + kN) / host));
        return 0;
    }
    if (cmd == "autorate") {
        // Per-group rate choice by measured folded energy (ENGINE_QUEUE 33): starting from every group at the base rate
        // (--rate, a power of two of 48 kHz), raise one group's level at a time, keeping the raise that lowers the
        // aliasing gate figure (aliasMeasure) most, until it is under CE_AUTORATE_GATE (-60 dB) or nothing helps. Prints
        // the netlist's "multirate.rates" (factors of a 48 kHz host). The netlist's own rates are ignored here; run it
        // per control-setting class (--ctl ...) and keep the higher factor per part. Offline: a few alias runs per group.
        const double gate = std::getenv("CE_AUTORATE_GATE") ? std::atof(std::getenv("CE_AUTORATE_GATE")) : -60.0;
        const int base = static_cast<int>(std::lround(a.rate / 48000.0));
        int maxLv = 0;
        while ((base << (maxLv + 1)) <= 32) ++maxLv;
        // one part per nonlinear group stands for its group (the ladder raises the group holding the part)
        std::vector<std::string> refs;
        for (const auto& g : c.groups())
            if (g.nn > 0 && !g.parts.empty()) refs.push_back(n.parts[static_cast<std::size_t>(g.parts.front())].ref);
        std::vector<int> lv(refs.size(), 0);
        auto run = [&](const std::vector<int>& L, double& cpu) {
            std::string spec;
            for (std::size_t i = 0; i < refs.size(); ++i)
                if (L[i] > 0) spec += (spec.empty() ? "" : ",") + refs[i] + "=" + std::to_string(L[i]);
            if (spec.empty()) rig::platform::unsetEnv("CE_LEVELS");
            else rig::platform::setEnv("CE_LEVELS", spec.c_str());
            Circuit ck;
            if (!build(a, n, ck)) return 1e9;
            double hz = 0, secs = 0;
            int f = 1;
            const double db = aliasMeasure(a, n, ck, hz, secs, f);
            cpu = 100.0 * secs / (76800.0 / 48000.0);
            return db;
        };
        double cpu = 0;
        double now = run(lv, cpu);
        std::printf("base x%d, all groups: alias %.1f dB, %.1f %% of one core\n", base, now, cpu);
        while (now > gate) {
            int bestG = -1;
            double bestDb = now, bestCpu = 0;
            int bestLv = 0;
            // every higher level of every group (a converter boundary adds its own folding, so 2x the rate can measure
            // worse than the base while 4x is better: the steps are not monotone)
            for (std::size_t gi = 0; gi < refs.size(); ++gi)
                for (int L = lv[gi] + 1; L <= maxLv; ++L) {
                    auto t = lv;
                    t[gi] = L;
                    double tc = 0;
                    const double db = run(t, tc);
                    std::printf("  try %s x%d: alias %.1f dB, %.1f %%\n", refs[gi].c_str(), base << L, db, tc);
                    // the cheapest raise that meets the gate; while none does, the lowest aliasing (within 0.5 dB: cheaper)
                    const bool pass = db <= gate, bestPass = bestG >= 0 && bestDb <= gate;
                    const bool better = pass ? (!bestPass || tc < bestCpu)
                                             : !bestPass && (db < bestDb - 0.5 || (bestG >= 0 && std::abs(db - bestDb) <= 0.5 && tc < bestCpu));
                    if (better) bestG = static_cast<int>(gi), bestLv = L, bestDb = db, bestCpu = tc;
                }
            if (bestG < 0) {
                std::printf("no single raise lowers the aliasing further\n");
                break;
            }
            lv[static_cast<std::size_t>(bestG)] = bestLv;
            now = bestDb;
            cpu = bestCpu;
            std::printf("raise %s to x%d: alias %.1f dB, %.1f %% of one core\n", refs[static_cast<std::size_t>(bestG)].c_str(), base << lv[static_cast<std::size_t>(bestG)], now, cpu);
        }
        // cost pass: lower each raised group a level at a time while the gate still holds (a later raise can make an
        // earlier one partly unnecessary)
        for (bool lowered = now <= gate; lowered;) {
            lowered = false;
            for (std::size_t gi = 0; gi < refs.size(); ++gi) {
                if (lv[gi] == 0) continue;
                auto t = lv;
                --t[gi];
                double tc = 0;
                const double db = run(t, tc);
                std::printf("  try %s x%d: alias %.1f dB, %.1f %%\n", refs[gi].c_str(), base << t[gi], db, tc);
                if (db <= gate) {
                    lv = t;
                    now = db;
                    cpu = tc;
                    lowered = true;
                    std::printf("lower %s to x%d: alias %.1f dB, %.1f %% of one core\n", refs[gi].c_str(), base << t[gi], now, cpu);
                }
            }
        }
        std::string js;
        for (std::size_t i = 0; i < refs.size(); ++i)
            if (lv[i] > 0) js += (js.empty() ? "" : ", ") + std::string("\"") + refs[i] + "\": " + std::to_string(base << lv[i]);
        std::printf("%s: \"multirate\": {\"rates\": {%s}}  (alias %.1f dB, gate %.1f, %.1f %% of one core)\n", n.id.c_str(), js.c_str(), now, gate, cpu);
        return now <= gate ? 0 : 3;
    }
    if (cmd == "render") {
        // One input signal from a WAV, a "time value" CSV or a sine (the left input; io.inputR takes its own options).
        auto loadSignal = [&](const std::string& wavPath, const std::string& csvPath, bool sine, double sf, double sa, std::vector<double>& in) {
            double inRate = a.rate;
            if (!wavPath.empty()) {
                if (!ce_tools::readWav(wavPath, in, inRate)) {
                    std::fprintf(stderr, "cannot read %s\n", wavPath.c_str());
                    return false;
                }
                for (auto& x : in) x *= a.gain;
                if (std::abs(inRate - a.rate) > 1) {
                    // linear-phase-free simple resample by integer factor (render rate must be a multiple)
                    const int f = static_cast<int>(std::lround(a.rate / inRate));
                    std::vector<double> up(in.size() * static_cast<std::size_t>(f));
                    for (std::size_t i = 0; i < in.size(); ++i)
                        for (int k = 0; k < f; ++k) {
                            const double nx = i + 1 < in.size() ? in[i + 1] : in[i];
                            up[i * static_cast<std::size_t>(f) + static_cast<std::size_t>(k)] = in[i] + (nx - in[i]) * k / f;
                        }
                    in.swap(up);
                }
            } else if (!csvPath.empty()) {
                std::ifstream f(csvPath);
                double t, v;
                while (f >> t >> v) in.push_back(v);
            } else if (sine) {
                const auto N = static_cast<std::size_t>(a.secs * a.rate);
                in.resize(N);
                for (std::size_t i = 0; i < N; ++i) in[i] = sa * std::sin(2 * M_PI * sf * static_cast<double>(i) / a.rate);
            }
            return true;
        };
        std::vector<double> in, inR;
        if (!loadSignal(a.wav, a.csvIn, true, a.sineF, a.sineA, in)) return 1;
        const bool hasR = c.hasInputR();
        if (hasR) {
            if (!loadSignal(a.wavR, a.csvInR, a.sineR, a.sineRF, a.sineRA, inR)) return 1;
            if (a.wavR.empty() && a.csvInR.empty() && !a.sineR) inR = in; // a mono feed: R = L
            inR.resize(in.size(), 0.0);
        } else if (!a.wavR.empty() || !a.csvInR.empty() || a.sineR) {
            std::fprintf(stderr, "warning: the netlist has no io.inputR; the right input is ignored\n");
        }
        std::vector<int> probes;
        for (const auto& p : a.probes) probes.push_back(c.node(p));
        const bool wav = a.out.size() > 4 && a.out.substr(a.out.size() - 4) == ".wav";
        std::vector<double> y(in.size());
        std::ofstream o;
        if (!wav) o.open(a.out);
        std::string line;
        char num[40];
        // Multirate (CE_MULTIRATE): --rate is the internal rate; the circuit is fed and read at half of it.
        const std::size_t step = c.isMultirate() ? 2 : 1;
        if (step == 2) y.resize(in.size() / 2);
        for (std::size_t i = 0; i + step - 1 < in.size(); i += step) {
            const std::size_t j = i / step;
            if (hasR) c.setInputR(inR[i]);
            y[j] = step == 2 ? c.processHost(in[i]) : c.process(in[i]);
            if (!wav) {
                std::snprintf(num, sizeof num, "%.10e %.12e", static_cast<double>(j) * static_cast<double>(step) / a.rate, y[j]);
                line = num;
                if (c.hasOutputR()) {
                    std::snprintf(num, sizeof num, " %.12e", c.outputR());
                    line += num;
                }
                for (int p : probes) {
                    std::snprintf(num, sizeof num, " %.12e", c.nodeVoltage(p));
                    line += num;
                }
                o << line << "\n";
            }
        }
        if (wav) ce_tools::writeWav(a.out, y, a.rate / static_cast<double>(step));
        if (std::getenv("CE_KBENCH")) // per-group Newton micro-benchmark on the last sample
            for (std::size_t g = 0; g < c.groups().size(); ++g) {
                const auto b = c.benchNewton(g, 200000);
                std::fprintf(stderr, "  kbench group %zu (%d ports): %.0f ns/solve, %.2f iterations (%.0f ns/iteration), device sweep %.0f ns\n", g, c.groups()[g].nn, b[0],
                             b[1], b[1] > 0 ? b[0] / b[1] : 0.0, b[2]);
                if (b.size() > 3)
                    std::fprintf(stderr, "    H %.0f entries (%.0f non-zero), Dp %.0f (%.0f), ny %.0f ns %.0f W %.0f nuc %.0f\n", b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10]);
            }
        if (std::getenv("CE_PROFILE")) {
            std::fprintf(stderr, "%s", c.describeGroups().c_str());
            const double ns = static_cast<double>(c.diagnostics().samples);
            for (std::size_t g = 0; g < c.groupTimesNs().size(); ++g)
                std::fprintf(stderr, "  group %zu: %.0f ns/internal sample, Newton %.0f\n", g, c.groupTimesNs()[g] / ns, c.newtonTimesNs()[g] / ns);
        }
        const auto& d = c.diagnostics();
        std::fprintf(stderr, "samples %llu  newton %.2f it/sample  max %d  homotopies %llu  substeps %llu  failures %llu  lte %llu\n",
                     static_cast<unsigned long long>(d.samples), d.samples ? static_cast<double>(d.newtonIterations) / static_cast<double>(d.samples) : 0.0,
                     d.maxIterations, static_cast<unsigned long long>(d.homotopies), static_cast<unsigned long long>(d.substeps), static_cast<unsigned long long>(d.failures),
                     static_cast<unsigned long long>(d.lteSubsteps));
        if (std::getenv("CE_GROUPS")) std::fprintf(stderr, "%s", c.describeGroups().c_str());
        return 0;
    }
    if (cmd == "tolerance") {
        // Monte-Carlo: every part with a tolerance varies uniformly within ±tolerance (pots and laws scale their
        // whole range). Reports the spread of the small-signal response per octave and of THD at a driving level.
        const int runs = std::getenv("CE_RUNS") ? std::atoi(std::getenv("CE_RUNS")) : 100;
        const double level = std::getenv("CE_LEVEL") ? std::atof(std::getenv("CE_LEVEL")) : 0.3;
        const std::vector<double> f = {63, 125, 250, 500, 1000, 2000, 4000, 8000};
        std::vector<std::vector<double>> db(f.size());
        std::vector<double> thd;
        unsigned rng = 12345;
        auto uni = [&]() { rng = rng * 1664525u + 1013904223u; return (rng >> 8) / 16777216.0; };
        int ok = 0;
        for (int run = 0; run < runs; ++run) {
            Netlist m = n;
            for (auto& p : m.parts)
                if (p.tolerance > 0 && p.hasValue && p.type != "V") {
                    const double k = 1.0 + p.tolerance * (2.0 * uni() - 1.0);
                    p.value.nominal *= k;
                    p.value.min *= k;
                    p.value.max *= k;
                }
            Circuit c2;
            CompileOptions o;
            o.sampleRate = a.rate;
            std::string err;
            if (!c2.compile(m, o, err)) continue;
            const auto v = ctlVector(m, a);
            for (std::size_t i = 0; i < v.size(); ++i) c2.setControlImmediate(static_cast<int>(i), v[i]);
            c2.reset();
            const auto h = c2.smallSignalResponse(f, c2.node(m.io.output));
            for (std::size_t i = 0; i < f.size(); ++i) db[i].push_back(20 * std::log10(std::abs(h[i]) + 1e-30));
            // THD at 1 kHz over the last 10 ms of 60 ms
            const int N = static_cast<int>(0.06 * a.rate), per = static_cast<int>(std::lround(a.rate / 1000.0));
            std::vector<double> y;
            for (int i = 0; i < N; ++i) {
                const double out = c2.process(level * std::sin(2 * M_PI * 1000.0 * i / a.rate));
                if (i >= N - 10 * per) y.push_back(out);
            }
            auto harm = [&](int hn) {
                std::complex<double> acc = 0;
                for (std::size_t i = 0; i < y.size(); ++i) acc += y[i] * std::polar(1.0, -2 * M_PI * hn * 1000.0 * static_cast<double>(i) / a.rate);
                return std::abs(acc);
            };
            double hs = 0;
            for (int hn = 2; hn <= 9; ++hn) hs += harm(hn) * harm(hn);
            thd.push_back(100.0 * std::sqrt(hs) / harm(1));
            ++ok;
        }
        auto stats = [](std::vector<double> v) {
            std::sort(v.begin(), v.end());
            double mean = 0;
            for (double x : v) mean += x;
            mean /= static_cast<double>(v.size());
            double var = 0;
            for (double x : v) var += (x - mean) * (x - mean);
            return std::array<double, 5>{mean, std::sqrt(var / static_cast<double>(v.size())), v.front(), v[v.size() / 20], v[v.size() - 1 - v.size() / 20]};
        };
        std::printf("Monte-Carlo tolerance: %s, %d/%d units, controls as given, level %.2f V\n", n.id.c_str(), ok, runs, level);
        std::printf("| band | mean dB | σ dB | min | p5 | p95 |\n|---|---|---|---|---|---|\n");
        for (std::size_t i = 0; i < f.size(); ++i) {
            const auto s5 = stats(db[i]);
            std::printf("| %g Hz | %+.2f | %.2f | %+.2f | %+.2f | %+.2f |\n", f[i], s5[0], s5[1], s5[2], s5[3], s5[4]);
        }
        const auto t5 = stats(thd);
        std::printf("| THD 1 kHz | %.2f %% | %.2f | %.2f | %.2f | %.2f |\n", t5[0], t5[1], t5[2], t5[3], t5[4]);
        return 0;
    }
    if (cmd == "ac") {
        std::vector<double> f;
        for (int i = 0; i <= 60; ++i) f.push_back(20.0 * std::pow(1000.0, i / 60.0));
        const auto h = c.smallSignalResponse(f, c.node(n.io.output));
        for (std::size_t i = 0; i < f.size(); ++i)
            std::printf("%.6e %.6f %.4f\n", f[i], 20 * std::log10(std::abs(h[i])), std::arg(h[i]) * 180 / M_PI);
        return 0;
    }
    if (cmd == "bench") {
        const auto N = static_cast<std::size_t>(a.benchSecs * a.rate);
        std::vector<double> in(N), out(N);
        // guitar-like test: decaying chord partials at a driving level
        for (std::size_t i = 0; i < N; ++i) {
            const double t = static_cast<double>(i) / a.rate;
            const double env = std::exp(-1.5 * std::fmod(t, 2.0));
            in[i] = 0.3 * env * (std::sin(2 * M_PI * 82.4 * t) + 0.6 * std::sin(2 * M_PI * 123.5 * t) + 0.4 * std::sin(2 * M_PI * 164.8 * t));
        }
        // Minimum over repeated runs: robust against other load on the machine.
        const int reps = std::getenv("CE_REPS") ? std::atoi(std::getenv("CE_REPS")) : 5;
        // Intra-unit lanes (docs §10.33): blocks of CE_BLOCK internal samples (default 128: a 64-frame host block at 2x),
        // each with the helper lane forked onto the RtPool, as the unit renders. CE_NOLANES=1: no plan, processBlock.
        const bool lanes = c.lanesPlanned();
        const int blk = std::getenv("CE_BLOCK") ? std::max(1, std::atoi(std::getenv("CE_BLOCK"))) : 128;
        if (lanes) (void)rig::RtPool::instance(); // start the workers outside the timing
        double s = 1e30;
        std::vector<double> blockUs; // CE_BLOCKTIME=1: each block's wall time in the last run (the callback's share)
        const bool blockTime = std::getenv("CE_BLOCKTIME") != nullptr;
        for (int r = 0; r < reps; ++r) {
            blockUs.clear();
            const auto t0 = std::chrono::steady_clock::now();
            if (c.isMultirate())
                for (std::size_t i = 0; i < N / 2; ++i) out[i] = c.processHost(in[2 * i]);
            else if (lanes || blockTime) {
                const ScopedFlushDenormals ftz;
                for (std::size_t o = 0; o < N; o += static_cast<std::size_t>(blk)) {
                    const auto b0 = std::chrono::steady_clock::now();
                    {
                        const LaneBlock lb(c, lanes);
                        const std::size_t e = std::min(N, o + static_cast<std::size_t>(blk));
                        for (std::size_t i = o; i < e; ++i) out[i] = c.process(in[i]);
                    }
                    if (blockTime) blockUs.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - b0).count());
                }
            } else
                c.processBlock(in.data(), out.data(), static_cast<int>(N));
            s = std::min(s, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        }
        if (!blockUs.empty()) {
            std::sort(blockUs.begin(), blockUs.end());
            auto q = [&](double p) { return blockUs[std::min(blockUs.size() - 1, static_cast<std::size_t>(p * static_cast<double>(blockUs.size())))]; };
            const double deadline = 1e6 * blk / a.rate; // the block's duration at the internal rate
            std::size_t over = 0;
            for (double x : blockUs) over += x > deadline;
            std::printf("block wall time (%d samples, last run): p50 %.1f us, p99 %.1f us, p99.9 %.1f us, max %.1f us; deadline %.1f us, %zu of %zu over\n",
                        blk, q(0.5), q(0.99), q(0.999), blockUs.back(), deadline, over, blockUs.size());
        }
        // FNV-1a of the last run's output bytes: lanes on / off must print the same hash (bit-identical).
        std::uint64_t hash = 1469598103934665603ull;
        for (std::size_t i = 0; i < (c.isMultirate() ? N / 2 : N); ++i) {
            unsigned char b[sizeof(double)];
            std::memcpy(b, &out[i], sizeof b);
            for (unsigned char x : b) hash = (hash ^ x) * 1099511628211ull;
        }
        std::printf("output hash %016llx\n%s", static_cast<unsigned long long>(hash), c.describeLanes().c_str());
        if (lanes) {
            const auto ls = c.laneStats();
            std::printf("lane samples: %llu on two lanes, %llu on one; helper ran %llu groups, audio thread claimed %llu lane-B groups\n",
                        static_cast<unsigned long long>(ls.duoSamples), static_cast<unsigned long long>(ls.soloSamples),
                        static_cast<unsigned long long>(ls.helperGroups), static_cast<unsigned long long>(ls.steals));
            std::printf("lane blocks: %llu, helper attached %llu times (detached idle %llu, waiting %llu); %llu one-lane samples before the attach\n",
                        static_cast<unsigned long long>(ls.blocks), static_cast<unsigned long long>(ls.attaches), static_cast<unsigned long long>(ls.idleDetaches),
                        static_cast<unsigned long long>(ls.waitDetaches), static_cast<unsigned long long>(ls.soloBeforeAttach));
        }
        const auto& d = c.diagnostics();
        std::printf("%s: %.0f samples/s internal, %.3f %% of one core at %.0f Hz internal rate (%.1f ns/sample); newton %.2f it/sample; homotopies %llu; failures %llu\n",
                    n.id.c_str(), static_cast<double>(N) / s, 100.0 * s / a.benchSecs, a.rate, 1e9 * s / static_cast<double>(N),
                    static_cast<double>(d.newtonIterations) / static_cast<double>(d.samples), static_cast<unsigned long long>(d.homotopies), static_cast<unsigned long long>(d.failures));
        if (std::getenv("CE_PROFILE")) {
            std::printf("%s", c.describeGroups().c_str());
            std::printf("  active-set Newton: %llu solved, %llu fell back\n", static_cast<unsigned long long>(d.activeSolves), static_cast<unsigned long long>(d.activeMisses));
            const auto& t = c.groupTimesNs();
            double tot = 0;
            for (double x : t) tot += x;
            for (std::size_t g = 0; g < t.size(); ++g)
                std::printf("  group %zu: %.1f ns/sample (%.0f %%), Newton %.1f\n", g, t[g] / static_cast<double>(d.samples), 100.0 * t[g] / std::max(tot, 1e-9),
                            c.newtonTimesNs()[g] / static_cast<double>(d.samples));
        }
        if (a.real) return realBench(a, n);
        return 0;
    }
    std::fprintf(stderr, "unknown command %s\n", cmd.c_str());
    return 2;
}
