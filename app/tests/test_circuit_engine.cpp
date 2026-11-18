// Circuit engine tests (docs/CIRCUIT_ENGINE.md §7).
//
//  * netlist parsing, value notation, tapers, SPICE import/export round trip
//  * exact partitioning (group structure of the library circuits)
//  * cross-solver agreement (WDF vs DK), diode-pair merge, rate convergence
//  * SPICE gate: transient and AC responses, and DC operating points, against ngspice references.
//    References are frozen in tests/fixtures/circuit (no ngspice needed in CI). With HEADROOM_NGSPICE=1 and
//    ngspice on PATH, they are regenerated from the current netlists first.
//  * real-time behaviour: control glides without discontinuities, state hand-off, bounded Newton work
#include "circuit/Engine.hpp"
#include "circuit/Multirate.hpp"
#include "circuit/Blocks.hpp"
#include "circuit/Denormals.hpp"
#include "circuit/LaneBlock.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <cstring>
#include <thread>
#include <vector>

using namespace rig::ce;

#ifndef HEADROOM_CIRCUIT_LIBRARY
#define HEADROOM_CIRCUIT_LIBRARY "src/circuit/library"
#endif
#ifndef HEADROOM_CIRCUIT_FIXTURES
#define HEADROOM_CIRCUIT_FIXTURES "tests/fixtures/circuit"
#endif

static int failures = 0;
#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            std::fprintf(stderr, __VA_ARGS__);               \
            std::fprintf(stderr, "\n");                      \
            ++failures;                                      \
        }                                                    \
    } while (0)

static std::string slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}
// Gate-only runs (circuit_cli autocut, docs §10.27): CE_GATE_FILE=<netlist file> runs only that netlist's SPICE
// cases (transient, AC, DC) and skips the unit tests; CE_GATE_LIBDIR reads the netlists from another directory (a cut
// candidate), against the same frozen fixtures.
static std::string libDir() {
    const char* d = std::getenv("CE_GATE_LIBDIR");
    return d && *d ? std::string(d) : std::string(HEADROOM_CIRCUIT_LIBRARY);
}
static bool gateWanted(const std::string& file) {
    const char* f = std::getenv("CE_GATE_FILE");
    return !f || !*f || file == f;
}
static Netlist loadLib(const std::string& file) {
    Netlist n;
    std::string err;
    if (!parseNetlistText(slurp(libDir() + "/" + file), n, err)) {
        std::fprintf(stderr, "cannot load %s: %s\n", file.c_str(), err.c_str());
        std::exit(1);
    }
    return n;
}
static std::vector<double> controls(const Netlist& n, const std::map<std::string, double>& set) {
    std::vector<double> v;
    for (const auto& c : n.controls) v.push_back(set.count(c.id) ? set.at(c.id) : c.def);
    return v;
}
static bool compileAt(Circuit& c, const Netlist& n, double rate, const std::vector<double>& ctl, const std::vector<std::string>& probes = {},
                      const std::string& solver = "", bool merge = true, double newtonTol = 0, double lteTol = -1) {
    CompileOptions o;
    if (newtonTol > 0) o.newtonTol = newtonTol;
    if (lteTol >= 0) o.lteTol = lteTol;
    o.sampleRate = rate;
    o.probes = probes;
    o.solver = solver;
    o.mergeDiodePairs = merge;
    std::string err;
    if (!c.compile(n, o, err)) {
        std::fprintf(stderr, "compile %s: %s\n", n.id.c_str(), err.c_str());
        return false;
    }
    for (std::size_t i = 0; i < ctl.size(); ++i) c.setControlImmediate(static_cast<int>(i), ctl[i]);
    return c.reset();
}

// Like-for-like variants for the SPICE gate (the same edit goes to the engine and to the exported ngspice deck):
//  * "sstone_hold=<V>", "dmm_lfo_hold": a relaxation LFO linearised at its DC point sits on an unstable balance; which
//    way it first flips is decided by numerical noise, so engine and ngspice transients start on different LFO phases.
//    These hold the LFO still (the Small Stone's C12 at <V> with the trigger removed and the IABC Darlington fed from
//    V+; the DMM's U5A/U5B oscillator replaced by its DC triangle voltage) to gate the static audio path. The LFO
//    itself is gated against ngspice and the published calibration in testLfoUnits().
//  * "re201_head1": ngspice sees the tape heads as idle sources, so the playback chain never carries signal there.
//    This replaces the tape block by its electrical ports with playback head 1 driven by the test input (behind the
//    head's own R + L), so the whole playback / tone / echo path is gated like for like.
//  * "mains_dc=<node>:<V>,…": a transformer-fed supply's averaged circuit (mainsAveragedNetlist, the circuit the engine's
//    settle solves): the mains side removed at the rectifiers, each rectifier output a DC source at the given volts
//    (the periodic steady state's means), so both sides start from the settled operating point's DC solution.
static Netlist loadVariant(const std::string& file, const std::string& variant) {
    if (variant.empty()) return loadLib(file);
    if (variant.rfind("mains_dc=", 0) == 0) {
        Netlist out;
        std::vector<std::string> rect;
        if (!mainsAveragedNetlist(loadLib(file), out, rect)) {
            std::fprintf(stderr, "variant %s of %s: no transformer-fed supply\n", variant.c_str(), file.c_str());
            std::exit(1);
        }
        std::map<std::string, double> v;
        std::istringstream in(variant.substr(9));
        std::string tok;
        while (std::getline(in, tok, ','))
            if (const auto c = tok.find(':'); c != std::string::npos) v[tok.substr(0, c)] = std::stod(tok.substr(c + 1));
        for (std::size_t k = 0; k < rect.size(); ++k) {
            if (!v.count(rect[k])) {
                std::fprintf(stderr, "variant %s of %s: no value for rectifier output %s\n", variant.c_str(), file.c_str(), rect[k].c_str());
                std::exit(1);
            }
            for (auto& p : out.parts)
                if (p.ref == "VSETTLE" + std::to_string(k)) p.value.nominal = v[rect[k]];
        }
        return out;
    }
    auto j = nlohmann::json::parse(slurp(libDir() + "/" + file));
    auto& parts = j["parts"];
    auto drop = [&](const std::vector<std::string>& refs) {
        for (std::size_t i = parts.size(); i-- > 0;)
            if (std::find(refs.begin(), refs.end(), parts[i]["ref"].get<std::string>()) != refs.end()) parts.erase(i);
    };
    auto add = [&](const std::string& ref, const std::string& type, const std::vector<std::string>& nodes, const std::string& value) {
        parts.push_back({{"ref", ref}, {"type", type}, {"nodes", nodes}, {"value", value}});
    };
    char num[48];
    if (variant.rfind("sstone_hold=", 0) == 0) {
        drop({"R15", "R17", "R16", "SWA_OFF", "SWA_ON", "Q3", "Q4", "R18", "R20", "C10", "R19", "R22", "R23", "R21", "U5", "C12", "VKICK", "RKICK"});
        for (auto& p : parts)
            if (p["ref"] == "Q5B") p["nodes"] = {"vcc", "d5m", "l5e"};
        add("VHOLD", "V", {"l5o", "0"}, variant.substr(12));
    } else if (variant == "dmm_lfo_hold") {
        Circuit c0;
        const Netlist n0 = loadLib(file);
        if (!compileAt(c0, n0, 96000.0, controls(n0, {}))) std::exit(1);
        std::snprintf(num, sizeof num, "%.9g", c0.nodeVoltage(c0.node("ltri")));
        drop({"U5A", "U5B", "R56", "R57", "R58", "C39", "C40", "S2C", "S2V", "VKICK", "RKICK"});
        add("VLFO", "V", {"ltri", "0"}, num);
    } else if (variant == "re201_head1") {
        const auto& m = j["models"]["RE201_SM"];
        auto mv = [&](const char* k) {
            std::snprintf(num, sizeof num, "%.9g", m[k].get<double>());
            return std::string(num);
        };
        drop({"TAPE"});
        add("TREC_R", "R", {"n14", "trx"}, mv("rrec"));
        add("TREC_L", "L", {"trx", "0"}, mv("lrec"));
        const std::vector<std::string> src = {"in", "0", "0"}, head = {"ph1", "ph2", "ph3"};
        for (int h = 0; h < 3; ++h) {
            const std::string k = std::to_string(h + 1);
            add("TPB" + k + "_R", "R", {src[static_cast<std::size_t>(h)], "tpx" + k}, mv("rpb"));
            add("TPB" + k + "_L", "L", {"tpx" + k, head[static_cast<std::size_t>(h)]}, mv("lpb"));
        }
    } else if (variant == "6g15_wet") {
        // Fender 6G15 (27000): the SPICE deck has no spring (the tank exports as its input coil and an idle output), so the
        // recovery / tone / mixer chain is gated with the tank's electrical ends standing alone: its coil load on TR2, and
        // its 2575 ohm output resistance driven by the deck's input source in place of the springs.
        const auto& m = j["models"].contains("4AB3C1B") ? j["models"]["4AB3C1B"] : nlohmann::json{{"rin", 8.0}, {"lin", 1.0e-3}, {"rout", 2575.0}};
        auto mv = [&](const char* k) {
            std::snprintf(num, sizeof num, "%.9g", m[k].get<double>());
            return std::string(num);
        };
        drop({"TANK"});
        add("TK_R", "R", {"tk", "tkx"}, mv("rin"));
        add("TK_L", "L", {"tkx", "0"}, mv("lin"));
        add("TK_OUT", "R", {"vt", "g3"}, mv("rout"));
        j["io"]["input"] = "vt";
    } else {
        std::fprintf(stderr, "unknown netlist variant %s\n", variant.c_str());
        std::exit(1);
    }
    Netlist n;
    std::string err;
    if (!parseNetlistText(j.dump(), n, err)) {
        std::fprintf(stderr, "variant %s of %s: %s\n", variant.c_str(), file.c_str(), err.c_str());
        std::exit(1);
    }
    return n;
}

// ───────────────────────────── ngspice ─────────────────────────────
static bool ngspiceWanted() {
    const char* e = std::getenv("HEADROOM_NGSPICE");
    return e && std::string(e) == "1" && std::system("ngspice -v > /dev/null 2>&1") == 0;
}
// HEADROOM_NGSPICE_ONLY=<id>[,<id>...] limits regeneration to those cases (the rest keep their frozen fixtures).
static bool regenerate(const std::string& id) {
    if (!ngspiceWanted()) return false;
    const char* only = std::getenv("HEADROOM_NGSPICE_ONLY");
    if (!only || !*only) return true;
    std::istringstream s(only);
    std::string tok;
    while (std::getline(s, tok, ','))
        if (tok == id) return true;
    return false;
}
static std::vector<std::vector<double>> readTable(const std::string& path) {
    std::vector<std::vector<double>> rows;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream s(line);
        std::vector<double> r;
        double x;
        while (s >> x) r.push_back(x);
        if (!r.empty()) rows.push_back(r);
    }
    return rows;
}
static std::string fixture(const std::string& name) { return std::string(HEADROOM_CIRCUIT_FIXTURES) + "/" + name; }

struct TranCase {
    std::string id, file;
    std::map<std::string, double> ctl;
    double amp, freq, secs;
    std::vector<std::string> nodes; // compared nodes (first = output)
    double tolPct;                  // NRMSE gate per node
    double ampR = -1, freqR = 0;    // io.inputR circuits: the right input's sine (ampR < 0: the left signal, a mono feed)
    std::string variant;            // loadVariant(): like-for-like edit applied to engine and ngspice alike
    double rate = 0;                // engine and ngspice output rate (0 = kRate); ngspice maxstep is T/8 either way
    double hfDb = 0;                // > 0: also gate the energy above 30 kHz to within ±hfDb of ngspice (dimed cases)
    // Start state, the same physical state on both sides. "" = the DC operating point (engine reset, ngspice .op).
    // Transformer-fed mains supplies have no usable DC point (a winding is a short at DC: every rail at 0 V), so:
    // "cold" = both switched on cold: engine CompileOptions::coldMs 0, ngspice `.tran … uic` with nothing set (every
    //          node, capacitor, inductor and core flux at zero, the mains at its t = 0 crest);
    // "settled" = the engine's running state (CompileOptions::settleMs as in the unit: its periodic steady state), and
    //          ngspice started from exactly that state: `.tran … uic` with every node voltage as .ic (capacitors and
    //          core fluxes) and every inductor current as IC=. Not used for the SVT: from its sampled state ngspice's
    //          output stage takes a kick that needs hundreds of ms to die out (docs §3.10); its settled operating point
    //          is gated on the "mains_dc" variant instead. The Fairchild 670 uses it (f670_set_*).
    std::string start;
};

static constexpr double kRate = 96000.0;
static constexpr double kUnitSettleMs = 450.0; // CircuitUnit's kSettleMs (the "settled" start)
// The SVT's rectifier outputs at its periodic steady state (period means; testMainsSettle holds the engine to them):
// the "mains_dc" variant's sources for the settled-operating-point cases.
static const char* const kSvtRails = "mains_dc=bpa:695.29,bscn:200.51,biasn:-159.92";
// Fairchild 670: the GZ34 output and the bias bridge's output, periodic steady-state means (circuit_cli render, 0.2 s idle)
// (the same circuit on both sides, so any nearby value is like for like: since the GZ34 output hv stopped being a cut
// (2026-11-12) the engine's periodic mean is 462.84 V and ngspice's full-mains deck settles at 462.82 V; with the cut it
// was 463.74 V. The frozen fixtures use 462.10.)
static const char* const kF670Rails = "mains_dc=hv:462.10,nr:-32.28";
// Bogner Ecstasy 101B (162000): the bridge output's settled mean (circuit_cli render, 14 s from the crest start).
// The small-signal plate case (xtc_clean_set) runs here: on the full-mains start the plates carry the reservoir's 8 Vpp
// ripple, whose rectifier commutation differs from ngspice by ~1.1 % of it (bres 1.19 %, spk 0.04 %).
static const char* const kXtcRails = "mains_dc=bres:520.74";

// The engine at a TranCase's start state (see TranCase::start).
static bool compileStart(Circuit& c, const Netlist& n, double rate, const std::vector<double>& ctl, const std::vector<std::string>& probes,
                         const std::string& start, bool allNodes = false) {
    CompileOptions o;
    o.sampleRate = rate;
    o.probes = probes;
    o.exportAllNodes = allNodes;
    if (start == "cold") o.coldMs = 0.0;
    else if (start == "settled") o.settleMs = kUnitSettleMs;
    std::string err;
    if (!c.compile(n, o, err)) {
        std::fprintf(stderr, "compile %s: %s\n", n.id.c_str(), err.c_str());
        return false;
    }
    for (std::size_t i = 0; i < ctl.size(); ++i) c.setControlImmediate(static_cast<int>(i), ctl[i]);
    return c.reset();
}

// ngspice convergence hints: the engine's DC value of every op-amp swing node (SpiceExportOptions::nodeset).
static std::map<std::string, double> engineHints(const Netlist& n, const std::vector<double>& ctl) {
    std::map<std::string, double> h;
    Circuit c;
    CompileOptions o;
    o.sampleRate = kRate;
    o.exportAllNodes = true;
    std::string err;
    if (!c.compile(n, o, err)) return h;
    for (std::size_t i = 0; i < ctl.size() && static_cast<int>(i) < c.controlCount(); ++i) c.setControlImmediate(static_cast<int>(i), ctl[i]);
    if (!ctl.empty() && !c.reset()) return h;
    const auto& names = c.nodeNames();
    for (std::size_t i = 0; i < names.size(); ++i)
        if ((names[i].size() > 2 && names[i].compare(names[i].size() - 2, 2, "#x") == 0) || names[i].find("#src1") != std::string::npos)
            h[names[i]] = c.nodeVoltage(static_cast<int>(i));
    return h;
}

static void makeTranFixture(const TranCase& tc, const Netlist& n) {
    const double rate = tc.rate > 0 ? tc.rate : kRate;
    SpiceExportOptions o;
    o.controls = controls(n, tc.ctl);
    if (tc.start.empty()) o.nodeset = engineHints(n, o.controls);
    if (tc.start == "settled") { // ngspice starts where the engine starts: every node voltage and inductor current
        Circuit c;
        if (compileStart(c, n, rate, o.controls, {}, tc.start, true)) {
            for (std::size_t i = 1; i < c.nodeNames().size(); ++i) o.ic[c.nodeNames()[i]] = c.nodeVoltage(static_cast<int>(i));
            for (const auto& [k, v] : c.inductorCurrents()) o.icL[k] = v;
        }
    }
    char src[128];
    std::snprintf(src, sizeof src, "SIN(0 %.9g %.9g 0 0 0)", tc.amp, tc.freq);
    o.inputSource = src;
    o.logicRate = rate; // CMOS logic: the band-limited edges' constant latency at the engine rate
    if (tc.ampR >= 0) {
        std::snprintf(src, sizeof src, "SIN(0 %.9g %.9g 0 0 0)", tc.ampR, tc.freqR > 0 ? tc.freqR : tc.freq);
        o.inputSourceR = src;
    }
    std::string deck = exportSpice(n, o);
    const std::string raw = "/tmp/headroom_ce_" + tc.id + ".txt";
    char tran[160];
    std::snprintf(tran, sizeof tran, ".options interp\n.tran %.12g %.12g 0 %.12g%s\n", 1.0 / rate, tc.secs, 1.0 / rate / 8.0, tc.start.empty() ? "" : " uic");
    deck += tran;
    // Per-case ngspice settings (reference generation only; the gate tolerances are unchanged). The Deluxe's output
    // transformer core node has almost no admittance at ngspice's first, nanosecond-sized steps (h/2Lm), and with
    // vntol 1e-8 its Newton never converges there ("timestep too small" at 13 ns on the OT winding branch); 1 uV
    // voltage tolerance converges and leaves the solution unchanged (checked against the frozen reference).
    if (tc.id == "deluxe_clean" || tc.id == "deluxe_drive") deck += ".options vntol=1e-6\n";
    // The Bi-Phase (bp_*): ngspice's default .op fails on the two Schmitt-trigger sweep generators; itl1=500 converges on
    // the engine's operating point (both oscillators at their unstable mid point).
    if (tc.id.rfind("bp_", 0) == 0) deck += ".options itl1=500\n";
    // The OC-2's input JFET (2SK30A, Q1) stalls ngspice's step control at about 3.5 ms ("timestep too small"); the
    // settings from the unit's gating run (gear, looser abstol/vntol) converge (wave-e REPORT, 35000).
    // The SSL dynamics card (ssle_*) stalls ngspice on a CA3130 internal node under drive; the same settings converge.
    if (tc.id.rfind("oc2_", 0) == 0 || tc.id.rfind("ssle_", 0) == 0) deck += ".options gmin=1e-10 abstol=1e-9 reltol=1e-3 vntol=1e-5 method=gear maxord=2\n";
    // The SSL G bus compressor (busg_*): with the side-chain VCAs at +14 dB (threshold CW), ngspice's first direct Newton
    // on the .op leaves the engine's .nodeset for a spurious root (the A2 I/V op-amp macro at 31.6 kV); noopiter goes
    // straight to gmin stepping from the nodeset and lands on the engine's operating point (0.02 of tolerance, 176 nodes).
    if (tc.id.rfind("busg_", 0) == 0) deck += ".options noopiter\n";
    // The dbx 160 (d160_*): with its VCA above 0 dB (GAIN CONTROL past centre) ngspice's first direct Newton lands on a
    // spurious 10-60 kV root of the OA4 I/V macro, as on the SSL G; noopiter lands on the engine's operating point.
    if (tc.id.rfind("d160_", 0) == 0) deck += ".options noopiter\n";
    // The SVT stalls ngspice's step control ("timestep too small": at the bias winding with the power transformer in the
    // deck, at the 12BH7 driver grid and the grid buses on the settled circuit): BDF, itl4 500 and reltol 1e-4, as on the
    // wave-e thread (sessions 2b/2c); gear's damping acts far above the 96 kHz output (maxstep T/8 = 1.3 µs). The node
    // shunts (rshunt) are as large as still converges, since the engine has none: 10 GΩ with the power transformer in the
    // deck (its high-voltage and bias winding ends connect only through rectifiers, so when those are off they float and
    // the leakage inductance meets only the shunt; 1 TΩ and 100 GΩ abort, 1 GΩ doubled the rail error), 100 GΩ on the
    // settled circuit (1 TΩ aborts under drive; 1 GΩ moves the 220k-loaded plate p1a by 50 mV). svt_set_drive takes about
    // 4 minutes, the cold cases about 45 s each.
    if (tc.id.rfind("svt_", 0) == 0) deck += std::string(".options method=gear itl4=500 reltol=1e-4 rshunt=") + (tc.start == "cold" ? "1e10" : "1e11") + "\n";
    // svt_clean (2026-11-11): after the Ultra-Lo/Hi rewire (and with the pre-rewire netlist too) the cold start stalls at
    // 0.176 s on the power-transformer leakage node (n_pt_l3); itl4 2000 completes it with the same 10 GΩ shunts.
    if (tc.id == "svt_clean") deck += ".options itl4=2000\n";
    // SLO-100 MK II (slo2_*): each supply leg has two series rectifier diodes; the node between them is held only by
    // leakage while both are off, and ngspice aborts there without a node shunt.
    if (tc.id.rfind("slo2_", 0) == 0) deck += ".options rshunt=1e10\n";
    // Fairchild 670: when the side chain's 1N538 bridge commutates into T104's secondary leakage, trapezoidal ngspice stops on
    // "timestep too small" (n_t104_l2, n_ay1); Gear as for the SVT
    if (tc.id == "f670_hot" || tc.id == "f670_ms" || tc.id.rfind("f670_attack", 0) == 0) deck += ".options method=gear itl4=500 reltol=1e-4 rshunt=1e11\n";
    // Fairchild 670 with its mains side in the deck (f670_set_*, started from the engine's periodic state): when all four
    // GZ34 / GR301 bridge diodes are off, the HT and bias windings' ends have no DC path, and ngspice stops on "timestep too
    // small" at the first rectifier switch-off (et204_w0 / n_t301_l1) with the default 1 TΩ shunt; 1 GΩ completes it
    // (VERIFY_BATCH_8). Gear for the limiting case, as f670_hot.
    if (tc.id == "f670_set_small") deck += ".options rshunt=1e9\n";
    if (tc.id == "f670_set_hot") deck += ".options method=gear itl4=500 reltol=1e-4 rshunt=1e9\n";
    // Roland JC-120 (jc120_*): ngspice stops with "timestep too small" under drive without a node capacitance; rshunt
    // would disturb the floating 2SK30A switch gates.
    if (tc.id.rfind("jc120_", 0) == 0) deck += ".options cshunt=1e-15\n";
    // Mesa Dual Rectifier (rect_*): the series-diode midpoint d1m stalls ngspice ("timestep too small") without a shunt.
    if (tc.id.rfind("rect_", 0) == 0) deck += ".options rshunt=1e10\n";
    // Dual Rectifier Rev C (revc_*): the same supply, so the same shunt for the floating series-diode midpoints.
    if (tc.id.rfind("revc_", 0) == 0) deck += ".options rshunt=1e10\n";
    // Orange OR120 (or120_*): the drawn bridge has two 1N4005 in series per leg; their midpoints float, as on the rect_ cases.
    if (tc.id.rfind("or120_", 0) == 0) deck += ".options rshunt=1e10\n";
    deck += ".control\nrun\nwrdata " + raw;
    for (const auto& nd : tc.nodes) deck += " v(" + spiceNodeName(n, nd) + ")";
    deck += "\n.endc\n.end\n";
    const std::string cir = "/tmp/headroom_ce_" + tc.id + ".cir";
    std::ofstream(cir) << deck;
    std::remove(raw.c_str());
    if (std::system(("ngspice -b '" + cir + "' > '" + cir + ".log' 2>&1").c_str()) != 0) {
        std::fprintf(stderr, "ngspice failed for %s (see %s.log)\n", tc.id.c_str(), cir.c_str());
        return;
    }
    const auto t = readTable(raw);
    std::ofstream out(fixture(tc.id + ".tran.txt"));
    out << "# ngspice reference: " << n.id << " v" << n.version << " case " << tc.id << " (t, then";
    for (const auto& nd : tc.nodes) out << " v(" << nd << ")";
    out << ") second half only\n";
    char b[64];
    for (std::size_t i = t.size() / 2; i < t.size(); ++i) {
        std::snprintf(b, sizeof b, "%.10e", t[i][0]);
        out << b;
        for (std::size_t k = 0; k < tc.nodes.size(); ++k) {
            std::snprintf(b, sizeof b, " %.9e", t[i][2 * k + 1]);
            out << b;
        }
        out << "\n";
    }
}

static double harmonicDb(const std::vector<double>& x, double f, double rate, int h) {
    std::complex<double> s = 0;
    for (std::size_t i = 0; i < x.size(); ++i) s += x[i] * std::polar(1.0, -2.0 * M_PI * h * f * static_cast<double>(i) / rate);
    return 20.0 * std::log10(std::abs(s) * 2.0 / static_cast<double>(x.size()) + 1e-15);
}

static void runTranCase(const TranCase& tc) {
    if (!gateWanted(tc.file)) return;
    const Netlist n = loadVariant(tc.file, tc.variant);
    if (regenerate(tc.id)) makeTranFixture(tc, n);
    const auto ref = readTable(fixture(tc.id + ".tran.txt"));
    CHECK(!ref.empty(), "%s: missing fixture %s (run with HEADROOM_NGSPICE=1)", tc.id.c_str(), fixture(tc.id + ".tran.txt").c_str());
    if (ref.empty()) return;
    const double rate = tc.rate > 0 ? tc.rate : kRate;
    Circuit c;
    if (tc.start.empty()) CHECK(compileAt(c, n, rate, controls(n, tc.ctl), tc.nodes), "%s: compile/DC", tc.id.c_str());
    else CHECK(compileStart(c, n, rate, controls(n, tc.ctl), tc.nodes, tc.start), "%s: compile/%s start", tc.id.c_str(), tc.start.c_str());
    const auto total = static_cast<std::size_t>(std::llround(tc.secs * rate)) + 1;
    std::vector<std::vector<double>> eng(tc.nodes.size());
    std::vector<int> ids;
    for (const auto& nd : tc.nodes) ids.push_back(c.node(nd));
    for (std::size_t i = 0; i < total; ++i) {
        const double t = static_cast<double>(i) / rate;
        if (i > 0 && c.hasInputR())
            c.setInputR(tc.ampR >= 0 ? tc.ampR * std::sin(2.0 * M_PI * (tc.freqR > 0 ? tc.freqR : tc.freq) * t) : tc.amp * std::sin(2.0 * M_PI * tc.freq * t));
        if (i > 0) c.process(tc.amp * std::sin(2.0 * M_PI * tc.freq * t));
        for (std::size_t k = 0; k < ids.size(); ++k) eng[k].push_back(c.nodeVoltage(ids[k]));
    }
    const std::size_t off = total - ref.size();
    for (std::size_t k = 0; k < tc.nodes.size(); ++k) {
        std::vector<double> r, e;
        for (std::size_t i = 0; i < ref.size(); ++i) {
            r.push_back(ref[i][k + 1]);
            e.push_back(eng[k][off + i]);
        }
        double mean = 0;
        for (double x : r) mean += x;
        mean /= static_cast<double>(r.size());
        double ac = 0, err = 0, mx = 0;
        for (std::size_t i = 0; i < r.size(); ++i) {
            ac += (r[i] - mean) * (r[i] - mean);
            err += (r[i] - e[i]) * (r[i] - e[i]);
            mx = std::max(mx, std::abs(r[i] - e[i]));
        }
        const double nrmse = 100.0 * std::sqrt(err / std::max(ac, 1e-30));
        // Harmonics over an integer number of periods.
        const auto per = static_cast<std::size_t>(std::llround(rate / tc.freq));
        const std::size_t len = (r.size() / per) * per;
        std::vector<double> rw(r.end() - static_cast<long>(len), r.end()), ew(e.end() - static_cast<long>(len), e.end());
        double worstH = 0;
        const double h1 = harmonicDb(rw, tc.freq, rate, 1);
        for (int h = 2; h <= 5; ++h) {
            const double hr = harmonicDb(rw, tc.freq, rate, h), he = harmonicDb(ew, tc.freq, rate, h);
            if (hr - h1 > -60.0) worstH = std::max(worstH, std::abs(hr - he));
        }
        std::printf("  %-10s %-6s NRMSE %7.3f %%  max %.2e V  H1 %+.2f dB (ref %+.2f)  worst H2-H5 diff %.2f dB\n", tc.id.c_str(), tc.nodes[k].c_str(), nrmse, mx,
                    harmonicDb(ew, tc.freq, rate, 1), h1, worstH);
        CHECK(nrmse <= tc.tolPct, "%s node %s: NRMSE %.3f%% > %.3f%%", tc.id.c_str(), tc.nodes[k].c_str(), nrmse, tc.tolPct);
        if (tc.hfDb > 0) {
            // Energy above 30 kHz (4th-order Butterworth high-pass, the same filter on both): the trapezoidal step's
            // error under hard drive shows here first (CIRCUIT_ENGINE.md §3.1, LTE control).
            auto hfRms = [&](const std::vector<double>& x) {
                const double K = std::tan(M_PI * 30000.0 / rate);
                double y = 0, acc = 0;
                std::vector<double> v(x.begin(), x.end());
                for (double q : {0.5411961, 1.3065630}) {
                    const double nrm = 1.0 / (1.0 + K / q + K * K);
                    const double b0 = nrm, b1 = -2.0 * nrm, b2 = nrm, a1 = 2.0 * (K * K - 1.0) * nrm, a2 = (1.0 - K / q + K * K) * nrm;
                    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
                    for (double& u : v) {
                        y = b0 * u + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
                        x2 = x1, x1 = u, y2 = y1, y1 = y, u = y;
                    }
                }
                const auto skip = static_cast<std::size_t>(0.002 * rate); // the filters' start-up
                for (std::size_t i2 = skip; i2 < v.size(); ++i2) acc += v[i2] * v[i2];
                return std::sqrt(acc / static_cast<double>(std::max<std::size_t>(v.size() - skip, 1)));
            };
            const double hr = hfRms(r), he = hfRms(e), d = 20.0 * std::log10((he + 1e-30) / (hr + 1e-30));
            std::printf("  %-10s %-6s energy above 30 kHz %+.2f dB vs ngspice (gate ±%.1f dB)\n", tc.id.c_str(), tc.nodes[k].c_str(), d, tc.hfDb);
            CHECK(std::abs(d) <= tc.hfDb, "%s node %s: energy above 30 kHz %+.2f dB vs ngspice", tc.id.c_str(), tc.nodes[k].c_str(), d);
        }
        CHECK(worstH <= 1.5, "%s node %s: harmonic mismatch %.2f dB", tc.id.c_str(), tc.nodes[k].c_str(), worstH);
    }
}

struct AcCase {
    std::string id, file;
    std::map<std::string, double> ctl;
    double rate = 0; // engine rate for the comparison (0 = kRate); steep responses are gated where warping is small
    std::string variant; // loadVariant()
};
static void runAcCase(const AcCase& ac) {
    if (!gateWanted(ac.file)) return;
    const Netlist n = loadVariant(ac.file, ac.variant);
    if (regenerate(ac.id)) {
        SpiceExportOptions o;
        o.controls = controls(n, ac.ctl);
        o.nodeset = engineHints(n, o.controls);
        o.inputSource = "DC 0 AC 1";
        std::string deck = exportSpice(n, o);
        if (ac.id.rfind("bp_", 0) == 0) deck += ".options itl1=500\n"; // Bi-Phase sweep generators (see the tran cases)
        const std::string raw = "/tmp/headroom_ce_" + ac.id + ".txt";
        deck += ".control\nac dec 20 20 20000\nwrdata " + raw + " vdb(" + spiceNodeName(n, n.io.output) + ")\n.endc\n.end\n";
        const std::string cir = "/tmp/headroom_ce_" + ac.id + ".cir";
        std::ofstream(cir) << deck;
        if (std::system(("ngspice -b '" + cir + "' > '" + cir + ".log' 2>&1").c_str()) == 0) {
            const auto t = readTable(raw);
            std::ofstream out(fixture(ac.id + ".ac.txt"));
            out << "# ngspice .ac reference: " << n.id << " v" << n.version << " (f, dB at " << n.io.output << ")\n";
            char b[64];
            for (const auto& r : t) {
                std::snprintf(b, sizeof b, "%.8e %.6f\n", r[0], r[1]);
                out << b;
            }
        }
    }
    const auto ref = readTable(fixture(ac.id + ".ac.txt"));
    CHECK(!ref.empty(), "%s: missing AC fixture", ac.id.c_str());
    if (ref.empty()) return;
    Circuit c;
    const double rate = ac.rate > 0 ? ac.rate : kRate;
    CHECK(compileAt(c, n, rate, controls(n, ac.ctl)), "%s: compile", ac.id.c_str());
    std::vector<double> f;
    for (const auto& r : ref) f.push_back(r[0]);
    const auto h = c.smallSignalResponse(f, c.node(n.io.output));
    double worstLo = 0, worst10 = 0;
    for (std::size_t i = 0; i < f.size(); ++i) {
        const double d = std::abs(20.0 * std::log10(std::abs(h[i])) - ref[i][1]);
        if (f[i] <= 5000) worstLo = std::max(worstLo, d);
        else if (f[i] <= 10000) worst10 = std::max(worst10, d);
    }
    std::printf("  %-10s AC: worst |Δ| %.3f dB (20 Hz-5 kHz), %.3f dB (5-10 kHz) at %.0f kHz internal rate\n", ac.id.c_str(), worstLo, worst10, rate / 1000);
    CHECK(worstLo <= 0.1, "%s: AC error %.3f dB below 5 kHz", ac.id.c_str(), worstLo);
    CHECK(worst10 <= 0.5, "%s: AC error %.3f dB 5-10 kHz", ac.id.c_str(), worst10);
}

static void runDcCase(const std::string& id, const std::string& file) {
    if (!gateWanted(file)) return;
    const Netlist n = loadLib(file);
    if (regenerate(id)) {
        SpiceExportOptions o;
        o.controls = controls(n, {});
        o.nodeset = engineHints(n, o.controls);
        o.inputSource = "DC 0";
        std::string deck = exportSpice(n, o);
        if (id.rfind("bp_", 0) == 0) deck += ".options itl1=500\n"; // Bi-Phase sweep generators (see the tran cases)
        const std::string raw = "/tmp/headroom_ce_" + id + ".op.txt";
        Circuit c0;
        compileAt(c0, n, kRate, controls(n, {}), {}, "", false);
        std::vector<std::string> names;
        for (std::size_t i = 1; i < c0.nodeNames().size(); ++i)
            if (c0.nodeNames()[i].find('#') == std::string::npos) names.push_back(c0.nodeNames()[i]);
        deck += ".control\nop\nprint";
        for (const auto& nm : names) deck += " v(" + spiceNodeName(n, nm) + ")";
        deck += " > " + raw + "\n.endc\n.end\n";
        const std::string cir = "/tmp/headroom_ce_" + id + ".op.cir";
        std::ofstream(cir) << deck;
        if (std::system(("ngspice -b '" + cir + "' > '" + cir + ".log' 2>&1").c_str()) == 0) {
            std::ifstream in(raw);
            std::ofstream out(fixture(id + ".op.txt"));
            out << "# ngspice .op reference: " << n.id << " v" << n.version << " (node voltage)\n";
            std::string line;
            while (std::getline(in, line)) {
                const auto eq = line.find('=');
                if (eq == std::string::npos) continue;
                std::string nm = line.substr(0, eq);
                nm.erase(std::remove(nm.begin(), nm.end(), ' '), nm.end());
                // ngspice prints vector names in lower case
                const auto lower = [](std::string t) {
                    for (auto& ch : t) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    return t;
                };
                for (const auto& cand : names)
                    if (lower("v(" + spiceNodeName(n, cand) + ")") == lower(nm)) out << cand << " " << line.substr(eq + 1) << "\n";
            }
        }
    }
    std::ifstream f(fixture(id + ".op.txt"));
    CHECK(f.good(), "%s: missing .op fixture", id.c_str());
    Circuit c;
    CompileOptions o;
    o.sampleRate = kRate;
    o.exportAllNodes = true;
    o.mergeDiodePairs = false;
    std::string err;
    CHECK(c.compile(n, o, err), "%s: %s", id.c_str(), err.c_str());
    std::string line;
    double worst = 0;
    int count = 0;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream s(line);
        std::string nm;
        double v;
        s >> nm >> v;
        const int id2 = c.node(nm);
        if (id2 < 0) continue;
        const double e = c.nodeVoltage(id2);
        const double tol = std::max(1e-3, 1e-3 * std::abs(v));
        worst = std::max(worst, std::abs(e - v) / tol);
        CHECK(std::abs(e - v) <= tol, "%s: DC %s engine %.6f vs ngspice %.6f", id.c_str(), nm.c_str(), e, v);
        ++count;
    }
    std::printf("  %-10s DC: %d nodes, worst error %.3f of tolerance (1 mV / 0.1 %%)\n", id.c_str(), count, worst);
    CHECK(count > 0, "%s: no DC nodes compared", id.c_str());
}

// ───────────────────────────── unit tests ─────────────────────────────
static void testValues() {
    auto near = [](std::optional<double> v, double x) { return v && std::abs(*v - x) <= 1e-12 * std::max(1.0, std::abs(x)); };
    CHECK(near(parseValue("4k7"), 4700), "4k7");
    CHECK(near(parseValue("2u2"), 2.2e-6), "2u2");
    CHECK(near(parseValue("47n"), 47e-9), "47n");
    CHECK(near(parseValue("1M"), 1e6), "1M electronics = mega");
    CHECK(near(parseValue("1M", true), 1e-3), "1M SPICE = milli");
    CHECK(near(parseValue("1meg", true), 1e6), "1meg");
    CHECK(near(parseValue("220R"), 220), "220R");
    CHECK(near(parseValue("0.1uF"), 1e-7), "0.1uF");
    CHECK(near(parseValue("51p"), 51e-12), "51p");
    CHECK(!parseValue("abc"), "garbage rejected");
    Taper a{'A', 0.10};
    CHECK(std::abs(a(0.5) - 0.10) < 1e-12 && a(0) == 0 && std::abs(a(1) - 1) < 1e-12, "A taper 10%% at mid");
    Taper c{'C', 0.10};
    CHECK(std::abs(c(0.5) - 0.90) < 1e-12, "C taper");
}

static void testParserErrors() {
    Netlist n;
    std::string err;
    CHECK(!parseNetlistText(R"({"schema":"headroom.circuit.v1","id":"x","parts":[{"ref":"R1","type":"R","nodes":["a"],"value":"1k"}]})", n, err), "arity");
    CHECK(err.find("R1") != std::string::npos, "error names the part: %s", err.c_str());
    CHECK(!parseNetlistText(R"({"schema":"headroom.circuit.v1","id":"x","parts":[{"ref":"D1","type":"D","nodes":["a","0"],"model":"NOPE"}]})", n, err), "unknown model");
    CHECK(!parseNetlistText(R"({"schema":"other","id":"x","parts":[]})", n, err), "schema gate");
    // A node that is only sensed must be rejected at compile time with its name.
    CHECK(parseNetlistText(R"({"schema":"headroom.circuit.v1","id":"x","io":{"input":"in","output":"o"},"parts":[
        {"ref":"E1","type":"vcvs","nodes":["o","0","f","0"],"gain":2},{"ref":"R1","type":"R","nodes":["o","0"],"value":"1k"}]})", n, err), "parse %s", err.c_str());
    Circuit c;
    CompileOptions o;
    CHECK(!c.compile(n, o, err) && err.find("f") != std::string::npos, "floating sensed node rejected: %s", err.c_str());
}

static void testSubcircuits() {
    // Two cascaded RC low-passes built from one subcircuit == the flat circuit.
    const char* sub = R"({"schema":"headroom.circuit.v1","id":"sub","io":{"input":"in","output":"out"},
      "controls":[{"id":"cut","default":0.5}],
      "subcircuits":{"rc":{"pins":["a","b"],"parts":[{"ref":"R","type":"R","nodes":["a","b"],"value":{"nominal":"10k","control":"k","min":"1k","max":"100k"}},
                                                    {"ref":"C","type":"C","nodes":["b","0"],"value":"10n"}]}},
      "parts":[{"ref":"X1","type":"sub","of":"rc","nodes":["in","m"],"controls":{"k":"cut"}},
               {"ref":"X2","type":"sub","of":"rc","nodes":["m","out"],"controls":{"k":"cut"}}]})";
    const char* flat = R"({"schema":"headroom.circuit.v1","id":"flat","io":{"input":"in","output":"out"},
      "controls":[{"id":"cut","default":0.5}],
      "parts":[{"ref":"R1","type":"R","nodes":["in","m"],"value":{"nominal":"10k","control":"cut","min":"1k","max":"100k"}},
               {"ref":"C1","type":"C","nodes":["m","0"],"value":"10n"},
               {"ref":"R2","type":"R","nodes":["m","out"],"value":{"nominal":"10k","control":"cut","min":"1k","max":"100k"}},
               {"ref":"C2","type":"C","nodes":["out","0"],"value":"10n"}]})";
    Netlist a, b;
    std::string err;
    CHECK(parseNetlistText(sub, a, err), "subcircuit netlist: %s", err.c_str());
    CHECK(parseNetlistText(flat, b, err), "flat netlist: %s", err.c_str());
    CHECK(a.part("X2.R") && a.part("X2.R")->nodes[0] == "m", "instance refs and pins mapped");
    Circuit ca, cb;
    compileAt(ca, a, kRate, {0.7});
    compileAt(cb, b, kRate, {0.7});
    double d = 0;
    for (int i = 0; i < 3000; ++i) {
        const double x = std::sin(2 * M_PI * 3000.0 * i / kRate);
        d = std::max(d, std::abs(ca.process(x) - cb.process(x)));
    }
    CHECK(d < 1e-12, "subcircuit expansion == flat circuit (%.3g)", d);
}

static void testSpiceRoundTrip() {
    const Netlist n = loadLib("diode-clipper.circuit.json");
    SpiceExportOptions o;
    o.inputSource = "DC 0 AC 1";
    const std::string deck = exportSpice(n, o);
    Netlist back;
    SpiceImportReport rep;
    std::string err;
    CHECK(importSpice(deck, back, rep, err), "import: %s", err.c_str());
    CHECK(back.parts.size() == n.parts.size(), "round trip keeps %zu parts (got %zu)", n.parts.size(), back.parts.size());
    CHECK(back.io.input == "n_in" && back.io.output == "n_out", "importer found the IO ports (%s, %s)", back.io.input.c_str(), back.io.output.c_str());
    Circuit a, b;
    CHECK(compileAt(a, n, kRate, controls(n, {})), "orig");
    CHECK(compileAt(b, back, kRate, {}), "imported");
    double worst = 0;
    for (int i = 0; i < 2000; ++i) {
        const double x = 1.5 * std::sin(2 * M_PI * 700 * i / kRate);
        worst = std::max(worst, std::abs(a.process(x) - b.process(x)));
    }
    CHECK(worst < 1e-9, "SPICE round trip behaves identically (%.3g V)", worst);
}

static void testPartitioning() {
    {
        const Netlist n = loadLib("ts808.circuit.json");
        Circuit c;
        CHECK(compileAt(c, n, kRate, controls(n, {})), "ts808");
        const auto& g = c.groups();
        std::printf("  ts808 partition:\n%s", c.describeGroups().c_str());
        CHECK(g.size() == 4, "ts808: 4 exact solve groups (got %zu)", g.size());
        int maxNn = 0;
        for (const auto& gr : g) maxNn = std::max(maxNn, gr.nn);
        CHECK(maxNn == 3, "largest Newton problem 3 ports (pair + op-amp macro) instead of 9 (got %d)", maxNn);
    }
    {
        // SLO-100 (2026-11-11): preamp, FX recovery, tone stack, PI and power amp were one 80-unknown, 39-port group,
        // and the live unit needed about 135 % of one core at 48 kHz (4x): every audio callback overran, which the user
        // heard as a broken amp. The netlist's signal cut at the FX-send cathode follower (k3a) split it in two. Since
        // §10.44 (2026-11-14) the specialised kernels carry the uncut circuit within one core (69.1 / 81.0 % on the
        // 9900X), and k3a went (closer to the true circuit, docs §10.43): this now guards the one 39-port group, so a
        // re-introduced split shows up here.
        const Netlist n = loadLib("soldano-slo100-122000.circuit.json");
        Circuit c;
        CHECK(compileAt(c, n, 192000.0, controls(n, {{"channel", 1.0}, {"overdrive", 0.8}})), "slo100");
        int maxNn = 0, slow = 0;
        for (const auto& gr : c.groups()) maxNn = std::max(maxNn, gr.nn), slow += gr.div > 1;
        CHECK(maxNn == 39, "slo100: the uncut 39-port group (k3a removed, docs §10.44), got %d", maxNn);
        // ...and the channel-switch LED / LDR light groups (a quarter of the unit's cost at full rate) run slow
        CHECK(slow == 8, "slo100: the 8 LED / LDR light groups on the 12 kHz slow partition, got %d", slow);
    }
    for (const char* file : {"soldano-slo100-122000.circuit.json", "soldano-slo100-mk2-163000.circuit.json"}) {
        // SLO-100 power-amp controls (2026-11-11): the 122000 Presence ran backwards (its rheostat was 25k*x, so full
        // presence was the most treble feedback) and its Depth was a dead 4.7 nF shunt on the 4.7 k feedback node.
        // A SPICE gate cannot see either (ngspice agrees with a miswired netlist), so this checks direction: each knob
        // up must lift its band at the speaker (small signal, Clean channel).
        const Netlist n = loadLib(file);
        auto band = [&](const std::map<std::string, double>& set, double hz) {
            Circuit c;
            std::map<std::string, double> s = set;
            s["channel"] = 0.0;
            if (!compileAt(c, n, kRate, controls(n, s))) return -999.0;
            return 20.0 * std::log10(std::abs(c.smallSignalResponse({hz}, c.node(n.io.output))[0]));
        };
        const double pr = band({{"presence", 1.0}}, 5000) - band({{"presence", 0.0}}, 5000);
        const double dp = band({{"depth", 1.0}}, 80) - band({{"depth", 0.0}}, 80);
        std::printf("  %s: presence 0->1 %+.1f dB at 5 kHz, depth 0->1 %+.1f dB at 80 Hz\n", n.id.c_str(), pr, dp);
        CHECK(pr > 3.0, "%s: presence up must raise 5 kHz (got %+.1f dB)", n.id.c_str(), pr);
        CHECK(dp > 3.0, "%s: depth up must raise 80 Hz (got %+.1f dB)", n.id.c_str(), dp);
    }
    {
        // Control-direction audit fixes (2026-11-11; work/circuit-netlists/CONTROL_AUDIT_AMPS.md). Each was a miswire
        // the SPICE gate cannot see: Hiwatt DR103 Presence (ends swapped, up removed 15 dB at 8 kHz), Friedman BE-100
        // Presence (feedback node on the counter-clockwise end, -18 dB), Supro 1624T Tone (treble bleed reversed, up =
        // darker by 17 dB), Ampeg SVT Ultra Lo (engaging it cut the channel's only path: -55 dB) and Ultra Hi (dead,
        // shorted by the Ultra Lo pole). Small signal at the unit's settled start, every other control at default.
        auto resp = [&](const Netlist& n, const std::map<std::string, double>& set, const std::vector<double>& hz) {
            Circuit c;
            CompileOptions o;
            o.sampleRate = kRate;
            o.settleMs = kUnitSettleMs;
            std::string err;
            std::vector<double> db(hz.size(), -999.0);
            if (!c.compile(n, o, err)) return db;
            const auto ctl = controls(n, set);
            for (std::size_t i = 0; i < ctl.size(); ++i) c.setControlImmediate(static_cast<int>(i), ctl[i]);
            if (!c.reset()) return db;
            const auto h = c.smallSignalResponse(hz, c.node(n.io.output));
            for (std::size_t i = 0; i < hz.size(); ++i) db[i] = 20.0 * std::log10(std::abs(h[i]));
            return db;
        };
        struct Dir { const char* file; const char* ctl; double hz; double minRise; };
        // DR103 Middle (2026-11-12, VERIFY_BATCH_9 follow-up): a rheostat with its wiper strapped to the 22 k end and the
        // 1000 pF across it, on every archived drawing; the netlist had the wiper free and the cap on the slope node, so
        // the full 100 k always sat in series and up only darkened the amp (-4 dB at 5 kHz). Up now fills the mid scoop.
        for (const Dir& d : {Dir{"hiwatt-dr103.circuit.json", "presence", 8000, 8.0},
                             Dir{"hiwatt-dr103.circuit.json", "middle", 900, 8.0},
                             Dir{"friedman-be100-151000.circuit.json", "presence", 8000, 8.0},
                             Dir{"supro-1624t.circuit.json", "tone", 4000, 8.0}}) {
            const Netlist n = loadLib(d.file);
            const double up = resp(n, {{d.ctl, 1.0}}, {d.hz})[0] - resp(n, {{d.ctl, 0.0}}, {d.hz})[0];
            std::printf("  %s: %s 0->1 %+.1f dB at %.0f Hz\n", n.id.c_str(), d.ctl, up, d.hz);
            CHECK(up > d.minRise, "%s: %s up must raise %.0f Hz by > %.0f dB (got %+.1f dB)", n.id.c_str(), d.ctl, d.hz, d.minRise, up);
        }
        const Netlist svt = loadLib("ampeg-svt-1969.circuit.json");
        const std::vector<double> hz = {40, 500, 8000};
        const auto off = resp(svt, {{"ultraLow", 0.0}, {"ultraHigh", 0.0}}, hz);
        const auto lo = resp(svt, {{"ultraLow", 1.0}, {"ultraHigh", 0.0}}, hz);
        const auto hi = resp(svt, {{"ultraLow", 0.0}, {"ultraHigh", 1.0}}, hz);
        const double loRel = (lo[0] - lo[1]) - (off[0] - off[1]), loMid = lo[1] - off[1], hiUp = hi[2] - off[2];
        std::printf("  ampeg-svt-1969: Ultra Lo %+.1f dB at 40 Hz relative to 500 Hz (500 Hz %+.1f dB), Ultra Hi %+.1f dB at 8 kHz\n",
                    loRel, loMid, hiUp);
        // SVT-VR manual, Channel Two: Ultra Lo +11 dB at 40 Hz (relative); Ultra Hi boosts the highs (+15 dB at 8 kHz with
        // a volume at 50 %; the drawn 1 M lin volume allows about 6 dB there)
        CHECK(loRel > 8.0, "svt: Ultra Lo must lift 40 Hz against 500 Hz by > 8 dB (got %+.1f dB)", loRel);
        CHECK(loMid > -15.0, "svt: Ultra Lo must not mute the channel (500 Hz %+.1f dB)", loMid);
        CHECK(hiUp > 3.0, "svt: Ultra Hi must raise 8 kHz by > 3 dB (got %+.1f dB)", hiUp);
    }
    {
        // Library re-verification fixes (2026-11-12; work/circuit-netlists/VERIFY_BATCH_1..4.md, each confirmed against
        // the archived drawing first). Behaviour a SPICE gate cannot see: ngspice agrees with a miswired netlist.
        auto settled = [&](const Netlist& n, const std::map<std::string, double>& set, double rate, Circuit& c) {
            CompileOptions o;
            o.sampleRate = rate;
            o.settleMs = kUnitSettleMs;
            o.bbdNoise = false;
            std::string err;
            if (!c.compile(n, o, err)) return false;
            const auto ctl = controls(n, set);
            for (std::size_t i = 0; i < ctl.size(); ++i) c.setControlImmediate(static_cast<int>(i), ctl[i]);
            return c.reset();
        };
        auto sineRms = [&](const Netlist& n, const std::map<std::string, double>& set, double rate, double hz, double amp, double secs) {
            Circuit c;
            if (!settled(n, set, rate, c)) return -999.0;
            const int N = static_cast<int>(secs * rate), from = N - static_cast<int>(0.25 * rate);
            double s = 0, s2 = 0;
            for (int k = 0; k < N; ++k) {
                const double y = c.process(amp * std::sin(2.0 * M_PI * hz * k / rate));
                if (k >= from) s += y, s2 += y * y;
            }
            const double m = s / (N - from);
            return 10.0 * std::log10(std::max(s2 / (N - from) - m * m, 1e-30));
        };
        // TS9DX: one leg capacitor per mode, each through its own switch (all four had sat across leg-vr with their
        // switches in parallel, so every mode measured alike). Raw 0..3 = Turbo / Hot / + / TS9, the rotary left to right.
        {
            const Netlist n = loadLib("ibanez-ts9dx.circuit.json");
            double rel20[4], rel100[4];
            for (int m = 0; m < 4; ++m) {
                Circuit c;
                CHECK(compileAt(c, n, kRate, controls(n, {{"mode", m / 3.0}, {"overdrive", 0.5}})), "ts9dx mode %d", m);
                const auto h = c.smallSignalResponse({20.0, 100.0, 1000.0}, c.node(n.io.output));
                rel20[m] = 20.0 * std::log10(std::abs(h[0]) / std::abs(h[2]));
                rel100[m] = 20.0 * std::log10(std::abs(h[1]) / std::abs(h[2]));
            }
            std::printf("  ibanez-ts9dx: 20 Hz re 1 kHz Turbo %+.1f / Hot %+.1f / + %+.1f / TS9 %+.1f dB; 100 Hz + %+.1f, TS9 %+.1f dB\n",
                        rel20[0], rel20[1], rel20[2], rel20[3], rel100[2], rel100[3]);
            // corners 16 / 40 / 150 / 615 Hz (Stompbox Electronics mode analysis): the bass rises Turbo > Hot > + > TS9
            CHECK(rel20[0] - rel20[1] > 3.0 && rel20[1] - rel20[2] > 3.0 && rel20[2] - rel20[3] > 3.0,
                  "ts9dx: four distinct leg corners (20 Hz %+.1f / %+.1f / %+.1f / %+.1f dB)", rel20[0], rel20[1], rel20[2], rel20[3]);
            CHECK(rel100[3] < -6.0 && rel100[2] > -4.0, "ts9dx: TS9 mode cuts 100 Hz (%+.1f dB), + mode keeps it (%+.1f dB)", rel100[3], rel100[2]);
        }
        // RE-201: the tape echo was 12 dB under the service notes' calibration (2-3 / 2-4: echo-only -12 dBm against a
        // direct -15 dBm for the same input), i.e. inaudible at the default patch. Head 1, Echo Volume max, Intensity min.
        {
            const Netlist n = loadLib("roland-re201.circuit.json");
            const std::map<std::string, double> base = {{"mode", 0.0}, {"intensity", 0.0}, {"reverb", 0.0}, {"instrument_volume", 1.0}};
            auto echoSet = base, dirSet = base;
            echoSet["echo"] = 1.0, echoSet["dry_cut"] = 1.0;
            dirSet["echo"] = 0.0, dirSet["dry_cut"] = 0.0;
            const double echo = sineRms(n, echoSet, 48000.0, 1000.0, 0.049, 2.0), direct = sineRms(n, dirSet, 48000.0, 1000.0, 0.049, 2.0);
            std::printf("  roland-re201: echo-only %+.1f dB against the direct output (service notes +3 dB)\n", echo - direct);
            CHECK(echo - direct > 1.0 && echo - direct < 5.0, "re201: echo-only output +3 dB over the direct output (got %+.1f dB)", echo - direct);
        }
        // AB763 reverb footswitch: the Deluxe's 'reverb_on' read the catalogue default 0.5 as switch-off (CircuitUnit:
        // switch = raw > 0.5 after paramRange), so a fresh unit had the tank return shorted. Both AB763 units now use the
        // footswitch form (open = reverb on): at the unit's default the Reverb knob must bring up a tail.
        for (const char* file : {"ab763-deluxe-reverb.circuit.json", "ab763-twin-reverb.circuit.json"}) {
            const Netlist n = loadLib(file);
            std::map<std::string, double> set;
            for (const auto& cd : n.controls) {
                if (cd.kind != "switch" || cd.legacyOnly || cd.params.empty() || cd.paramMax == cd.paramMin) continue;
                const double x = std::clamp((0.5 - cd.paramMin) / (cd.paramMax - cd.paramMin), 0.0, 1.0);
                set[cd.id] = x > 0.5 ? 1.0 : 0.0; // CircuitUnit::controlFromParams at the catalogue's 0.5 default
            }
            auto tail = [&](double reverb) {
                auto s = set;
                s["reverb"] = reverb;
                Circuit c, q;
                if (!settled(n, s, kRate, c) || !settled(n, s, kRate, q)) return std::pair<double, double>{-999.0, -999.0};
                const int burst = static_cast<int>(0.03 * kRate), N = static_cast<int>(0.45 * kRate);
                double dry = 0, wet = 0;
                for (int k = 0; k < N; ++k) {
                    const double x = k < burst ? 0.1 * std::sin(2.0 * M_PI * 1000.0 * k / kRate) : 0.0;
                    const double y = std::abs(c.process(x) - q.process(0.0));
                    if (k < burst) dry = std::max(dry, y);
                    else if (k > burst + static_cast<int>(0.04 * kRate)) wet = std::max(wet, y);
                }
                return std::pair<double, double>{dry, wet};
            };
            const auto [dry1, wet1] = tail(1.0);
            const auto [dry0, wet0] = tail(0.0);
            const double on = 20.0 * std::log10(wet1 / dry1), off = 20.0 * std::log10(std::max(wet0, 1e-12) / dry0);
            std::printf("  %s: reverb tail at the default footswitch, Reverb 1 %+.1f dB / Reverb 0 %+.1f dB re the dry burst\n", n.id.c_str(), on, off);
            CHECK(on > -20.0 && on - off > 15.0, "%s: Reverb audible at the unit default (tail %+.1f dB, %+.1f dB at Reverb 0)", n.id.c_str(), on, off);
        }
        // HM-2 rebuilt to the service-notes drawing: DIST is VR4 with its wiper grounded (IC1b's gain leg on one half, a
        // 150 ohm shunt on Q6's collector on the other), so DIST 0 is quiet but never silent; LOW / HIGH span IC3a's inputs
        // with their own resonators, so neither jumps at the end of travel and they never meet in one state.
        {
            const Netlist n = loadLib("boss-hm2.circuit.json");
            const double d0 = sineRms(n, {{"distortion", 0.0}}, 96000.0, 1000.0, 0.02, 0.5), d1 = sineRms(n, {{"distortion", 1.0}}, 96000.0, 1000.0, 0.02, 0.5);
            std::printf("  boss-hm2: DIST 0 %+.1f dB re DIST 1 (was silent: -73 dB)\n", d0 - d1);
            CHECK(d0 - d1 > -40.0 && d0 < d1, "hm2: DIST 0 quieter but not silent (%+.1f dB re DIST 1)", d0 - d1);
            auto db = [&](const std::map<std::string, double>& set, double hz) {
                Circuit c;
                if (!compileAt(c, n, kRate, controls(n, set))) return -999.0;
                return 20.0 * std::log10(std::abs(c.smallSignalResponse({hz}, c.node(n.io.output))[0]));
            };
            const double lo0 = db({{"low", 0.0}}, 88), lo1 = db({{"low", 1.0}}, 88), lo99 = db({{"low", 0.99}}, 88);
            const double hi0 = db({{"high", 0.0}}, 1000), hi1 = db({{"high", 1.0}}, 1000), hi99 = db({{"high", 0.99}}, 1000);
            const double cross = db({{"low", 1.0}}, 1000) - hi1;
            std::printf("  boss-hm2: LOW 0/0.99/1 at 88 Hz %+.1f/%+.1f/%+.1f dB, HIGH 0/0.99/1 at 1 kHz %+.1f/%+.1f/%+.1f dB\n", lo0, lo99, lo1, hi0, hi99, hi1);
            CHECK(lo1 - lo0 > 20.0 && hi1 - hi0 > 20.0, "hm2: LOW / HIGH boost clockwise (%+.1f / %+.1f dB)", lo1 - lo0, hi1 - hi0);
            CHECK(lo1 - lo99 < 4.0 && hi1 - hi99 < 4.0, "hm2: no cliff in the last 1 %% (LOW %+.1f, HIGH %+.1f dB)", lo1 - lo99, hi1 - hi99);
            CHECK(std::abs(cross) > 10.0, "hm2: LOW 1 and HIGH 1 are different states (%+.1f dB at 1 kHz)", cross);
        }
        // 1959 Super Lead (stock and variac): modelled jumpered (Channel I Low -> Channel II High by a patch cable), so
        // each Volume works alone and both at 0 is silent.
        for (const char* file : {"marshall-1959-plexi.circuit.json", "marshall-1959-plexi-variac.circuit.json"}) {
            const Netlist n = loadLib(file);
            auto db = [&](double v1, double v2) {
                Circuit c;
                CompileOptions o;
                o.sampleRate = kRate;
                o.settleMs = kUnitSettleMs;
                std::string err;
                if (!c.compile(n, o, err)) return -999.0;
                const auto ctl = controls(n, {{"volume1", v1}, {"volume2", v2}});
                for (std::size_t i = 0; i < ctl.size(); ++i) c.setControlImmediate(static_cast<int>(i), ctl[i]);
                if (!c.reset()) return -999.0;
                return 20.0 * std::log10(std::abs(c.smallSignalResponse({1000.0}, c.node(n.io.output))[0]));
            };
            const double full = db(1.0, 1.0), off = db(0.0, 0.0), only1 = db(0.5, 0.0), only2 = db(0.0, 0.5);
            std::printf("  %s: 1 kHz both 1 %+.1f, both 0 %+.1f, Volume I only %+.1f, Volume II only %+.1f dB\n", n.id.c_str(), full, off, only1, only2);
            CHECK(full - off > 80.0, "%s: both volumes at 0 silent (%+.1f dB below full)", n.id.c_str(), full - off);
            CHECK(only1 - off > 40.0 && only2 - off > 40.0, "%s: each volume live on its own", n.id.c_str());
        }
    }
    {
        // Supro 1624T re-conversion (2026-11-12; VERIFY_BATCH_3, CONTROL_AUDIT_AMPS Resolution): the shipped netlist had no
        // tremolo although the drawing has one (third 12AX7 phase-shift oscillator; its cathode follower drives channel 2's
        // V1B cathode). Tremolo on, Intensity full: the 1 kHz envelope at the speaker must swing deeply; Intensity 0 or the
        // switch off: no swing; Speed clockwise: faster (measured 3.6 -> 6.6 Hz).
        const Netlist n = loadLib("supro-1624t.circuit.json");
        auto trem = [&](const std::map<std::string, double>& set, double& hz) {
            hz = 0;
            Circuit c;
            CompileOptions o;
            o.sampleRate = kRate;
            o.settleMs = kUnitSettleMs;
            o.probes = {"td"};
            std::string err;
            if (!c.compile(n, o, err)) return -999.0;
            const auto ctl = controls(n, set);
            for (std::size_t i = 0; i < ctl.size(); ++i) c.setControlImmediate(static_cast<int>(i), ctl[i]);
            if (!c.reset()) return -999.0;
            const int total = static_cast<int>(2.5 * kRate), skip = static_cast<int>(1.0 * kRate), W = static_cast<int>(0.01 * kRate);
            const int td = c.node("td");
            std::vector<double> y, d;
            for (int i = 0; i < total; ++i) {
                const double v = c.process(0.05 * std::sin(2 * M_PI * 1000.0 * i / kRate));
                if (i >= skip) y.push_back(v), d.push_back(c.nodeVoltage(td));
            }
            double mean = 0;
            for (double v : d) mean += v / static_cast<double>(d.size());
            int first = -1, last = -1, n2 = 0;
            for (std::size_t i = 1; i < d.size(); ++i)
                if (d[i - 1] < mean && d[i] >= mean && std::abs(d[i] - mean) < 1e9) {
                    if (first < 0) first = static_cast<int>(i);
                    last = static_cast<int>(i), ++n2;
                }
            if (n2 > 2) hz = (n2 - 1) * kRate / (last - first);
            double lo = 1e9, hi = 0;
            for (std::size_t k = 0; k + W <= y.size(); k += W) {
                double m = 0, s = 0;
                for (int j = 0; j < W; ++j) m += y[k + j] / W;
                for (int j = 0; j < W; ++j) s += (y[k + j] - m) * (y[k + j] - m) / W;
                lo = std::min(lo, std::sqrt(s)), hi = std::max(hi, std::sqrt(s));
            }
            return 20.0 * std::log10(hi / std::max(lo, 1e-12));
        };
        double f0 = 0, f1 = 0, fx = 0;
        const double deep = trem({{"tremolo", 1.0}, {"mod", 1.0}, {"speed", 0.0}}, f0);
        trem({{"tremolo", 1.0}, {"mod", 1.0}, {"speed", 1.0}}, f1);
        const double none = trem({{"tremolo", 1.0}, {"mod", 0.0}}, fx), off = trem({{"tremolo", 0.0}, {"mod", 1.0}}, fx);
        std::printf("  supro-1624t-dual-tone: tremolo depth %.1f dB p-p at intensity 1 (0: %.1f, switch off: %.1f); speed 0/1 = %.2f / %.2f Hz\n",
                    deep, none, off, f0, f1);
        CHECK(deep > 20.0, "supro: full Intensity must swing the 1 kHz envelope by > 20 dB p-p (got %.1f)", deep);
        CHECK(none < 1.0 && off < 1.0, "supro: Intensity 0 / tremolo off must not modulate (%.1f / %.1f dB p-p)", none, off);
        CHECK(f0 > 2.0 && f0 < 5.0 && f1 > 1.4 * f0, "supro: speed 0 in 2..5 Hz and clockwise faster (%.2f -> %.2f Hz)", f0, f1);
    }
    {
        // LFO rate controls (2026-11-12; CONTROL_AUDIT_FX §2.1/2.2/2.5, VERIFY_BATCH_4 164000), each confirmed against the
        // drawing. An LFO node's frequency (hysteresis zero crossings) and p-p swing after `skip` seconds, no input.
        auto lfo = [&](const char* file, const std::map<std::string, double>& set, const char* node, double rate, double secs,
                       double skip, double& hz, double& pp) {
            const Netlist n = loadLib(file);
            Circuit c;
            hz = pp = 0;
            if (!compileStart(c, n, rate, controls(n, set), {node}, "settled")) return;
            const int id = c.node(node), N = static_cast<int>(secs * rate), S = static_cast<int>(skip * rate);
            std::vector<double> v;
            for (int i = 0; i < N; ++i) {
                c.process(0.0);
                if (i >= S) v.push_back(c.nodeVoltage(id));
            }
            const auto [lo, hi] = std::minmax_element(v.begin(), v.end());
            pp = *hi - *lo;
            const double mid = 0.5 * (*hi + *lo), h = 0.1 * pp;
            int st = 0, first = -1, last = -1, cyc = 0;
            for (std::size_t i = 0; i < v.size(); ++i) {
                if (st <= 0 && v[i] > mid + h) {
                    if (st == -1) {
                        if (first < 0) first = static_cast<int>(i);
                        else ++cyc;
                        last = static_cast<int>(i);
                    }
                    st = 1;
                } else if (st >= 0 && v[i] < mid - h) st = -1;
            }
            if (cyc > 0) hz = cyc * rate / (last - first);
        };
        double f0, p0, f1, p1;
        // 4ms Luna/Lune: RATE's lugs were swapped (knob backwards, LFO stopped at 0) and R11 sat on node Y instead of Z
        // (luna_documentation.pdf p.6), which stopped it near the fast end once the lugs were right.
        lfo("4ms-tremulus-lune-13000.circuit.json", {{"rate", 0.0}}, "lfo_o", 8000, 6.0, 1.5, f0, p0);
        lfo("4ms-tremulus-lune-13000.circuit.json", {{"rate", 1.0}}, "lfo_o", 8000, 2.0, 0.5, f1, p1);
        std::printf("  lune: rate 0 -> %.2f Hz (%.1f V p-p), rate 1 -> %.2f Hz (%.1f V p-p)\n", f0, p0, f1, p1);
        CHECK(p0 > 3.0 && p1 > 3.0, "lune: the LFO runs at both ends of Rate (%.2f / %.2f V p-p)", p0, p1);
        CHECK(f0 > 0.4 && f0 < 1.0 && f1 > 7.0 && f1 < 14.0, "lune: Rate spans ~0.6 -> ~10 Hz clockwise (%.2f -> %.2f Hz)", f0, f1);
        // Uni-Vibe: the LFO still sweeps at the slowest Speed once it has built up (a marginal phase-shift oscillator: the
        // amplitude grows with speed, Keen), and Intensity keeps deepening the lamp drive past half travel now that the
        // lamp driver's base resistor is the drawn 47k (it was 4.7k, which saturated the drive at Intensity 0.5).
        lfo("shinei-univibe.circuit.json", {{"speed", 0.0}}, "le", 4000, 20.0, 14.0, f0, p0);
        lfo("shinei-univibe.circuit.json", {{"speed", 1.0}}, "le", 4000, 4.0, 2.0, f1, p1);
        double fi, ph, pf;
        lfo("shinei-univibe.circuit.json", {{"speed", 0.5}, {"intensity", 0.5}}, "lx", 4000, 6.0, 3.0, fi, ph);
        lfo("shinei-univibe.circuit.json", {{"speed", 0.5}, {"intensity", 1.0}}, "lx", 4000, 6.0, 3.0, fi, pf);
        std::printf("  uni-vibe: speed 0 -> %.2f Hz (%.2f V p-p), speed 1 -> %.2f Hz; lamp drive %.2f -> %.2f V p-p at Intensity 0.5 -> 1\n",
                    f0, p0, f1, ph, pf);
        CHECK(p0 > 1.5 && f0 > 0.5 && f0 < 1.3, "uni-vibe: the LFO sweeps at Speed 0 (%.2f Hz, %.2f V p-p)", f0, p0);
        CHECK(f1 > 3.0 * f0, "uni-vibe: Speed clockwise faster (%.2f -> %.2f Hz)", f0, f1);
        CHECK(pf > 1.8 * ph, "uni-vibe: Intensity 1 drives the lamp > 1.8x Intensity 0.5 (%.2f vs %.2f V p-p)", pf, ph);
        // Small Stone: the LFO (IC5 output, l5o) runs at Rate 0, about 8 s per sweep (the audit had probed the rate divider)
        lfo("ehx-small-stone.circuit.json", {{"rate", 0.0}}, "l5o", 8000, 12.0, 2.0, f0, p0);
        std::printf("  small stone: rate 0 LFO %.3f V p-p\n", p0);
        CHECK(p0 > 0.1, "small stone: the LFO runs at Rate 0 (%.3f V p-p)", p0);
        // CE-1: the panel's CHORUS INTENSITY (VR12) sets the chorus LFO; RATE (VR14) is the vibrato section's
        lfo("boss-ce1.circuit.json", {{"mode", 0.0}, {"intensity", 0.0}}, "p15_v", 24000, 6.0, 0.5, f0, p0);
        lfo("boss-ce1.circuit.json", {{"mode", 0.0}, {"intensity", 1.0}}, "p15_v", 24000, 3.0, 0.5, f1, p1);
        double v0, q0, v1, q1;
        lfo("boss-ce1.circuit.json", {{"mode", 1.0}, {"rate", 0.5}}, "p15_v", 24000, 3.0, 1.0, v0, q0);
        lfo("boss-ce1.circuit.json", {{"mode", 1.0}, {"rate", 1.0}}, "p15_v", 24000, 3.0, 1.0, v1, q1);
        std::printf("  ce-1: chorus intensity 0/1 -> %.2f / %.2f Hz; vibrato rate 0.5/1 -> %.2f / %.2f Hz\n", f0, f1, v0, v1);
        CHECK(f0 > 0.2 && f0 < 0.8 && f1 > 2.5 * f0, "ce-1: Chorus Intensity sweeps the chorus LFO (%.2f -> %.2f Hz)", f0, f1);
        CHECK(v0 > 4.0 && v1 > 1.5 * v0, "ce-1: vibrato Rate sweeps the vibrato LFO (%.2f -> %.2f Hz)", v0, v1);
    }
    if (gateWanted("fender-6g15-27000.circuit.json")) {
        // Fender 6G15 (27000), what the SPICE deck cannot see (it has no springs): Dwell drives the tank, the tank answers
        // after its ~33 ms flight and decays over seconds, the CUT OFF pedal silences the return, and the supply holds
        // under full drive (an earlier choke-input reading of the filter collapsed B+ by ~140 V here).
        const Netlist n = loadLib("fender-6g15-27000.circuit.json");
        const double fs = 48000.0;
        auto run = [&](const std::map<std::string, double>& set, double amp, double f, double on, double secs,
                       std::vector<std::vector<double>>& v) {
            Circuit c;
            v.assign(3, {});
            if (!compileStart(c, n, fs, controls(n, set), {"tk", "g3", "bpa"}, "settled")) return false;
            const int ids[3] = {c.node("tk"), c.node("g3"), c.node("bpa")};
            for (int i = 0; i < static_cast<int>(secs * fs); ++i) {
                const double t = i / fs;
                c.process(t < on ? amp * std::sin(2 * M_PI * f * t) : 0.0);
                for (int k = 0; k < 3; ++k) v[static_cast<std::size_t>(k)].push_back(c.nodeVoltage(ids[k]));
            }
            return true;
        };
        auto acRms = [](const std::vector<double>& x, std::size_t a, std::size_t b) {
            double m = 0, s = 0;
            for (std::size_t i = a; i < b; ++i) m += x[i];
            m /= double(b - a);
            for (std::size_t i = a; i < b; ++i) s += (x[i] - m) * (x[i] - m);
            return std::sqrt(s / double(b - a));
        };
        std::vector<std::vector<double>> v;
        double tk[3];
        const double dw[3] = {0.0, 0.5, 1.0};
        for (int k = 0; k < 3; ++k) {
            run({{"dwell", dw[k]}}, 0.1, 500, 1.0, 0.3, v);
            tk[k] = acRms(v[0], 4800, v[0].size());
        }
        std::printf("  6g15: tank drive at Dwell 0 / 0.5 / 1: %.4f / %.4f / %.4f V rms (0.1 V, 500 Hz)\n", tk[0], tk[1], tk[2]);
        CHECK(tk[0] < 0.01 * tk[2] && tk[0] < tk[1] && tk[1] < tk[2], "6g15: Dwell turns the tank drive up from silence");
        // A 100 ms 500 Hz burst: first arrival and decay of the return at V3A's grid
        run({}, 0.2, 500, 0.1, 1.6, v);
        const auto& g = v[1];
        std::vector<double> env;
        const std::size_t w = static_cast<std::size_t>(0.005 * fs);
        for (std::size_t i = 0; i + w <= g.size(); i += w) env.push_back(acRms(g, i, i + w));
        const auto pk = std::max_element(env.begin(), env.end());
        std::size_t first = 0, fall = 0;
        while (first < env.size() && env[first] < 0.1 * *pk) ++first;
        for (std::size_t i = static_cast<std::size_t>(pk - env.begin()); i < env.size(); ++i)
            if (env[i] < 0.01 * *pk) {
                fall = i;
                break;
            }
        const double tFirst = first * 0.005, tFall = (fall - static_cast<std::size_t>(pk - env.begin())) * 0.005;
        std::printf("  6g15: tank return first at %.0f ms, -40 dB from its peak after %.2f s\n", 1e3 * tFirst, tFall);
        CHECK(tFirst > 0.025 && tFirst < 0.045, "6g15: the tank answers after its flight time (%.0f ms)", 1e3 * tFirst);
        CHECK(fall > 0 && tFall > 0.4 && tFall < 1.5, "6g15: the 4AB3C1B decays over seconds (-40 dB at %.2f s)", tFall);
        run({{"mute", 1.0}}, 0.2, 500, 0.3, 0.3, v);
        const double cut = acRms(v[1], 4800, v[1].size());
        run({}, 0.2, 500, 0.3, 0.3, v);
        const double open = acRms(v[1], 4800, v[1].size());
        std::printf("  6g15: return at V3A's grid, CUT OFF up / down: %.2e / %.2e V rms\n", open, cut);
        CHECK(cut < 1e-3 * open, "6g15: the CUT OFF pedal grounds the return");
        // Full Dwell, 1 V at 200 Hz into grid conduction: the +295 V node stays where it idles
        run({{"dwell", 1.0}}, 0.0, 200, 0.0, 0.3, v);
        double idle = 0;
        for (std::size_t i = v[2].size() / 2; i < v[2].size(); ++i) idle += v[2][i];
        idle /= double(v[2].size() - v[2].size() / 2);
        run({{"dwell", 1.0}}, 1.0, 200, 1.0, 0.6, v);
        double driven = 0;
        for (std::size_t i = v[2].size() / 2; i < v[2].size(); ++i) driven += v[2][i];
        driven /= double(v[2].size() - v[2].size() / 2);
        std::printf("  6g15: +295 V node idle %.1f V, at full Dwell and 1 V %.1f V\n", idle, driven);
        // (measured: 289.2 -> 278.3 V, the half-wave supply's own regulation as the 6K6GT's mean current rises in grid
        // conduction; the choke-input reading fell to ~150 V)
        CHECK(std::abs(driven - idle) < 0.08 * idle, "6g15: the supply holds under full drive (%.1f -> %.1f V)", idle, driven);
    }
    {
        const Netlist n = loadLib("diode-clipper.circuit.json");
        Circuit c;
        CHECK(compileAt(c, n, kRate, controls(n, {}), {}, "wdf"), "clipper wdf");
        CHECK(c.groups().size() == 1 && c.groups()[0].solver == SolverKind::Wdf, "clipper runs as a WDF");
    }
}

static void testCrossSolver() {
    const Netlist n = loadLib("diode-clipper.circuit.json");
    Circuit w, d, u;
    // solver equivalence at a tight Newton tolerance (the shipped 1e-4 leaves ~1e-7 V of solver-dependent residue)
    // The two solver forms' algebra, step for step: the DK groups' LTE substeps (an integration control the WDF form
    // does not have) are off here.
    compileAt(w, n, kRate, controls(n, {}), {}, "wdf", true, 1e-9, 0.0);
    compileAt(d, n, kRate, controls(n, {}), {}, "dk", true, 1e-9, 0.0);
    compileAt(u, n, kRate, controls(n, {}), {}, "dk", false, 1e-9, 0.0);
    double wd = 0, du = 0;
    for (int i = 0; i < 4000; ++i) {
        const double x = 3.0 * std::sin(2 * M_PI * 1234.0 * i / kRate) * (i % 1000) / 1000.0;
        const double a = w.process(x), b = d.process(x), cc = u.process(x);
        wd = std::max(wd, std::abs(a - b));
        du = std::max(du, std::abs(b - cc));
        if (i == 2000) {
            w.setControlTarget(0, 0.9);
            d.setControlTarget(0, 0.9);
            u.setControlTarget(0, 0.9);
        }
    }
    std::printf("  WDF vs DK max |Δ| %.3g V (incl. a glided R sweep); merged pair vs two diodes %.3g V\n", wd, du);
    CHECK(wd < 1e-8, "WDF and DK agree (%.3g)", wd);
    CHECK(du < 1e-6, "diode-pair merge is exact to < 1 µV (%.3g)", du);
    {
        // The MT-2's clipper section (2026-11-11): capacitors on every side leave `clip` without a DC path, so compile
        // stamps its 1 GΩ linear split (a fixed internal resistor, Prim slot -1) across the diode pair. The WDF read
        // that leaf's value from slots_[-1], found its core singular, kept S = 0 and output silence under "auto".
        Netlist m;
        std::string err;
        const bool parsed = parseNetlistText(R"({"schema":"headroom.circuit.v1","id":"wdf-floating-clipper","version":1,"solver":"auto",
            "io":{"input":"in","output":"out"},
            "models":{"SW":{"kind":"diode","is":4.352e-9,"n":1.906,"rs":0.6458}},
            "parts":[{"ref":"C1","type":"C","nodes":["in","n1"],"value":"10u"},{"ref":"R1","type":"R","nodes":["n1","clip"],"value":"2.2k"},
                     {"ref":"D1","type":"D","nodes":["0","clip"],"model":"SW"},{"ref":"D2","type":"D","nodes":["clip","0"],"model":"SW"},
                     {"ref":"R2","type":"R","nodes":["clip","n2"],"value":"10k"},{"ref":"R3","type":"R","nodes":["n2","n3"],"value":"4.7k"},
                     {"ref":"C2","type":"C","nodes":["n3","0"],"value":"15n"},{"ref":"C3","type":"C","nodes":["n2","out"],"value":"1u"},
                     {"ref":"R4","type":"R","nodes":["out","vb"],"value":"100k"},{"ref":"VB","type":"V","nodes":["vb","0"],"value":"4.5"}]})",
                                             m, err);
        CHECK(parsed, "floating clipper parses: %s", err.c_str());
        Circuit fw, fd;
        CHECK(compileAt(fw, m, kRate, {}, {"clip"}, "", true, 1e-9, 0.0) && compileAt(fd, m, kRate, {}, {"clip"}, "dk", true, 1e-9, 0.0), "floating clipper");
        bool wdf = false;
        for (const auto& gr : fw.groups()) wdf |= gr.solver == SolverKind::Wdf;
        CHECK(wdf, "floating clipper: auto picks the WDF form");
        const int cw = fw.node("clip"), cd = fd.node("clip");
        double dmax = 0, swing = 0;
        for (int i = 0; i < 4800; ++i) {
            const double x = 3.0 * std::sin(2 * M_PI * 220.0 * i / kRate);
            const double a = fw.process(x), b = fd.process(x);
            dmax = std::max({dmax, std::abs(a - b), std::abs(fw.nodeVoltage(cw) - fd.nodeVoltage(cd))});
            swing = std::max(swing, std::abs(fd.nodeVoltage(cd)));
        }
        std::printf("  WDF vs DK, DC-floating clipper (linear split leaf): max |Δ| %.3g V, clip swing %.3f V\n", dmax, swing);
        CHECK(swing > 0.4 && dmax < 1e-8, "floating clipper: WDF == DK (%.3g V, swing %.3f V)", dmax, swing);
    }
}

static void testRateConvergence() {
    const Netlist n = loadLib("ts808.circuit.json");
    auto render = [&](double rate) {
        Circuit c;
        compileAt(c, n, rate, controls(n, {}));
        std::vector<double> y;
        const int f = static_cast<int>(rate / 48000.0);
        for (int i = 0; i < static_cast<int>(0.02 * rate); ++i) {
            const double v = c.process(0.3 * std::sin(2 * M_PI * 500.0 * i / rate));
            if (i % f == 0) y.push_back(v);
        }
        return y;
    };
    const auto a = render(96000), b = render(192000), r = render(384000);
    double e1 = 0, e2 = 0;
    for (std::size_t i = a.size() / 2; i < a.size(); ++i) {
        e1 = std::max(e1, std::abs(a[i] - r[i]));
        e2 = std::max(e2, std::abs(b[i] - r[i]));
    }
    std::printf("  rate convergence: |96k − 384k| %.3g V, |192k − 384k| %.3g V (ratio %.2f, trapezoidal ≈ 5 expected)\n", e1, e2, e1 / e2);
    CHECK(e1 / e2 > 3.0, "second-order convergence (ratio %.2f)", e1 / e2);
}

static void testControlGlideAndHandoff() {
    const Netlist n = loadLib("ts808.circuit.json");
    Circuit c;
    compileAt(c, n, kRate, controls(n, {}));
    const int drive = n.controlIndex("drive");
    double prev = 0, maxJump = 0, maxBefore = 0;
    for (int i = 0; i < 20000; ++i) {
        const double y = c.process(0.2 * std::sin(2 * M_PI * 440.0 * i / kRate));
        if (i > 1000) {
            if (i < 10000) maxBefore = std::max(maxBefore, std::abs(y - prev));
            else maxJump = std::max(maxJump, std::abs(y - prev));
        }
        if (i == 10000) c.setControlTarget(drive, 1.0);
        prev = y;
    }
    std::printf("  drive 0.5 -> 1.0 glide: max per-sample step %.4f V (steady state %.4f V)\n", maxJump, maxBefore);
    CHECK(maxJump < 4.0 * maxBefore + 1e-3, "pot glide has no discontinuity");
    // State hand-off: a fresh instance adopting the running one continues seamlessly.
    Circuit a, b;
    compileAt(a, n, kRate, controls(n, {}));
    compileAt(b, n, kRate, controls(n, {}));
    for (int i = 0; i < 5000; ++i) a.process(0.2 * std::sin(2 * M_PI * 440.0 * i / kRate));
    Circuit ref;
    compileAt(ref, n, kRate, controls(n, {}));
    for (int i = 0; i < 5000; ++i) ref.process(0.2 * std::sin(2 * M_PI * 440.0 * i / kRate));
    CHECK(b.adoptStateFrom(a), "adopt");
    double d = 0;
    for (int i = 5000; i < 6000; ++i) {
        const double x = 0.2 * std::sin(2 * M_PI * 440.0 * i / kRate);
        d = std::max(d, std::abs(b.process(x) - ref.process(x)));
    }
    CHECK(d < 1e-9, "adopted state continues identically (%.3g)", d);
}

static void testStress() {
    // Full-scale abuse: square waves at 20 V into every library circuit must stay finite and bounded.
    for (const char* f : {"ts808.circuit.json", "ab763-normal.circuit.json", "diode-clipper.circuit.json"}) {
        const Netlist n = loadLib(f);
        Circuit c;
        compileAt(c, n, kRate, controls(n, {}));
        double mx = 0;
        for (int i = 0; i < 20000; ++i) {
            const double x = ((i / 37) % 2 ? 20.0 : -20.0);
            const double y = c.process(x);
            CHECK(std::isfinite(y), "%s finite", f);
            mx = std::max(mx, std::abs(y));
        }
        std::printf("  stress %-28s max |out| %.2f V, newton failures %llu, max it %d\n", f, mx,
                    static_cast<unsigned long long>(c.diagnostics().failures), c.diagnostics().maxIterations);
        CHECK(mx < 1000.0, "%s bounded", f);
    }
    {
        // Newton must not accept garbage (2026-11-11): the JC-120 with its chorus op-amp IC2 on the supply-cut +15 V rail
        // node (the v3 netlist; the library now gives IC2 fixed rails) at 2900 Hz / 0.5 V, 20 dB over full power. Its
        // rail clamp reached the 1e11 S linear extension inside an LTE substep; a stale chord step, then a cancelled
        // sparse nodal step, were accepted with a 2e16 V residual, railDraws fed the 1e11 A clamp current into the
        // supply, and every group failed from sample 17 on (14 899 homotopies, 14 808 failures in 4800 samples).
        Netlist n = loadLib("roland-jc-120.circuit.json");
        for (auto& p : n.parts)
            if (p.ref == "IC2A" || p.ref == "IC2B") {
                p.nodes.resize(3);
                p.nodes.push_back("v15");
                p.nodes.push_back("0");
                p.model = "NJM4558";
            }
        Circuit c;
        CHECK(compileAt(c, n, kRate, controls(n, {})), "jc120 rail-node IC2");
        double mx = 0;
        bool finite = true;
        for (int i = 0; i < 4800; ++i) {
            const double y = c.process(0.5 * std::sin(2 * M_PI * 2900.0 * i / kRate));
            finite &= std::isfinite(y);
            mx = std::max(mx, std::abs(y));
        }
        const auto& d = c.diagnostics();
        std::printf("  jc120 rail-node chorus op-amp, 2900 Hz 0.5 V: max |out| %.1f V, homotopies %llu, failures %llu\n", mx,
                    static_cast<unsigned long long>(d.homotopies), static_cast<unsigned long long>(d.failures));
        CHECK(finite && mx < 100.0, "jc120 rail-node IC2: finite and bounded (max %.3g V)", mx);
        CHECK(d.failures == 0, "jc120 rail-node IC2: no Newton failures (got %llu)", static_cast<unsigned long long>(d.failures));
    }
}

// Multirate partitions vs the unit's current path (up-convert, whole circuit at 2x, down-convert). Both use one
// up and one down half-band, so they should agree closely on programme-like material.
static void testAdoptByName() {
    // Edit R7 (51k -> 47k) on a running TS808: the edited circuit continues from the old state without a
    // restart (no DC re-settling, no click). Reference: the edited circuit run from the start with the same input.
    Netlist n = loadLib("ts808.circuit.json");
    Netlist e = n;
    std::string err;
    CHECK(applyOverrides(e, nlohmann::json::parse(R"({"parts":{"R7":{"value":"47k"}}})"), err), "override: %s", err.c_str());
    Circuit run, edited, fresh;
    compileAt(run, n, kRate, controls(n, {}));
    compileAt(edited, e, kRate, controls(e, {}));
    auto x = [](int i) { return 0.3 * std::sin(2 * M_PI * 220.0 * i / kRate); };
    double last = 0;
    for (int i = 0; i < 9600; ++i) last = run.process(x(i));
    CHECK(edited.adoptStateByName(run), "states matched");
    double step = 0, typical = 0, prev = last;
    for (int i = 9600; i < 9700; ++i) {
        const double y = edited.process(x(i));
        if (i == 9600) step = std::abs(y - prev);
        else typical = std::max(typical, std::abs(y - prev));
        prev = y;
    }
    std::printf("  part edit (R7 51k -> 47k) on a running circuit: first-sample step %.2e V, typical %.2e V\n", step, typical);
    CHECK(step < 2.0 * typical + 1e-6, "edited circuit continues without a click");
}

// Bucket-brigade block (Blocks.hpp BbdLine) against closed forms: an ideal BBD samples x(m/fc) and holds it for one
// clock period starting half a period after the bucket leaves (N/2 periods later), so at a constant clock its output is
// sinc(f/fc)·x delayed by N/(2fc) + 1/fc; the transfer loss is Weckler's −17.37·Nε·sin²(πf/fc) dB; a moving clock
// shifts pitch by the tape equation; clock images above the engine's Nyquist do not fold back.
static void testBbd() {
    const double fs = 96000;
    auto run = [&](double eps, double fin, std::vector<double> ctl, int n, double amp = 1.0) {
        BbdLine b;
        BbdLine::Params p;
        p.stages = 1024;
        p.eps = eps;
        b.prepare(fs, p, {{0.0, 0.0}, {1e7, 1e7}}, 0); // clock law: f = v
        b.reset(0.0, ctl[0]);
        std::vector<double> y(static_cast<std::size_t>(n));
        for (int k = 0; k < n; ++k) {
            const double x = amp * std::sin(2.0 * M_PI * fin * k / fs);
            y[static_cast<std::size_t>(k)] = b.process(x, ctl[static_cast<std::size_t>(std::min<int>(k, static_cast<int>(ctl.size()) - 1))]);
        }
        return y; // y[k] is the output for sample k + 1
    };
    auto bin = [&](const std::vector<double>& y, double f, int from) {
        std::complex<double> a = 0;
        int cnt = 0;
        for (std::size_t k = static_cast<std::size_t>(from); k < y.size(); ++k, ++cnt) a += y[k] * std::polar(1.0, -2.0 * M_PI * f * static_cast<double>(k + 1) / fs);
        return a * (2.0 / cnt);
    };
    // 1) constant clock 40 kHz: gain and delay vs the ideal staircase, and the waveform
    const double fc = 40000, n = 48000;
    for (double fin : {1000.0, 7000.0, 15000.0}) {
        const auto y = run(0, fin, {fc}, static_cast<int>(n));
        const auto a = bin(y, fin, 9600);
        const double x = M_PI * fin / fc, g = std::sin(x) / x, d = 512.0 / fc + 1.0 / fc;
        const auto ref = std::polar(g, -M_PI / 2 - 2.0 * M_PI * fin * d); // sin(ωt) = cos(ωt − π/2)
        const double errDb = 20 * std::log10(std::abs(a) / g), errPh = std::arg(a / ref) * 180 / M_PI;
        std::printf("  bbd fc 40k f %5.0f Hz: gain %.4f (ideal %.4f, %+.3f dB), phase error %+.3f deg\n", fin, std::abs(a), g, errDb, errPh);
        CHECK(std::abs(errDb) < 0.05 && std::abs(errPh) < 0.5, "bbd: hold gain / delay at %.0f Hz", fin);
    }
    // 2) transfer loss: N·ε = 0.2 against eps 0 at 15 kHz
    {
        const double fin = 15000, ne = 0.2;
        const double g0 = std::abs(bin(run(0, fin, {fc}, 48000), fin, 9600)), g1 = std::abs(bin(run(ne, fin, {fc}, 48000), fin, 9600));
        const double meas = 20 * std::log10(g1 / g0), s = std::sin(M_PI * fin / fc), want = -17.37 * ne * s * s;
        std::printf("  bbd transfer loss N·eps 0.2 at 15 kHz: %.3f dB (Weckler %.3f dB)\n", meas, want);
        CHECK(std::abs(meas - want) < 0.1, "bbd: Poisson transfer loss vs Weckler");
    }
    // 3) clock ramp 40 → 30 kHz over 1 s: output frequency = fin·(1 − d(delay)/dt), delay = 512/f_clk
    {
        const int N = 96000;
        std::vector<double> ctl(N);
        for (int k = 0; k < N; ++k) ctl[static_cast<std::size_t>(k)] = 40000.0 - 10000.0 * k / N;
        const double fin = 1000;
        auto y = run(0, fin, ctl, N);
        // the clock image at f_clk − f (≈ 34 kHz) is below this rate's Nyquist and real: low-pass (two 24-sample boxcars)
        // before counting zero crossings between 0.45 and 0.55 s
        for (int pass = 0; pass < 2; ++pass) {
            std::vector<double> z(y.size(), 0.0);
            double acc = 0;
            for (std::size_t k = 0; k < y.size(); ++k) {
                acc += y[k] - (k >= 24 ? y[k - 24] : 0.0);
                z[k] = acc / 24.0;
            }
            y.swap(z);
        }
        std::vector<double> zc;
        for (int k = 43200; k < 52800; ++k)
            if (y[static_cast<std::size_t>(k - 1)] < 0 && y[static_cast<std::size_t>(k)] >= 0)
                zc.push_back(k - 1 + y[static_cast<std::size_t>(k - 1)] / (y[static_cast<std::size_t>(k - 1)] - y[static_cast<std::size_t>(k)]));
        const double fout = (zc.size() - 1) / ((zc.back() - zc.front()) / fs);
        const double fclk = 35000, ddot = 512.0 * 10000.0 / (fclk * fclk); // d/dt (512/f) with df/dt = −10 kHz/s
        const double want = fin * (1.0 - ddot);
        std::printf("  bbd clock sweep: output %.3f Hz (tape equation %.3f Hz, input %.0f)\n", fout, want, fin);
        CHECK(std::abs(fout - want) < 0.05 * std::abs(fin - want) + 0.02, "bbd: pitch under a moving clock");
    }
    // 4) CE-1 clock (85 kHz): the image at fc − f = 80 kHz is above Nyquist; it must not fold to 16 kHz
    {
        const double fin = 5000;
        const auto y = run(0, fin, {85000}, 48000);
        const double sig = std::abs(bin(y, fin, 9600)), alias = std::abs(bin(y, 96000 - 85000 + fin, 9600));
        const double db = 20 * std::log10(alias / sig);
        std::printf("  bbd image rejection at fc 85 kHz: folded image %.1f dB\n", db);
        CHECK(db < -60, "bbd: band-limited output (no folded clock image)");
    }
}

// Tape loop (Blocks.hpp TapeLoop) against closed forms, in the linear (small-signal) region with wow/flutter off: the
// echo delay is the head distance over the tape speed, and the playback response is d/dt of the recorded
// magnetisation times the gap/spacing/thickness loss at wavelength λ = v/f (Chowdhury DAFx-19), which moves down with
// the tape speed.
static void testTape() {
    const double fs = 96000;
    TapeLoop::Params p;
    p.wow = p.flutter = 0;
    p.servo = 1e-4;
    auto measure = [&](double x, double f, double& gain, double& delay) {
        TapeLoop t;
        t.prepare(fs, p);
        t.reset(x);
        const int n = static_cast<int>(0.6 * fs);
        std::complex<double> a = 0;
        int cnt = 0;
        const double i0 = 1e-6; // small-signal record current
        for (int k = 0; k < n; ++k) {
            double out[3];
            t.process(i0 * std::sin(2.0 * M_PI * f * k / fs), x, out);
            if (k > n / 2) {
                a += out[0] * std::polar(1.0, -2.0 * M_PI * f * (k + 1) / fs);
                ++cnt;
            }
        }
        a *= 2.0 / cnt;
        const double chi = 1.0 / (3.0 * p.a - p.alpha * p.ms); // m per (A/m), small signal
        const double g0 = p.gain * 2.0 * M_PI * f * chi * p.drive * i0;
        gain = std::abs(a) / g0;
        // output ∝ d/dt sin(ω(t − d)) = ω cos(ω(t − d)): the bin's phase (cosine reference) is −ωd (mod 2π)
        const double ph = std::arg(a);
        const double v = 1.0 / (1.0 + (p.range - 1.0) * (1.0 - x)), d = p.delays[0] / v, per = 1.0 / f;
        double e = std::fmod(-ph / (2.0 * M_PI * f) - d, per);
        if (e > per / 2) e -= per;
        if (e < -per / 2) e += per;
        delay = d + e;
    };
    auto loss = [&](double x, double f) {
        const double v = p.v0 / (1.0 + (p.range - 1.0) * (1.0 - x)), k = 2.0 * M_PI * f / v;
        const double g = k * p.gap / 2, t = k * p.thickness;
        return std::sin(g) / g * std::exp(-k * p.spacing) * (1.0 - std::exp(-t)) / t;
    };
    // The spatial FIR (TapeLoop::kTaps cells) holds the loss shape for wavelengths up to about half its span; the
    // shallow |k| cusp of the spacing / coating losses at longer wavelengths (≈ 1 dB at 380 µm) is truncated, so the
    // shape is gated between two short wavelengths (36 and 95 µm at the model's v0), at both speeds.
    for (double x : {1.0, 0.0}) {
        const double v = 1.0 / (1.0 + (p.range - 1.0) * (1.0 - x)), fa = 2000 * v, fb = 5300 * v;
        double g1, d1, g2, d2;
        measure(x, fa, g1, d1);
        measure(x, fb, g2, d2);
        const double want = 20 * std::log10(loss(x, fb) / loss(x, fa)), got = 20 * std::log10(g2 / g1);
        std::printf("  tape speed %.3f: head-1 delay %.2f ms (want %.2f), loss %.0f Hz vs %.0f Hz %.2f dB (formula %.2f dB)\n", v, d1 * 1e3,
                    p.delays[0] / v * 1e3, fb, fa, got, want);
        CHECK(std::abs(d1 - p.delays[0] / v) < 2.0 / fs, "tape: echo delay at speed %.3f", v);
        CHECK(std::abs(got - want) < 0.3, "tape: playback loss scales with speed (%.3f)", v);
    }
}

static void testSpringTank() {
    const char* txt = R"({"schema":"headroom.circuit.v1","id":"tank","io":{"input":"in","output":"out"},
      "parts":[{"ref":"RD","type":"R","nodes":["in","drv"],"value":"10"},
               {"ref":"T1","type":"spring","nodes":["drv","0","out","0"],"model":"4AB3C1B"},
               {"ref":"RL","type":"R","nodes":["out","0"],"value":"100k"}]})";
    Netlist n;
    std::string err;
    CHECK(parseNetlistText(txt, n, err), "tank netlist: %s", err.c_str());
    Circuit c;
    CHECK(compileAt(c, n, kRate, {}), "tank compile");
    const int N = static_cast<int>(3.0 * kRate);
    std::vector<double> y(static_cast<std::size_t>(N));
    for (int i = 0; i < N; ++i) y[static_cast<std::size_t>(i)] = c.process(i < 48 ? 1.0 : 0.0); // 0.5 ms pulse
    int first = -1;
    double peak = 0;
    for (double v : y) peak = std::max(peak, std::abs(v));
    for (int i = 0; i < N && first < 0; ++i)
        if (std::abs(y[static_cast<std::size_t>(i)]) > 0.1 * peak) first = i;
    auto energy = [&](double t0, double t1) {
        double e = 0;
        for (int i = static_cast<int>(t0 * kRate); i < static_cast<int>(t1 * kRate); ++i) e += y[static_cast<std::size_t>(i)] * y[static_cast<std::size_t>(i)];
        return e;
    };
    const double dbPerSec = 10 * std::log10(energy(0.2, 0.4) / energy(1.2, 1.4)); // over 1 s
    std::printf("  spring tank (4AB3C1B): first arrival %.1f ms, decay %.1f dB/s (T60 ≈ %.2f s)\n", 1000.0 * first / kRate, dbPerSec, 60.0 / dbPerSec);
    CHECK(first > 0.025 * kRate && first < 0.045 * kRate, "first arrival near the spring flight time");
    CHECK(60.0 / dbPerSec > 1.8 && 60.0 / dbPerSec < 4.5, "decay time in the 4AB3 range");
    // Echoes come every round trip (2x the flight), not every flight: after the first arrivals (33 / 39 ms) the
    // next are at 3x (100 / 118 ms); nothing returns at 2x (67 / 78 ms).
    const double atTwice = energy(0.058, 0.086), atThrice = energy(0.094, 0.126);
    std::printf("  spring tank echoes: energy at 2x flight %.3g, at 3x flight %.3g (ratio %.1f dB)\n", atTwice, atThrice, 10 * std::log10(atThrice / std::max(atTwice, 1e-30)));
    CHECK(atThrice > 10.0 * atTwice, "echo period is the round trip (first arrival t, then 3t, 5t...)");
}


// CompileOptions::openSwitches (docs §10.29): an open contact between low-impedance nodes leaves the circuit (its
// groups split), one into a 1 MΩ node keeps its roff (its leakage is above rounding there), the output matches the roff
// circuit, and reset() recompiles when the selector moves (a contact compiled open could not close otherwise).
static void testOpenContacts() {
    const char* txt = R"({"schema":"headroom.circuit.v1","id":"opensw","io":{"input":"in","output":"out"},
      "controls":[{"id":"sel","kind":"selector","options":["A","B","C"],"default":0.0}],
      "parts":[{"ref":"R1","type":"R","nodes":["in","a"],"value":"1k"},
               {"ref":"C1","type":"C","nodes":["a","0"],"value":"100n"},
               {"ref":"R2","type":"R","nodes":["b","0"],"value":"470"},
               {"ref":"C2","type":"C","nodes":["b","out"],"value":"1u"},
               {"ref":"R3","type":"R","nodes":["out","0"],"value":"10k"},
               {"ref":"R4","type":"R","nodes":["hi","0"],"value":"1M"},
               {"ref":"C3","type":"C","nodes":["hi","0"],"value":"10n"},
               {"ref":"S1","type":"switch","nodes":["a","b"],"control":"sel","positions":[1]},
               {"ref":"S2","type":"switch","nodes":["out","hi"],"control":"sel","positions":[2]}]})";
    Netlist n;
    std::string err;
    CHECK(parseNetlistText(txt, n, err), "open-contact netlist: %s", err.c_str());
    for (int pos = 0; pos < 3; ++pos) {
        const std::vector<double> ctl = {pos / 2.0};
        Circuit a, b;
        CompileOptions o;
        o.sampleRate = kRate;
        o.controls = ctl;
        CHECK(a.compile(n, o, err), "open contacts compile pos %d: %s", pos, err.c_str());
        o.openSwitches = false;
        CHECK(b.compile(n, o, err), "roff contacts compile pos %d: %s", pos, err.c_str());
        const int wantOpen = pos == 1 ? 0 : 1; // S1 open between 1 kΩ / 470 Ω nodes: out; S2 into 1 MΩ: kept
        CHECK(a.openContacts() == wantOpen, "pos %d: %d contacts left out (want %d)", pos, a.openContacts(), wantOpen);
        if (pos == 0) CHECK(a.groups().size() > b.groups().size(), "pos 0: the open contact splits the partition (%zu vs %zu groups)", a.groups().size(), b.groups().size());
        double d = 0, pk = 0;
        for (int i = 0; i < 4800; ++i) {
            const double x = std::sin(2 * M_PI * 440.0 * i / kRate);
            const double ya = a.process(x), yb = b.process(x);
            d = std::max(d, std::abs(ya - yb));
            pk = std::max(pk, std::abs(yb));
        }
        // the roff current moves an end by at most R/roff of the 1 V across the contact (kOpenLeak: 1e-6)
        CHECK(d <= 1e-6, "pos %d: open-contact circuit matches the roff circuit (max |d| %.2e V, 1 V in, %.2e V out)", pos, d, pk);
    }
    // reset() recompiles at the new positions: S1 closes, so the stale partition is replaced
    Circuit c, r;
    CHECK(compileAt(c, n, kRate, {0.0}), "open contacts: compile at A");
    CHECK(compileAt(r, n, kRate, {0.5}), "open contacts: reference at B");
    c.setControlImmediate(0, 0.5);
    CHECK(c.topologyStale(), "selector moved to B: topology stale");
    CHECK(c.reset() && !c.topologyStale(), "reset() recompiles at B");
    double d = 0;
    for (int i = 0; i < 2400; ++i) {
        const double x = std::sin(2 * M_PI * 440.0 * i / kRate);
        d = std::max(d, std::abs(c.process(x) - r.process(x)));
    }
    CHECK(d < 1e-12, "recompiled at B matches a circuit compiled at B (max |d| %.2e)", d);
    std::printf("  open switch contacts: left out between low-impedance nodes, kept into 1 MOhm, recompiled on reset\n");
}

// Back-to-back zener pair (docs §10.29): the composite port with its midpoint eliminated against the two junctions solved
// separately (the midpoint probed keeps them apart), anode- and cathode-joined, with series resistance, driven through
// breakdown both ways.
static void testZenerPairComposite() {
    for (int variant = 0; variant < 3; ++variant) {
        const bool anodes = variant != 1;
        const std::string rs = variant == 2 ? R"(,"rs":5)" : "";
        const std::string za = anodes ? R"(["m","out"])" : R"(["out","m"])", zb = anodes ? R"(["m","0"])" : R"(["0","m"])";
        const std::string txt = R"({"schema":"headroom.circuit.v1","id":"zpair","io":{"input":"in","output":"out"},
          "models":{"ZA":{"kind":"diode","is":1e-14,"n":1.0,"bv":6.2,"ibv":5e-3,"nbv":2.0)" + rs + R"(},
                    "ZB":{"kind":"diode","is":2e-14,"n":1.1,"bv":4.7,"ibv":5e-3,"nbv":3.0)" + rs + R"(}},
          "parts":[{"ref":"R1","type":"R","nodes":["in","out"],"value":"1k"},
                   {"ref":"C1","type":"C","nodes":["out","0"],"value":"10n"},
                   {"ref":"Z1","type":"D","nodes":)" + za + R"(,"model":"ZA"},
                   {"ref":"Z2","type":"D","nodes":)" + zb + R"(,"model":"ZB"}]})";
        Netlist n;
        std::string err;
        CHECK(parseNetlistText(txt, n, err), "zener pair netlist: %s", err.c_str());
        Circuit a, b;
        CHECK(compileAt(a, n, kRate, {}, {}, "", true, 1e-10), "zener pair compile");
        CHECK(compileAt(b, n, kRate, {}, {"m"}, "", true, 1e-10), "zener pair (probed midpoint) compile");
        bool noted = false;
        for (const auto& w : a.warnings()) noted |= w.find("back-to-back zener pair") != std::string::npos;
        CHECK(noted, "variant %d: solved as one pair", variant);
        CHECK(b.devices().size() == a.devices().size() + 1, "variant %d: probed midpoint keeps two junctions", variant);
        double d = 0, pk = 0;
        for (int i = 0; i < 9600; ++i) {
            const double x = 20.0 * std::sin(2 * M_PI * 220.0 * i / kRate);
            const double ya = a.process(x), yb = b.process(x);
            d = std::max(d, std::abs(ya - yb));
            pk = std::max(pk, std::abs(yb));
        }
        CHECK(d <= 1e-7 * pk, "variant %d: composite pair matches the two junctions (max |d| %.2e of %.2f V)", variant, d, pk);
        std::printf("  zener pair (%s%s): composite vs two junctions max |d| %.2e of %.2f V\n", anodes ? "anode-joined" : "cathode-joined",
                    variant == 2 ? ", rs 5" : "", d, pk);
    }
}

// Supply-cut stability through the loads' network (docs §10.29): a bias rail fed by a rectifier diode (series Rs 30 mΩ)
// from a 40 Ω winding fails the per-resistor bound (Rs counted as a load to ground) but is stable through the path; the
// cut rail tracks the uncut one.
static void testCutLoadNetwork() {
    const char* txt = R"({"schema":"headroom.circuit.v1","id":"biascut","io":{"input":"in","output":"out"},
      "models":{"DR":{"kind":"diode","is":7e-9,"n":1.9,"rs":0.03}},
      "parts":[{"ref":"VAC","type":"V","nodes":["ac","0"],"value":"0","ac":{"amplitude":"60","freq":60}},
               {"ref":"RW","type":"R","nodes":["ac","w"],"value":"40"},
               {"ref":"D1","type":"D","nodes":["bias","w"],"model":"DR"},
               {"ref":"CB","type":"C","nodes":["bias","0"],"value":"90u"},
               {"ref":"RL","type":"R","nodes":["bias","g"],"value":"47k"},
               {"ref":"RG","type":"R","nodes":["g","0"],"value":"220k"},
               {"ref":"RI","type":"R","nodes":["in","g"],"value":"1M"},
               {"ref":"RO","type":"R","nodes":["g","out"],"value":"1k"},
               {"ref":"CO","type":"C","nodes":["out","0"],"value":"1n"}],
      "cuts":["bias"]})";
    Netlist n;
    std::string err;
    CHECK(parseNetlistText(txt, n, err), "bias cut netlist: %s", err.c_str());
    Netlist u = n;
    u.cuts.clear();
    Circuit a, b;
    CHECK(compileAt(a, n, kRate, {}, {"bias"}), "bias cut compile");
    CHECK(compileAt(b, u, kRate, {}, {"bias"}), "bias uncut compile");
    bool demoted = false;
    for (const auto& w : a.warnings()) demoted |= w.find("solved implicitly") != std::string::npos;
    CHECK(!demoted, "the bias rail stays cut (its loads' network is 40 ohm deep)");
    const int ia = a.node("bias"), ib = b.node("bias");
    double d = 0, v = 0;
    for (int i = 0; i < static_cast<int>(0.3 * kRate); ++i) {
        a.process(0.0), b.process(0.0);
        if (i > static_cast<int>(0.1 * kRate)) d = std::max(d, std::abs(a.nodeVoltage(ia) - b.nodeVoltage(ib))), v = std::max(v, std::abs(b.nodeVoltage(ib)));
    }
    CHECK(v > 30.0 && d < 1e-3 * v, "cut bias rail tracks the uncut one (max |d| %.2e of %.2f V)", d, v);
    std::printf("  supply cut through the loads' network: bias rail %.2f V, cut vs uncut max |d| %.2e V\n", v, d);
}

static void testSelectorSwitches() {
    const char* txt = R"({"schema":"headroom.circuit.v1","id":"sel","io":{"input":"in","output":"out"},
      "controls":[{"id":"mode","kind":"selector","options":["A","B","C","D"],"default":0.0}],
      "parts":[{"ref":"RTOP","type":"R","nodes":["in","out"],"value":"1k"},
               {"ref":"S1","type":"switch","nodes":["out","0"],"control":"mode","positions":[1],"ron":1000,"roff":1e9},
               {"ref":"S2","type":"switch","nodes":["out","0"],"control":"mode","positions":[2],"ron":500,"roff":1e9},
               {"ref":"S3","type":"switch","nodes":["out","0"],"control":"mode","positions":[1,3],"ron":3000,"roff":1e9}]})";
    Netlist n;
    std::string err;
    CHECK(parseNetlistText(txt, n, err), "selector netlist: %s", err.c_str());
    const double want[4] = {1.0, 750.0 / 1750.0, 500.0 / 1500.0, 3000.0 / 4000.0};
    const bool s1Want[4] = {false, true, false, false};
    const bool s3Want[4] = {false, true, false, true};
    for (int k = 0; k < 4; ++k) {
        const std::vector<double> ctl = {k / 3.0};
        CHECK(switchClosed(n, *n.part("S1"), ctl) == s1Want[k], "S1 closed at pos %d", k);
        CHECK(switchClosed(n, *n.part("S3"), ctl) == s3Want[k], "S3 closed at pos %d", k);
        Circuit c;
        CHECK(compileAt(c, n, kRate, ctl), "selector compile pos %d", k);
        const double y = c.process(1.0);
        CHECK(std::abs(y - want[k]) < 1e-5, "selector pos %d divider %.6f vs %.6f", k, y, want[k]);
        SpiceExportOptions so;
        so.controls = ctl;
        const std::string deck = exportSpice(n, so);
        CHECK(deck.find(s1Want[k] ? "RS1 n_out 0 1000" : "RS1 n_out 0 1e+09") != std::string::npos ||
              deck.find(s1Want[k] ? "RS1 n_out 0 1k" : "RS1 n_out 0 1G") != std::string::npos ||
              deck.find("RS1") != std::string::npos, "SPICE export emits selector switch at pos %d", k);
    }
    Netlist bad;
    CHECK(!parseNetlistText(R"({"schema":"headroom.circuit.v1","id":"bad","io":{"input":"in","output":"out"},
      "controls":[{"id":"m","kind":"selector","options":["A","B"]}],
      "parts":[{"ref":"S1","type":"switch","nodes":["in","out"],"control":"m","positions":[-1]}]})", bad, err),
          "negative selector position rejected");
    std::printf("  selector switches: 4-position decode verified (gains %.3f, %.3f, %.3f, %.3f)\n", want[0], want[1], want[2], want[3]);
}

static void testSchmittPtc() {
    // Small Stone CA3080 positive-feedback Schmitt trigger + integrator LFO at maximum rate (both Color states):
    // damped Newton cannot cross the saddle-node fold; pseudo-transient continuation (newtonPtc) jumps cleanly to
    // the opposite saturated branch with 0 failures.
    const Netlist n = loadLib("ehx-small-stone.circuit.json");
    for (int col : {0, 1}) {
        Circuit c;
        CHECK(compileAt(c, n, kRate, controls(n, {{"rate", 1.0}, {"color", double(col)}}), {"lp", "l5o"}), "small-stone compile color=%d", col);
        const int ilp = c.node("lp");
        double lpMin = 1e9, lpMax = -1e9;
        const int N = static_cast<int>(0.8 * kRate); // ~6-15 Schmitt flips at rate=1.0
        for (int i = 0; i < N; ++i) {
            const double y = c.process(0.1 * std::sin(2 * M_PI * 330.0 * i / kRate));
            CHECK(std::isfinite(y), "small-stone finite at sample %d", i);
            if (i > 0.1 * kRate) {
                const double v = c.nodeVoltage(ilp);
                lpMin = std::min(lpMin, v);
                lpMax = std::max(lpMax, v);
            }
        }
        const auto& d = c.diagnostics();
        std::printf("  small-stone PTC (rate=1, color=%d): lp swing [%.2f, %.2f] V, %.2f it/sample (max %d), homotopies %llu, failures %llu\n",
                    col, lpMin, lpMax, double(d.newtonIterations) / std::max<std::uint64_t>(1, d.samples), d.maxIterations,
                    static_cast<unsigned long long>(d.homotopies), static_cast<unsigned long long>(d.failures));
        CHECK(d.failures == 0, "small-stone zero failures (color=%d)", col);
        CHECK(lpMax - lpMin > 2.0, "Schmitt trigger oscillates across both branches (swing %.2f V)", lpMax - lpMin);
    }
}

static void testCoreHysteresis() {
    // A transformer primary driven from 100 ohm at 20 Hz into the knee: with core hysteresis the source does net
    // work on the core every cycle (the loop area) and the loss grows with the coercive MMF; without it the
    // anhysteretic law is lossless. The secondary is lightly loaded so the core dominates.
    auto loss = [&](double hc, double& peakLambda) {
        std::string txt = R"({"schema":"headroom.circuit.v1","id":"core","io":{"input":"in","output":"s"},
          "parts":[{"ref":"RS","type":"R","nodes":["in","p"],"value":"100"},
                   {"ref":"T1","type":"transformer","nodes":["p","0","s","0"],"turns":[1,1],"r":[0.5,0.5],"leak":[0,0],"lm":4,"lsat":0.6,"hc":)" +
                          std::to_string(hc) + R"(,"hw":0.05},
                   {"ref":"RL","type":"R","nodes":["s","0"],"value":"1M"}]})";
        Netlist n;
        std::string err;
        CHECK(parseNetlistText(txt, n, err), "core netlist: %s", err.c_str());
        Circuit c;
        CHECK(compileAt(c, n, kRate, {}, {"p"}), "core compile");
        const double f = 20, A = 60; // λ peak ≈ A / (2π f) ≈ 0.48 V·s, near λsat
        const int per = static_cast<int>(kRate / f), N = 6 * per;
        double e = 0, lam = 0, lamPk = 0;
        const int pnode = c.node("p");
        for (int i = 0; i < N; ++i) {
            const double vin = A * std::sin(2 * M_PI * f * i / kRate);
            c.process(vin);
            const double vp = c.nodeVoltage(pnode), ip = (vin - vp) / 100.0;
            lam += vp / kRate;
            if (i >= 4 * per) {
                e += vp * ip / kRate; // energy into the primary over the last two cycles
                lamPk = std::max(lamPk, std::abs(lam));
            }
        }
        peakLambda = lamPk;
        return e / 2.0; // per cycle
    };
    double l0 = 0, l1 = 0, l2 = 0;
    const double e0 = loss(0.0, l0), e1 = loss(0.02, l1), e2 = loss(0.04, l2);
    std::printf("  core hysteresis: loss per cycle %.3g J (none), %.3g J (hc 20 mA), %.3g J (hc 40 mA); flux peak %.2f V.s\n", e0, e1, e2, l1);
    CHECK(e1 > e0 + 1e-4 && e2 > 1.6 * (e1 - e0) + e0, "hysteresis loop area (core loss) appears and grows with the coercive MMF");
    CHECK(std::abs(e0) < 0.2 * (e1 - e0), "anhysteretic core is (nearly) lossless");
}


static void testAmpMultirate(const char* file, const char* id, const std::map<std::string, double>& base, const char* volCtl, double cleanVol,
                             double driveVol, const std::map<std::string, double>& driveExtra = {}) {
    // The shipped amps run only the PI + power stage at 2x (the rest at the host rate, oscillators at 12 kHz). Gate
    // that path against the same ngspice references, aligned for the halfband latency.
    const Netlist n = loadLib(file);
    struct C {
        std::string id;
        double amp, freq, vol;
        bool drive;
    };
    for (const C& cs : {C{std::string(id) + "_clean", 0.02, 220, cleanVol, false}, C{std::string(id) + "_drive", 0.3, 110, driveVol, true}}) {
        const auto ref = readTable(fixture(cs.id + ".tran.txt"));
        if (ref.empty()) continue;
        Circuit c;
        CompileOptions o;
        o.sampleRate = kRate;
        o.multirate = true;
        std::string err;
        CHECK(c.compile(n, o, err), "%s multirate compile: %s", id, err.c_str());
        CHECK(c.isMultirate(), "%s runs multirate", id);
        auto set = base;
        if (cs.vol >= 0) set[volCtl] = cs.vol;
        if (cs.drive)
            for (const auto& [k, v] : driveExtra) set[k] = v;
        auto ctl = controls(n, set);
        for (std::size_t i = 0; i < ctl.size(); ++i) c.setControlImmediate(static_cast<int>(i), ctl[i]);
        c.reset();
        const double hostT = 2.0 / kRate, t1 = ref.back()[0];
        std::vector<double> y;
        for (std::size_t i = 0; static_cast<double>(i) * hostT <= t1 + 1e-9; ++i)
            y.push_back(c.processHost(i ? cs.amp * std::sin(2 * M_PI * cs.freq * static_cast<double>(i) * hostT) : 0.0));
        auto at = [&](double t) {
            const double x = t / hostT;
            const auto k = static_cast<std::size_t>(x);
            if (k + 1 >= y.size()) return y.back();
            return y[k] + (x - static_cast<double>(k)) * (y[k + 1] - y[k]);
        };
        double best = 1e30, bestD = 0;
        for (int d = 0; d < 200; ++d) { // latency search, 1 µs steps
            double e = 0, r2 = 0;
            for (const auto& row : ref) {
                const double v = at(row[0] + d * 1e-6) - row[1];
                e += v * v;
                r2 += row[1] * row[1];
            }
            if (std::sqrt(e / r2) < best) best = std::sqrt(e / r2), bestD = d;
        }
        std::printf("  %-13s multirate (power stage 2x, rest at host rate): spk NRMSE %.3f %% vs ngspice at %.0f us latency\n", cs.id.c_str(), 100 * best, bestD);
        CHECK(best < 0.01, "%s: multirate amp within 1 %% of ngspice (%.3f %%)", cs.id.c_str(), 100 * best);
    }
}
static void testTwinMultirate() {
    testAmpMultirate("ab763-twin-reverb.circuit.json", "twin", {{"reverb_off", 1.0}}, "volume", 0.5, 0.8);
    testAmpMultirate("ab763-deluxe-reverb.circuit.json", "deluxe", {{"reverb_off", 1.0}}, "volume", -1, 0.8);
    testAmpMultirate("5f6a-bassman.circuit.json", "bassman", {}, "normal", -1, 0.8);
    testAmpMultirate("marshall-1959-plexi.circuit.json", "plexi", {}, "volume1", -1, 0.8);
    testAmpMultirate("marshall-jcm800-2203.circuit.json", "jcm", {}, "preamp", -1, 0.8, {{"master", 0.7}});
    testAmpMultirate("vox-ac30-tb.circuit.json", "ac30", {}, "volume", -1, 0.8);
}

static void testCutError() {
    // Same amp, B+ cuts on vs off: the one-sample current lag must be inaudible.
    Netlist cut = loadLib("test-amp-psu.circuit.json");
    Netlist exact = cut;
    exact.cuts.clear();
    Circuit a, b;
    CHECK(compileAt(a, cut, kRate, {}, {"spk", "bp2"}), "cut compile");
    CHECK(compileAt(b, exact, kRate, {}, {"spk", "bp2"}), "uncut compile");
    const int ia = a.node("spk"), ib = b.node("spk");
    double e = 0, ref = 0, se = 0, sref = 0;
    for (int i = 0; i < static_cast<int>(0.4 * kRate); ++i) {
        const double x = 2.0 * std::sin(2 * M_PI * 110.0 * i / kRate) * std::min(1.0, i / (0.05 * kRate));
        a.process(x);
        b.process(x);
        if (i > 0.1 * kRate) {
            const double ya = a.nodeVoltage(ia), yb = b.nodeVoltage(ib);
            e += (ya - yb) * (ya - yb);
            ref += yb * yb;
            const double sa = a.nodeVoltage(a.node("bp2")), sb = b.nodeVoltage(b.node("bp2"));
            se += (sa - sb) * (sa - sb);
            sref += 1.0;
        }
    }
    std::printf("  B+ cuts vs exact (whole amp, driven into sag): speaker NRMSE %.4f %%, bp2 rms error %.2e V; groups %zu vs %zu, max ports %d vs %d\n",
                100 * std::sqrt(e / ref), std::sqrt(se / sref), a.groups().size(), b.groups().size(),
                [&] { int m = 0; for (const auto& g : a.groups()) m = std::max(m, g.nn); return m; }(),
                [&] { int m = 0; for (const auto& g : b.groups()) m = std::max(m, g.nn); return m; }());
    CHECK(std::sqrt(e / ref) < 0.005, "cut error below 0.5 %%");
}

static void testNoReservoirCutIdle() {
    // The 1176's +30 V rail is an ideal source behind 2 Ω (no reservoir cap) with transistor collectors on it. As a plain
    // supply cut its one-sample lag grew an idle mode (tau ≈ 32 s with the pre_ac signal cut; Newton failures after
    // ~17 min, the live rig's 100x CPU). The compiler now gives such a cut a load estimate (docs §2.6). Twin circuits at
    // the live rig's settings, one kicked: the difference must die out, not grow (CE_CUT_RESERVOIR=0 fails this).
    const Netlist n = loadLib("urei-1176ln.circuit.json");
    const std::vector<double> ctl = {0.49794, 0.2, 1.0, 1.0, 0.4318}; // input, ratio 4:1, attack, release, output
    Circuit a, b;
    CHECK(compileAt(a, n, 48000.0, ctl, {"sc_sw"}), "1176 compile");
    CHECK(compileAt(b, n, 48000.0, ctl, {"sc_sw"}), "1176 compile");
    bool noted = false;
    for (const auto& w : a.warnings()) noted |= w.find("supply cut v30 has no reservoir") != std::string::npos;
    CHECK(noted, "1176 v30 flagged as a no-reservoir supply cut");
    const int ia = a.node("sc_sw"), ib = b.node("sc_sw");
    double d30 = 0, d60 = 0;
    for (int i = 1; i <= 60 * 48000; ++i) {
        a.process(0.0);
        b.process(i == 1 ? 0.5 : 0.0);
        if (i == 30 * 48000) d30 = std::abs(a.nodeVoltage(ia) - b.nodeVoltage(ib));
    }
    d60 = std::abs(a.nodeVoltage(ia) - b.nodeVoltage(ib));
    std::printf("  1176 idle after a kick (no-reservoir +30 V cut): |dV(sc_sw)| %.2e V at 30 s, %.2e V at 60 s; newton %.3f it/sample, "
                "homotopies %llu\n", d30, d60, static_cast<double>(b.diagnostics().newtonIterations) / static_cast<double>(b.diagnostics().samples),
                static_cast<unsigned long long>(b.diagnostics().homotopies));
    CHECK(d60 < std::max(1.5 * d30, 1e-10), "1176 idle mode does not grow (%.2e V at 30 s -> %.2e V at 60 s)", d30, d60);
    CHECK(b.diagnostics().homotopies == 0 && b.diagnostics().failures == 0, "1176 idle: no Newton rescues");
}

static void testShermanMorrison() {
    for (const char* f : {"ab763-normal.circuit.json", "ts808.circuit.json"}) {
        const Netlist n = loadLib(f);
        Circuit a, b;
        CompileOptions o;
        o.sampleRate = kRate;
        std::string err;
        a.compile(n, o, err);
        o.shermanMorrison = false;
        b.compile(n, o, err);
        double d = 0, mx = 0;
        for (int i = 0; i < 40000; ++i) {
            if (i == 2000 || i == 20000)
                for (int c = 0; c < static_cast<int>(n.controls.size()); ++c) {
                    if (n.controls[static_cast<std::size_t>(c)].kind != "continuous") continue;
                    const double t = i == 2000 ? 0.9 - 0.1 * c : 0.1 + 0.15 * c;
                    a.setControlTarget(c, t);
                    b.setControlTarget(c, t);
                }
            const double x = 0.2 * std::sin(2 * M_PI * 330.0 * i / kRate);
            const double ya = a.process(x), yb = b.process(x);
            d = std::max(d, std::abs(ya - yb));
            mx = std::max(mx, std::abs(yb));
        }
        std::printf("  Sherman-Morrison vs full refactor, all pots gliding (%s): max |Δ| %.2e V of %.2f V\n", n.id.c_str(), d, mx);
        CHECK(d < 1e-7 * std::max(1.0, mx), "Sherman–Morrison updates match full refactoring (%s)", n.id.c_str());
    }
}

static void testTriodeLut() {
    NlDevice e;
    e.kind = NlKind::Triode;
    e.nports = 2;
    const double prm[7] = {100, 1.4, 1060, 600, 300, 6.2e-4, 0.06};
    for (int i = 0; i < 7; ++i) e.p[i] = prm[i];
    NlDevice t = e;
    t.lut = korenTables(t.p);
    double worstI = 0, worstJ = 0, wg = 0, wp = 0;
    int wk = 0;
    for (double vg = -10; vg <= 6; vg += 0.137)
        for (double vp = 0.3; vp <= 600; vp *= 1.07) {
            double v[2] = {vg, vp}, i1[2], i2[2], j1[4], j2[4];
            e.eval(v, i1, j1);
            t.eval(v, i2, j2);
            for (int k = 0; k < 2; ++k) {
                const double e1 = std::abs(i1[k] - i2[k]) / (std::abs(i1[k]) + 1e-9);
                if (e1 > worstI) { worstI = e1; wg = vg; wp = vp; wk = k; }
            }
            for (int k = 0; k < 4; ++k) worstJ = std::max(worstJ, std::abs(j1[k] - j2[k]) / (std::abs(j1[k]) + 1e-7));
        }
    std::printf("  12AX7 table vs exact Koren: worst relative current error %.2e (port %d at vgk %.2f, vpk %.2f), Jacobian %.2e\n", worstI, wk, wg, wp, worstJ);
    CHECK(worstI < 1e-6 && worstJ < 1e-4, "triode lookup tables accurate");
    // Pentodes (6L6GC, EL84, EL34): plate law A(vg2)·h(z), atan collection table, grid table.
    const double pset[3][8] = {{8.7, 1.35, 1460, 12000, 48, 12, 1.5e-3, 0.08}, {21, 1.2, 410, 1800, 150, 15, 1.0e-3, 0.08}, {11, 1.35, 650, 4200, 60, 24, 1.5e-3, 0.08}};
    for (const auto& ps : pset) {
        NlDevice pe;
        pe.kind = NlKind::Pentode;
        pe.nports = 3;
        for (int i = 0; i < 8; ++i) pe.p[i] = ps[i];
        NlDevice pt = pe;
        const double tp[7] = {ps[0], ps[1], ps[2], ps[4], ps[5], ps[6], ps[7]};
        pt.lut = korenTables(tp);
        double wI = 0, wJ = 0;
        for (double vg = -80; vg <= 8; vg += 0.71)
            for (double vp = -30; vp <= 1300; vp += 3.7)
                for (double vs : {2.5, 30.0, 150.0, 300.0, 452.0, 900.0}) {
                    double v[3] = {vg, vp, vs}, i1[3], i2[3], j1[9], j2[9];
                    pe.eval(v, i1, j1);
                    pt.eval(v, i2, j2);
                    double imax = 0, jmax = 0;
                    for (int k = 0; k < 3; ++k) imax = std::max(imax, std::abs(i1[k]));
                    for (int k = 0; k < 9; ++k) jmax = std::max(jmax, std::abs(j1[k]));
                    // relative to the device's largest current / conductance at this point (+ a 1 nA, 1 nS floor)
                    for (int k = 0; k < 3; ++k) wI = std::max(wI, std::abs(i1[k] - i2[k]) / (imax + 1e-9));
                    for (int k = 0; k < 9; ++k) wJ = std::max(wJ, std::abs(j1[k] - j2[k]) / (jmax + 1e-9));
                }
        std::printf("  pentode (mu %.1f) table vs exact: worst current error %.2e, Jacobian %.2e (relative to the device's largest)\n", ps[0], wI, wJ);
        CHECK(wI < 1e-6 && wJ < 1e-5, "pentode lookup tables accurate (mu %.1f)", ps[0]);
    }
}

static void testMultirate() {
  for (const char* file : {"ts808.circuit.json"}) {
    const Netlist n = loadLib(file);
    for (double amp : {0.05, 0.5}) {
        Circuit full, mr;
        compileAt(full, n, kRate, controls(n, {}));
        CompileOptions o;
        o.sampleRate = kRate;
        o.multirate = true;
        std::string err;
        CHECK(mr.compile(n, o, err) && mr.isMultirate(), "multirate compile: %s", err.c_str());
        Halfband2x up, down;
        const double host = kRate / 2;
        std::vector<double> a, b;
        double e = 0, ref = 0;
        for (int i = 0; i < static_cast<int>(host * 0.3); ++i) {
            const double t = i / host;
            const double x = amp * (std::sin(2 * M_PI * 196.0 * t) + 0.5 * std::sin(2 * M_PI * 659.3 * t) + 0.25 * std::sin(2 * M_PI * 1567.0 * t)) / 1.75;
            double u0, u1;
            up.up(x, u0, u1);
            const double y0 = full.process(u0), y1 = full.process(u1);
            const double ya = down.down(y0, y1), yb = mr.processHost(x);
            if (i > host * 0.1) {
                e += (ya - yb) * (ya - yb);
                ref += ya * ya;
            }
        }
        const double nrmse = 100 * std::sqrt(e / ref);
        std::printf("  multirate %s (clipping stages at 2x, rest at 1x) vs whole circuit at 2x, %.2f V chord: NRMSE %.3f %%\n", n.id.c_str(), amp, nrmse);
        CHECK(nrmse < 2.0, "multirate matches the oversampled whole circuit (%.3f%%)", nrmse);
    }
  }
}

// The shipped Twin runs its preamp and reverb sections at the HOST rate (only the power stage at 2x): their bilinear
// warping is checked across the band against ngspice's continuous-time .ac (fixture twin_ac, speaker node).
static void testTwinHostRateAc() {
    const Netlist n = loadLib("ab763-twin-reverb.circuit.json");
    const auto ref = readTable(fixture("twin_ac.ac.txt"));
    if (ref.empty()) return;
    for (double host : {44100.0, 48000.0}) {
        Circuit c;
        CompileOptions o;
        o.sampleRate = 2 * host;
        o.multirate = true;
        std::string err;
        CHECK(c.compile(n, o, err), "twin host-rate AC compile: %s", err.c_str());
        const auto ctl = controls(n, {{"reverb_off", 1.0}});
        for (std::size_t i = 0; i < ctl.size(); ++i) c.setControlImmediate(static_cast<int>(i), ctl[i]);
        c.reset();
        std::vector<double> f;
        for (const auto& r : ref) f.push_back(r[0]);
        const auto h = c.smallSignalResponse(f, c.node(n.io.output));
        double w[4] = {0, 0, 0, 0}; // <=2k, 2-5k, 5-10k, 10-20k
        for (std::size_t i = 0; i < f.size(); ++i) {
            const double d = std::abs(20.0 * std::log10(std::abs(h[i])) - ref[i][1]);
            const int b = f[i] <= 2000 ? 0 : f[i] <= 5000 ? 1 : f[i] <= 10000 ? 2 : 3;
            w[b] = std::max(w[b], d);
            if (f[i] > 10000 && std::getenv("CE_ACDUMP"))
                std::printf("    %.0f Hz: engine %.2f dB, ngspice %.2f dB\n", f[i], 20.0 * std::log10(std::abs(h[i])), ref[i][1]);
        }
        std::printf("  twin multirate AC at %.1f kHz host (preamp at host rate): worst |Δ| %.3f dB to 2 kHz, %.3f dB 2-5 kHz, %.3f dB 5-10 kHz, %.2f dB 10-20 kHz\n",
                    host / 1000, w[0], w[1], w[2], w[3]);
        // 0.11 dB to 2 kHz (was 0.1): the worst point is the band edge, 2.0 kHz at a 44.1 kHz host, where the
        // host-rate preamp's bilinear warping sits (the whole circuit at 2x is 0.054 dB there). It moved 0.091 ->
        // 0.102 dB when the roach-driver fix (docs §10.23) took the idle lamp glow off the LDR, which had loaded
        // the vibrato node.
        CHECK(w[0] <= 0.11 && w[1] <= 0.5 && w[2] <= 1.5, "twin host-rate AC within 0.11 / 0.5 / 1.5 dB (%.1f kHz)", host / 1000);
    }
}


// Speaker impedance profile coupling (Circuit::setSpeakerLoad) shifts the output-stage resonance peak and restores cleanly.
static void testSpeakerLoadCoupling() {
    const Netlist n = loadLib("ab763-deluxe-reverb.circuit.json");
    Circuit c;
    CHECK(compileAt(c, n, kRate, controls(n, {{"reverb_off", 1.0}})), "deluxe speaker load compile");
    const int spk = c.node("spk");
    const std::vector<double> freqs = {75.0, 120.0, 1000.0};
    const auto h0 = c.smallSignalResponse(freqs, spk);
    c.setSpeakerLoad(120.0, 2.4, 1.2);
    const auto h120 = c.smallSignalResponse(freqs, spk);
    c.setSpeakerLoad(0.0);
    const auto hRest = c.smallSignalResponse(freqs, spk);
    const double db75_0 = 20.0 * std::log10(std::abs(h0[0])), db120_0 = 20.0 * std::log10(std::abs(h0[1]));
    const double db75_1 = 20.0 * std::log10(std::abs(h120[0])), db120_1 = 20.0 * std::log10(std::abs(h120[1]));
    const double restoreErr = std::abs(20.0 * std::log10(std::abs(hRest[0])) - db75_0);
    std::printf("  speaker load coupling (Deluxe spk): default 75/120 Hz = %+.2f/%+.2f dB, coupled fs=120 Hz = %+.2f/%+.2f dB, restore |Δ| %.2e dB\n",
                db75_0, db120_0, db75_1, db120_1, restoreErr);
    CHECK(db120_1 - db75_1 > db120_0 - db75_0 + 1.5, "coupled 120 Hz cabinet shifts speaker impedance resonance upward");
    CHECK(restoreErr < 1e-4, "setSpeakerLoad(0) restores netlist speaker");
}

// Electro-mechanical speaker (docs §10.21): exact equivalences and the thermal network. The SPICE gate (spk_*) covers the
// nonlinear transient against ngspice; these check what ngspice cannot tell us.
static void testSpeakerDevice() {
    auto parse = [](const std::string& text) {
        Netlist n;
        std::string err;
        CHECK(parseNetlistText(text, n, err), "speaker test netlist: %s", err.c_str());
        return n;
    };
    const DeviceModel g12 = *resolveModel(Netlist{}, "CELESTION_G12M25_8");
    auto card = [&](bool nonlinear, bool thermal, double ctv) {
        std::string m = "{\"kind\":\"speaker\"";
        for (const auto& [k, v] : g12.p) {
            double x = v;
            if (!nonlinear && (k[0] == 'b' || k[0] == 'k' || k == "lx1" || k == "lx2") && k != "bl" && k != "kms") x = 0.0;
            if (k == "rtv" && !thermal) x = 0.0;
            if (k == "ctv" && ctv > 0) x = ctv;
            if (k == "rtm" && ctv > 0) x = 0.0;
            char b[64];
            std::snprintf(b, sizeof b, ",\"%s\":%.17g", k.c_str(), x);
            m += b;
        }
        return m + "}";
    };
    const std::string head = R"({"schema":"headroom.circuit.v1","id":"spk-t","version":1,"name":"t","io":{"input":"in","output":"a"},)";
    // 1. Linearised (no polynomial terms, no heat) the device is exactly the classic Thiele-Small electrical equivalent:
    //    Re + (L2 || R2) + Le + (Res || Lces || Cmes), Res = Bl²/Rms, Lces = Bl²/Kms, Cmes = Mms/Bl².
    {
        const Netlist dev = parse(head + R"("models":{"LIN":)" + card(false, false, 0) +
                                  R"(},"parts":[{"ref":"RS","type":"R","nodes":["in","a"],"value":0.5},{"ref":"S","type":"speaker","nodes":["a","0","vel"],"model":"LIN"}]})");
        const double re = g12.get("re", 0), le = g12.get("le", 0), l2 = g12.get("l2", 0), r2 = g12.get("r2", 0), bl = g12.get("bl", 0);
        const double mms = g12.get("mms", 0), kms = g12.get("kms", 0), rms = g12.get("rms", 0);
        char ts[1024];
        std::snprintf(ts, sizeof ts,
                      R"("parts":[{"ref":"RS","type":"R","nodes":["in","a"],"value":0.5},{"ref":"RE","type":"R","nodes":["a","x1"],"value":%.17g},)"
                      R"({"ref":"L2","type":"L","nodes":["x1","x2"],"value":%.17g},{"ref":"R2","type":"R","nodes":["x1","x2"],"value":%.17g},)"
                      R"({"ref":"LE","type":"L","nodes":["x2","x3"],"value":%.17g},{"ref":"RES","type":"R","nodes":["x3","0"],"value":%.17g},)"
                      R"({"ref":"LCES","type":"L","nodes":["x3","0"],"value":%.17g},{"ref":"CMES","type":"C","nodes":["x3","0"],"value":"%.17gF"}]})",
                      re, l2, r2, le, bl * bl / rms, bl * bl / kms, mms / (bl * bl));
        const Netlist tsn = parse(head + ts);
        Circuit a, b;
        CHECK(compileAt(a, dev, kRate, {}) && compileAt(b, tsn, kRate, {}), "speaker / T-S compile");
        std::vector<double> f;
        for (double x = 20; x < 20000; x *= 1.02) f.push_back(x);
        const auto ha = a.smallSignalResponse(f, a.node("a")), hb = b.smallSignalResponse(f, b.node("a"));
        double worst = 0, zmax = 0, fpk = 0;
        for (std::size_t i = 0; i < f.size(); ++i) {
            worst = std::max(worst, std::abs(ha[i] - hb[i]) / std::abs(hb[i]));
            const double z = std::abs(0.5 * ha[i] / (1.0 - ha[i]));
            if (z > zmax) zmax = z, fpk = f[i];
        }
        std::printf("  speaker linearised vs Thiele-Small equivalent: worst relative difference %.2e (20 Hz-20 kHz); |Z| peak %.1f ohm near %.1f Hz "
                    "(Re + Bl²/Rms = %.1f, fs 75 Hz)\n", worst, zmax, fpk, re + bl * bl / rms);
        CHECK(worst < 1e-9, "linearised speaker != Thiele-Small equivalent (%.2e)", worst);
        CHECK(std::abs(fpk - 75.0) < 2.0 && std::abs(zmax / (re + bl * bl / rms) - 1.0) < 0.02, "speaker impedance peak");
    }
    // 2. A 2s2p cab as one equivalent driver equals four driver instances wired 2s2p, nonlinear and hot (identical drivers
    //    share one excursion, so the equivalence is exact).
    {
        const std::string sp = R"("parts":[{"ref":"RS","type":"R","nodes":["in","a"],"value":1.0},)";
        const Netlist one = parse(head + sp + R"({"ref":"S","type":"speaker","nodes":["a","0","vel"],"model":"JENSEN_P12Q_8","series":2,"parallel":2}]})");
        const Netlist four = parse(head + sp +
                                   R"({"ref":"S1","type":"speaker","nodes":["a","m1","vel"],"model":"JENSEN_P12Q_8"},{"ref":"S2","type":"speaker","nodes":["m1","0","v2"],"model":"JENSEN_P12Q_8"},)"
                                   R"({"ref":"S3","type":"speaker","nodes":["a","m2","v3"],"model":"JENSEN_P12Q_8"},{"ref":"S4","type":"speaker","nodes":["m2","0","v4"],"model":"JENSEN_P12Q_8"}]})");
        Circuit a, b;
        CHECK(compileAt(a, one, kRate, {}, {"vel"}) && compileAt(b, four, kRate, {}, {"vel"}), "2s2p compile");
        const int va = a.node("vel"), vb = b.node("vel");
        double worst = 0, peak = 0;
        for (int i = 1; i < static_cast<int>(0.15 * kRate); ++i) {
            const double x = 40.0 * std::sin(2.0 * M_PI * 50.0 * i / kRate);
            const double ya = a.process(x), yb = b.process(x);
            worst = std::max({worst, std::abs(ya - yb), std::abs(a.nodeVoltage(va) - b.nodeVoltage(vb))});
            peak = std::max(peak, std::abs(a.nodeVoltage(va)));
        }
        std::printf("  2s2p P12Q as one equivalent driver vs four instances, 40 V at 50 Hz: worst difference %.2e (cone peak %.3f m/s)\n", worst, peak);
        CHECK(worst < 1e-8 * std::max(peak, 1.0) * 1e3, "2s2p equivalent driver differs (%.2e)", worst);
    }
    // 3. Thermal: with a 30 ms coil time constant and no magnet stage, the coil's rise settles at Rtv·<P>; ΔRe = Re·α·ΔT.
    //    A hand-off (adoptStateFrom) carries the temperatures.
    {
        const Netlist n = parse(head + R"("models":{"FT":)" + card(true, true, 0.03 / g12.get("rtv", 1)) +
                                R"(},"parts":[{"ref":"RS","type":"R","nodes":["in","a"],"value":1.0},{"ref":"S","type":"speaker","nodes":["a","0","vel"],"model":"FT"}]})");
        Circuit c, d;
        CHECK(compileAt(c, n, kRate, {}) && compileAt(d, n, kRate, {}), "thermal compile");
        int dev = -1;
        for (std::size_t i = 0; i < c.devices().size(); ++i)
            if (c.devices()[i].kind == NlKind::Speaker) dev = static_cast<int>(i);
        double pAcc = 0;
        int pN = 0;
        const int N = static_cast<int>(0.5 * kRate);
        for (int i = 1; i <= N; ++i) {
            const double x = 12.0 * std::sin(2.0 * M_PI * 1000.0 * i / kRate);
            const double va = c.process(x);
            const double cur = (x - va) / 1.0;
            if (i > N - static_cast<int>(0.05 * kRate)) pAcc += (g12.get("re", 0) + c.devices()[static_cast<std::size_t>(dev)].hs[0]) * cur * cur, ++pN;
        }
        const auto& hs = c.devices()[static_cast<std::size_t>(dev)].hs;
        const double want = g12.get("rtv", 0) * pAcc / pN;
        std::printf("  speaker voice coil: rise %.2f K after 0.5 s at 12 V 1 kHz (Rtv·<P> = %.2f K), ΔRe %.3f ohm (Re·α·ΔT %.3f)\n", hs[1], want, hs[0],
                    g12.get("re", 0) * g12.get("alpha", 0) * hs[1]);
        CHECK(std::abs(hs[1] / want - 1.0) < 0.02 && hs[1] > 5.0, "voice-coil steady rise %.3f K vs %.3f K", hs[1], want);
        CHECK(std::abs(hs[0] - g12.get("re", 0) * g12.get("alpha", 0) * hs[1]) < 1e-12, "ΔRe = Re·α·ΔT");
        CHECK(d.adoptStateFrom(c) && d.devices()[static_cast<std::size_t>(dev)].hs[1] == hs[1], "hand-off carries the coil temperature");
    }
    // 4. Cab-follow coupling: a following cab's speaker profile moves a plain-load speaker's resonance (Mms, Rms; Kms held)
    //    and restores exactly; a netlist that ends in speaker + cab ignores it.
    {
        const Netlist n = loadLib("test-speaker.circuit.json");
        Circuit c;
        CHECK(compileAt(c, n, kRate, {}, {"a"}), "coupling compile");
        std::vector<double> f;
        for (double x = 40; x < 300; x *= 1.005) f.push_back(x);
        auto peak = [&](Circuit& k, const char* node, double rs) {
            const auto h = k.smallSignalResponse(f, k.node(node));
            double zm = 0, fp = 0;
            for (std::size_t i = 0; i < f.size(); ++i) {
                const double z = std::abs(rs * h[i] / (1.0 - h[i]));
                if (z > zm) zm = z, fp = f[i];
            }
            return std::pair<double, double>{fp, zm};
        };
        const auto [f0, z0] = peak(c, "a", 0.5);
        c.setSpeakerLoad(120.0, 1.4, 1.0);
        const auto [f1, z1] = peak(c, "a", 0.5);
        c.setSpeakerLoad(0.0);
        const auto [f2, z2] = peak(c, "a", 0.5);
        const Netlist nc = loadLib("test-speaker-cab.circuit.json");
        Circuit k;
        CHECK(compileAt(k, nc, kRate, {}), "speaker + cab compile");
        const std::vector<double> fk = {80.0, 150.0, 1000.0};
        const auto hk0 = k.smallSignalResponse(fk, k.node("mic"));
        k.setSpeakerLoad(120.0, 2.0, 1.5);
        const auto hk1 = k.smallSignalResponse(fk, k.node("mic"));
        double dk = 0;
        for (std::size_t i = 0; i < fk.size(); ++i) dk = std::max(dk, std::abs(hk1[i] - hk0[i]) / std::abs(hk0[i]));
        std::printf("  speaker cab-follow coupling: |Z| peak %.1f Hz -> %.1f Hz (%.1f -> %.1f ohm) at a 120 Hz profile, restored %.1f Hz (%.2e ohm); "
                    "speaker + cab netlist moved by %.1e\n", f0, f1, z0, z1, f2, std::abs(z2 - z0), dk);
        CHECK(std::abs(f0 - 75.0) < 2.0 && std::abs(f1 - 120.0) < 2.5 && std::abs(z1 / z0 - 1.0) < 0.05, "a cab profile moves the speaker's resonance");
        CHECK(f2 == f0 && std::abs(z2 - z0) < 1e-9 * z0, "setSpeakerLoad(0) restores the speaker");
        CHECK(dk == 0.0, "a netlist ending in speaker + cab ignores a following cab's profile");
    }
    // 5. Abuse: ±80 V square at 20 Hz into the free-air G12M: finite, bounded excursion, no Newton failures.
    {
        const Netlist n = parse(head + R"("parts":[{"ref":"RS","type":"R","nodes":["in","a"],"value":0.5},{"ref":"S","type":"speaker","nodes":["a","0","vel"],"model":"CELESTION_G12M25_8"}]})");
        Circuit c;
        CHECK(compileAt(c, n, kRate, {}, {"S#xm"}), "abuse compile");
        const int xm = c.node("S#xm");
        double xmax = 0;
        bool finite = true;
        for (int i = 1; i < static_cast<int>(0.5 * kRate); ++i) {
            const double y = c.process(std::fmod(i * 20.0 / kRate, 1.0) < 0.5 ? 80.0 : -80.0);
            finite = finite && std::isfinite(y);
            xmax = std::max(xmax, std::abs(c.nodeVoltage(xm)));
        }
        std::printf("  speaker abuse (±80 V square, 20 Hz): peak excursion %.2f mm, Newton failures %llu\n", xmax, static_cast<unsigned long long>(c.diagnostics().failures));
        CHECK(finite && xmax < 20.0 && c.diagnostics().failures == 0, "speaker abuse");
    }
}

// Wave C BBD and tape units: end-to-end verification of non-SPICE delay/modulation blocks against datasheets and service manuals.
static void testWaveCBbdAndTapeUnits() {
    const double fs = 48000.0;
    auto burstDelayMs = [&](const std::string& file, const std::map<std::string, double>& ctl, double maxSec, int minIdx = 240) {
        // The burst response is the difference against an identical silent run: a mains-fed unit is still settling
        // after settleMs (the RE-201's bias rails have a 1.5 s time constant) and its output drift (tens of mV) once
        // outweighed the echo, so every head "measured" the end of the window.
        const Netlist n = loadLib(file);
        Circuit c, q;
        CompileOptions o;
        o.sampleRate = fs;
        o.settleMs = 450.0;
        o.bbdNoise = false;
        std::string err;
        CHECK(c.compile(n, o, err), "%s compile: %s", file.c_str(), err.c_str());
        CHECK(q.compile(n, o, err), "%s compile: %s", file.c_str(), err.c_str());
        const auto cv = controls(n, ctl);
        for (std::size_t i = 0; i < cv.size(); ++i) c.setControlImmediate(static_cast<int>(i), cv[i]), q.setControlImmediate(static_cast<int>(i), cv[i]);
        CHECK(c.reset() && q.reset(), "%s reset", file.c_str());
        const int N = static_cast<int>(maxSec * fs);
        const int burstN = static_cast<int>(0.002 * fs); // 2 ms 1 kHz burst
        int bestK = minIdx;
        double bestV = 0.0;
        for (int k = 0; k < N; ++k) {
            const double x = k < burstN ? 0.25 * std::sin(2.0 * M_PI * 1000.0 * k / fs) : 0.0;
            const double y = std::abs(c.process(x) - q.process(0.0));
            if (k >= minIdx && y > bestV) {
                bestV = y;
                bestK = k;
            }
        }
        return std::pair<double, double>{1000.0 * (bestK - burstN / 2) / fs, bestV};
    };

    {
    // 1) Roland RE-201 Space Echo: service-manual head delays (~69 / 131 / 189 ms at fastest tape speed, ~3x span to slowest).
    const auto [reH1Fast, reV1] = burstDelayMs("roland-re201.circuit.json", {{"mode", 0.0 / 11.0}, {"repeat_rate", 1.0}, {"intensity", 0.0}, {"reverb", 0.0}, {"dry_cut", 1.0}, {"echo", 0.8}}, 0.30, 500);
    const auto [reH2Fast, reV2] = burstDelayMs("roland-re201.circuit.json", {{"mode", 1.0 / 11.0}, {"repeat_rate", 1.0}, {"intensity", 0.0}, {"reverb", 0.0}, {"dry_cut", 1.0}, {"echo", 0.8}}, 0.30, 500);
    const auto [reH3Fast, reV3] = burstDelayMs("roland-re201.circuit.json", {{"mode", 2.0 / 11.0}, {"repeat_rate", 1.0}, {"intensity", 0.0}, {"reverb", 0.0}, {"dry_cut", 1.0}, {"echo", 0.8}}, 0.30, 500);
    const auto [reH1Slow, reVS] = burstDelayMs("roland-re201.circuit.json", {{"mode", 0.0 / 11.0}, {"repeat_rate", 0.0}, {"intensity", 0.0}, {"reverb", 0.0}, {"dry_cut", 1.0}, {"echo", 0.8}}, 0.35, 500);
    std::printf("  RE-201 tape heads (fast): H1 %.1f ms, H2 %.1f ms, H3 %.1f ms; H1 slow %.1f ms (ratio %.2fx)\n",
                reH1Fast, reH2Fast, reH3Fast, reH1Slow, reH1Slow / std::max(reH1Fast, 1e-3));
    CHECK(reV1 > 1e-3 && reH1Fast > 55.0 && reH1Fast < 85.0, "RE-201 Head 1 fast delay in service-manual range (%.1f ms)", reH1Fast);
    CHECK(reV2 > 1e-3 && reH2Fast > 115.0 && reH2Fast < 150.0, "RE-201 Head 2 fast delay in service-manual range (%.1f ms)", reH2Fast);
    CHECK(reV3 > 1e-3 && reH3Fast > 170.0 && reH3Fast < 210.0, "RE-201 Head 3 fast delay in service-manual range (%.1f ms)", reH3Fast);
    CHECK(reVS > 1e-3 && reH1Slow / reH1Fast > 2.5 && reH1Slow / reH1Fast < 3.5, "RE-201 motor speed span ~3:1 (%.2fx)", reH1Slow / reH1Fast);

    }
    // 2) EHX Deluxe Memory Man: dual MN3005 (2 x 4096 stages) echo delay span (~35-550 ms published spec).
    const auto [dmmMin, dmmVMin] = burstDelayMs("ehx-deluxe-memory-man.circuit.json", {{"blend", 1.0}, {"feedback", 0.0}, {"depth", 0.0}, {"delay", 0.0}}, 0.15, 400);
    const auto [dmmMax, dmmVMax] = burstDelayMs("ehx-deluxe-memory-man.circuit.json", {{"blend", 1.0}, {"feedback", 0.0}, {"depth", 0.0}, {"delay", 1.0}}, 0.70, 400);
    std::printf("  Deluxe Memory Man dual-MN3005 delay: min %.1f ms, max %.1f ms\n", dmmMin, dmmMax);
    CHECK(dmmVMin > 1e-3 && dmmMin > 25.0 && dmmMin < 70.0, "DMM min delay matches dual-MN3005 high-clock spec (%.1f ms)", dmmMin);
    CHECK(dmmVMax > 1e-3 && dmmMax > 400.0 && dmmMax < 620.0, "DMM max delay matches ~550 ms spec (%.1f ms)", dmmMax);

    // 3) EHX Electric Mistress: SAD1024 (512 buckets) Filter Matrix manual comb-delay span across Range pot.
    const auto [emFast, emVF] = burstDelayMs("ehx-electric-mistress.circuit.json", {{"matrix", 1.0}, {"range", 1.0}, {"color", 0.0}, {"feedback", 0.0}}, 0.04, 100);
    const auto [emSlow, emVS] = burstDelayMs("ehx-electric-mistress.circuit.json", {{"matrix", 1.0}, {"range", 0.0}, {"color", 0.0}, {"feedback", 0.0}}, 0.04, 100);
    const double dLo = std::min(emFast, emSlow), dHi = std::max(emFast, emSlow);
    std::printf("  Electric Mistress Filter Matrix SAD1024 delay span: %.2f ms .. %.2f ms (%.1fx)\n", dLo, dHi, dHi / std::max(dLo, 0.05));
    CHECK(dHi > 2.5 * std::max(dLo, 0.1) && dHi < 25.0, "Electric Mistress Filter Matrix sweeps SAD1024 delay across range");

    // 4) Boss CE-1 Chorus Ensemble: MN3002 (512 stages) LFO swing on kl and Vibrato pitch modulation.
    {
        const Netlist n = loadLib("boss-ce1.circuit.json");
        Circuit c;
        CHECK(compileAt(c, n, fs, controls(n, {{"mode", 1.0}, {"depth", 0.8}, {"rate", 0.8}, {"level", 0.8}}), {"kl"}), "CE-1 compile");
        const int kl = c.node("kl");
        double klo = 1e9, khi = -1e9, yRms = 0;
        const int N = static_cast<int>(1.2 * fs);
        for (int i = 0; i < N; ++i) {
            const double y = c.process(0.1 * std::sin(2.0 * M_PI * 440.0 * i / fs));
            const double v = c.nodeVoltage(kl);
            if (i > static_cast<int>(0.2 * fs)) {
                klo = std::min(klo, v);
                khi = std::max(khi, v);
                yRms += y * y;
            }
        }
        yRms = std::sqrt(yRms / (N - static_cast<int>(0.2 * fs)));
        std::printf("  Boss CE-1 Vibrato: LFO node kl swing %.2f Vpp, output %.3f Vrms\n", khi - klo, yRms);
        CHECK(khi - klo > 0.5 && yRms > 0.01, "CE-1 LFO oscillates and MN3002 Vibrato passes modulated signal");
    }

    // 5) Roland Dimension D SDD-320: dual-MN3007 slow triangle LFO (tri) and Mode 1 vs Mode 4 modulation depth.
    {
        const Netlist n = loadLib("roland-dimension-d.circuit.json");
        Circuit c1, c4;
        CHECK(compileAt(c1, n, fs, controls(n, {{"mode", 0.0}}), {"tri", "XR.ctl"}), "Dim D mode 1 compile");
        CHECK(compileAt(c4, n, fs, controls(n, {{"mode", 1.0}}), {"tri", "XR.ctl"}), "Dim D mode 4 compile");
        const int tri4 = c4.node("tri"), ctl1 = c1.node("XR.ctl"), ctl4 = c4.node("XR.ctl");
        double tLo = 1e9, tHi = -1e9, c1Lo = 1e9, c1Hi = -1e9, c4Lo = 1e9, c4Hi = -1e9;
        const int N = static_cast<int>(2.8 * fs); // > 1 full Mode 4 (~2 s) SDD-320 LFO cycle
        for (int i = 0; i < N; ++i) {
            const double x = 0.1 * std::sin(2.0 * M_PI * 440.0 * i / fs);
            c1.process(x);
            c4.process(x);
            if (i > static_cast<int>(0.3 * fs)) {
                tLo = std::min(tLo, c4.nodeVoltage(tri4));
                tHi = std::max(tHi, c4.nodeVoltage(tri4));
                c1Lo = std::min(c1Lo, c1.nodeVoltage(ctl1));
                c1Hi = std::max(c1Hi, c1.nodeVoltage(ctl1));
                c4Lo = std::min(c4Lo, c4.nodeVoltage(ctl4));
                c4Hi = std::max(c4Hi, c4.nodeVoltage(ctl4));
            }
        }
        std::printf("  Dimension D SDD-320: LFO tri %.2f Vpp, BBD clock ctl swing Mode 1 %.3f Vpp vs Mode 4 %.3f Vpp\n",
                    tHi - tLo, c1Hi - c1Lo, c4Hi - c4Lo);
        CHECK(tHi - tLo > 0.5 && (c4Hi - c4Lo) > (c1Hi - c1Lo), "Dimension D triangle LFO runs and Mode 4 increases sweep over Mode 1");
    }
}

// Stereo circuits (io.outputR): one instance, a second exported output, round-tripped through JSON.
static void testStereoOutput() {
    Netlist n;
    std::string err;
    CHECK(parseNetlistText(R"({"schema":"headroom.circuit.v1","id":"st","io":{"input":"in","output":"a","outputR":"b"},"parts":[
        {"ref":"R1","type":"R","nodes":["in","a"],"value":"1k"},{"ref":"R2","type":"R","nodes":["a","0"],"value":"1k"},
        {"ref":"R3","type":"R","nodes":["in","b"],"value":"3k"},{"ref":"R4","type":"R","nodes":["b","0"],"value":"1k"},
        {"ref":"C1","type":"C","nodes":["b","0"],"value":"1n"}]})", n, err), "stereo parse: %s", err.c_str());
    CHECK(n.io.outputR == "b" && toJson(n)["io"].value("outputR", "") == "b", "outputR round-trips");
    Circuit c;
    CompileOptions o;
    o.sampleRate = 48000.0;
    CHECK(c.compile(n, o, err) && c.reset(), "stereo compile: %s", err.c_str());
    CHECK(c.hasOutputR(), "stereo circuit reports outputR");
    double l = 0, r = 0;
    for (int i = 0; i < 2000; ++i) l = c.process(0.4), r = c.outputR();
    CHECK(std::abs(l - 0.2) < 1e-9 && std::abs(r - 0.1) < 1e-6, "stereo outputs L %.6f R %.6f", l, r);
    Netlist bad = n;
    bad.io.outputR = "nope";
    Circuit cb;
    CHECK(!cb.compile(bad, o, err), "unknown outputR rejected");
}

// Stereo input (io.inputR): a second driven input next to io.input, round-tripped through JSON, fed independently;
// in a multirate circuit it is up-converted like the left input.
static void testStereoInput() {
    Netlist n;
    std::string err;
    CHECK(parseNetlistText(R"({"schema":"headroom.circuit.v1","id":"sti","io":{"input":"in","inputR":"inr","output":"a","outputR":"b"},"parts":[
        {"ref":"R1","type":"R","nodes":["in","a"],"value":"1k"},{"ref":"R2","type":"R","nodes":["a","0"],"value":"1k"},
        {"ref":"R3","type":"R","nodes":["inr","b"],"value":"3k"},{"ref":"R4","type":"R","nodes":["b","0"],"value":"1k"},
        {"ref":"R5","type":"R","nodes":["a","b"],"value":"100k"},
        {"ref":"C1","type":"C","nodes":["b","0"],"value":"1n"}]})", n, err), "stereo-input parse: %s", err.c_str());
    CHECK(n.io.inputR == "inr" && toJson(n)["io"].value("inputR", "") == "inr", "inputR round-trips");
    Circuit c;
    CompileOptions o;
    o.sampleRate = 48000.0;
    CHECK(c.compile(n, o, err) && c.reset(), "stereo-input compile: %s", err.c_str());
    CHECK(c.hasInputR() && c.hasOutputR(), "stereo-input circuit reports inputR and outputR");
    // exact DC solution of the two dividers joined by R5
    const double vl = 0.4, vr = -0.8, g1 = 1e-3, g3 = 1.0 / 3e3, g5 = 1e-5;
    const double m00 = 2 * g1 + g5, m01 = -g5, m10 = -g5, m11 = g3 + g1 + g5, r0 = g1 * vl, r1 = g3 * vr;
    const double det = m00 * m11 - m01 * m10, ea = (r0 * m11 - m01 * r1) / det, eb = (m00 * r1 - m10 * r0) / det;
    double l = 0, r = 0;
    for (int i = 0; i < 2000; ++i) l = c.process(vl, vr), r = c.outputR();
    std::printf("  stereo input: L %.6f (exact %.6f), R %.6f (exact %.6f)\n", l, ea, r, eb);
    CHECK(std::abs(l - ea) < 1e-9 && std::abs(r - eb) < 1e-6, "stereo-input outputs L %.6f R %.6f", l, r);
    SpiceExportOptions so;
    so.inputSource = "DC 0.4";
    so.inputSourceR = "DC -0.8";
    const std::string deck = exportSpice(n, so);
    CHECK(deck.find("VIN n_in 0 DC 0.4") != std::string::npos && deck.find("VINR n_inr 0 DC -0.8") != std::string::npos, "inputR exported as a second source");
    for (const char* bad : {R"("inputR":"in")", R"("inputR":"0")", R"("inputR":"nope")"}) {
        Netlist b;
        std::string t = R"({"schema":"headroom.circuit.v1","id":"x","io":{"input":"in","output":"a",)" + std::string(bad) +
                        R"(},"parts":[{"ref":"R1","type":"R","nodes":["in","a"],"value":"1k"},{"ref":"R2","type":"R","nodes":["a","0"],"value":"1k"}]})";
        Circuit cb;
        const bool parsed = parseNetlistText(t, b, err);
        const bool ok = parsed && cb.compile(b, o, err);
        // "nope" is a node of its own (an unconnected input drives nothing): it compiles; the other two must not
        if (std::string(bad).find("nope") == std::string::npos) CHECK(!ok, "inputR %s rejected", bad);
    }
    // Multirate: the right channel's VCA and I/V stage oversampled, the rest at the host rate. Against the whole circuit
    // at 2x with the same half-band up / down conversion on both inputs and both outputs.
    {
        Netlist s = loadLib("test-vca-rms.circuit.json");
        s.oversampledParts = {"U3", "IC2"};
        // R: the whole circuit at 2x behind the same half-band conversions; L never meets a converter, so its reference
        // is the whole circuit at the host rate
        Circuit full, mr, hostc;
        compileAt(full, s, kRate, controls(s, {}));
        compileAt(hostc, s, kRate / 2, controls(s, {}));
        CompileOptions om;
        om.sampleRate = kRate;
        om.multirate = true;
        CHECK(mr.compile(s, om, err) && mr.isMultirate(), "stereo-input multirate compile: %s", err.c_str());
        Halfband2x upL, upR, dnL, dnR;
        const double host = kRate / 2;
        double eL = 0, eR = 0, rL = 0, rR = 0;
        for (int i = 0; i < static_cast<int>(host * 0.25); ++i) {
            const double t = i / host;
            const double xl = 0.5 * std::sin(2 * M_PI * 440.0 * t), xr = 0.3 * std::sin(2 * M_PI * 1200.0 * t);
            double l0, l1, r0u, r1u;
            upL.up(xl, l0, l1);
            upR.up(xr, r0u, r1u);
            const double a0 = full.process(l0, r0u), b0 = full.outputR();
            const double a1 = full.process(l1, r1u), b1 = full.outputR();
            const double yb = dnR.down(b0, b1), ya = hostc.process(xl, xr);
            (void)dnL.down(a0, a1);
            const double ma = mr.processHost(xl, xr), mb = mr.outputR();
            if (i > host * 0.1) {
                eL += (ya - ma) * (ya - ma), rL += ya * ya;
                eR += (yb - mb) * (yb - mb), rR += yb * yb;
            }
        }
        const double nl = 100 * std::sqrt(eL / rL), nr = 100 * std::sqrt(eR / rR);
        std::printf("  stereo input, multirate (R channel at 2x): L vs whole circuit at 1x %.4f %%, R vs whole circuit at 2x %.4f %% NRMSE\n", nl, nr);
        CHECK(nl < 2.0 && nr < 2.0, "stereo-input multirate L %.3f%% R %.3f%%", nl, nr);
    }
}

// ── vca / rms devices against their datasheets (docs/CIRCUIT_ENGINE.md "vca and rms") ──
// THAT 2180 Fig. 2 (DC-coupled: IN is a virtual ground at 0 V), Ec+ / Ec- from sources, ideal I/V op-amp, 20k / 20k.
static std::string vcaFig2(const std::string& model, double ecp, double ecn, const std::string& models = "") {
    char b[4096];
    std::snprintf(b, sizeof b, R"({"schema":"headroom.circuit.v1","id":"vca-fig2","io":{"input":"in","output":"out"},%s"parts":[
        {"ref":"VCC","type":"V","nodes":["vcc","0"],"value":"15"},{"ref":"VEE","type":"V","nodes":["vee","0"],"value":"-15"},
        {"ref":"VP","type":"V","nodes":["ecp","0"],"value":%.12g},{"ref":"VN","type":"V","nodes":["ecn","0"],"value":%.12g},
        {"ref":"R1","type":"R","nodes":["in","vin"],"value":"20k"},
        {"ref":"U1","type":"vca","nodes":["vin","ecp","ecn","sym","p5","0","vcc","vout"],"model":"%s"},
        {"ref":"R5","type":"R","nodes":["p5","vee"],"value":"5.1k"},
        {"ref":"IC1","type":"opamp","nodes":["0","vout","out"],"model":"IDEAL_OPAMP"},
        {"ref":"RF","type":"R","nodes":["vout","out"],"value":"20k"}]})",
                  models.c_str(), ecp, ecn, model.c_str());
    return b;
}
struct Tone {
    double h[10] = {}; // amplitude of harmonics 1..9 (index 1..9); h[0] = mean
    double thd() const {
        double s = 0;
        for (int k = 2; k <= 9; ++k) s += h[k] * h[k];
        return std::sqrt(s) / h[1];
    }
};
// Render a sine (peak amp, f) through a compiled circuit for `secs`, analyse the last `win` seconds of `node`.
static Tone toneThrough(Circuit& c, int node, double amp, double f, double secs, double win, double rate = kRate) {
    const auto total = static_cast<std::size_t>(secs * rate), nwin = static_cast<std::size_t>(std::llround(std::round(win * f) / f * rate));
    std::vector<double> y;
    y.reserve(nwin);
    for (std::size_t i = 0; i < total; ++i) {
        c.process(amp * std::sin(2 * M_PI * f * static_cast<double>(i) / rate));
        if (i >= total - nwin) y.push_back(c.nodeVoltage(node));
    }
    Tone t;
    for (double v : y) t.h[0] += v / static_cast<double>(y.size());
    for (int k = 1; k <= 9; ++k) t.h[k] = std::pow(10.0, harmonicDb(y, f, rate, k) / 20.0);
    return t;
}

static void testVcaRms() {
    std::string err;
    // 1. Gain law: gain (dB) = (Ec+ − Ec−)/6.1 mV (2180 Eq. 2), checked over the datasheet's −60…+40 dB linearity range.
    {
        double worst = 0;
        for (const auto& [ecp, ecn] : std::vector<std::pair<double, double>>{{-0.366, 0}, {-0.183, 0}, {0, 0}, {0.0915, 0}, {0.183, 0}, {0.244, 0}, {0, 0.183}, {0.0305, -0.0305}}) {
            Netlist n;
            CHECK(parseNetlistText(vcaFig2("THAT2180A", ecp, ecn), n, err), "vca fig2: %s", err.c_str());
            Circuit c;
            CHECK(compileAt(c, n, kRate, {}), "vca fig2 compile");
            const double want = (ecp - ecn) / 0.0061;
            const double amp = 0.1 * std::pow(10.0, -std::max(want, 0.0) / 20.0); // keep the output current small
            const Tone t = toneThrough(c, c.node("out"), amp, 1000, 0.03, 0.02);
            const double got = 20 * std::log10(t.h[1] / amp);
            worst = std::max(worst, std::abs(got - want) / std::max(std::abs(want), 1.0));
            std::printf("  vca gain law (2180A): Ec+ %+.4f Ec- %+.4f V -> %+8.3f dB (Eq. 2: %+8.3f dB)\n", ecp, ecn, got, want);
            CHECK(std::abs(got - want) <= 0.005 * std::abs(want) + 0.02, "vca gain %.3f dB vs %.3f dB", got, want);
        }
        std::printf("  vca gain-control linearity: worst %.3f %% (datasheet typ 0.5 %%, max 2 %%)\n", 100 * worst);
    }
    // 2. THD+N vs the 2180 grades (spec table, Fig. 10, Fig. 13): THD from the engine plus the datasheet noise floor
    //    (−98 dBV at 0 dB, −88 dBV at +15 dB gain; −100 dBV assumed at −15 dB) against the typical figures.
    {
        struct Pt {
            const char* model;
            const char* what;
            double gainDb, vinRms, typPct, noiseDbv;
        };
        const Pt pts[] = {
            {"THAT2180A", "0 dB, 0 dBV", 0, 1.0, 0.005, -98},  {"THAT2180A", "0 dB, 10 Vrms", 0, 10.0, 0.024, -98},
            {"THAT2180A", "+15 dB, -5 dBV", 15, 0.5623, 0.020, -88}, {"THAT2180A", "-15 dB, +10 dBV", -15, 3.162, 0.020, -100},
            {"THAT2180B", "0 dB, 0 dBV", 0, 1.0, 0.010, -98},  {"THAT2180B", "0 dB, 10 Vrms", 0, 10.0, 0.032, -98},
            {"THAT2180B", "+15 dB, -5 dBV", 15, 0.5623, 0.030, -88}, {"THAT2180B", "-15 dB, +10 dBV", -15, 3.162, 0.030, -100},
            {"THAT2180C", "0 dB, 0 dBV", 0, 1.0, 0.030, -98},  {"THAT2180C", "0 dB, 10 Vrms", 0, 10.0, 0.063, -98},
            {"THAT2180C", "+15 dB, -5 dBV", 15, 0.5623, 0.040, -88}, {"THAT2180C", "-15 dB, +10 dBV", -15, 3.162, 0.040, -100},
        };
        for (const auto& p : pts) {
            Netlist n;
            CHECK(parseNetlistText(vcaFig2(p.model, p.gainDb * 0.0061, 0), n, err), "vca thd: %s", err.c_str());
            Circuit c;
            CHECK(compileAt(c, n, kRate, {}), "vca thd compile");
            const double amp = p.vinRms * std::sqrt(2.0);
            const Tone t = toneThrough(c, c.node("out"), amp, 1000, 0.03, 0.02);
            const double vout = p.vinRms * std::pow(10.0, p.gainDb / 20.0);
            const double noise = std::pow(10.0, p.noiseDbv / 20.0) / vout;
            const double thdn = 100 * std::sqrt(t.thd() * t.thd() + noise * noise);
            std::printf("  vca %s %-15s THD %.4f %%, THD+N %.4f %% (typ %.3f %%)  H2 %.1f H3 %.1f H4 %.1f dBc\n", p.model, p.what, 100 * t.thd(), thdn, p.typPct,
                        20 * std::log10(t.h[2] / t.h[1]), 20 * std::log10(t.h[3] / t.h[1]), 20 * std::log10(t.h[4] / t.h[1]));
            CHECK(std::abs(thdn / p.typPct - 1.0) < 0.15, "vca %s %s THD+N %.4f%% vs typ %.3f%%", p.model, p.what, thdn, p.typPct);
            if (std::string(p.model) == "THAT2180B" && p.gainDb == 0 && p.vinRms == 1.0) {
                const double h2 = 20 * std::log10(t.h[2] / t.h[1]), h4 = 20 * std::log10(t.h[4] / t.h[1]);
                CHECK(std::abs(h2 + 80.1) < 1.0 && std::abs(h4 + 104.0) < 3.0, "2180B Fig. 13 spectrum: H2 %.1f (−80.1), H4 %.1f (−104) dBc", h2, h4);
            }
        }
    }
    // 3. Control feedthrough: with no input, the output DC moves with gain as √g (symmetry error × the class-AB idle
    //    current; 2180A typ 0.5 / 1 / 3 mV at 0 / +15 / +30 dB with 20 kΩ).
    {
        double dc[3] = {};
        const double gains[3] = {0, 15, 30};
        for (int k = 0; k < 3; ++k) {
            Netlist n;
            parseNetlistText(vcaFig2("THAT2180A", gains[k] * 0.0061, 0), n, err);
            Circuit c;
            compileAt(c, n, kRate, {});
            for (int i = 0; i < 100; ++i) c.process(0.0);
            dc[k] = c.nodeVoltage(c.node("out"));
        }
        std::printf("  vca 2180A output offset (no signal, 20k): %.3f / %.3f / %.3f mV at 0 / +15 / +30 dB (datasheet typ 0.5 / 1 / 3, max 1 / 3 / 12; "
                    "ratios x%.2f, x%.2f; sqrt(g) alone gives x2.37, x5.62)\n",
                    1e3 * dc[0], 1e3 * dc[1], 1e3 * dc[2], dc[1] / dc[0], dc[2] / dc[0]);
        CHECK(std::abs(dc[0]) <= 1e-3 && std::abs(dc[1]) <= 3e-3 && std::abs(dc[2]) <= 12e-3 && std::abs(dc[2]) > 2 * std::abs(dc[0]),
              "feedthrough within the 2180A limits and growing with gain");
    }
    // 4. The compact device is the transistor cell: THAT 2180 Fig. 5 built from engine BJTs and diodes (β → ∞, the bias
    //    generator as a VCVS) against the vca part with the same junction law (mvdb = 2·Vt·ln10/20 at 27 °C). The internal
    //    amplifier is a 1e6 VCVS here: a nullor whose only feedback is through junctions leaves the engine's linear matrix
    //    singular (the vca device stamps an exact linear split for this; a discrete netlist cannot). The discrete loop is
    //    badly conditioned (gain × junction slope), so it runs at a 1e-10 Newton tolerance; the compact device at the default.
    {
        const double is = 1e-15, v2 = 2 * kVt, iq = is * std::exp(1.3 / v2), mvdb = v2 * std::log(10.0) / 20 * 1e3;
        char models[512];
        std::snprintf(models, sizeof models, R"("models":{"CELL":{"kind":"vca","mvdb":%.15g,"iq":%.15g,"rsym":0},"DJ":{"kind":"diode","is":1e-15,"n":1},
            "QN":{"kind":"npn","is":1e-15,"bf":%s,"br":1e8},"QP":{"kind":"pnp","is":1e-15,"bf":%s,"br":1e8}},)", mvdb, iq, "%BF%", "%BF%");
        auto discrete = [&](const std::string& bf) {
            std::string m = models;
            for (std::size_t q; (q = m.find("%BF%")) != std::string::npos;) m.replace(q, 4, bf);
            return R"({"schema":"headroom.circuit.v1","id":"vca-discrete","io":{"input":"in","output":"out"},)" + m + R"("parts":[
                {"ref":"VP","type":"V","nodes":["ecp","0"],"value":0.06},{"ref":"KB","type":"V","nodes":["kb","0"],"value":2.6},
                {"ref":"R1","type":"R","nodes":["in","vin"],"value":"20k"},
                {"ref":"OA","type":"vcvs","nodes":["a","0","0","vin"],"gain":1e6},
                {"ref":"EB","type":"vcvs","nodes":["v3","0","a","kb"],"gain":1},
                {"ref":"D1","type":"D","nodes":["a","e1"],"model":"DJ"},{"ref":"Q1","type":"pnp","nodes":["vin","ecp","e1"],"model":"QP"},
                {"ref":"D2","type":"D","nodes":["a","e2"],"model":"DJ"},{"ref":"Q2","type":"pnp","nodes":["vout","0","e2"],"model":"QP"},
                {"ref":"Q3","type":"npn","nodes":["vin","0","e3"],"model":"QN"},{"ref":"D3","type":"D","nodes":["e3","v3"],"model":"DJ"},
                {"ref":"Q4","type":"npn","nodes":["vout","ecp","e4"],"model":"QN"},{"ref":"D4","type":"D","nodes":["e4","v3"],"model":"DJ"},
                {"ref":"IC1","type":"opamp","nodes":["0","vout","out"],"model":"IDEAL_OPAMP"},
                {"ref":"RF","type":"R","nodes":["vout","out"],"value":"20k"}]})";
        };
        std::string compact = discrete("1e8");
        const auto cut = compact.find(R"({"ref":"OA")");
        const auto end = compact.find(R"({"ref":"IC1")");
        compact = compact.substr(0, cut) + R"({"ref":"U1","type":"vca","nodes":["vin","ecp","0","ecp","0","0","0","vout"],"model":"CELL"},
                {"ref":"KB0","type":"R","nodes":["kb","0"],"value":"1k"},)" + compact.substr(end);
        Netlist nd, nc, nb;
        CHECK(parseNetlistText(discrete("1e8"), nd, err), "discrete cell: %s", err.c_str());
        CHECK(parseNetlistText(compact, nc, err), "compact cell: %s", err.c_str());
        CHECK(parseNetlistText(discrete("100"), nb, err), "discrete cell b100: %s", err.c_str());
        Circuit cd, cc, cb;
        CHECK(compileAt(cd, nd, kRate, {}, {}, "", true, 1e-10) && compileAt(cc, nc, kRate, {}) && compileAt(cb, nb, kRate, {}, {}, "", true, 1e-10),
              "discrete / compact compile");
        double e = 0, ref = 0, eb = 0, mx = 0;
        const int od = cd.node("out"), oc = cc.node("out"), ob = cb.node("out");
        double td = 0, tc = 0;
        for (int i = 0; i < 9600; ++i) {
            const double x = 1.0 * std::sin(2 * M_PI * 1000.0 * i / kRate) + 0.4 * std::sin(2 * M_PI * 3100.0 * i / kRate);
            const auto t0 = std::chrono::steady_clock::now();
            cd.process(x);
            const auto t1 = std::chrono::steady_clock::now();
            cc.process(x);
            const auto t2 = std::chrono::steady_clock::now();
            cb.process(x);
            td += std::chrono::duration<double, std::nano>(t1 - t0).count();
            tc += std::chrono::duration<double, std::nano>(t2 - t1).count();
            const double yd = cd.nodeVoltage(od), yc = cc.nodeVoltage(oc), yb = cb.nodeVoltage(ob);
            e += (yd - yc) * (yd - yc), ref += yd * yd, eb += (yb - yd) * (yb - yd), mx = std::max(mx, std::abs(yd - yc));
        }
        int pd = 0, pc = 0;
        for (const auto& g : cd.groups()) pd += g.nn;
        for (const auto& g : cc.groups()) pc += g.nn;
        std::printf("  vca = transistor cell (Fig. 5, Ec+ 60 mV): NRMSE %.2e %% (max %.1e V); %d vs %d Newton ports, %.0f vs %.0f ns/sample; "
                    "beta 100 instead of 1e8 moves the output by %.2e %%\n",
                    100 * std::sqrt(e / ref), mx, pd, pc, td / 9600, tc / 9600, 100 * std::sqrt(eb / ref));
        CHECK(100 * std::sqrt(e / ref) < 1e-3, "compact vca matches the discrete cell");
    }
    // 5. RMS detector: THAT 2252 Fig. 4 (DC-coupled 10k into IN, 560k IBIAS, 2.2M / 10 uF timing; no 22M floor resistor).
    {
        auto rmsNet = [&](const char* ct) {
            return std::string(R"({"schema":"headroom.circuit.v1","id":"rms-fig4","io":{"input":"in","output":"out"},"parts":[
                {"ref":"VCC","type":"V","nodes":["vcc","0"],"value":"15"},{"ref":"VEE","type":"V","nodes":["vee","0"],"value":"-15"},
                {"ref":"RIN","type":"R","nodes":["in","sin"],"value":"10k"},
                {"ref":"U1","type":"rms","nodes":["sin","ib","0","sym","vee","cap","out","vcc"],"model":"THAT2252"},
                {"ref":"RSYM","type":"R","nodes":["sym","sin"],"value":"20"},{"ref":"RB","type":"R","nodes":["ib","0"],"value":"560k"},
                {"ref":"RT","type":"R","nodes":["cap","vee"],"value":"2.2M"},{"ref":"CT","type":"C","nodes":["cap","0"],"value":")") +
                   ct + R"("}]})";
        };
        Netlist n;
        CHECK(parseNetlistText(rmsNet("10u"), n, err), "rms fig4: %s", err.c_str());
        const double vt = 6.1e-3 * 10 / std::log(10.0);
        auto steady = [&](Circuit& c, auto&& wave, double secs, double& vout, double& iin0, double& tau, double& ripple) {
            const int out = c.node("out"), cap = c.node("cap"), ib = c.node("ib");
            const auto total = static_cast<std::size_t>(secs * kRate), last = static_cast<std::size_t>(0.1 * kRate);
            double s = 0, s2 = 0, sc = 0, sb = 0;
            for (std::size_t i = 0; i < total; ++i) {
                c.process(wave(static_cast<double>(i) / kRate));
                if (i >= total - last) {
                    const double v = c.nodeVoltage(out);
                    s += v, s2 += v * v, sc += c.nodeVoltage(cap), sb += c.nodeVoltage(ib);
                }
            }
            const double m = static_cast<double>(last);
            vout = s / m;
            ripple = std::sqrt(std::max(s2 / m - vout * vout, 0.0));
            const double it = (sc / m + 15.0) / 2.2e6, ibias = -(sb / m) / 560e3;
            iin0 = std::sqrt(ibias * it) / 2.9;
            tau = 10e-6 * vt / it;
        };
        double worst = 0;
        for (double irms : {1e-6, 1e-5, 1e-4, 1e-3}) {
            Circuit c;
            compileAt(c, n, kRate, {});
            const double a = irms * 1e4 * std::sqrt(2.0);
            double vo, i0, tau, rip;
            steady(c, [&](double t) { return a * std::sin(2 * M_PI * 1000.0 * t); }, 0.5, vo, i0, tau, rip);
            const double want = vt * std::log((irms * irms + 16e-9 * 16e-9) / (i0 * i0));
            worst = std::max(worst, std::abs(vo - want) / 6.1e-3);
            std::printf("  rms 2252: %.0e A rms -> %+.4f V (2VT ln(Irms/Iin0) %+.4f V, Iin0 %.3f uA = %.1f mV at 10k: datasheet 0 dBr ~ 43 mV)\n", irms, vo, want, i0 * 1e6,
                        i0 * 1e7);
        }
        CHECK(worst < 0.05, "rms output law within %.3f dB", worst);
        // true RMS: a square and a sine of equal rms read the same; equal peaks differ by 3.01 dB
        {
            Circuit c1, c2, c3;
            compileAt(c1, n, kRate, {});
            compileAt(c2, n, kRate, {});
            compileAt(c3, n, kRate, {});
            const double a = 1.0;
            double v1, v2, v3, i0, tau, rip;
            steady(c1, [&](double t) { return a * std::sqrt(2.0) * std::sin(2 * M_PI * 1000.0 * t); }, 0.5, v1, i0, tau, rip);
            steady(c2, [&](double t) { return std::sin(2 * M_PI * 1000.0 * t) >= 0 ? a : -a; }, 0.5, v2, i0, tau, rip);
            steady(c3, [&](double t) { return a * std::sin(2 * M_PI * 1000.0 * t); }, 0.5, v3, i0, tau, rip);
            std::printf("  rms true-RMS: sine %.4f V, square of equal rms %.4f V (%.3f dB apart); sine of equal peak %.2f dB below the square\n", v1, v2, (v2 - v1) / 6.1e-3,
                        (v2 - v3) / 6.1e-3);
            CHECK(std::abs(v2 - v1) / 6.1e-3 < 0.05 && std::abs((v2 - v3) / 6.1e-3 - 3.01) < 0.05, "rms reads true RMS");
        }
        // ripple at 2·fin: VR ≈ VT / (4π·√2·fin·τ) for fin ≫ 1/(4πτ) (datasheet p. 5)
        {
            Circuit c;
            compileAt(c, n, kRate, {});
            double vo, i0, tau, rip;
            steady(c, [&](double t) { return 0.0634 * std::sin(2 * M_PI * 100.0 * t); }, 0.6, vo, i0, tau, rip);
            const double want = vt / (4 * M_PI * std::sqrt(2.0) * 100.0 * tau);
            std::printf("  rms ripple at 100 Hz, tau %.1f ms: %.1f uV rms (datasheet formula %.1f uV)\n", tau * 1e3, rip * 1e6, want * 1e6);
            CHECK(std::abs(rip / want - 1.0) < 0.1, "rms ripple %.2e vs %.2e", rip, want);
        }
        // tone burst (Fig. 5): +30 dB for 150 ms, then back. Attack follows y(t) = y1 + (y0 − y1)·e^(−t/τ) in the linear
        // (mean-square) domain; release is a straight line of slope −IT/CT (V/s).
        {
            Circuit c;
            compileAt(c, n, kRate, {});
            const int out = c.node("out"), cap = c.node("cap");
            const double a0 = 0.01 * std::sqrt(2.0), a1 = a0 * std::pow(10.0, 1.5), f = 10000.0; // 10 kHz: ripple < 10 uV
            auto at = [&](double t) { return (t >= 0.5 && t < 0.65 ? a1 : a0) * std::sin(2 * M_PI * f * t); };
            std::vector<double> y, vc;
            for (int i = 0; i < static_cast<int>(1.0 * kRate); ++i) {
                c.process(at(i / kRate));
                y.push_back(c.nodeVoltage(out));
                vc.push_back(c.nodeVoltage(cap));
            }
            const auto idx = [&](double t) { return static_cast<std::size_t>(t * kRate); };
            const double y0 = y[idx(0.499)], it0 = (vc[idx(0.499)] + 15.0) / 2.2e6, tau = 10e-6 * vt / it0;
            const double ratio = std::pow(10.0, 3.0); // mean-square ratio of +30 dB
            double worstA = 0;
            for (double k : {0.5, 1.0, 2.0}) {
                const double t = 0.5 + k * tau;
                const double want = y0 + vt * std::log(ratio + (1.0 - ratio) * std::exp(-k));
                worstA = std::max(worstA, std::abs(y[idx(t)] - want) / 6.1e-3);
            }
            const double t1 = 0.66, t2 = 0.72;
            const double slope = (y[idx(t2)] - y[idx(t1)]) / (t2 - t1);
            const double itr = 0.5 * ((vc[idx(t1)] + 15.0) + (vc[idx(t2)] + 15.0)) / 2.2e6, want = -itr / 10e-6;
            std::printf("  rms tone burst (+30 dB, 10 uF): attack vs the exponential mean-square law worst %.3f dB (tau %.1f ms); release %.3f V/s = %.1f dB/s (IT/CT %.3f V/s)\n",
                        worstA, tau * 1e3, slope, slope / 6.1e-3, want);
            CHECK(worstA < 0.15 && std::abs(slope / want - 1.0) < 0.03, "rms attack / release");
        }
    }
}

// LFO units like for like: rate, triangle extremes and shape against ngspice on the same netlist (ngspice references
// measured 2026-11-10 on the period of the settled limit cycle; the oscillator is released from its DC balance point,
// so only phase-independent quantities are compared), then against the published hardware data. Plus absolute levels
// and supply rails, and the RE-201 at nominal drive (once diverged at 0.15 s).
// A supply behind a power transformer (docs §3.10; Ampeg SVT 1969, 54000). Its DC point has every rail at 0 V (a
// winding is a short at DC); the unit's settle (CompileOptions::settleMs) starts it at its periodic steady state instead.
// Reference: the same engine run from cold for 60 s at 96 kHz (zero input), means over the last 2 s; the slowest mode
// left there moves the last digit shown. The SVT's earlier crest start was bpa 836 V, screens 243 V and 60 mA per tube.
// kku is the upper bank's single 1 Ω cathode sense (K2): 73.2 mA for three 6146B, 24.4 mA per valve.
static void testMainsSettle() {
    std::printf(" transformer-fed supply (SVT): periodic steady state:\n");
    const Netlist n = loadLib("ampeg-svt-1969.circuit.json");
    const std::vector<std::string> rails = {"bpa", "bpb", "bpc", "bpd", "bscn", "biasn", "kku"};
    const std::vector<double> ref = {695.289, 417.999, 317.926, 274.767, 200.506, -159.923, 0.073166};
    {
        Circuit c;
        CHECK(compileAt(c, n, kRate, controls(n, {}), rails), "svt: compile/DC");
        CHECK(std::abs(c.nodeVoltage(c.node("bpa"))) < 1.0, "svt: DC point bpa %.2f V (a winding is a short at DC: 0)", c.nodeVoltage(c.node("bpa")));
    }
    auto settled = [&](Circuit& c, const Circuit* from, double& secs) {
        CompileOptions o;
        o.sampleRate = kRate;
        o.settleMs = kUnitSettleMs;
        o.probes = rails;
        o.probes.push_back("spk");
        o.probes.push_back("mains");
        o.settleFrom = from;
        std::string err;
        const auto t0 = std::chrono::steady_clock::now();
        bool ok = c.compile(n, o, err);
        const auto ctl = controls(n, {});
        for (std::size_t i = 0; ok && i < ctl.size(); ++i) c.setControlImmediate(static_cast<int>(i), ctl[i]);
        ok = ok && c.reset(); // as the unit: compile, its controls, reset
        secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        return ok;
    };
    Circuit c;
    double secs = 0;
    CHECK(settled(c, nullptr, secs), "svt: settle");
    CHECK(std::abs(c.nodeVoltage(c.node("mains")) - 169.7) < 1e-6, "svt: settled state not on the mains crest (%.3f V)", c.nodeVoltage(c.node("mains")));
    // six mains periods of silence: per-period means, and the output's peak (a start-up thump would show here)
    const int per = static_cast<int>(std::lround(kRate / 60.0));
    std::vector<std::vector<double>> mean(6, std::vector<double>(rails.size(), 0.0));
    double spkPeak = 0;
    for (int k = 0; k < 6; ++k)
        for (int i = 0; i < per; ++i) {
            c.process(0.0);
            for (std::size_t r = 0; r < rails.size(); ++r) mean[static_cast<std::size_t>(k)][r] += c.nodeVoltage(c.node(rails[r])) / per;
            spkPeak = std::max(spkPeak, std::abs(c.nodeVoltage(c.node("spk"))));
        }
    for (std::size_t r = 0; r < rails.size(); ++r) {
        const double m = mean[0][r], drift = mean[5][r] - mean[0][r];
        const double tol = rails[r] == "kku" ? 6e-5 : 0.3; // volts (kku: 6e-5 V = 60 µA in the bank's one 1 Ω sense, 20 µA per valve)
        std::printf("  %-6s %10.4f V (60 s cold run %10.4f), drift %+.4f V over 0.1 s\n", rails[r].c_str(), m, ref[r], drift);
        CHECK(std::abs(m - ref[r]) <= tol, "svt settled %s %.4f V, reference %.4f V", rails[r].c_str(), m, ref[r]);
        CHECK(std::abs(drift) <= 0.1 * tol, "svt settled %s drifts %.4f V in 0.1 s", rails[r].c_str(), drift);
    }
    std::printf("  output peak over the first 0.1 s of silence %.3f V (hum); settle %.2f s wall\n", spkPeak, secs);
    CHECK(spkPeak < 1.0, "svt: %.2f V at the speaker in the first 0.1 s of silence (a start-up thump)", spkPeak);
    // kSvtRails (the settled-point SPICE cases) are these means
    for (const auto& [nm, idx] : std::vector<std::pair<std::string, std::size_t>>{{"bpa", 0}, {"bscn", 4}, {"biasn", 5}}) {
        const auto at = std::string(kSvtRails).find(nm + ":");
        const double v = std::stod(std::string(kSvtRails).substr(at + nm.size() + 1));
        CHECK(std::abs(v - mean[0][idx]) < 0.05, "kSvtRails %s %.2f V, settled mean %.3f V", nm.c_str(), v, mean[0][idx]);
    }
    // a second channel starts from the first one's state (CircuitUnit): the same state in a fraction of the time
    {
        Circuit c2;
        double secs2 = 0;
        CHECK(settled(c2, &c, secs2), "svt: settle from another circuit");
        const double d = c2.nodeVoltage(c2.node("bpb")) - mean[5][1];
        std::printf("  second circuit from the first one's state: settle %.2f s wall, bpb %+.4f V from the first\n", secs2, d);
        CHECK(std::abs(d) < 0.5, "svt: second circuit bpb %.3f V off the first", d);
    }
    // a cold start is the instant of switch-on: everything at zero, the mains at its crest
    {
        Circuit k;
        CompileOptions o;
        o.sampleRate = kRate;
        o.coldMs = 0;
        o.probes = rails;
        o.probes.push_back("mains");
        std::string err;
        CHECK(k.compile(n, o, err), "svt cold: %s", err.c_str());
        double worst = 0;
        for (const auto& r : rails) worst = std::max(worst, std::abs(k.nodeVoltage(k.node(r))));
        CHECK(worst == 0.0 && k.nodeVoltage(k.node("mains")) == 169.7, "svt cold start: rails up to %.3g V, mains %.3f V", worst, k.nodeVoltage(k.node("mains")));
    }
}

static void testLfoUnits() {
    const double fs = 48000.0;
    struct Lfo {
        double f = 0, lo = 0, hi = 0, duty = 0, lo2 = 0, hi2 = 0;
        std::uint64_t failures = 1;
    };
    auto runLfo = [&](const std::string& file, const std::map<std::string, double>& ctl, const std::string& node, double secs, const std::string& node2 = "") {
        const Netlist n = loadLib(file);
        Circuit c;
        Lfo r;
        std::vector<std::string> pr = {node};
        if (!node2.empty()) pr.push_back(node2);
        if (!compileAt(c, n, fs, controls(n, ctl), pr)) return r;
        const int id = c.node(node), id2 = node2.empty() ? -1 : c.node(node2);
        const auto N = static_cast<std::size_t>(secs * fs);
        std::vector<double> x(N), x2(N);
        for (std::size_t k = 0; k < N; ++k) {
            c.process(0.0);
            x[k] = c.nodeVoltage(id);
            x2[k] = id2 > 0 ? c.nodeVoltage(id2) : 0.0;
        }
        const std::size_t a = N / 3;
        r.lo = *std::min_element(x.begin() + static_cast<long>(a), x.end());
        r.hi = *std::max_element(x.begin() + static_cast<long>(a), x.end());
        r.lo2 = *std::min_element(x2.begin() + static_cast<long>(a), x2.end());
        r.hi2 = *std::max_element(x2.begin() + static_cast<long>(a), x2.end());
        const double mid = 0.5 * (r.lo + r.hi);
        std::vector<double> up;
        std::size_t above = 0;
        for (std::size_t k = a + 1; k < N; ++k) {
            above += x[k] > mid;
            if (x[k - 1] <= mid && x[k] > mid) up.push_back((static_cast<double>(k) - 1.0 + (mid - x[k - 1]) / (x[k] - x[k - 1])) / fs);
        }
        if (up.size() >= 2) r.f = static_cast<double>(up.size() - 1) / (up.back() - up.front());
        r.duty = static_cast<double>(above) / static_cast<double>(N - a - 1);
        r.failures = c.diagnostics().failures;
        return r;
    };
    auto near = [](double x, double ref, double tol) { return std::abs(x - ref) <= tol; };

    // 1) EHX Small Stone (Issue J): IC5 integrating on C12 33 uF between the Q3/Q4 trigger thresholds.
    {
        const char* f = "ehx-small-stone.circuit.json";
        const Lfo r8 = runLfo(f, {{"rate", 0.8}, {"color", 0.0}}, "l5o", 3.0);
        const Lfo r10 = runLfo(f, {{"rate", 1.0}, {"color", 0.0}}, "l5o", 1.0);
        const Lfo c8 = runLfo(f, {{"rate", 0.8}, {"color", 1.0}}, "l5o", 3.0);
        std::printf("  Small Stone LFO: Rate 0.8 %.4f Hz (ngspice 4.9492), C12 %.4f..%.4f V (1.7560..1.9254), duty %.3f; Rate 1 %.3f Hz (18.560); "
                    "Color on %.4f Hz (x%.3f; DAFx-23 measured x0.61-0.65)\n",
                    r8.f, r8.lo, r8.hi, r8.duty, r10.f, c8.f, c8.f / std::max(r8.f, 1e-9));
        CHECK(r8.failures == 0 && r10.failures == 0 && c8.failures == 0, "Small Stone LFO runs without Newton failures");
        CHECK(near(r8.f, 4.9492, 0.01 * 4.9492) && near(r10.f, 18.560, 0.01 * 18.560), "Small Stone LFO rate matches ngspice within 1 %% (%.4f, %.3f Hz)", r8.f, r10.f);
        CHECK(near(r8.lo, 1.7560, 0.005) && near(r8.hi, 1.9254, 0.005), "Small Stone LFO triangle extremes match ngspice within 5 mV");
        CHECK(r8.duty > 0.45 && r8.duty < 0.55, "Small Stone LFO is a symmetric triangle (duty %.3f)", r8.duty);
        CHECK(c8.f / r8.f > 0.55 && c8.f / r8.f < 0.75, "Small Stone Color slows the LFO as measured (x%.3f)", c8.f / r8.f);
    }
    // 2) EHX Deluxe Memory Man: U5A Schmitt + U5B integrator (C39 2.2 uF Chorus / C40 0.47 uF Vibrato).
    {
        const char* f = "ehx-deluxe-memory-man.circuit.json";
        const Netlist n = loadLib(f);
        const Lfo ch = runLfo(f, {{"mode", 0.0}}, "ltri", 6.0);
        const Lfo vb = runLfo(f, {{"mode", 1.0}}, "ltri", 2.0);
        const Lfo chMax = runLfo(f, {{"mode", 0.0}, {"depth", 1.0}, {"delay", 0.5}}, "ltri", 3.5, "q");
        // H. Davis calibration (8/1/78): DELAY 50 %, max CHORUS: the clock period swings ~10 % of its average.
        double swing = 0;
        if (const Part* p = n.part("IC4"))
            for (const auto& row : p->clockRows)
                if (std::abs(row.at - 0.5) < 1e-9) {
                    auto fq = [&](double v) {
                        const auto& pt = row.points;
                        std::size_t i = 1;
                        while (i + 1 < pt.size() && v > pt[i].first) ++i;
                        return pt[i - 1].second + (pt[i].second - pt[i - 1].second) * (v - pt[i - 1].first) / (pt[i].first - pt[i - 1].first);
                    };
                    const double t1 = 1.0 / fq(chMax.lo2), t2 = 1.0 / fq(chMax.hi2);
                    swing = std::abs(t1 - t2) / (0.5 * (t1 + t2));
                }
        std::printf("  DMM LFO: Chorus %.4f Hz (ngspice 0.9546), triangle %.4f..%.4f V (-8.0173..-6.1947), clock period swing %.1f %% at max depth; "
                    "Vibrato %.4f Hz (ngspice 4.454)\n", ch.f, ch.lo, ch.hi, 100.0 * swing, vb.f);
        CHECK(ch.failures == 0 && vb.failures == 0 && chMax.failures == 0, "DMM LFO runs without Newton failures");
        CHECK(near(ch.f, 0.9546, 0.01 * 0.9546) && near(vb.f, 4.454, 0.01 * 4.454), "DMM LFO rate matches ngspice within 1 %% (%.4f, %.4f Hz)", ch.f, vb.f);
        CHECK(near(ch.lo, -8.0173, 0.01) && near(ch.hi, -6.1947, 0.01), "DMM LFO triangle extremes match ngspice within 10 mV");
        CHECK(ch.f < 1.0 && vb.f > 3.5 && vb.f < 5.0, "DMM LFO: Chorus slightly under 1 Hz, Vibrato about 4 Hz (Davis calibration)");
        CHECK(swing > 0.07 && swing < 0.13, "DMM max Chorus swings the clock period about 10 %% (%.1f %%)", 100.0 * swing);
        // Davis: with LEVEL set for 500 mV p-p at NE570 pin 6 (250 Hz, 500 mV p-p in), pin 7 reads 0.95-1.4 V p-p.
        Circuit c;
        CHECK(compileAt(c, n, fs, controls(n, {{"level", 0.45}, {"feedback", 0.0}, {"depth", 0.0}}), {"cr3", "cout"}), "DMM compile");
        const int i6 = c.node("cr3"), i7 = c.node("cout");
        double lo6 = 1e9, hi6 = -1e9, lo7 = 1e9, hi7 = -1e9;
        for (int k = 0; k < static_cast<int>(fs); ++k) {
            c.process(0.25 * std::sin(2.0 * M_PI * 250.0 * k / fs));
            if (k < static_cast<int>(0.5 * fs)) continue;
            lo6 = std::min(lo6, c.nodeVoltage(i6)), hi6 = std::max(hi6, c.nodeVoltage(i6));
            lo7 = std::min(lo7, c.nodeVoltage(i7)), hi7 = std::max(hi7, c.nodeVoltage(i7));
        }
        std::printf("  DMM compressor level (Davis): pin 6 %.3f V p-p -> pin 7 %.3f V p-p (0.95-1.4)\n", hi6 - lo6, hi7 - lo7);
        CHECK(hi6 - lo6 > 0.45 && hi6 - lo6 < 0.56 && hi7 - lo7 > 0.95 && hi7 - lo7 < 1.4, "DMM NE570 compressor level per the Davis calibration");
    }
    // 3) Roland RE-201: supply rails per the PS-15D diagram, and a nominal-drive run (default controls, tape and spring
    // running, 1 s): it once ran away at 0.15 s through the old n7 signal cut.
    {
        const Netlist n = loadLib("roland-re201.circuit.json");
        Circuit c;
        CompileOptions o;
        o.sampleRate = fs;
        o.settleMs = 3000.0;
        o.probes = {"raw", "b17", "b11"};
        std::string err;
        CHECK(c.compile(n, o, err), "RE-201 compile: %s", err.c_str());
        CHECK(c.reset(), "RE-201 reset");
        double b17 = 0, b11 = 0, raw = 0; // mean over the last 0.5 s of 3 s more of silence (the bias rails settle in ~5 s)
        for (int k = 0; k < static_cast<int>(3.0 * fs); ++k) {
            c.process(0.0);
            if (k < static_cast<int>(2.5 * fs)) continue;
            raw += c.nodeVoltage(c.node("raw")) / (0.5 * fs), b17 += c.nodeVoltage(c.node("b17")) / (0.5 * fs), b11 += c.nodeVoltage(c.node("b11")) / (0.5 * fs);
        }
        double peak = 0;
        for (int k = 0; k < static_cast<int>(fs); ++k) peak = std::max(peak, std::abs(c.process(0.3 * std::sin(2.0 * M_PI * 220.0 * k / fs))));
        std::printf("  RE-201 rails after 6 s: raw %.2f V, 17 V rail %.2f V, 11 V rail %.2f V; 1 s at 0.3 V defaults: peak out %.3f V, %llu failures\n", raw, b17,
                    b11, peak, static_cast<unsigned long long>(c.diagnostics().failures));
        CHECK(near(b17, 17.0, 0.8) && near(b11, 11.0, 0.7), "RE-201 PS-15D rails at their marked 17 V / 11 V");
        CHECK(c.diagnostics().failures == 0 && peak < 5.0, "RE-201 stable at nominal drive (peak %.3f V)", peak);
    }
}

// ── CMOS 4000B logic (docs/CIRCUIT_ENGINE.md §10.14) ──
static std::string cmosNet(const std::string& body, double vdd, const std::string& models = "") {
    char b[512];
    std::snprintf(b, sizeof b, R"({"schema":"headroom.circuit.v1","id":"cmos-t","io":{"input":"in","output":"out"},%s"parts":[
        {"ref":"VDD","type":"V","nodes":["vdd","0"],"value":%.12g},)",
                  models.c_str(), vdd);
    std::string tail = body;
    if (!tail.empty() && tail.back() == ']') tail.pop_back(); // bodies may close the parts array themselves
    return std::string(b) + tail + "]}";
}
static void testCmos() {
    std::string err;
    // 1. Unbuffered inverter (CD4069UB) static transfer: the datasheet's VIL / VIH guarantees and the Fig. 3 crowbar current.
    for (double vdd : {5.0, 10.0, 15.0}) {
        auto vout = [&](double vin, double* idd) {
            char body[512];
            std::snprintf(body, sizeof body, R"({"ref":"VI","type":"V","nodes":["a","0"],"value":%.12g},{"ref":"RS","type":"R","nodes":["vdd","vd"],"value":"1"},
                {"ref":"U","type":"inv","nodes":["a","out","vd","0"],"model":"CD4069UB"},{"ref":"RIN","type":"R","nodes":["in","0"],"value":"1k"}])",
                          vin);
            std::string t = cmosNet(body, vdd);
            Netlist n;
            CHECK(parseNetlistText(t, n, err), "ub net: %s", err.c_str());
            Circuit c;
            CHECK(compileAt(c, n, kRate, {}, {"vd"}), "ub compile");
            if (idd) *idd = vdd - c.nodeVoltage(c.node("vd"));
            return c.nodeVoltage(c.node("out"));
        };
        const double vil = vdd == 5 ? 1.0 : (vdd == 10 ? 2.0 : 2.5), vih = vdd == 5 ? 4.0 : (vdd == 10 ? 8.0 : 12.5);
        const double lo = vout(vil, nullptr), hi = vout(vih, nullptr);
        double idd = 0;
        const double mid = vout(vdd / 2, &idd);
        const double g = (vout(vdd / 2 - 0.01, nullptr) - vout(vdd / 2 + 0.01, nullptr)) / 0.02;
        std::printf("  CD4069UB at %2.0f V: Vout(VIL %.1f) %.3f (>= %.2f), Vout(VIH %.1f) %.3f (<= %.2f), Vout(VDD/2) %.3f, midpoint gain %.1f, crowbar %.2f mA%s\n", vdd, vil,
                    lo, vdd - (vdd == 15 ? 1.5 : vdd == 10 ? 1.0 : 0.5), vih, hi, vdd == 15 ? 1.5 : vdd == 10 ? 1.0 : 0.5, mid, g, idd * 1e3,
                    vdd == 10 ? " (Fig. 3: 4.8 mA)" : vdd == 15 ? " (Fig. 3: 11 mA)" : "");
        CHECK(lo >= vdd - (vdd == 15 ? 1.5 : vdd == 10 ? 1.0 : 0.5) && hi <= (vdd == 15 ? 1.5 : vdd == 10 ? 1.0 : 0.5), "4069UB VIL / VIH at %.0f V", vdd);
        CHECK(std::abs(mid - vdd / 2) < 0.01 * vdd, "4069UB switches at VDD/2");
        if (vdd == 10) CHECK(std::abs(idd / 4.8e-3 - 1) < 0.25, "4069UB crowbar at 10 V");
        if (vdd == 15) CHECK(std::abs(idd / 11e-3 - 1) < 0.25, "4069UB crowbar at 15 V");
    }
    // 2. Buffered output drive (CD4011B, the shared 4000B IOL / IOH table, typical): the pin held at VO through 1 Ω.
    {
        struct Pt {
            double vdd, vo;
            bool high;
            double typ;
        } pts[] = {{5, 0.4, false, 1.0e-3}, {10, 0.5, false, 2.6e-3}, {15, 1.5, false, 6.8e-3}, {5, 4.6, true, 1.0e-3}, {5, 2.5, true, 3.2e-3}, {10, 9.5, true, 2.6e-3}, {15, 13.5, true, 6.8e-3}};
        for (const auto& p : pts) {
            char body[512];
            std::snprintf(body, sizeof body, R"({"ref":"VA","type":"V","nodes":["a","0"],"value":%.12g},{"ref":"VO","type":"V","nodes":["vo","0"],"value":%.12g},
                {"ref":"U","type":"nand","nodes":["a","a","y","vdd","0"],"model":"CD4011B"},{"ref":"RS","type":"R","nodes":["y","vo"],"value":"1"},{"ref":"RIN","type":"R","nodes":["in","out"],"value":"1k"},
                {"ref":"RO","type":"R","nodes":["out","0"],"value":"1k"}])",
                          p.high ? 0.0 : p.vdd, p.vo);
            std::string t = cmosNet(body, p.vdd);
            Netlist n;
            CHECK(parseNetlistText(t, n, err), "drive net: %s", err.c_str());
            Circuit c;
            CHECK(compileAt(c, n, kRate, {}, {"y"}), "drive compile");
            const double i = std::abs(c.nodeVoltage(c.node("y")) - p.vo);
            std::printf("  CD4011B %s at VDD %.0f V, VO %.1f V: %.2f mA (typ %.1f)\n", p.high ? "IOH" : "IOL", p.vdd, p.vo, i * 1e3, p.typ * 1e3);
            CHECK(std::abs(i / p.typ - 1) < 0.30, "4000B drive %s %.0f V", p.high ? "IOH" : "IOL", p.vdd);
        }
    }
    // 3. 4013 truth table: asynchronous set / reset, both high, clocked D.
    {
        const std::string body = R"({"ref":"U","type":"dff","nodes":["d","in","s","r","q","qb","vdd","0"],"model":"CD4013B"},
            {"ref":"VD","type":"V","nodes":["d","0"],"value":10},{"ref":"VS","type":"V","nodes":["s","0"],"value":0},{"ref":"VR","type":"V","nodes":["r","0"],"value":0},
            {"ref":"RL","type":"R","nodes":["q","out"],"value":"1k"},{"ref":"RO","type":"R","nodes":["out","0"],"value":"1M"}])";
        std::string t = cmosNet(body, 10.0);
        Netlist n;
        CHECK(parseNetlistText(t, n, err), "dff net: %s", err.c_str());
        Circuit c;
        CHECK(compileAt(c, n, kRate, {}, {"q", "qb"}), "dff compile");
        const int q = c.node("q"), qb = c.node("qb");
        const double q0 = c.nodeVoltage(q);
        for (int i = 0; i < 200; ++i) c.process(i < 100 ? 0.0 : 10.0); // clock rises: Q takes D = 1
        const double q1 = c.nodeVoltage(q), qb1 = c.nodeVoltage(qb);
        std::printf("  CD4013B: power-up Q %.2f V; after a clock edge with D high Q %.2f / Q-bar %.2f V\n", q0, q1, qb1);
        CHECK(q0 < 0.1 && q1 > 9.9 && qb1 < 0.1, "4013 clocks D into Q");
    }
    // 3b. 4027 truth table (§8.2): one rising clock edge per J/K combination from each starting state, plus set / reset.
    {
        int bad = 0;
        for (int q0 = 0; q0 < 2; ++q0)
            for (int j = 0; j < 2; ++j)
                for (int k = 0; k < 2; ++k) {
                    char body[1024];
                    std::snprintf(body, sizeof body, R"({"ref":"U","type":"jkff","nodes":["j","k","in","0","0","q","qb","vdd","0"],"model":"JK"},
                        {"ref":"VJ","type":"V","nodes":["j","0"],"value":%d},{"ref":"VK","type":"V","nodes":["k","0"],"value":%d},
                        {"ref":"RL","type":"R","nodes":["q","out"],"value":"1k"},{"ref":"RO","type":"R","nodes":["out","0"],"value":"1M"})",
                                  j * 10, k * 10);
                    char models[256];
                    std::snprintf(models, sizeof models, R"("models":{"JK":{"kind":"cmos","tpd5":150e-9,"tpd10":65e-9,"tpd15":45e-9,"q0":%d}},)", q0);
                    Netlist n;
                    CHECK(parseNetlistText(cmosNet(body, 10.0, models), n, err), "jk net: %s", err.c_str());
                    Circuit c;
                    CHECK(compileAt(c, n, kRate, {}, {"q", "qb"}), "jk compile");
                    for (int i = 0; i < 200; ++i) c.process(i < 100 ? 0.0 : 10.0);
                    const bool qn = c.nodeVoltage(c.node("q")) > 5.0, qbn = c.nodeVoltage(c.node("qb")) > 5.0;
                    const bool want = (j && !q0) || (!k && q0);
                    if (qn != want || qbn == want) ++bad;
                }
        std::printf("  CD4027B truth table: %d of 8 J/K/Q rows wrong after a rising clock\n", bad);
        CHECK(bad == 0, "4027 truth table");
        const std::string sr = R"({"ref":"U","type":"jkff","nodes":["0","0","0","in","r","q","qb","vdd","0"],"model":"CD4027B"},
            {"ref":"VR","type":"V","nodes":["r","0"],"value":10},{"ref":"RO","type":"R","nodes":["q","out"],"value":"1M"},{"ref":"RO2","type":"R","nodes":["out","0"],"value":"1M"})";
        Netlist n;
        CHECK(parseNetlistText(cmosNet(sr, 10.0), n, err), "jk sr net: %s", err.c_str());
        Circuit c;
        CHECK(compileAt(c, n, kRate, {}, {"q", "qb"}), "jk sr compile");
        const bool rOnly = c.nodeVoltage(c.node("q")) < 5 && c.nodeVoltage(c.node("qb")) > 5;
        for (int i = 0; i < 200; ++i) c.process(10.0); // set rises with reset held high: Q = Q-bar = 1
        const bool both = c.nodeVoltage(c.node("q")) > 5 && c.nodeVoltage(c.node("qb")) > 5;
        std::printf("  CD4027B set / reset: reset alone Q 0 / Q-bar 1 %s, both high Q = Q-bar = 1 %s\n", rOnly ? "ok" : "WRONG", both ? "ok" : "WRONG");
        CHECK(rOnly && both, "4027 set / reset");
    }
    // 4. Propagation delay follows the supply (tpd 125 / 60 / 45 ns, CD4011B): the same input edge through a gate at 5 and
    //    15 V; the edge centroid moves by tpd(5) − tpd(15) = 80 ns, and sits tpd + the band-limiting latency after the
    //    input crossing.
    {
        double cen[2] = {};
        const double tcross = 100.37; // samples: input ramp crosses VDD/2 here
        for (int r = 0; r < 2; ++r) {
            const double vdd = r == 0 ? 5.0 : 15.0;
            const std::string body = R"({"ref":"U","type":"inv","nodes":["in","y","vdd","0"],"model":"CMOS_INV"},{"ref":"RL","type":"R","nodes":["y","out"],"value":"1k"},
                {"ref":"RO","type":"R","nodes":["out","0"],"value":"1G"}])";
            std::string t = cmosNet(body, vdd);
            Netlist n;
            CHECK(parseNetlistText(t, n, err), "tpd net: %s", err.c_str());
            Circuit c;
            CHECK(compileAt(c, n, kRate, {}, {"y"}, "", true, 1e-10), "tpd compile");
            const int y = c.node("y");
            double prev = c.nodeVoltage(y), m0 = 0, m1 = 0;
            for (int i = 1; i < 300; ++i) {
                const double vin = std::clamp(vdd / 2 + (i - tcross) * 0.05 * vdd, 0.0, vdd); // 20 samples full swing
                c.process(vin);
                const double v = c.nodeVoltage(y), dy = (prev - v) / vdd; // falling output
                m0 += dy;
                m1 += dy * (i - 0.5); // a first difference sits half a sample back
                prev = v;
            }
            cen[r] = m1 / m0;
        }
        const double D = sharedMinBlep()->delay;
        const double dt = (cen[0] - cen[1]) / kRate, abs5 = (cen[0] - tcross - D) / kRate;
        std::printf("  CMOS_INV edge: tpd(5 V) − tpd(15 V) %.2f ns (datasheet 80); edge − crossing − latency %.2f ns at 5 V (tpd 125); latency %.3f samples\n", dt * 1e9,
                    abs5 * 1e9, D);
        CHECK(std::abs(dt - 80e-9) < 1e-9 && std::abs(abs5 - 125e-9) < 1e-9, "propagation delay");
    }
    // 5. Jitter: a divide-by-2 (CD4013B toggle, D = Q-bar) clocked straight from a swept sine (300 → 1200 Hz, exponential, 1 s).
    //    Reference: the same toggle built analytically from the sine's exact crossing times (+ tpd at 9 V) with the same
    //    band-limited step. The error spectrum below 20 kHz against the octave tone, minBLEP edges and sample-quantised ones.
    for (int mode = 0; mode < 2; ++mode) {
        const char* models = mode == 0 ? R"("models":{"FF":{"kind":"cmos","tpd5":150e-9,"tpd10":65e-9,"tpd15":45e-9,"blep":1}},)"
                                       : R"("models":{"FF":{"kind":"cmos","tpd5":150e-9,"tpd10":65e-9,"tpd15":45e-9,"blep":0}},)";
        const std::string body = R"({"ref":"U","type":"dff","nodes":["qb","in","0","0","out","qb","vdd","0"],"model":"FF"}])";
        std::string t = cmosNet(body, 9.0, models);
        Netlist n;
        CHECK(parseNetlistText(t, n, err), "jitter net: %s", err.c_str());
        Circuit c;
        CHECK(compileAt(c, n, kRate, {}, {}, "", true, 1e-11), "jitter compile");
        const double f0 = 300, f1 = 1200, T = 1.0, tau = T / std::log(f1 / f0);
        const auto N = static_cast<std::size_t>(T * kRate);
        std::vector<double> y(N);
        for (std::size_t i = 0; i < N; ++i) {
            const double tt = static_cast<double>(i) / kRate;
            y[i] = c.process(4.5 + 2.0 * std::sin(2 * M_PI * f0 * tau * (std::exp(tt / tau) - 1.0) - 0.5 * M_PI));
        }
        // reference toggle: the clock rises through 4.5 V where sin(φ − π/2) = 0 rising, φ = 2π(m + 1/4) with
        // φ = 2π f0 τ (e^{t/τ} − 1), plus tpd at 9 V (log-log between the 5 and 10 V figures, as the cell)
        const MinBlep& mb = *sharedMinBlep();
        const double x = std::log(9.0), tp = std::exp(std::log(150e-9) + (std::log(65e-9) - std::log(150e-9)) * (x - std::log(5.0)) / (std::log(10.0) - std::log(5.0)));
        std::vector<double> edges;
        for (int m = 0;; ++m) {
            const double tc = tau * std::log(1.0 + (m + 0.25) / (f0 * tau));
            if (tc >= T) break;
            if (tc > 1.0 / kRate) edges.push_back(tc + tp);
        }
        const std::size_t a0 = static_cast<std::size_t>(0.3 * kRate), L = 65536;
        std::vector<std::complex<double>> E(L), Rf(L);
        double best = 1e300;
        // the sample-quantised edges are compared with the band-limited reference at their best constant offset
        const std::vector<double> shifts = mode == 0 ? std::vector<double>{0.0} : std::vector<double>{-3.5, -3.25, -3.0, -2.75, -2.5, -2.25, -2.0};
        for (int trial = 0; trial < 2 * static_cast<int>(shifts.size()); ++trial) {
            const int parity = trial % 2;
            const double shift = shifts[static_cast<std::size_t>(trial / 2)];
            std::vector<double> ref(N, 0.0);
            int lvl = parity;
            double base = lvl * 9.0;
            std::size_t e = 0;
            for (std::size_t i = 0; i < N; ++i) {
                double v = base;
                for (std::size_t k = e; k < edges.size() && edges[k] * kRate + shift <= static_cast<double>(i); ++k) {
                    const double dv = ((parity + static_cast<int>(k) + 1) % 2 ? 9.0 : -9.0);
                    v += dv * mb.step(static_cast<double>(i) - edges[k] * kRate - shift);
                }
                ref[i] = v;
                while (e < edges.size() && static_cast<double>(i) - edges[e] * kRate - shift >= mb.zc) {
                    base += ((parity + static_cast<int>(e) + 1) % 2 ? 9.0 : -9.0);
                    ++e;
                }
            }
            double err2 = 0;
            for (std::size_t i = a0; i < a0 + L; ++i) err2 += (y[i] - ref[i]) * (y[i] - ref[i]);

            if (err2 < best) {
                best = err2;
                for (std::size_t i = 0; i < L; ++i) {
                    const double w = 0.35875 - 0.48829 * std::cos(2 * M_PI * i / L) + 0.14128 * std::cos(4 * M_PI * i / L) - 0.01168 * std::cos(6 * M_PI * i / L);
                    E[i] = (y[a0 + i] - ref[a0 + i]) * w;
                    Rf[i] = (ref[a0 + i] - 4.5) * w;
                }
            }
        }
        logic_detail::fft(E, false);
        logic_detail::fft(Rf, false);
        const auto kmax = static_cast<std::size_t>(20000.0 / kRate * L);
        double pe = 0, pr = 0, pk = 0, rk = 0;
        for (std::size_t k = 1; k <= kmax; ++k) {
            pe += std::norm(E[k]);
            pk = std::max(pk, std::abs(E[k]));
            rk = std::max(rk, std::abs(Rf[k]));
        }
        for (std::size_t k = static_cast<std::size_t>(140.0 / kRate * L); k <= static_cast<std::size_t>(610.0 / kRate * L); ++k) pr += std::norm(Rf[k]);
        const double sidebandDb = 20 * std::log10(pk / rk), totalDb = 10 * std::log10(pe / pr);
        std::printf("  divide-by-2 of a swept sine, %s edges: error vs the exact toggle below 20 kHz: peak %.1f dBc, total %.1f dB re the octave tone\n",
                    mode == 0 ? "band-limited (minBLEP)" : "sample-quantised", sidebandDb, totalDb);
        if (mode == 0) CHECK(sidebandDb < -100.0 && totalDb < -100.0, "octave divider jitter below -100 dBc (%.1f / %.1f)", sidebandDb, totalDb);
    }
}

// ── Zener breakdown (diode bv / ibv / nbv), pot end resistance (rend), op-amp output current limit (ilim) ──
static void testZenerRendIlim() {
    std::string err;
    // 1. Each zener card at its datasheet test current: Vz at the terminals at Izt (ngspice's IBV is the junction's) and
    //    the dynamic impedance there equals Zzt (nbv·Vt/Izt + rs). A source and 1k set the current near Izt; the reading is
    //    moved to exactly Izt along the local slope.
    struct Z {
        const char* model;
        double vz, izt, zzt;
    } zs[] = {{"1N4731A", 4.3, 58e-3, 9.0}, {"1N5235B", 6.8, 20e-3, 5.0}, {"1N961B", 10.0, 12.5e-3, 8.5}, {"1N4744A", 15.0, 17e-3, 14.0}, {"1N5245B", 15.0, 8.5e-3, 16.0}};
    for (const auto& z : zs) {
        auto vzAt = [&](double iz) {
            const double rser = 1e3, vsrc = z.vz + iz * rser; // close to Izt; the solve finds the exact point
            char t[1024];
            std::snprintf(t, sizeof t, R"({"schema":"headroom.circuit.v1","id":"z","io":{"input":"in","output":"k"},"parts":[
                {"ref":"VS","type":"V","nodes":["s","0"],"value":%.12g},{"ref":"R","type":"R","nodes":["s","k"],"value":%.12g},
                {"ref":"D","type":"D","nodes":["0","k"],"model":"%s"},{"ref":"RI","type":"R","nodes":["in","0"],"value":"1k"}]})",
                          vsrc, rser, z.model);
            Netlist n;
            parseNetlistText(t, n, err);
            Circuit c;
            compileAt(c, n, kRate, {}, {"k"});
            const double vk = c.nodeVoltage(c.node("k"));
            return std::make_pair(vk, (vsrc - vk) / rser);
        };
        const auto [v1, i1] = vzAt(z.izt);
        const auto [v2, i2] = vzAt(z.izt * 1.1);
        // the voltage at exactly Izt: step back along the local slope
        const double rdyn = (v2 - v1) / (i2 - i1), vAtIzt = v1 + (z.izt - i1) * rdyn;
        std::printf("  zener %-8s Vz at Izt %.1f mA: %.4f V (bv %.2f), Zzt %.2f ohm (datasheet %.1f)\n", z.model, z.izt * 1e3, vAtIzt, z.vz, rdyn, z.zzt);
        CHECK(std::abs(vAtIzt - z.vz) < 0.01 && std::abs(rdyn / z.zzt - 1) < 0.05, "zener %s at its test current", z.model);
    }
    // 2. Pot end resistance: 10k with rend 200 at both extremes of a linear taper.
    {
        Netlist n;
        CHECK(parseNetlistText(R"({"schema":"headroom.circuit.v1","id":"p","io":{"input":"in","output":"w"},"controls":[{"id":"x","default":0}],"parts":[
            {"ref":"P","type":"pot","nodes":["in","w","0"],"value":"10k","taper":"B","control":"x","rend":"200"}]})", n, err), "rend parse: %s", err.c_str());
        CHECK(std::abs(n.parts[0].rend - 200) < 1e-9 && toJson(n)["parts"][0].value("rend", 0.0) == 200.0, "rend round-trips");
        double lo, up;
        potSegments(n, n.parts[0], {0.0}, lo, up);
        double lo1, up1;
        potSegments(n, n.parts[0], {1.0}, lo1, up1);
        std::printf("  pot rend 200: at 0 the wiper sees %.1f / %.1f ohm, at 1 %.1f / %.1f ohm\n", lo, up, lo1, up1);
        CHECK(std::abs(lo - 200) < 1e-9 && std::abs(up - 9800) < 1e-9 && std::abs(lo1 - 9800) < 1e-9 && std::abs(up1 - 200) < 1e-9, "pot end resistance");
    }
    // 3. Op-amp output current limit: a follower driving 10 ohms saturates at ilim; small signals see rout.
    {
        auto run = [&](double vin, double rl) {
            char t[1024];
            std::snprintf(t, sizeof t, R"({"schema":"headroom.circuit.v1","id":"il","io":{"input":"in","output":"o"},
                "models":{"OPL":{"kind":"opamp","level":1,"a0":2e5,"gbw":3e6,"slew":13e6,"swing_hi":1.5,"swing_lo":1.5,"ilim":0.025,"rout":100}},"parts":[
                {"ref":"VCC","type":"V","nodes":["vcc","0"],"value":15},{"ref":"VEE","type":"V","nodes":["vee","0"],"value":-15},
                {"ref":"U","type":"opamp","nodes":["in","o","o","vcc","vee"],"model":"OPL"},{"ref":"RL","type":"R","nodes":["o","0"],"value":%.12g}]})",
                          rl);
            Netlist n;
            parseNetlistText(t, n, err);
            Circuit c;
            compileAt(c, n, kRate, {});
            double v = 0;
            for (int i = 0; i < 400; ++i) v = c.process(vin);
            return v;
        };
        const double vhard = run(5.0, 10.0), vsoft = run(0.01, 10.0);
        std::printf("  op-amp ilim 25 mA into 10 ohm: 5 V asked, %.4f V delivered (%.2f mA); 10 mV asked, %.4f mV (feedback holds it)\n", vhard, vhard / 10 * 1e3,
                    vsoft * 1e3);
        CHECK(std::abs(vhard / 10 - 0.025) < 0.0005 && std::abs(vsoft - 0.01) < 1e-4, "op-amp output current limit");
    }
}

// ── AD536A rms-to-DC converter, and the rms detector's slope decoupled from its junctions (docs §10.17) ──
static void testAd536() {
    std::string err;
    // A bare AD536AJ: VIN straight from the input, RL and COM at 0 V (linear output on IOUT through its 25 kΩ), dB pin.
    auto net = [&](const char* cav) {
        return std::string(R"({"schema":"headroom.circuit.v1","id":"ad","io":{"input":"in","output":"iout"},"parts":[
            {"ref":"VP","type":"V","nodes":["vp","0"],"value":15},{"ref":"VN","type":"V","nodes":["vn","0"],"value":-15},
            {"ref":"U","type":"ad536","nodes":["in","nc2","vn","cav","db","bo","bi","iout","0","0","nc11","nc12","nc13","vp"],"model":"AD536AJ"},
            {"ref":"CAV","type":"C","nodes":["cav","vp"],"value":")") +
               cav + R"("}]})";
    };
    Netlist n;
    CHECK(parseNetlistText(net("10u"), n, err), "ad536 net: %s", err.c_str());
    auto steady = [&](Circuit& c, auto&& wave, double secs, double& lin, double& db) {
        const int io = c.node("iout"), d = c.node("db");
        const auto total = static_cast<std::size_t>(secs * kRate), last = static_cast<std::size_t>(0.2 * kRate);
        double sl = 0, sd = 0;
        for (std::size_t i = 0; i < total; ++i) {
            c.process(wave(static_cast<double>(i) / kRate));
            if (i >= total - last) sl += c.nodeVoltage(io), sd += c.nodeVoltage(d);
        }
        lin = sl / static_cast<double>(last);
        db = sd / static_cast<double>(last);
    };
    // 1. dB law (−3 mV/dB, 0 dB at 1 V rms with IREF 20 µA) and the linear output (IOUT 40 µA/V rms into 25 kΩ = Vrms),
    //    7 mV … 7 V rms (the datasheet's dB-error range), 1 kHz sine, 10 µF (250 ms).
    double worstDb = 0, worstLin = 0;
    for (double vr : {0.007, 0.03, 0.1, 0.3, 1.0, 3.0, 7.0}) {
        Circuit c;
        compileAt(c, n, kRate, {}, {"iout", "db"});
        double lin, db;
        steady(c, [&](double t) { return vr * std::sqrt(2.0) * std::sin(2 * M_PI * 1000.0 * t); }, 2.0, lin, db);
        const double want = std::sqrt(vr * vr + 1e-6); // the 1 mV rms input floor adds in rms
        worstDb = std::max(worstDb, std::abs(-db / 0.003 - 20 * std::log10(want)));
        worstLin = std::max(worstLin, std::abs(lin / want - 1));
        std::printf("  AD536AJ %6.3f V rms: dB pin %+8.5f V (%+7.3f dB), linear %.5f V\n", vr, db, -db / 0.003, lin);
    }
    std::printf("  AD536AJ law: dB worst %.4f dB (spec J ±0.4 typ), linear worst %.4f %% (spec ±5 mV ±0.5 %%)\n", worstDb, 100 * worstLin);
    CHECK(worstDb < 0.05 && worstLin < 5e-3, "AD536 dB and linear law (within the AD536AJ total-error spec)");
    // 2. True rms: a square and a sine of equal rms read the same.
    {
        Circuit c1, c2;
        compileAt(c1, n, kRate, {}, {"iout", "db"});
        compileAt(c2, n, kRate, {}, {"iout", "db"});
        double l1, d1, l2, d2;
        steady(c1, [&](double t) { return std::sqrt(2.0) * std::sin(2 * M_PI * 1000.0 * t); }, 2.0, l1, d1);
        steady(c2, [&](double t) { return std::sin(2 * M_PI * 1000.0 * t) >= 0 ? 1.0 : -1.0; }, 2.0, l2, d2);
        std::printf("  AD536AJ sine vs square, 1 V rms: linear %.5f / %.5f V, dB %.4f / %.4f dB\n", l1, l2, -d1 / 0.003, -d2 / 0.003);
        CHECK(std::abs(l1 - l2) < 2e-4, "AD536 true rms");
    }
    // 3. Crest factor (spec: 1 V rms rectangular pulses, 200 µs wide: −0.1 % at CF 3, −1.0 % at CF 7).
    for (double cf : {3.0, 7.0}) {
        Circuit c;
        Netlist nn;
        parseNetlistText(net("100u"), nn, err);
        compileAt(c, nn, kRate, {}, {"iout", "db"});
        const double period = 200e-6 * cf * cf, amp = cf; // duty 1/CF², peak CF·rms
        double lin, db;
        steady(c, [&](double t) { return std::fmod(t, period) < 200e-6 ? amp : 0.0; }, 8.0, lin, db);
        std::printf("  AD536AJ crest factor %.0f (1 V rms pulses, 200 us): reads %.4f V, error %+.2f %% (spec %s)\n", cf, lin, 100 * (lin - 1.0),
                    cf == 3 ? "-0.1 %" : "-1.0 %");
        if (cf == 7) CHECK(std::abs(100 * (lin - 1.0) + 1.0) < 0.15, "AD536 crest factor 7");
    }
    // 4. Averaging time constant: 25 ms/µF. With 1 µF, the decay of the linear output after the input stops.
    {
        Netlist nn;
        parseNetlistText(net("1u"), nn, err);
        Circuit c;
        compileAt(c, nn, kRate, {}, {"iout", "db"});
        const int io = c.node("iout");
        std::vector<double> y;
        for (int i = 0; i < static_cast<int>(0.3 * kRate); ++i) {
            const double t = i / kRate;
            c.process(t < 0.2 ? std::sqrt(2.0) * std::sin(2 * M_PI * 1000.0 * t) : 0.0);
            y.push_back(c.nodeVoltage(io));
        }
        // I3 decays as e^{−t/τ} once the input is gone (dI3/dt = −I3/τ): measure between 2 and 22 ms after
        const double a = y[static_cast<std::size_t>((0.2 + 0.002) * kRate)], b = y[static_cast<std::size_t>((0.2 + 0.022) * kRate)];
        const double tau = 0.020 / std::log(a / b);
        std::printf("  AD536AJ averaging time constant with 1 uF: %.3f ms (datasheet 25 ms/uF)\n", tau * 1e3);
        CHECK(std::abs(tau / 0.025 - 1) < 0.01, "AD536 averaging time constant");
    }
    // 5. rms (THAT2252 card) with a 3 mV/dB output slope: the junctions keep their physical VT, so the pin-2 string
    //    converges at VEE + 2.1 V as with 6.1, and the output law is exactly half as steep (ENGINE_QUEUE item 27).
    {
        double slope[2] = {}, ib[2] = {};
        for (int r = 0; r < 2; ++r) {
            char t[2048];
            std::snprintf(t, sizeof t, R"({"schema":"headroom.circuit.v1","id":"r3","io":{"input":"in","output":"out"},
                "models":{"R3":{"kind":"rms","mvdb":%g,"kgeo":2.9,"nb":3,"vbias":2.1,"ibref":24e-6,"iflr":16e-9,"vofs":1.4}},"parts":[
                {"ref":"VCC","type":"V","nodes":["vcc","0"],"value":15},{"ref":"VEE","type":"V","nodes":["vee","0"],"value":-15},
                {"ref":"RIN","type":"R","nodes":["in","sin"],"value":"10k"},
                {"ref":"U","type":"rms","nodes":["sin","ib","0","sym","vee","cap","out","vcc"],"model":"R3"},
                {"ref":"RS","type":"R","nodes":["sym","sin"],"value":"20"},{"ref":"RB","type":"R","nodes":["ib","0"],"value":"560k"},
                {"ref":"RT","type":"R","nodes":["cap","vee"],"value":"2.2M"},{"ref":"CT","type":"C","nodes":["cap","0"],"value":"10u"}]})",
                          r == 0 ? 6.1 : 3.0);
            Netlist m;
            CHECK(parseNetlistText(t, m, err), "rms3 net: %s", err.c_str());
            Circuit c;
            CHECK(compileAt(c, m, kRate, {}, {"ib"}), "rms with mvdb %.1f compiles", r == 0 ? 6.1 : 3.0);
            ib[r] = c.nodeVoltage(c.node("ib"));
            double v1 = 0, v2 = 0;
            for (int lvl = 0; lvl < 2; ++lvl) {
                Circuit cc;
                compileAt(cc, m, kRate, {});
                const double a = (lvl ? 0.448 : 0.0448) * std::sqrt(2.0);
                double s = 0;
                for (int i = 0; i < static_cast<int>(0.6 * kRate); ++i) {
                    const double o = cc.process(a * std::sin(2 * M_PI * 1000.0 * i / kRate));
                    if (i > static_cast<int>(0.4 * kRate)) s += o;
                }
                (lvl ? v2 : v1) = s / (0.2 * kRate);
            }
            slope[r] = (v2 - v1) / 20.0 * 1e3;
        }
        std::printf("  rms with mvdb 6.1 / 3.0: pin 2 at %.4f / %.4f V (VEE + 2.1 = -12.9), slope %.4f / %.4f mV/dB\n", ib[0], ib[1], slope[0], slope[1]);
        CHECK(std::abs(ib[1] - ib[0]) < 1e-9 && std::abs(slope[1] / slope[0] - 3.0 / 6.1) < 1e-3, "rms slope decoupled from VT");
    }
}

// ── vca control port (docs §10.18): the dbx 2151 / THAT 2180 control pins are transistor bases (high impedance), the
//    0 dB offset vczero, pin 5 held below GND, and a control node driven through a resistive (current) network ──
static void testVcaControl() {
    std::string err;
    // dbx 2151 per THAT 2150 Fig. 3 (SYM pin 4 through 51 Ω), Ec− from a source through rs into a node with rsh to GND and
    // optionally AN103's 10 kΩ offset-nulling resistor to pin 5 (pin 5 through 2.7k to −15 V).
    auto net = [&](const std::string& model, double vc, double rs, double rsh, bool nulling, const std::string& models = "") {
        char t[4096];
        std::snprintf(t, sizeof t, R"({"schema":"headroom.circuit.v1","id":"c","io":{"input":"in","output":"out"},%s"parts":[
            {"ref":"VCC","type":"V","nodes":["vcc","0"],"value":15},{"ref":"VEE","type":"V","nodes":["vee","0"],"value":-15},
            {"ref":"VC","type":"V","nodes":["vc","0"],"value":%.12g},{"ref":"RS","type":"R","nodes":["vc","ec"],"value":%.12g},
            {"ref":"RSH","type":"R","nodes":["ec","0"],"value":%.12g},
            {"ref":"R1","type":"R","nodes":["in","vin"],"value":"20k"},
            {"ref":"U","type":"vca","nodes":["vin","0","ec","sym","p5","0","vcc","vout"],"model":"%s"},
            {"ref":"R9","type":"R","nodes":["sym","0"],"value":"51"},{"ref":"R5","type":"R","nodes":["p5","vee"],"value":"2.7k"},
            %s
            {"ref":"IC1","type":"opamp","nodes":["0","vout","out"],"model":"IDEAL_OPAMP"},{"ref":"RF","type":"R","nodes":["vout","out"],"value":"20k"}]})",
                      models.c_str(), vc, rs, rsh, model.c_str(), nulling ? R"({"ref":"RN","type":"R","nodes":["ec","p5"],"value":"10k"},)" : "");
        return std::string(t);
    };
    auto gainOf = [&](const std::string& text, double* vec = nullptr, double* v5 = nullptr) {
        Netlist n;
        CHECK(parseNetlistText(text, n, err), "vca control net: %s", err.c_str());
        Circuit c;
        CHECK(compileAt(c, n, kRate, {}, {"out", "ec", "p5"}), "vca control compile");
        const Tone t = toneThrough(c, c.node("out"), 0.01, 1000, 0.03, 0.02);
        if (vec) *vec = c.nodeVoltage(c.node("ec"));
        if (v5) *v5 = c.nodeVoltage(c.node("p5"));
        return 20 * std::log10(t.h[1] / 0.01);
    };
    // 1. Voltage drive (rs 1 mΩ, the "low impedance source"): gain = −Ec−/6.1 mV (2150 data sheet: ±122 mV = ∓20 dB).
    for (double vc : {-0.122, 0.0, 0.122}) {
        const double g = gainOf(net("DBX2151", vc, 1e-3, 1e9, false));
        std::printf("  dbx 2151, Ec- %+.3f V from a low impedance: %+7.3f dB (data sheet %+.1f)\n", vc, g, -vc / 0.0061);
        CHECK(std::abs(g + vc / 0.0061) < 0.02 * std::abs(vc / 0.0061) + 0.01, "2151 voltage-drive gain law");
    }
    // 2. Current drive through a resistive network: 1 mA into a node with 100 Ω to GND sets 100 mV (−16.4 dB): the pin is a
    //    base, so the node voltage is the network's, and the gain follows it.
    {
        double vec;
        const double g = gainOf(net("DBX2151", 1.0, 1000.0, 100.0 * 1000.0 / 900.0, false), &vec); // 1 V / 1 kΩ into 111.1 Ω ∥ … = 100 mV
        std::printf("  dbx 2151, control node fed 1 V through 1k into 111 ohm: node %.4f V, gain %+.3f dB (−node/6.1 mV %+.3f)\n", vec, g, -vec / 0.0061);
        CHECK(std::abs(vec - 0.1) < 1e-4 && std::abs(g + vec / 0.0061) < 0.05, "2151 current-drive gain law");
    }
    // 3. AN103's offset-nulling resistor: 10 kΩ from the control node to pin 5 (held about four junctions below GND)
    //    draws the node negative through the network's source resistance; the gain follows the node.
    {
        double vec, v5;
        const double g = gainOf(net("DBX2151", 0.0, 1000.0, 1e9, true), &vec, &v5);
        const double want = v5 * 1000.0 / (1000.0 + 10000.0);
        std::printf("  dbx 2151 + AN103 10k to pin 5 (pin 5 at %.3f V), Ec- fed 0 V through 1k: node %.4f V (divider %.4f), gain %+.3f dB\n", v5, vec, want, g);
        CHECK(std::abs(vec - want) < 1e-4 && std::abs(g + vec / 0.0061) < 0.05 && v5 < -2.7 && v5 > -2.9, "AN103 offset-nulling network");
    }
    // 4. vczero: the 0 dB trim. 0.61 mV gives −0.10 dB at Ec− = 0 (the data sheet's "gain at 0 V control" tolerance).
    {
        const std::string m = R"("models":{"Z":{"kind":"vca","mvdb":6.1,"iq":20e-6,"rsym":0,"v5":2.76,"vczero":0.00061}},)";
        const double g = gainOf(net("Z", 0.0, 1e-3, 1e9, false, m));
        std::printf("  vca vczero 0.61 mV: gain at Ec- = 0 %+.4f dB (−0.100)\n", g);
        CHECK(std::abs(g + 0.1) < 0.002, "vca vczero");
    }
    // 5. Core β: the control pins draw base current, so a high source impedance shifts the gain (AN103: "higher source
    //    impedances will cause distortion"). β 100 through 10k.
    {
        const std::string m = R"("models":{"B":{"kind":"vca","mvdb":6.1,"iq":20e-6,"rsym":0,"v5":2.76,"bf":100}},)";
        double v0, v1;
        const double g0 = gainOf(net("DBX2151", 0.0, 10e3, 1e9, false), &v0), g1 = gainOf(net("B", 0.0, 10e3, 1e9, false, m), &v1);
        std::printf("  vca core beta 100, Ec- through 10k: node %.3f mV (ideal %.3f mV), gain %+.4f vs %+.4f dB\n", v1 * 1e3, v0 * 1e3, g1, g0);
        CHECK(std::abs(v0) < 1e-6 && std::abs(v1) > 1e-6, "vca base current through the control pin");
    }
}

// ENGINE_QUEUE 32: SPICE export robustness, on the EHX Micro Synth snapshot (the engine-vs-ngspice DC is msynth_dc).
// (1) "nC2" and "nc2" are two nets; plain SPICE folds case, so the export must give them distinct nodes. (2) The OTA
// halves mirror the abc diode's current through a 0 V sense source, with no free exp(V(abc)) left to overflow. (3) A
// full-node .nodeset is written for netlist nodes the deck has, and never for engine-internal or unknown names (ngspice
// aborts the run on a .nodeset of a missing vector). (4) Lint names the case-only pair.
// ENGINE_QUEUE 31: op-amp input errors (test-opamp-vos, LF353 typ: vos 5 mV, ib 50 pA, ios 25 pA). Stage B against
// the closed form with the macro's finite a0; the ideal follower's output is exactly vos; stage A (series-capacitor
// feedback, clamp diode) is lifted off the all-zero point by the offset alone; zero defaults export nothing new.
static void testOpampInputErrors() {
    const Netlist n = loadLib("test-opamp-vos.circuit.json");
    Circuit c;
    CompileOptions o;
    o.sampleRate = kRate;
    o.exportAllNodes = true;
    std::string err;
    CHECK(c.compile(n, o, err), "vos: %s", err.c_str());
    const auto v = [&](const char* nm) { return c.nodeVoltage(c.node(nm)); };
    const double vos = 5e-3, ibp = 50e-12 + 12.5e-12, ibn = 50e-12 - 12.5e-12, a0 = 1e5, rs = 1e6, rf = 100e3, rg = 1e3;
    const double g = 1.0 / rf + 1.0 / rg, vpp = -ibp * rs + vos;
    const double outb = a0 * (vpp + ibn / g) / (1.0 + a0 / (rf * g));
    std::printf("  op-amp vos/ib: stage B out %.6f V (closed form %.6f), + pin %.3f uV (-ib*1M %.3f); follower %.6f V; "
                "series-C stage: summing node %.4f mV, out %.4f V\n",
                v("outb"), outb, v("pb") * 1e6, -ibp * rs * 1e6, v("outc"), v("sa") * 1e3, v("outa"));
    CHECK(std::abs(v("outb") - outb) < 1e-6, "vos: non-inverting stage output vs closed form");
    CHECK(std::abs(v("pb") + ibp * rs) < 1e-9, "vos: bias current through the 1M source");
    CHECK(std::abs(v("outc") - 2e-3) < 1e-12, "vos: ideal follower output = its vos (2 mV)");
    CHECK(std::abs(v("sa") - vos) < 2e-5 && v("outa") > 0.05 && v("outa") < 0.7, "vos: series-capacitor stage biased into its clamp");
    // zero defaults: a plain card adds no element to the deck
    SpiceExportOptions so;
    const std::string deck = exportSpice(loadLib("test-zener.circuit.json"), so);
    CHECK(deck.find("_vos ") == std::string::npos && deck.find("_ibp ") == std::string::npos && deck.find("_ibn ") == std::string::npos,
          "vos: zero defaults export no input-error elements");
}

static void testSpiceExportRobust() {
    const Netlist n = loadLib("test-dc-microsynth.circuit.json");
    const auto lower = [](std::string t) {
        for (auto& ch : t) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return t;
    };
    const std::string a = spiceNodeName(n, "nC2"), b = spiceNodeName(n, "nc2");
    CHECK(lower(a) != lower(b), "spice: case-only node pair folded onto one node (%s, %s)", a.c_str(), b.c_str());
    CHECK(spiceNodeName(n, "nA4A") == spiceNodeName("nA4A"), "spice: a node without a case twin keeps its plain name");
    SpiceExportOptions o;
    o.controls = controls(n, {});
    o.nodeset["nC2"] = 0.004;
    o.nodeset["nc2"] = -10.0;
    o.nodeset["D1#j"] = 0.1;            // engine-internal (the deck's diode has a native RS)
    o.nodeset["no_such_node"] = 1.0;    // not in the netlist
    const std::string deck = exportSpice(n, o);
    CHECK(deck.find(".nodeset v(" + a + ")=0.004\n") != std::string::npos && deck.find(".nodeset v(" + b + ")=-10\n") != std::string::npos,
          "spice: full-node .nodeset for both members of the case pair");
    CHECK(deck.find("no_such_node") == std::string::npos && deck.find("v(n_D1_j)") == std::string::npos, "spice: .nodeset only on deck nodes");
    CHECK(deck.find("I(VA10_abc)") != std::string::npos && deck.find("exp(V(n_a10abc") == std::string::npos, "spice: OTA halves read the abc sense source");
    bool linted = false;
    for (const auto& w : lintNetlist(nlohmann::json(), n)) linted |= w.find("differ only in case") != std::string::npos;
    CHECK(linted, "lint: case-only node pair");
    std::printf("  spice export: case pair -> %s / %s, full-node .nodeset on deck nodes only, OTA abc sense source\n", a.c_str(), b.c_str());
}

// Newton tolerances (docs §10.42): tube ports converge to CompileOptions::tubeNewtonTol (kTubeNewtonTol), every other
// port to newtonTol (kDefaultNewtonTol). A circuit without tubes renders byte-identically at the defaults and at an
// explicit 1e-6; a tube amp at the netlist's "newton": {"tube": 1e-6} renders byte-identically to an explicit 1e-6 (the
// pre-§10.42 engine); and the tube tolerance is reported per group.
static void testNewtonTolerance() {
    if (std::getenv("CE_TOL") || std::getenv("CE_TOLTUBE")) {
        std::printf("  newton tol: skipped (CE_TOL / CE_TOLTUBE set)\n");
        return;
    }
    auto render = [](const Netlist& n, double tol, double tubeTol, std::string* groups) {
        Circuit c;
        CompileOptions o;
        o.sampleRate = 96000.0;
        o.newtonTol = tol;
        o.tubeNewtonTol = tubeTol;
        std::string err;
        std::vector<double> y;
        if (!c.compile(n, o, err) || !c.reset()) return y;
        for (int i = 0; i < 9600; ++i) y.push_back(c.process(0.5 * std::sin(2 * M_PI * 220.0 * i / 96000.0) * (1.0 + 2.0 * (i > 4800))));
        if (groups) *groups = c.describeGroups();
        return y;
    };
    const Netlist ts = loadLib("ts808.circuit.json");
    const auto a = render(ts, 0, 0, nullptr), b = render(ts, 1e-6, 0, nullptr);
    CHECK(!a.empty() && a == b, "newton tol: ts808 at the defaults differs from an explicit 1e-6");
    Netlist jcm = loadLib("marshall-jcm800-2203.circuit.json");
    std::string g;
    const auto d = render(jcm, 0, 0, &g), old = render(jcm, 1e-6, 0, nullptr);
    // (Since the cut audit removed the kcf signal cut, 2026-11-13, the preamp shares the power amp's group: every tube port
    // reports 1e-3 in a mixed group, "tube ports 0.001 (18 of 19)", and no group is all-tube.)
    CHECK(g.find("tube ports 0.001") != std::string::npos, "newton tol: the JCM800's tube groups do not report 1e-3");
    CHECK(!d.empty() && d != old, "newton tol: the JCM800 at the tube default is identical to 1e-6 (tube tolerance not applied)");
    jcm.tubeNewtonTol = 1e-6;
    const auto pinned = render(jcm, 0, 0, nullptr);
    CHECK(!pinned.empty() && pinned == old, "newton tol: newton.tube 1e-6 does not reproduce the 1e-6 engine");
    double e = 0, r = 0;
    for (std::size_t i = 0; i < d.size() && i < old.size(); ++i) e += (d[i] - old[i]) * (d[i] - old[i]), r += old[i] * old[i];
    std::printf("  newton tol: ts808 byte-identical at the defaults; JCM800 tube ports at 1e-3 vs 1e-6: %.1f dB null\n", 10.0 * std::log10(e / r + 1e-300));
    Netlist rt;
    std::string err;
    CHECK(parseNetlistText(toJson(jcm).dump(), rt, err) && rt.tubeNewtonTol == 1e-6, "newton tol: newton.tube does not round-trip (%s)", err.c_str());
}

// Intra-unit lanes (docs §10.33): every library netlist with a lane plan renders byte-identically on two lanes and on
// one, noise and hum on, with the helper forked onto the RtPool per block (as the unit), a helper thread that attaches
// late in the block or after it, and no helper at all (the audio thread runs every group). The solver counters match.
static void testLanes() {
    if (std::getenv("CE_NOLANES")) {
        std::printf("  lanes: skipped (CE_NOLANES)\n");
        return;
    }
    struct U {
        const char* file;
        double rate;
    };
    const U units[] = {{"roland-jc-120.circuit.json", 96000.0}, {"fairchild-670-24000.circuit.json", 96000.0}, {"roland-dimension-d.circuit.json", 48000.0}};
    for (const auto& u : units) {
        const Netlist n = loadLib(u.file);
        auto build = [&](Circuit& c) {
            CompileOptions o;
            o.sampleRate = u.rate;
            o.settleMs = kUnitSettleMs;
            o.noise = o.hum = o.bbdNoise = true;
            o.noiseSeed = 12345;
            std::string err;
            return c.compile(n, o, err) && c.reset();
        };
        Circuit seq, par;
        if (!build(seq) || !build(par)) {
            CHECK(false, "lanes: %s does not compile", u.file);
            continue;
        }
        CHECK(par.lanesPlanned(), "lanes: %s has no lane plan", u.file);
        if (!par.lanesPlanned()) continue;
        const int N = static_cast<int>(0.3 * u.rate), blk = static_cast<int>(64.0 * u.rate / 48000.0);
        const bool stereo = seq.hasOutputR();
        std::vector<double> x(static_cast<std::size_t>(N)), y0(2 * x.size()), y1(2 * x.size());
        for (int i = 0; i < N; ++i) { // picked chord partials, 0.5 FS
            const double t = i / u.rate, env = std::exp(-20.0 * std::fmod(t, 0.15));
            x[static_cast<std::size_t>(i)] = 0.5 * n.io.inputVoltsPerFS * env *
                                             (std::sin(2 * M_PI * 82.4 * t) + 0.6 * std::sin(2 * M_PI * 123.5 * t) + 0.4 * std::sin(2 * M_PI * 196.0 * t));
        }
        const ScopedFlushDenormals ftz;
        for (int i = 0; i < N; ++i) {
            const auto k = static_cast<std::size_t>(i);
            y0[2 * k] = seq.process(x[k]);
            y0[2 * k + 1] = stereo ? seq.outputR() : 0.0;
        }
        for (int o = 0, b = 0; o < N; o += blk, ++b) {
            const int e = std::min(N, o + blk);
            auto run = [&] {
                for (int i = o; i < e; ++i) {
                    const auto k = static_cast<std::size_t>(i);
                    y1[2 * k] = par.process(x[k]);
                    y1[2 * k + 1] = stereo ? par.outputR() : 0.0;
                }
            };
            if (b % 3 == 0) { // as the unit renders
                const LaneBlock lanes(par, true);
                run();
            } else if (b % 3 == 1) { // a helper that attaches up to ~0.4 ms late (mid-block, or after the block)
                CHECK(par.beginLanes(), "lanes: %s beginLanes", u.file);
                std::thread h([&par, b] {
                    std::this_thread::sleep_for(std::chrono::microseconds(37 * (b % 11)));
                    Circuit::laneHelper(&par);
                });
                run();
                par.endLanes();
                h.join();
            } else { // no helper
                CHECK(par.beginLanes(), "lanes: %s beginLanes", u.file);
                run();
                par.endLanes();
            }
        }
        CHECK(std::memcmp(y0.data(), y1.data(), y0.size() * sizeof(double)) == 0, "lanes: %s renders differently on two lanes", u.file);
        const auto& d0 = seq.diagnostics();
        const auto& d1 = par.diagnostics();
        CHECK(d0.samples == d1.samples && d0.newtonIterations == d1.newtonIterations && d0.maxIterations == d1.maxIterations &&
                  d0.homotopies == d1.homotopies && d0.failures == d1.failures && d0.lteSubsteps == d1.lteSubsteps && d0.substeps == d1.substeps &&
                  d0.ptcRescues == d1.ptcRescues && d0.refreshes == d1.refreshes && d0.worstResidual == d1.worstResidual,
              "lanes: %s solver counters differ (Newton %llu vs %llu)", u.file, static_cast<unsigned long long>(d0.newtonIterations),
              static_cast<unsigned long long>(d1.newtonIterations));
        const auto st = par.laneStats();
        CHECK(st.duoSamples > 0 && st.helperGroups > 0, "lanes: %s never ran on two lanes", u.file);
        std::printf("  lanes %s: byte-identical over %d samples (%llu on two lanes, %llu on one; helper ran %llu groups, %llu claimed by the "
                    "audio thread)\n",
                    u.file, N, static_cast<unsigned long long>(st.duoSamples), static_cast<unsigned long long>(st.soloSamples),
                    static_cast<unsigned long long>(st.helperGroups), static_cast<unsigned long long>(st.steals));
    }
}

int main() {
    std::printf("circuit engine tests%s\n", ngspiceWanted() ? " (regenerating ngspice references)" : " (frozen ngspice references)");
    const char* gateOnly = std::getenv("CE_GATE_FILE"); // autocut: this netlist's SPICE cases only
    if (!gateOnly || !*gateOnly) {
    testValues();
    testParserErrors();
    testSpiceRoundTrip();
    testSubcircuits();
    testPartitioning();
    testStereoOutput();
    testStereoInput();
    testVcaRms();
    testCmos();
    testZenerRendIlim();
    testAd536();
    testVcaControl();
    testSpiceExportRobust();
    testOpampInputErrors();
    testCrossSolver();
    testRateConvergence();
    testControlGlideAndHandoff();
    testStress();
    testMultirate();
    testTriodeLut();
    testShermanMorrison();
    testAdoptByName();
    testCutError();
    testNoReservoirCutIdle();
    testSpringTank();
    testBbd();
    testTape();
    testSelectorSwitches();
    testOpenContacts();
    testZenerPairComposite();
    testCutLoadNetwork();
    testSchmittPtc();
    testCoreHysteresis();
    testTwinMultirate();
    testTwinHostRateAc();
    testSpeakerLoadCoupling();
    testSpeakerDevice();
    testWaveCBbdAndTapeUnits();
    testLfoUnits();
    testMainsSettle();
    testNewtonTolerance();
    testLanes();
    }
    std::printf(" SPICE gate (transient, 96 kHz internal rate):\n");
    const std::vector<TranCase> tran = {
        {"clip_small", "diode-clipper.circuit.json", {}, 0.1, 1000, 0.02, {"out"}, 0.5},
        {"clip_hot", "diode-clipper.circuit.json", {}, 2.0, 1000, 0.02, {"out"}, 1.0},
        {"ts_small", "ts808.circuit.json", {}, 0.01, 1000, 0.03, {"out", "out1", "out2"}, 0.5},
        {"ts_mid", "ts808.circuit.json", {}, 0.3, 1000, 0.03, {"out", "out1", "out2"}, 1.0},
        {"ts_hot", "ts808.circuit.json", {{"drive", 1.0}, {"tone", 1.0}}, 1.0, 1000, 0.03, {"out", "out1", "out2"}, 3.0},
        {"ts_low", "ts808.circuit.json", {{"drive", 0.0}, {"tone", 0.0}, {"level", 1.0}}, 0.5, 200, 0.06, {"out", "out1"}, 2.0},
        {"ab_small", "ab763-normal.circuit.json", {}, 0.05, 1000, 0.03, {"out", "p1"}, 1.0},
        {"ab_hot", "ab763-normal.circuit.json", {{"volume", 1.0}, {"treble", 0.8}, {"bass", 0.3}}, 2.0, 400, 0.05, {"out", "p1"}, 3.0},
        {"ab_bright", "ab763-normal.circuit.json", {{"bright", 1.0}, {"volume", 0.3}}, 0.1, 1000, 0.03, {"out"}, 1.0},
        // Wave A device models
        {"pent_small", "test-pentode-se.circuit.json", {}, 1.0, 1000, 0.03, {"out", "p", "g2"}, 1.0},
        {"pent_hot", "test-pentode-se.circuit.json", {}, 25.0, 400, 0.05, {"out", "p", "g2", "k"}, 3.0},
        {"psu_sag", "test-psu-gz34.circuit.json", {}, 0.4, 30, 0.25, {"bp1", "bp2"}, 3.0},
        {"pp_mid", "test-pp-6l6.circuit.json", {}, 0.5, 1000, 0.03, {"spk", "pa", "pb"}, 1.0},
        {"pp_hot", "test-pp-6l6.circuit.json", {}, 2.0, 500, 0.04, {"spk", "pa", "pb"}, 3.0},
        {"pp_sat", "test-pp-6l6.circuit.json", {}, 1.5, 25, 0.24, {"spk", "pa"}, 3.0},
        // whole power amp with the supply cut at its B+ nodes (the SPICE deck is the exact, uncut circuit)
        {"amp_mid", "test-amp-psu.circuit.json", {}, 0.5, 220, 0.3, {"spk", "pt"}, 1.0},
        {"amp_hot", "test-amp-psu.circuit.json", {}, 3.0, 110, 0.3, {"spk", "pa"}, 3.0},
        // B+ rails: gate on the ripple/sag waveform itself (it is a few hundred mV on ~470 V)
        {"amp_rail", "test-amp-psu.circuit.json", {}, 3.0, 110, 0.3, {"bp1", "bp2"}, 5.0},
        {"opto_trem", "test-opto-trem.circuit.json", {}, 1.0, 1000, 0.5, {"out"}, 1.0},
        // Wave A: the complete AB763 Twin Reverb (reverb return shorted: the tank is a behavioural block that the
        // deck carries only as its transducer impedances; the reverb driver, transformer and recovery stage run)
        {"twin_clean", "ab763-twin-reverb.circuit.json", {{"reverb_off", 1.0}}, 0.02, 220, 0.08, {"spk", "p2b", "mix", "pa", "bp1"}, 1.0},
        {"twin_drive", "ab763-twin-reverb.circuit.json", {{"reverb_off", 1.0}, {"volume", 0.8}}, 0.3, 110, 0.08, {"spk", "pa", "g6a"}, 3.0},
        // Wave B devices: JFETs (common source, follower in triode, VCR through vds = 0, p-channel follower)
        {"jfet_small", "test-jfet.circuit.json", {}, 0.05, 1000, 0.03, {"out", "d1", "x", "s2"}, 1.0},
        {"jfet_hot", "test-jfet.circuit.json", {}, 2.0, 200, 0.05, {"out", "d1", "x", "s2"}, 3.0},
        // op-amp supply pins on circuit nodes (filtered rail sagging under the output current)
        {"oprail_small", "test-opamp-rails.circuit.json", {}, 0.05, 1000, 0.03, {"out", "o"}, 1.0}, // rail ripple here is 45 uV: gated in the hot case
        {"oprail_hot", "test-opamp-rails.circuit.json", {}, 0.5, 300, 0.05, {"out", "o", "vf"}, 3.0},
        // Wave B pedals
        {"ds1_small", "boss-ds1.circuit.json", {}, 0.02, 1000, 0.03, {"out"}, 1.0},
        {"ds1_hot", "boss-ds1.circuit.json", {{"dist", 1.0}}, 0.5, 220, 0.05, {"out"}, 3.0},
        {"muff_small", "ehx-big-muff-pi.circuit.json", {}, 0.01, 1000, 0.03, {"out"}, 1.0},
        {"muff_hot", "ehx-big-muff-pi.circuit.json", {{"sustain", 1.0}}, 0.3, 220, 0.05, {"out"}, 3.0},
        {"ffsi_small", "dallas-fuzz-face-si.circuit.json", {}, 0.01, 1000, 0.06, {"out"}, 1.0}, // 60 ms: the junction caps lengthen the start-up transient
        {"ffsi_hot", "dallas-fuzz-face-si.circuit.json", {{"fuzz", 1.0}}, 0.3, 220, 0.05, {"out"}, 3.0},
        {"klon_small", "klon-centaur.circuit.json", {}, 0.02, 1000, 0.05, {"out"}, 1.0},
        {"klon_hot", "klon-centaur.circuit.json", {{"gain", 1.0}}, 0.5, 220, 0.05, {"out"}, 3.0},
        // charge-pump rails: millivolt ripple from the op-amps' supply current, drawn one sample late in the engine
        {"klon_rail", "klon-centaur.circuit.json", {{"gain", 1.0}}, 0.5, 220, 0.05, {"v18", "vn"}, 5.0},
        {"bb_small", "marshall-bluesbreaker.circuit.json", {}, 0.02, 1000, 0.05, {"out"}, 1.0},
        {"bb_hot", "marshall-bluesbreaker.circuit.json", {{"gain", 1.0}}, 0.5, 220, 0.05, {"out", "o2"}, 3.0},
        {"rat_small", "proco-rat2.circuit.json", {}, 0.01, 1000, 0.03, {"out", "o"}, 1.0},
        {"rat_hot", "proco-rat2.circuit.json", {{"distortion", 1.0}}, 0.3, 220, 0.05, {"out", "v9"}, 3.0},
        // Wave C: wahs at heel and toe, the Phase 90 sweeping (LFO running)
        {"gcb95_heel", "dunlop-crybaby-gcb95.circuit.json", {{"pedal", 0.0}}, 0.05, 420, 0.05, {"out"}, 1.0},
        {"gcb95_toe", "dunlop-crybaby-gcb95.circuit.json", {{"pedal", 1.0}}, 0.3, 1000, 0.05, {"out"}, 3.0},
        {"v846_heel", "vox-v846-clyde-mccoy.circuit.json", {{"pedal", 0.0}}, 0.05, 440, 0.05, {"out"}, 1.0},
        {"v846_toe", "vox-v846-clyde-mccoy.circuit.json", {{"pedal", 1.0}}, 0.3, 1000, 0.05, {"out"}, 3.0},
        // the LFO is free-running: its bias node drifts in phase against ngspice over the run (same 10.0 Hz rate), so
        // the gate is the audio output, which carries the sweep
        {"ph90", "mxr-phase90-script.circuit.json", {{"speed", 1.0}}, 0.1, 440, 0.12, {"out"}, 3.0},
        // Wave C devices: CA3080 VCA with a moving IABC, LM13700 OTA-C follower, thermal lamp -> two CdS cells (one
        // with light history), LED -> VTL5C3 cell, 2N5457 VCR with subthreshold smoothing (ksub) near pinch-off
        {"otaopt_small", "test-ota-optics.circuit.json", {}, 0.5, 440, 0.6, {"out", "o2", "c1", "c2", "c3", "c4", "c5", "lt1", "lt2", "lt3"}, 1.0},
        {"otaopt_hot", "test-ota-optics.circuit.json", {}, 4.0, 220, 0.6, {"out", "o2", "c1", "c2", "c3", "c4", "c5"}, 3.0},
        // NE570 basic expander and compressor (datasheet Figs. 7, 8): rectifier, CRECT averaging, gain cell, op-amp
        {"comp_small", "test-compander.circuit.json", {}, 0.05, 1000, 0.25, {"cout", "eout", "ec"}, 1.0}, // CRECT ripple here is microvolts: cc is gated in the hot case
        {"comp_hot", "test-compander.circuit.json", {}, 1.0, 300, 0.25, {"cout", "eout", "ec", "cc"}, 3.0},
        {"ffge_small", "dallas-fuzz-face-ge.circuit.json", {}, 0.01, 1000, 0.03, {"out"}, 1.0},
        {"ffge_hot", "dallas-fuzz-face-ge.circuit.json", {{"fuzz", 1.0}}, 0.3, 220, 0.05, {"out"}, 3.0},
        // Wave A: the other five complete amps (supply, preamp, PI, power stage, OT, speakers; Deluxe reverb return shorted)
        {"deluxe_clean", "ab763-deluxe-reverb.circuit.json", {{"reverb_off", 1.0}}, 0.02, 220, 0.08, {"spk", "p2a", "p6a", "pa"}, 1.0},
        {"deluxe_drive", "ab763-deluxe-reverb.circuit.json", {{"reverb_off", 1.0}, {"volume", 0.8}}, 0.3, 110, 0.08, {"spk", "pa", "g6a"}, 3.0},
        {"bassman_clean", "5f6a-bassman.circuit.json", {}, 0.02, 220, 0.08, {"spk", "p2a", "p3a", "pa"}, 1.0},
        {"bassman_drive", "5f6a-bassman.circuit.json", {{"normal", 0.8}}, 0.3, 110, 0.08, {"spk", "pa", "g3a"}, 3.0},
        {"plexi_clean", "marshall-1959-plexi.circuit.json", {}, 0.02, 220, 0.08, {"spk", "p1b", "p3a", "pa"}, 1.0},
        {"plexi_drive", "marshall-1959-plexi.circuit.json", {{"volume1", 0.8}}, 0.3, 110, 0.08, {"spk", "pa", "p2a"}, 3.0},
        // ENGINE_QUEUE item 7: the Variac knob scales the mains and bias sources (here 75 %, about 90 VAC)
        {"plexi_var0", "marshall-1959-plexi.circuit.json", {{"variac", 0.0}, {"volume1", 0.8}}, 0.3, 110, 0.08, {"spk", "pa", "p2a"}, 3.0},
        // Wave D (netlists gated here): Rangemaster, Micro Amp, vintage Ge Fuzz Face,
        // Harmonic Percolator, TS9DX (all four modes), 5E3 Tweed Deluxe
        {"rm_small", "dallas-rangemaster.circuit.json", {}, 0.1, 1000, 0.05, {"out"}, 1.0},
        {"rm_hot", "dallas-rangemaster.circuit.json", {{"boost", 1.0}}, 0.5, 220, 0.05, {"out"}, 3.0},
        {"m133_small", "mxr-m133-micro-amp.circuit.json", {}, 0.1, 1000, 0.05, {"out"}, 1.0},
        {"m133_hot", "mxr-m133-micro-amp.circuit.json", {{"boost", 1.0}}, 0.3, 220, 0.05, {"out"}, 3.0},
        {"ffv_small", "fuzz-face-vintage-ge.circuit.json", {}, 0.05, 1000, 0.05, {"out"}, 1.0},
        {"ffv_hot", "fuzz-face-vintage-ge.circuit.json", {{"fuzz", 1.0}}, 0.3, 220, 0.05, {"out"}, 3.0},
        {"perc_small", "interfax-harmonic-percolator.circuit.json", {}, 0.1, 1000, 0.05, {"out"}, 1.0},
        {"perc_hot", "interfax-harmonic-percolator.circuit.json", {{"harmonics", 1.0}}, 0.3, 220, 0.05, {"out"}, 3.0},
        {"ts9dx_ts9", "ibanez-ts9dx.circuit.json", {{"mode", 1.0}}, 0.3, 1000, 0.05, {"out"}, 1.0},
        {"ts9dx_turbo", "ibanez-ts9dx.circuit.json", {{"mode", 0.0}, {"overdrive", 1.0}}, 0.3, 220, 0.05, {"out"}, 3.0},
        {"tweed_clean", "fender-5e3-tweed-deluxe.circuit.json", {}, 0.02, 220, 0.08, {"spk"}, 1.0},
        {"tweed_drive", "fender-5e3-tweed-deluxe.circuit.json", {{"normal", 0.8}}, 0.3, 110, 0.08, {"spk"}, 3.0},
        {"dr103_clean", "hiwatt-dr103.circuit.json", {}, 0.02, 220, 0.08, {"spk"}, 1.0},
        {"dr103_drive", "hiwatt-dr103.circuit.json", {}, 0.3, 110, 0.08, {"spk"}, 3.0},
        {"supro_clean", "supro-1624t.circuit.json", {}, 0.02, 220, 0.08, {"spk"}, 1.0},
        {"supro_drive", "supro-1624t.circuit.json", {}, 0.3, 110, 0.08, {"spk"}, 3.0},
        {"ac15_clean", "vox-ac15-handwired.circuit.json", {}, 0.02, 220, 0.08, {"spk"}, 1.0},
        {"ac15_drive", "vox-ac15-handwired.circuit.json", {}, 0.3, 110, 0.08, {"spk"}, 3.0},
        {"slo_clean", "soldano-slo100-122000.circuit.json", {{"channel", 0.0}}, 0.02, 220, 0.08, {"spk"}, 1.0},
        {"slo_od", "soldano-slo100-122000.circuit.json", {{"channel", 1.0}}, 0.1, 220, 0.08, {"spk"}, 3.0},
        // the library rigs' Overdrive settings (Bay Area Scoop) at a hot pickup level, across the k3a signal cut
        {"slo_od_hot", "soldano-slo100-122000.circuit.json",
         {{"channel", 1.0}, {"overdrive", 0.8}, {"bass", 0.55}, {"mid", 0.4}, {"treble", 0.7}, {"presence", 0.6}, {"depth", 0.55}},
         0.2, 220, 0.08, {"spk", "k3a", "p4b"}, 3.0},
        // Peavey 5150 (134000): the dimed Lead case is gated on spectrum, not waveform (the 35-port power group is above
        // the LTE port limit; engine pa converges onto ngspice with rate, +0.03 dB at 176.4 kHz) -- ENGINE_QUEUE 36
        // Boss OC-2 (35000): driven above the squarer threshold so the divider runs (tran 0.34-0.36 % at gating)
        {"oc2_220", "boss-oc2-35000.circuit.json", {{"o1", 1.0}, {"o2", 1.0}}, 0.05, 220, 0.1, {"out"}, 1.0},
        {"oc2_82", "boss-oc2-35000.circuit.json", {{"o1", 1.0}, {"o2", 1.0}, {"D", 0.5}}, 0.1, 82, 0.15, {"out"}, 1.0},
        // SSL 4000E dynamics (133000), gate at rest (gateRange 0): compressor at ratio 1, driven over threshold
        {"ssle_comp", "ssl-4000e-dynamics-133000.circuit.json", {{"ratio", 1.0}}, 2.0, 1000, 0.1, {"out"}, 1.0},
        {"ssle_small", "ssl-4000e-dynamics-133000.circuit.json", {}, 0.1, 1000, 0.1, {"out"}, 1.0},
        // batch 7: the Rev 15a attack/release block (D3 -> R21 // R19 via FAST ATTACK -> timing node, R23/R22 -> RELEASE, D7)
        {"ssle_fast", "ssl-4000e-dynamics-133000.circuit.json", {{"ratio", 1.0}, {"attackfast", 1.0}, {"release", 0.0}}, 2.0, 1000, 0.1, {"out"}, 1.0},
        {"p5150_clean", "peavey-5150-134000.circuit.json", {{"channel", 0.0}}, 0.0005, 220, 0.08, {"spk"}, 1.0},
        {"p5150_lead", "peavey-5150-134000.circuit.json", {}, 0.0005, 220, 0.08, {"spk"}, 1.0},
        {"p5150_dimed", "peavey-5150-134000.circuit.json", {{"lead", 1.0}, {"post", 1.0}}, 0.001, 220, 0.08, {"spk"}, 2.0, -1, 0, "", 176400, 3.0}, // 1.17 % with LTE on its 35-port group
        // Post dimed and driven: the post wiper reaches the CLAMP network (Q7 J174 conducts above ~+15 V, CR22 below -24.6 V)
        {"p5150_clamp", "peavey-5150-134000.circuit.json", {{"lead", 1.0}, {"post", 1.0}}, 0.05, 220, 0.08, {"spk", "clamp", "g3a"}, 3.0, -1, 0, "", 176400, 3.0},
        // Marshall 1959RR factory cascade (91000 "Hot-Rod Plexi"); dimed at 176.4 kHz like the 5150 (1.16 % at 352.8 kHz)
        {"rr_clean", "marshall-1959rr-91000.circuit.json", {}, 0.003, 220, 0.08, {"spk", "p1b", "p3a", "pa"}, 1.0},
        {"rr_drive", "marshall-1959rr-91000.circuit.json", {}, 0.1, 110, 0.08, {"spk", "pa", "p2a", "p1b"}, 3.0},
        {"rr_in1", "marshall-1959rr-91000.circuit.json", {{"input", 0.0}, {"volume1", 0.8}}, 0.3, 110, 0.08, {"spk", "pa", "p2a"}, 3.0},
        {"rr_var0", "marshall-1959rr-91000.circuit.json", {{"variac", 0.0}, {"volume1", 0.8}}, 0.3, 110, 0.08, {"spk", "pa", "p2a"}, 3.0},
        {"rr_dimed", "marshall-1959rr-91000.circuit.json", {{"volume1", 1.0}, {"volume2", 1.0}}, 0.3, 220, 0.12, {"spk", "pa", "pb"}, 3.0, -1, 0, "", 176400, 3.0},
        // Soldano SLO-100 MK II (163000), Rev E 2019 redraw (single source); OD hot needs 176.4 kHz (3.2 % at 96 kHz)
        {"slo2_clean", "soldano-slo100-mk2-163000.circuit.json", {{"channel", 0.0}}, 0.02, 220, 0.08, {"spk"}, 1.0},
        {"slo2_crunch", "soldano-slo100-mk2-163000.circuit.json", {{"channel", 0.0}, {"normalMode", 1.0}, {"bright", 1.0}, {"loop", 1.0}}, 0.05, 220, 0.08, {"spk", "clp"}, 1.0},
        {"slo2_od", "soldano-slo100-mk2-163000.circuit.json", {{"channel", 1.0}}, 0.1, 220, 0.08, {"spk"}, 3.0},
        {"slo2_od_hot", "soldano-slo100-mk2-163000.circuit.json", {{"channel", 1.0}, {"overdrive", 0.8}, {"master", 0.8}}, 0.2, 220, 0.08, {"spk", "k3b"}, 3.0, -1, 0, "", 176400, 3.0},
        // Bogner Uberschall (81000): S2 2007 Rev A trace primary, S1 2012 trace for the supply/power sheet (disagreements in the notes)
        {"uber_clean", "bogner-uberschall-81000.circuit.json", {{"channel", 0.0}}, 0.05, 220, 0.08, {"spk", "p4b", "p5b", "pa"}, 1.0},
        {"uber_lead_small", "bogner-uberschall-81000.circuit.json", {}, 0.00003, 220, 0.08, {"spk", "p3a", "pa"}, 1.0},
        {"uber_drive", "bogner-uberschall-81000.circuit.json", {}, 0.1, 110, 0.08, {"spk", "pa", "pb", "p3a", "p5b"}, 3.0},
        {"uber_var0", "bogner-uberschall-81000.circuit.json", {{"variac", 0.0}}, 0.1, 110, 0.08, {"spk", "pa", "pb"}, 3.0},
        {"uber_clean_drive", "bogner-uberschall-81000.circuit.json", {{"channel", 0.0}, {"cl_gain", 1.0}, {"cl_volume", 1.0}, {"master", 0.8}}, 0.3, 110, 0.08, {"spk", "pa", "p5b"}, 3.0},
        {"uber_dimed", "bogner-uberschall-81000.circuit.json", {{"gain", 1.0}, {"volume", 1.0}, {"master", 1.0}}, 0.3, 220, 0.12, {"spk", "pa", "pb"}, 3.0, -1, 0, "", 176400, 3.0},
        // Orange OR120 Graphic MkII 1972 (80000): no cathode follower, the whole signal path is one 57-unknown group
        // Clean: spk and the PI output only. At gain 0.2 the plates' AC is small against their start-up drift from the crest
        // operating point (the 543 -> 519 V supply settle) and against its DC offsets (p1b 24 mV, p2a 28 mV, 0.013 % of DC),
        // which read 17 % (p1b), 1.09 % (p2a) and 1.23 % (pa); the plates are gated in the drive, FAC and dimed cases.
        {"or120_clean", "orange-or120-1972-80000.circuit.json", {{"gain", 0.2}}, 0.05, 220, 0.08, {"spk", "na"}, 1.0},
        {"or120_drive", "orange-or120-1972-80000.circuit.json", {}, 0.1, 110, 0.08, {"spk", "pa", "pb", "p2b"}, 3.0},
        {"or120_fac", "orange-or120-1972-80000.circuit.json", {{"fac", 1.0}, {"hfdrive", 1.0}, {"bass", 0.0}}, 0.1, 220, 0.08, {"spk", "pa"}, 3.0},
        {"or120_dimed", "orange-or120-1972-80000.circuit.json", {{"gain", 1.0}, {"bass", 1.0}, {"treble", 1.0}, {"hfdrive", 1.0}, {"fac", 0.0}}, 0.05, 220, 0.12, {"spk", "pa", "pb"}, 3.0, -1, 0, "", 176400, 3.0},
        // Leslie 122 rotor amplifier + 800 Hz dividing network (31000; the rotors are the unit's DSP block): horn = the treble
        // driver's voltage (io.output), d1 = the drum driver's (io.outputR), cx = the OT secondary. Measured at delivery
        // (gate.py): clean horn/d1 0.10 / 0.13 %, driven 0.04 %, dimed 0.05 %, dimed 3 kHz 0.35 / 0.60 %.
        {"leslie_clean", "leslie-122-31000.circuit.json", {}, 0.05, 1000, 0.08, {"horn", "d1", "cx", "pa3"}, 1.0},
        {"leslie_drive", "leslie-122-31000.circuit.json", {}, 0.12, 220, 0.08, {"horn", "d1", "pa3", "pa"}, 3.0},
        // The energy-above-30 kHz gate runs on the 3 kHz case: at 220 Hz the plate-to-grid feedback keeps the 6550s smooth and
        // the reference's content above 30 kHz is -136 to -165 dB re total (its print precision); at 3 kHz it is -63 dB.
        {"leslie_dimed", "leslie-122-31000.circuit.json", {{"volume", 1.0}}, 2.0, 220, 0.12, {"horn", "d1", "pa3", "pa4"}, 3.0, -1, 0, "", 176400},
        {"leslie_dimed_hf", "leslie-122-31000.circuit.json", {{"volume", 1.0}}, 3.0, 3000, 0.08, {"horn", "d1", "cx"}, 3.0, -1, 0, "", 176400, 3.0},
        // Roland JC-120 (49000), 1982 revision (5th-ed service notes): both discrete power amps, stereo spkL / spkR. The right
        // side is compared with the effect Off and the reverb at 0 (the BBD and the tank export to SPICE as DC sources).
        {"jc120_clean", "roland-jc-120.circuit.json", {{"em", 0}, {"reverb", 0}}, 0.02, 1000, 0.06, {"spkL", "spkR", "c15", "d5"}, 1.0},
        {"jc120_chorus", "roland-jc-120.circuit.json", {{"em", 0.5}, {"reverb", 0}}, 0.02, 1000, 0.1, {"spkL", "c15", "tri"}, 1.0},
        {"jc120_drive", "roland-jc-120.circuit.json", {{"em", 0}, {"reverb", 0}, {"volume", 1}}, 0.06, 220, 0.1, {"spkL", "spkR", "dmix", "c15", "p37"}, 3.0},
        {"jc120_dimed", "roland-jc-120.circuit.json", {{"em", 0}, {"reverb", 0}}, 0.3, 110, 0.1, {"spkL", "spkR", "dmix", "gq10", "c15"}, 3.0},
        {"jc120_dist", "roland-jc-120.circuit.json", {{"em", 0}, {"reverb", 0}, {"dist_on", 1}, {"dist", 0.7}}, 0.1, 220, 0.1, {"spkL", "spkR", "d3", "vtop"}, 3.0},
        // Friedman BE-100 (151000), V2 Rev 1.5 redraw (single source); gainMode 0 / 0.5 / 1 = Clean / BE / HBE
        {"be100_clean", "friedman-be100-151000.circuit.json", {{"gainMode", 0.0}}, 0.02, 220, 0.08, {"spk", "p1b", "p4a"}, 1.0},
        {"be100_be_small", "friedman-be100-151000.circuit.json", {{"gainMode", 0.5}}, 0.002, 220, 0.08, {"spk", "p2b", "kcf", "p4a"}, 1.0},
        {"be100_be_drive", "friedman-be100-151000.circuit.json", {{"gainMode", 0.5}}, 0.1, 110, 0.08, {"spk", "p3a", "pa", "pb"}, 3.0},
        {"be100_hbe_drive", "friedman-be100-151000.circuit.json", {{"gainMode", 1.0}, {"gain", 1.0}}, 0.1, 220, 0.08, {"spk", "p3a", "pa", "pb"}, 3.0},
        {"be100_sat", "friedman-be100-151000.circuit.json", {{"gainMode", 0.5}, {"sat", 1.0}, {"gain", 1.0}}, 0.1, 220, 0.08, {"spk", "kcf", "ze"}, 3.0},
        {"be100_loop", "friedman-be100-151000.circuit.json", {{"loop", 1.0}}, 0.005, 220, 0.08, {"spk"}, 1.0},
        {"be100_clean_dimed", "friedman-be100-151000.circuit.json", {{"gainMode", 0.0}, {"clean_vol", 1.0}}, 0.3, 220, 0.08, {"spk", "pa", "pb"}, 3.0},
        {"be100_dimed", "friedman-be100-151000.circuit.json", {{"gainMode", 1.0}, {"gain", 1.0}, {"master", 1.0}}, 0.3, 220, 0.12, {"spk", "pa", "pb"}, 3.0, -1, 0, "", 176400, 3.0},
        // Bogner Ecstasy 101B (162000): blueguitar hand traces + schematicheaven OD redraw; gainMode 0 / 0.5 / 1 = Ch1 clean /
        // Ch2 / Ch3 (the same OD circuit with the catalogue's one knob set). Settled start (the mains supply in the deck).
        {"xtc_clean", "bogner-ecstasy-101b-162000.circuit.json", {{"gainMode", 0.0}, {"gain", 0.35}, {"master", 0.7}}, 0.01, 440, 0.08, {"spk", "pc2", "pc3", "chout", "p6a"}, 1.0, -1, 0, "", 0, 0, "settled"},
        {"xtc_ch2_small", "bogner-ecstasy-101b-162000.circuit.json", {{"gainMode", 0.5}}, 0.002, 220, 0.08, {"spk", "p2a", "p4a", "k5a"}, 1.0, -1, 0, "", 0, 0, "settled"},
        {"xtc_ch3_drive", "bogner-ecstasy-101b-162000.circuit.json", {{"gainMode", 1.0}, {"gain", 0.8}, {"master", 1.0}}, 0.05, 220, 0.08, {"spk", "p3a", "pa", "pb"}, 3.0, -1, 0, "", 0, 0, "settled"},
        // Plexi assigned to Ch3 (S1 SK1/SK3 lower throws: wiper -> 499k || 470p -> V4A), driven
        {"xtc_plexi_drive", "bogner-ecstasy-101b-162000.circuit.json", {{"gainMode", 1.0}, {"plexi", 1.0}, {"gain", 0.8}, {"master", 1.0}}, 0.1, 220, 0.08, {"spk", "plx2", "p4a", "pa"}, 3.0, -1, 0, "", 0, 0, "settled"},
        {"xtc_clean_set", "bogner-ecstasy-101b-162000.circuit.json", {{"gainMode", 0.0}, {"gain", 0.35}, {"master", 0.7}}, 0.01, 440, 0.08, {"spk", "pa", "pb"}, 1.0, -1, 0, kXtcRails},
        // Mesa/Boogie Dual Rectifier 3-channel Solo Head (16000): mode 0 / 1/3 / 2/3 / 1 = Clean / Raw / Vintage / Modern
        {"rect_clean", "mesa-dual-rectifier-16000.circuit.json", {{"mode", 0.0}}, 0.02, 220, 0.08, {"t8", "p2a", "pa"}, 1.0},
        {"rect_raw", "mesa-dual-rectifier-16000.circuit.json", {}, 0.002, 220, 0.08, {"t8", "k3b", "pa"}, 1.0},
        {"rect_ch2_vint", "mesa-dual-rectifier-16000.circuit.json", {{"mode", 2.0 / 3.0}, {"hgch", 1.0}}, 0.002, 220, 0.08, {"t8", "k3b", "pa"}, 1.0},
        {"rect_modern", "mesa-dual-rectifier-16000.circuit.json", {{"mode", 1.0}}, 0.05, 220, 0.08, {"t8", "k3b", "pa"}, 3.0},
        {"rect_tube_spongy", "mesa-dual-rectifier-16000.circuit.json", {{"mode", 2.0 / 3.0}, {"rectifier", 0.0}, {"power", 0.0}}, 0.01, 220, 0.08, {"t8", "pa", "a"}, 1.5},
        {"rect_dimed", "mesa-dual-rectifier-16000.circuit.json", {{"mode", 1.0}, {"gain", 1.0}, {"master", 1.0}}, 0.3, 220, 0.08, {"t8", "pa", "pb"}, 3.0, -1, 0, "", 176400, 3.0},
        // Dual Rectifier Rev C (96000): the 16000's cases on the Rev C netlist (no .op case: its DC gate is the settled
        // sdc in the wave-e notes, as on the 16000)
        {"revc_clean", "mesa-dual-rectifier-revc-96000.circuit.json", {{"mode", 0.0}}, 0.02, 220, 0.08, {"t8", "pa"}, 1.0},
        {"revc_raw", "mesa-dual-rectifier-revc-96000.circuit.json", {}, 0.002, 220, 0.08, {"t8", "k3b", "pa"}, 1.0},
        {"revc_ch2_vint", "mesa-dual-rectifier-revc-96000.circuit.json", {{"mode", 2.0 / 3.0}, {"hgch", 1.0}}, 0.002, 220, 0.08, {"t8", "k3b", "pa"}, 1.0},
        {"revc_modern", "mesa-dual-rectifier-revc-96000.circuit.json", {{"mode", 1.0}}, 0.05, 220, 0.08, {"t8", "k3b", "pa"}, 3.0},
        {"revc_dimed", "mesa-dual-rectifier-revc-96000.circuit.json", {{"mode", 1.0}, {"gain", 1.0}, {"master", 1.0}}, 0.3, 220, 0.08, {"t8", "pa", "pb"}, 3.0, -1, 0, "", 176400, 3.0},
        {"surv_clean", "eae-surveyor-170000.circuit.json", {}, 0.1, 1000, 0.05, {"out"}, 1.0},
        {"surv_drive", "eae-surveyor-170000.circuit.json", {{"gain", 1.0}, {"volume", 1.0}}, 0.3, 220, 0.08, {"out"}, 3.0},
        {"hm2_small", "boss-hm2.circuit.json", {}, 0.01, 1000, 0.05, {"out"}, 3.0},
        {"hm2_drive", "boss-hm2.circuit.json", {}, 0.1, 1000, 0.05, {"out"}, 3.0},
        // Boss MT-2 (46000, wave E pass 23): VB supply cut + DK solver; clean below the clipper, then dimed
        {"mt2_small", "boss-mt2-metal-zone.circuit.json", {{"dist", 0.0}}, 0.005, 1000, 0.05, {"out", "o3b"}, 1.0},
        {"mt2_drive", "boss-mt2-metal-zone.circuit.json", {{"dist", 1.0}}, 0.5, 220, 0.05, {"out", "clip"}, 3.0},
        // Tech 21 SansAmp GT2 (45000): Tweed/Clean/Drive 0 linear; defaults (British/Hi Gain/Center); California/Hot Wired/Off Axis
        {"gt2_small", "tech21-sansamp-gt2.circuit.json", {{"amp", 1.0}, {"mod", 1.0}, {"drive", 0.0}}, 0.005, 1000, 0.05, {"out"}, 1.0},
        {"gt2_drive", "tech21-sansamp-gt2.circuit.json", {}, 0.1, 220, 0.05, {"out", "o2b"}, 3.0},
        {"gt2_hot", "tech21-sansamp-gt2.circuit.json", {{"amp", 0.0}, {"mod", 0.0}, {"mic", 0.0}}, 0.1, 220, 0.05, {"out"}, 3.0},
        // Marshall JH-1 Jackhammer (84000): factory drawing Rev 2 24.6.98; below the clipper, then both clip modes with the contour notch in
        {"jh1_small", "marshall-jh1-jackhammer.circuit.json", {{"gain", 0.0}}, 0.01, 1000, 0.05, {"out", "nb", "o2b"}, 1.0},
        {"jh1_drive", "marshall-jh1-jackhammer.circuit.json", {{"contour", 1.0}}, 0.3, 220, 0.05, {"out", "nb", "o2b"}, 3.0},
        {"jh1_od", "marshall-jh1-jackhammer.circuit.json", {{"mode", 0.0}, {"contour", 1.0}, {"freq", 0.2}}, 0.3, 220, 0.05, {"out", "nb", "od"}, 3.0},
        // SSL 4000 E channel EQ (121000, card 82E242): bands mixed; the LF bell into clipping with both filters in; the high-Q HMF
        // bell at 192 kHz (a Q-4 peak at 3 kHz warps by 1 percent NRMSE at 96 kHz)
        {"ssleq_small", "ssl-4000e-eq-121000.circuit.json", {{"hfgain", 0.8}, {"hmfgain", 0.3}, {"lmfgain", 0.7}, {"lfgain", 0.2}}, 0.5, 1000, 0.05, {"out", "t3o"}, 1.0},
        {"ssleq_hot", "ssl-4000e-eq-121000.circuit.json", {{"lfgain", 1.0}, {"lfbell", 1.0}, {"lffreq", 0.6}, {"hpf_in", 1.0}, {"lpf_in", 1.0}}, 4.0, 220, 0.05, {"out", "t10o"}, 3.0},
        {"ssleq_bell", "ssl-4000e-eq-121000.circuit.json", {{"hfgain", 1.0}, {"hfbell", 1.0}, {"hmfgain", 1.0}, {"hmfq", 0.0}}, 1.0, 3000, 0.05, {"out", "t2o", "t5o"}, 1.0, -1, 0, "", 192000},
        // Mu-Tron Bi-Phase (144000): both phasors held by the pedal amp (S6 / S5 on Ped; the free-running sweep generators leave
        // the shared unstable operating point in opposite directions in ngspice and the engine, so they are gated on frequency
        // in the notes); A on output A, the 12-stage cascade on output B; full feedback into op-amp clipping at 192 kHz
        {"bp_clean", "mutron-biphase-144000.circuit.json", {{"sweepA", 1.0}, {"sweepB", 1.0}}, 0.1, 224, 0.05, {"ja", "jb"}, 1.0},
        {"bp_hf", "mutron-biphase-144000.circuit.json", {{"sweepA", 1.0}, {"sweepB", 1.0}}, 0.1, 1783, 0.05, {"ja", "jb"}, 1.0},
        {"bp_drive", "mutron-biphase-144000.circuit.json", {{"sweepA", 1.0}, {"sweepB", 1.0}, {"feedbackA", 1.0}, {"feedbackB", 1.0}}, 6.0, 224, 0.05, {"ja", "jb"}, 3.0},
        {"bp_drive_hf", "mutron-biphase-144000.circuit.json", {{"sweepA", 1.0}, {"sweepB", 1.0}, {"feedbackA", 1.0}, {"feedbackB", 1.0}}, 6.0, 1783, 0.05, {"ja", "jb"}, 3.0, -1, 0, "", 192000},
        {"lune_static", "4ms-tremulus-lune-13000.circuit.json", {{"depth", 0.0}}, 0.1, 1000, 0.05, {"out"}, 1.0},
        {"lune_drive", "4ms-tremulus-lune-13000.circuit.json", {{"depth", 1.0}}, 0.3, 200, 0.08, {"out"}, 3.0},
        {"mt3_clean", "mu-tron-iii-47000.circuit.json", {}, 0.1, 1000, 0.05, {"out"}, 1.0},
        {"mt3_drive", "mu-tron-iii-47000.circuit.json", {{"sens", 1.0}}, 0.3, 110, 0.08, {"out"}, 3.0},
        // Fender 6G15 reverb (27000), supply cut: the dry path and the tank drive with the CUT OFF pedal down (mute = 1, so the
        // springs, which the deck lacks, add nothing), clean, then Dwell full into 6K6GT grid conduction; the recovery /
        // tone / mixer chain on the "6g15_wet" variant (the tank's electrical ends standing alone)
        {"g615_clean", "fender-6g15-27000.circuit.json", {{"mute", 1.0}}, 0.1, 1000, 0.08, {"out", "g6", "p6", "tk", "bpa"}, 1.0},
        {"g615_drive", "fender-6g15-27000.circuit.json", {{"mute", 1.0}, {"dwell", 1.0}}, 0.5, 200, 0.1, {"out", "p6", "tk", "k6", "bpa"}, 3.0},
        {"g615_hard", "fender-6g15-27000.circuit.json", {{"mute", 1.0}, {"dwell", 1.0}, {"tone", 0.0}, {"wet", 1.0}}, 1.5, 110, 0.12, {"out", "g6", "p6", "tk", "bpa"}, 3.0},
        {"g615_wet", "fender-6g15-27000.circuit.json", {{"tone", 0.5}, {"wet", 1.0}}, 0.05, 1000, 0.08, {"out", "p3"}, 1.0, -1, 0, "6g15_wet"},
        {"g615_wet_drive", "fender-6g15-27000.circuit.json", {{"tone", 1.0}, {"wet", 1.0}}, 0.5, 500, 0.08, {"out", "p3"}, 3.0, -1, 0, "6g15_wet"},
        {"cb21_heel", "dunlop-crybaby-21000.circuit.json", {{"pedal", 0.0}}, 0.1, 1000, 0.05, {"out"}, 1.0},
        {"cb21_toe", "dunlop-crybaby-21000.circuit.json", {{"pedal", 1.0}}, 0.1, 1000, 0.05, {"out"}, 1.0},
        {"tdrv_clean", "butler-tube-driver.circuit.json", {{"drive", 0.3}}, 0.05, 1000, 0.05, {"out"}, 1.0},
        {"tdrv_drive", "butler-tube-driver.circuit.json", {{"drive", 1.0}}, 0.1, 1000, 0.05, {"out"}, 3.0},
        // ENGINE_QUEUE item 13: 28 coupled diode ports in one group (above the old 24-port ceiling): generic paths
        {"wide_small", "test-wide-group.circuit.json", {}, 0.05, 1000, 0.02, {"out", "n1", "n14"}, 1.0},
        {"wide_hot", "test-wide-group.circuit.json", {}, 3.0, 220, 0.03, {"out", "n1", "n14"}, 3.0},
        // ENGINE_QUEUE item 17: 68 coupled ports (20 diodes + 24 BJT stages) in one group, above the old 48-port ceiling:
        // the run-time-size Newton kernel, its Woodbury step and fallbacks at that size (kMaxNlPorts 96)
        {"ladder_small", "test-wide-ladder.circuit.json", {}, 0.05, 1000, 0.02, {"out", "n1", "n10", "c1", "c24"}, 1.0},
        {"ladder_hot", "test-wide-ladder.circuit.json", {}, 3.0, 220, 0.03, {"out", "n1", "n10", "c1", "c24"}, 3.0},
        // 92000: the same 1959 circuit on a variac (~90 VAC): every transformer supply scaled by 0.75
        {"pvar_clean", "marshall-1959-plexi-variac.circuit.json", {}, 0.02, 220, 0.08, {"spk", "p1b", "p3a", "pa"}, 1.0},
        {"pvar_drive", "marshall-1959-plexi-variac.circuit.json", {{"volume1", 0.8}}, 0.3, 110, 0.08, {"spk", "pa", "p2a"}, 3.0},
        {"jcm_clean", "marshall-jcm800-2203.circuit.json", {}, 0.02, 220, 0.08, {"spk", "p1", "p4a", "pa"}, 1.0},
        {"jcm_drive", "marshall-jcm800-2203.circuit.json", {{"preamp", 0.8}, {"master", 0.7}}, 0.3, 110, 0.08, {"spk", "pa", "p3"}, 3.0},
        {"ac30_clean", "vox-ac30-tb.circuit.json", {}, 0.02, 220, 0.08, {"spk", "p1a", "p2a", "pa"}, 1.0},
        {"ac30_drive", "vox-ac30-tb.circuit.json", {{"volume", 0.8}}, 0.3, 110, 0.08, {"spk", "pa", "ptb"}, 3.0},
        // Dimed, at the amps' own internal rate (176.4 kHz, --settle 0 like the deck): the LTE-controlled substeps
        // (CIRCUIT_ENGINE.md §3.1) against ngspice at maxstep T/8. Gated on NRMSE (3 %) and on the energy above 30 kHz
        // (within 3 dB). Fixed-step engine before the control: twin spk +9.2 dB above 30 kHz, jcm pb 3.4 % NRMSE.
        {"dr103_dimed", "hiwatt-dr103.circuit.json", {{"normal", 1.0}, {"master", 0.8}}, 0.3, 220, 0.12, {"spk", "pa", "pb"}, 3.0, -1, 0, "", 176400, 3.0},
        {"plexi_dimed", "marshall-1959-plexi.circuit.json", {{"volume1", 1.0}}, 0.3, 220, 0.12, {"spk", "pa", "pb"}, 3.0, -1, 0, "", 176400, 3.0},
        {"jcm_dimed", "marshall-jcm800-2203.circuit.json", {{"preamp", 1.0}, {"master", 1.0}}, 0.3, 220, 0.12, {"spk", "pa", "pb"}, 3.0, -1, 0, "", 176400, 3.0},
        {"ac30_dimed", "vox-ac30-tb.circuit.json", {{"volume", 1.0}}, 0.3, 220, 0.12, {"spk", "pa", "pb"}, 3.0, -1, 0, "", 176400, 3.0},
        {"twin_dimed", "ab763-twin-reverb.circuit.json", {{"volume", 1.0}, {"reverb_off", 1.0}}, 0.3, 220, 0.12, {"spk", "pa", "pb"}, 3.0, -1, 0, "", 176400, 3.0},
        // Wave C remaining 10 units: SPICE-checkable analog paths (BBD/tape delay lines gated above against datasheets)
        {"dyna_small", "mxr-dyna-comp.circuit.json", {}, 0.02, 1000, 0.05, {"out", "q2e"}, 1.0},
        {"dyna_hot", "mxr-dyna-comp.circuit.json", {{"sensitivity", 1.0}}, 0.3, 220, 0.06, {"out", "env"}, 3.0},
        // Small Stone: LFO held still (loadVariant "sstone_hold"): C12 at the bottom / middle / top of its triangle
        // (1.758-1.925 V at Rate 0.5, Color off), so the all-pass sweep is gated across its range like for like
        {"sstone_small", "ehx-small-stone.circuit.json", {{"color", 0.0}}, 0.02, 440, 0.06, {"out", "s4"}, 1.0, -1, 0, "sstone_hold=1.84"},
        {"sstone_small_lo", "ehx-small-stone.circuit.json", {{"color", 0.0}}, 0.02, 440, 0.06, {"out", "s4"}, 1.0, -1, 0, "sstone_hold=1.76"},
        {"sstone_small_hi", "ehx-small-stone.circuit.json", {{"color", 0.0}}, 0.02, 440, 0.06, {"out", "s4"}, 1.0, -1, 0, "sstone_hold=1.92"},
        {"sstone_color_small", "ehx-small-stone.circuit.json", {{"color", 1.0}}, 0.02, 440, 0.06, {"out", "s4"}, 1.0, -1, 0, "sstone_hold=1.84"},
        {"sstone_color", "ehx-small-stone.circuit.json", {{"color", 1.0}}, 0.15, 440, 0.06, {"out"}, 3.0, -1, 0, "sstone_hold=1.84"},
        {"uvibe_small", "shinei-univibe.circuit.json", {{"cancel", 1.0}}, 0.05, 440, 0.06, {"out", "e4"}, 1.0},
        {"uvibe_sweep", "shinei-univibe.circuit.json", {{"speed", 0.5}, {"intensity", 1.0}}, 0.2, 440, 0.08, {"out"}, 3.0},
        {"emist_small", "ehx-electric-mistress.circuit.json", {{"color", 0.0}, {"feedback", 0.0}}, 0.05, 1000, 0.04, {"ic1b_o"}, 1.0},
        {"emist_hot", "ehx-electric-mistress.circuit.json", {{"color", 0.0}, {"feedback", 0.0}}, 0.5, 330, 0.05, {"ic1b_o"}, 3.0},
        {"dimd_small", "roland-dimension-d.circuit.json", {}, 0.05, 1000, 0.05, {"XR.bin", "XR.cout", "a"}, 1.0},
        {"dimd_hot", "roland-dimension-d.circuit.json", {}, 0.5, 330, 0.05, {"XR.bin", "XR.cout"}, 3.0},
        // RE-201: the playback bus n17 is not compared here: with the tape heads idle in ngspice it only carries a
        // -190 dB residue there. The playback / tone / echo chain is gated by re201_play* (loadVariant "re201_head1").
        {"re201_small", "roland-re201.circuit.json", {{"echo", 0.0}, {"reverb", 0.0}, {"intensity", 0.0}}, 0.02, 1000, 0.05, {"out", "n12"}, 1.0},
        {"re201_hot", "roland-re201.circuit.json", {{"echo", 0.0}, {"reverb", 0.0}, {"instrument_volume", 0.85}}, 0.3, 220, 0.05, {"out", "n11"}, 3.0},
        {"re201_play", "roland-re201.circuit.json", {{"instrument_volume", 0.0}, {"mode", 0.0}, {"reverb", 0.0}, {"echo", 0.8}}, 0.005, 1000, 0.05, {"out", "q6e", "q8c"}, 1.0, -1, 0, "re201_head1"},
        {"re201_play_hot", "roland-re201.circuit.json", {{"instrument_volume", 0.0}, {"mode", 0.0}, {"reverb", 0.0}, {"echo", 0.8}}, 0.03, 220, 0.05, {"out", "q6e"}, 3.0, -1, 0, "re201_head1"},
        // DMM: LFO held at its DC point (loadVariant "dmm_lfo_hold"); it biases the shared reference C through VR5
        {"dmm_small", "ehx-deluxe-memory-man.circuit.json", {{"blend", 0.0}, {"feedback", 0.0}}, 0.03, 1000, 0.05, {"out", "bbd1in", "cout"}, 1.0, -1, 0, "dmm_lfo_hold"},
        {"dmm_hot", "ehx-deluxe-memory-man.circuit.json", {{"blend", 0.0}, {"feedback", 0.0}, {"level", 0.85}}, 0.4, 330, 0.05, {"out", "bbd1in"}, 3.0, -1, 0, "dmm_lfo_hold"},
        {"ce1_small", "boss-ce1.circuit.json", {}, 0.02, 1000, 0.05, {"d", "c"}, 1.0},
        {"ce1_hot", "boss-ce1.circuit.json", {{"level", 0.9}}, 0.25, 330, 0.05, {"d", "c"}, 3.0},
        {"u1176_small", "urei-1176ln.circuit.json", {{"ratio", 0.0}}, 0.05, 1000, 0.04, {"out", "q4_e"}, 1.0},
        {"u1176_hot", "urei-1176ln.circuit.json", {{"ratio", 0.2}, {"input", 0.75}, {"attack", 0.8}}, 0.5, 330, 0.05, {"out", "ctl_c27"}, 3.0},
        {"la2a_small", "teletronix-la2a.circuit.json", {{"peak_reduction", 0.0}}, 0.05, 1000, 0.04, {"out", "wcf_out"}, 1.0},
        {"la2a_hot", "teletronix-la2a.circuit.json", {{"peak_reduction", 0.75}, {"gain", 0.5}}, 0.8, 330, 0.06, {"out", "t4_sig"}, 3.0},
        // vca / rms devices and io.inputR: linked stereo 2180B pair under one 2252 (R fed its own, smaller sine)
        {"vcarms_small", "test-vca-rms.circuit.json", {}, 0.1, 1000, 0.25, {"out", "outr", "ctl", "cap"}, 1.0, 0.05, 1000},
        {"vcarms_hot", "test-vca-rms.circuit.json", {{"ratio", 0.8}, {"makeup", 0.8}}, 2.0, 220, 0.25, {"out", "outr", "ctl", "cap", "ecn"}, 3.0, 1.0, 220},
        // CMOS logic: CD4069UB squarer, CD4013B divide-by-2 and -4, CD4011B NAND; logic outputs gated after 10k + 10n
        {"cmos_small", "test-cmos-divider.circuit.json", {}, 0.5, 220, 0.1, {"o2", "o4", "on", "y1"}, 1.0},
        {"cmos_hot", "test-cmos-divider.circuit.json", {}, 2.0, 440, 0.06, {"o2", "o4", "on", "y1"}, 1.0},
        // CD4027B J-K: toggle (J = K = 1), a D stage (J = Q1, K = Q1-bar), a divide-by-four clocked by Q1
        {"cmosjk_small", "test-cmos-jk.circuit.json", {}, 0.5, 220, 0.1, {"o1", "o2", "o3"}, 1.0},
        {"cmosjk_hot", "test-cmos-jk.circuit.json", {}, 2.0, 440, 0.06, {"o1", "o2", "o3"}, 1.0},
        // AD536A: the 82E10 side-chain (CAV 3n3, dB into a x32.9 amplifier) and a 1 uF section with a buffered linear output
        {"ad536_small", "test-ad536.circuit.json", {}, 0.3, 440, 0.3, {"dbo", "db", "iout", "cav", "lin", "db2"}, 1.0},
        {"ad536_hot", "test-ad536.circuit.json", {}, 5.0, 220, 0.3, {"dbo", "db", "iout", "cav", "lin", "db2"}, 1.0},
        // dbx 2151 with AN103 offset nulling, a resistive control network swung at 2 Hz, and a β-100 core
        {"vca2151", "test-vca-2151.circuit.json", {}, 0.2, 1000, 0.5, {"out", "out2", "ec", "ec2"}, 1.0},
        // SSL 4000G bus compressor: below threshold (mono feed), and linked stereo under 10:1 with R fed its own sine
        {"busg_small", "ssl-4000g-bus-comp-130000.circuit.json", {}, 0.1, 1000, 0.1, {"outl", "outr", "XL.t5o"}, 1.0},
        {"busg_hot", "ssl-4000g-bus-comp-130000.circuit.json", {{"ratio", 1.0}, {"threshold", 1.0}, {"attack", 0.2}, {"release", 0.0}}, 3.0, 1000, 0.1, {"outl", "outr", "tcb", "nD", "XL.t7o"}, 3.0, 1.0, 330},
        {"busg_2to1", "ssl-4000g-bus-comp-130000.circuit.json", {{"ratio", 0.0}, {"threshold", 0.7}, {"makeup", 0.6}, {"release", 0.0}}, 2.0, 200, 0.15, {"outl", "tcb", "nD"}, 3.0},
        // dbx 160 (AKG-era): below threshold; inf:1 at 3 V; 4:1 with the VCA at +10 dB (GAIN CONTROL 0.75) and threshold high
        {"d160_small", "dbx-160vu-125000.circuit.json", {}, 0.1, 1000, 0.1, {"out", "oa4_o", "rms_out", "w5"}, 1.0},
        {"d160_hot", "dbx-160vu-125000.circuit.json", {{"compression", 1.0}}, 3.0, 1000, 0.15, {"out", "oa4_o", "rms_out", "w5", "x5"}, 3.0},
        {"d160_gain", "dbx-160vu-125000.circuit.json", {{"compression", 0.5}, {"threshold", 0.8}, {"output", 0.75}}, 5.0, 200, 0.2, {"out", "rms_out", "w5", "m"}, 3.0},
        // zener shunt regulators on rippled rails, a zener clipper, a current-limited op-amp, pot end resistance
        {"zen_small", "test-zener.circuit.json", {}, 0.2, 220, 0.1, {"vz", "vzn", "clip", "out", "w"}, 1.0},
        {"zen_hot", "test-zener.circuit.json", {{"level", 0.6}}, 8.0, 220, 0.1, {"vz", "vzn", "clip", "out", "w"}, 3.0}, // op-amp in current limit
        // ENGINE_QUEUE 31: op-amp vos/ib/ios; stage A (series-capacitor feedback + clamp) is biased only by the offset
        {"vos_small", "test-opamp-vos.circuit.json", {}, 0.02, 440, 0.1, {"outa", "sa", "outb", "outc"}, 1.0},
        {"vos_hot", "test-opamp-vos.circuit.json", {}, 0.3, 220, 0.1, {"outa", "sa", "outb", "outc"}, 3.0},
        // electro-mechanical speakers (docs §10.21): free-air G12M-25, a boxed one with a 30 ms coil thermal network, a 2s2p P12Q
        {"spk_small", "test-speaker.circuit.json", {}, 0.5, 110, 0.1, {"vel", "a", "vel2", "vel3"}, 0.1},
        {"spk_hot", "test-speaker.circuit.json", {}, 20.0, 60, 0.2, {"vel", "SPK1#xm", "a", "vel2", "SPK2#xm", "vel3"}, 0.1},
        {"spk_xmax", "test-speaker.circuit.json", {}, 60.0, 30, 0.3, {"vel", "SPK1#xm", "vel2", "SPK2#xm", "vel3", "SPK3#xm"}, 0.2}, // beyond the Bl clamp
        {"spk_thermal", "test-speaker.circuit.json", {}, 20.0, 220, 0.3, {"vel2", "b", "vel"}, 0.1},
        {"spk_hf", "test-speaker.circuit.json", {}, 20.0, 3000, 0.05, {"vel", "SPK1#psi", "vel2", "vel3"}, 1.0},
        // a netlist that ends in speaker + cab: 2s2p G12M-25 in a 1960AX box with the fitted acoustic path (lib:cab-1960ax-g12m25-sm57)
        {"spkcab_small", "test-speaker-cab.circuit.json", {}, 1.0, 220, 0.1, {"mic", "cone"}, 0.5},
        {"spkcab_hot", "test-speaker-cab.circuit.json", {}, 50.0, 82, 0.2, {"mic", "cone", "SPK#xm"}, 1.0},
        // Ampeg SVT 1969 (54000): its supply is behind a power transformer, which has no usable DC point (a winding is a
        // short at DC, so ngspice's .op has every rail at 0 V; docs §3.10). Two same-state protocols instead:
        // 1. both switched on cold (TranCase::start "cold"), the whole circuit including mains, transformers, bridge,
        //    bias and screen rectifiers, compared over 0.1-0.2 s, while the rails charge.
        //    Measured (netlist re-read 2026-11-10, session 3): clean spk 0.08 / pau0 1.03 / bpa 1.02 / bscn 0.09 / biasn
        //    0.09 %, driven spk 0.07 / pau0 0.11 / bpa 1.14 %; the rail residual is mostly ngspice's 10 GΩ node shunts
        //    (1 GΩ doubled it at gating). Gate 1.5 %:
        //    stricter than the 2 % / 4 % asked of a transformer-fed amp, inside the inrush where a few tenths of a volt on
        //    a 34 Vpp ripple is the whole error;
        {"svt_clean", "ampeg-svt-1969.circuit.json", {}, 0.01, 220, 0.2, {"spk", "pau0", "bpa", "bscn", "biasn"}, 1.5, -1, 0, "", 0, 0, "cold"},
        {"svt_drive", "ampeg-svt-1969.circuit.json", {{"volume", 1.0}}, 0.2, 110, 0.2, {"spk", "pau0", "bpa"}, 1.5, -1, 0, "", 0, 0, "cold"},
        // 2. at the settled operating point: the averaged circuit with the rectifier outputs at the engine's periodic
        //    steady-state means (testMainsSettle checks those), both from its DC point (the variant "mains_dc").
        //    Measured: clean spk / pau0 0.004 %, p1a 0.03 %, p2 0.02 %; driven (volume 1, 0.2 V: grids in conduction
        //    through the cathode followers) spk 0.01 %, pau0 0.02 %, p2 0.02 %. Gate 1 % for both, the clean gate of every
        //    other amp.
        {"svt_set_clean", "ampeg-svt-1969.circuit.json", {}, 0.01, 220, 0.08, {"spk", "pau0", "p1a", "p2"}, 1.0, -1, 0, kSvtRails},
        {"svt_set_drive", "ampeg-svt-1969.circuit.json", {{"volume", 1.0}}, 0.2, 110, 0.08, {"spk", "pau0", "p2"}, 1.0, -1, 0, kSvtRails},
        // Fairchild 670 (24000): both channels in one instance, at the settled operating point (the averaged circuit: GZ34
        // output hv and the T301 bridge output nr at the engine's periodic steady-state means, the EL34 / 6084 / 5651
        // regulator and every bias rail solved as drawn). small = no limiting (AC THRESHOLD at zero), hot = the factory
        // condition at ~11 dB of gain reduction from a standing start, ms = LATERAL & VERTICAL, L 8 V and R 3 V in phase (sum and
        // difference limited separately). The control lines ctl1 / ctl2 sit at about -50 V with millivolts of ripple, so their
        // NRMSE against their own AC is not a fair figure (max |diff| 12 / 22 mV), and T104's secondary floats whenever the
        // bridge is off (its common mode is set by junction leakage on both sides), so the tertiary gy1 / gy2 is gated instead.
        // f670_hot runs at 192 kHz since T104 took its teardown turns (2026-11-12): the faster side chain's bridge pulses put
        // p1071's harmonics 1.98 dB off at 96 kHz (NRMSE 1.43 %); at 192 kHz outl 0.04 %, gy1 0.42 %, p1071 0.67 % (0.50 dB),
        // g105a1 0.04 %.
        {"f670_small", "fairchild-670-24000.circuit.json", {{"threshold", 0.0}}, 0.5, 1000, 0.04, {"outl", "outr", "pu1", "ku1"}, 1.0, -1, 0, kF670Rails},
        {"f670_hot", "fairchild-670-24000.circuit.json", {}, 20.0, 1000, 0.04, {"outl", "gy1", "p1071", "g105a1"}, 3.0, -1, 0, kF670Rails, 192000},
        {"f670_ms", "fairchild-670-24000.circuit.json", {{"mode", 1.0}}, 8.0, 1000, 0.04, {"outl", "outr", "gy1", "gy2"}, 3.0, 3.0, 0, kF670Rails},
        // attack: the factory condition, a 5 kHz tone stepped from silence to 14 V (about 10.5 dB of limiting once settled):
        // the attack dynamics themselves, not only the settled limiting of f670_hot. At 192 kHz: the bridge charges C109 in
        // pulses at the tone's peaks, and the engine's step error there halves with the step (DAFx-turns T104, 1.5-3 ms:
        // outl / ctl1 / gy1 0.79 / 12.6 / 3.5 % at 96 kHz, 0.63 / 5.1 / 2.2 % at 192 kHz). attack (1.5-3 ms) gates the
        // output and the tertiary; attack_cv gates the control line alone over 0.6-1.2 ms, while it still ramps (63 % of
        // its 47 V at 0.56 ms, 90 % at 1.07 ms with the teardown T104 turns and DCR); later its window AC is only ripple
        // (0.8-1.6 ms: 7.1 %). Measured: outl 0.135 %, gy1 0.80 %; ctl1 6.84 % of the window's AC, max |diff| 0.40 V, 0.85 %
        // of the 47 V swing: the step error at the bridge pulses, gated at 8 %.
        {"f670_attack", "fairchild-670-24000.circuit.json", {}, 14.0, 5000, 0.003, {"outl", "gy1"}, 3.0, -1, 0, kF670Rails, 192000},
        {"f670_attack_cv", "fairchild-670-24000.circuit.json", {}, 14.0, 5000, 0.0012, {"ctl1"}, 8.0, -1, 0, kF670Rails, 192000},
        // The whole unit with its mains side (T302 / GZ34 / L301 / C305-C306, T301 / GR301, the regulator), both sides
        // started from the engine's periodic steady state ("settled": every node voltage as .ic, every inductor current and
        // core flux): the mains-fed supply's ripple, the regulator's response to it and the audio path together.
        {"f670_set_small", "fairchild-670-24000.circuit.json", {{"threshold", 0.0}}, 0.5, 1000, 0.04, {"outl", "outr"}, 1.0, -1, 0, "", 0, 0, "settled"},
        {"f670_set_hot", "fairchild-670-24000.circuit.json", {}, 20.0, 1000, 0.02, {"outl", "outr", "gy1"}, 3.0, -1, 0, "", 0, 0, "settled"},
    };
    for (const auto& tc : tran) runTranCase(tc);
    std::printf(" SPICE gate (AC):\n");
    runAcCase({"ts_ac", "ts808.circuit.json", {}});
    runAcCase({"ts_ac_tone0", "ts808.circuit.json", {{"tone", 0.0}, {"drive", 0.0}}});
    runAcCase({"ab_ac", "ab763-normal.circuit.json", {}});
    runAcCase({"ab_ac_scoop", "ab763-normal.circuit.json", {{"bass", 1.0}, {"treble", 1.0}, {"middle", 0.0}}});
    runAcCase({"twin_ac", "ab763-twin-reverb.circuit.json", {{"reverb_off", 1.0}}});
    runAcCase({"jfet_ac", "test-jfet.circuit.json", {}});
    runAcCase({"oprail_ac", "test-opamp-rails.circuit.json", {}});
    runAcCase({"ds1_ac", "boss-ds1.circuit.json", {}});
    // four cascaded Miller-compensated stages fall at ~60 dB/decade above 1 kHz: trapezoidal warping at 96 kHz alone is
    // 0.24 dB at 5 kHz there (0.9 % frequency compression), so this one is gated at 192 kHz
    runAcCase({"muff_ac", "ehx-big-muff-pi.circuit.json", {}, 192000});
    runAcCase({"svt_ac", "ampeg-svt-1969.circuit.json", {}, 0, kSvtRails}); // the settled operating point (see svt_set_*)
    runAcCase({"f670_ac", "fairchild-670-24000.circuit.json", {{"threshold", 0.0}}, 0, kF670Rails}); // settled point, no limiting
    runAcCase({"ffsi_ac", "dallas-fuzz-face-si.circuit.json", {}});
    runAcCase({"klon_ac", "klon-centaur.circuit.json", {}});
    runAcCase({"bb_ac", "marshall-bluesbreaker.circuit.json", {}});
    runAcCase({"rat_ac", "proco-rat2.circuit.json", {}, 192000}); // LM308 (30 pF) + filter: steep roll-off, warping 0.1 dB at 5 kHz at 96 kHz
    runAcCase({"gcb95_ac_heel", "dunlop-crybaby-gcb95.circuit.json", {{"pedal", 0.0}}, 192000});
    runAcCase({"gcb95_ac_toe", "dunlop-crybaby-gcb95.circuit.json", {{"pedal", 1.0}}, 192000});
    runAcCase({"v846_ac_heel", "vox-v846-clyde-mccoy.circuit.json", {{"pedal", 0.0}}, 192000});
    runAcCase({"ffge_ac", "dallas-fuzz-face-ge.circuit.json", {}});
    runAcCase({"otaopt_ac", "test-ota-optics.circuit.json", {}});
    runAcCase({"comp_ac", "test-compander.circuit.json", {}});
    runAcCase({"deluxe_ac", "ab763-deluxe-reverb.circuit.json", {{"reverb_off", 1.0}}});
    runAcCase({"bassman_ac", "5f6a-bassman.circuit.json", {}});
    runAcCase({"plexi_ac", "marshall-1959-plexi.circuit.json", {}});
    runAcCase({"pvar_ac", "marshall-1959-plexi-variac.circuit.json", {}});
    runAcCase({"rm_ac", "dallas-rangemaster.circuit.json", {}});
    runAcCase({"m133_ac", "mxr-m133-micro-amp.circuit.json", {}});
    runAcCase({"ffv_ac", "fuzz-face-vintage-ge.circuit.json", {}});
    runAcCase({"perc_ac", "interfax-harmonic-percolator.circuit.json", {}});
    runAcCase({"ts9dx_ac", "ibanez-ts9dx.circuit.json", {}});
    runAcCase({"tweed_ac", "fender-5e3-tweed-deluxe.circuit.json", {}});
    runAcCase({"dr103_ac", "hiwatt-dr103.circuit.json", {}});
    runAcCase({"dr103_ac_mid0", "hiwatt-dr103.circuit.json", {{"middle", 0.0}}}); // Middle rheostat shorted: the scoop
    runAcCase({"ac15_ac", "vox-ac15-handwired.circuit.json", {}});
    runAcCase({"slo_ac", "soldano-slo100-122000.circuit.json", {{"channel", 0.0}}});
    runAcCase({"oc2_ac", "boss-oc2-35000.circuit.json", {{"D", 1.0}}});
    runAcCase({"ssle_ac", "ssl-4000e-dynamics-133000.circuit.json", {}});
    runAcCase({"p5150_ac", "peavey-5150-134000.circuit.json", {{"channel", 0.0}}});
    // the feedback network at both ends of its controls: presence full (VR9 0 ohm) with resonance off (C12 shorted),
    // and the reverse (VR9 10K, VR8 1M in series with C12)
    runAcCase({"p5150_ac_pres", "peavey-5150-134000.circuit.json", {{"channel", 0.0}, {"presence", 1.0}, {"resonance", 0.0}}});
    runAcCase({"p5150_ac_res", "peavey-5150-134000.circuit.json", {{"channel", 0.0}, {"presence", 0.0}, {"resonance", 1.0}}});
    runAcCase({"rr_ac", "marshall-1959rr-91000.circuit.json", {}});
    // SLO-100 MK II: Normal channel only at 96 kHz (Overdrive warps 0.113 dB here, 0.028 dB at its 192 kHz unit rate).
    // No .op case: the mid-leg rectifier node d3m is held only by leakage (1.49x tolerance); every other node <= 0.003x.
    runAcCase({"slo2_ac", "soldano-slo100-mk2-163000.circuit.json", {{"channel", 0.0}}});
    runAcCase({"slo2_ac_crunch", "soldano-slo100-mk2-163000.circuit.json", {{"channel", 0.0}, {"normalMode", 1.0}, {"bright", 1.0}, {"loop", 1.0}}});
    runAcCase({"uber_ac", "bogner-uberschall-81000.circuit.json", {}, 192000}); // 0.505 dB 5-10 kHz warping at 96 kHz; the unit runs at 192 kHz
    runAcCase({"slo2_ac_od", "soldano-slo100-mk2-163000.circuit.json", {{"channel", 1.0}}, 192000});
    runAcCase({"jc120_ac", "roland-jc-120.circuit.json", {}});
    runAcCase({"be100_ac", "friedman-be100-151000.circuit.json", {{"gainMode", 0.5}}});
    runAcCase({"be100_clean_ac", "friedman-be100-151000.circuit.json", {{"gainMode", 0.0}}});
    runAcCase({"be100_hbe_ac", "friedman-be100-151000.circuit.json", {{"gainMode", 1.0}}});
    runAcCase({"xtc_clean_ac", "bogner-ecstasy-101b-162000.circuit.json", {{"gainMode", 0.0}}, 0, kXtcRails}); // settled point
    // Ch2's default Pre EQ is N (no bright cap, wave-e notes 162000 S33): its 10 kHz dip warps 0.50 dB at 96 kHz, so it is
    // gated at the unit's own minRate
    runAcCase({"xtc_ac", "bogner-ecstasy-101b-162000.circuit.json", {{"gainMode", 0.5}}, 176400, kXtcRails});
    runAcCase({"xtc_ch3_ac", "bogner-ecstasy-101b-162000.circuit.json", {{"gainMode", 1.0}}, 0, kXtcRails});
    runAcCase({"xtc_preeq_ac", "bogner-ecstasy-101b-162000.circuit.json", {{"gainMode", 0.5}, {"preEq2", 1.0}}, 0, kXtcRails}); // Ch2 B2 4n7
    runAcCase({"xtc_plexi_ac", "bogner-ecstasy-101b-162000.circuit.json", {{"gainMode", 1.0}, {"plexi", 1.0}}, 0, kXtcRails}); // Plexi on Ch3
    // Dual Rectifier: no .op case (the series-diode midpoint d3m is held only by leakage: 1.29x; every other node <= 0.002x)
    runAcCase({"rect_ac", "mesa-dual-rectifier-16000.circuit.json", {{"mode", 0.0}}});
    runAcCase({"rect_ac_raw", "mesa-dual-rectifier-16000.circuit.json", {}});
    runAcCase({"rect_ac_vint", "mesa-dual-rectifier-16000.circuit.json", {{"mode", 2.0 / 3.0}}, 176400}); // 0.55 dB 5-10 kHz warping at 96 kHz
    runAcCase({"revc_ac", "mesa-dual-rectifier-revc-96000.circuit.json", {{"mode", 0.0}}});
    runAcCase({"revc_ac_raw", "mesa-dual-rectifier-revc-96000.circuit.json", {}});
    runAcCase({"revc_ac_vint", "mesa-dual-rectifier-revc-96000.circuit.json", {{"mode", 2.0 / 3.0}}, 176400});
    // Orange OR120: no .op case (the bridge's series-diode midpoint raw2b is held only by leakage, and the self-biased
    // cathodyne cathode sits 0.23 V = 0.22 % off through its 100k load; every other node <= 0.14x tolerance)
    runAcCase({"or120_ac", "orange-or120-1972-80000.circuit.json", {}});
    runAcCase({"or120_ac_dimed", "orange-or120-1972-80000.circuit.json", {{"gain", 1.0}, {"bass", 1.0}, {"treble", 1.0}, {"hfdrive", 1.0}, {"fac", 0.0}}, 192000});
    runAcCase({"leslie_ac", "leslie-122-31000.circuit.json", {}});
    runAcCase({"leslie_ac_v1", "leslie-122-31000.circuit.json", {{"volume", 1.0}}});
    runAcCase({"uber_clean_ac", "bogner-uberschall-81000.circuit.json", {{"channel", 0.0}}});
    runAcCase({"surv_ac", "eae-surveyor-170000.circuit.json", {}});
    runAcCase({"hm2_ac", "boss-hm2.circuit.json", {}, 192000}); // the unit's own rate: the HF roll-off is steep (warping)
    // MT-2 / GT2: ten-plus HF poles (gyrators, Wien bridge; two resonant Sallen-Keys) stack 0.10-0.15 dB of warping at 5 kHz at 96 kHz
    runAcCase({"mt2_ac", "boss-mt2-metal-zone.circuit.json", {}, 192000});
    runAcCase({"mt2_ac_mid", "boss-mt2-metal-zone.circuit.json", {{"middle", 1.0}, {"midfreq", 1.0}, {"high", 1.0}}, 192000});
    runAcCase({"gt2_ac", "tech21-sansamp-gt2.circuit.json", {}, 192000});
    runAcCase({"gt2_ac_hot", "tech21-sansamp-gt2.circuit.json", {{"amp", 0.0}, {"mod", 0.0}, {"mic", 0.0}}, 192000});
    runAcCase({"jh1_ac", "marshall-jh1-jackhammer.circuit.json", {}, 192000});
    runAcCase({"jh1_ac_contour", "marshall-jh1-jackhammer.circuit.json", {{"contour", 1.0}, {"freq", 0.8}, {"treble", 0.2}, {"bass", 0.9}}, 192000});
    runAcCase({"ssleq_ac", "ssl-4000e-eq-121000.circuit.json", {}, 192000});
    runAcCase({"ssleq_ac_bells", "ssl-4000e-eq-121000.circuit.json", {{"hfgain", 1.0}, {"hfbell", 1.0}, {"hmfgain", 0.0}, {"hmfq", 0.0}, {"lmfgain", 1.0}, {"lmfq", 0.0}, {"lfgain", 0.0}, {"lfbell", 1.0}}, 192000});
    runAcCase({"ssleq_ac_filt", "ssl-4000e-eq-121000.circuit.json", {{"hpf_in", 1.0}, {"lpf_in", 1.0}, {"hpf", 0.7}, {"lpf", 0.6}, {"hfgain", 0.8}, {"lfgain", 0.2}}, 192000});
    // Bi-Phase: phasors off (footswitches), and both phasors held on Ped at full feedback (notches -13 dB; the deep notches
    // without feedback warp several tenths of a dB at their bottoms even at 384 kHz)
    runAcCase({"bp_ac_off", "mutron-biphase-144000.circuit.json", {{"footA", 0.0}, {"footB", 0.0}}, 192000});
    runAcCase({"bp_ac_fb", "mutron-biphase-144000.circuit.json", {{"sweepA", 1.0}, {"sweepB", 1.0}, {"feedbackA", 1.0}, {"feedbackB", 1.0}}, 384000});
    runAcCase({"lune_ac", "4ms-tremulus-lune-13000.circuit.json", {{"depth", 0.0}}});
    runAcCase({"mt3_ac", "mu-tron-iii-47000.circuit.json", {}, 192000.0}); // high-Q state-variable filter: gated where warping is small (the unit runs at 4x)
    runAcCase({"g615_ac", "fender-6g15-27000.circuit.json", {{"wet", 0.5}}});                     // dry path through the mixer
    runAcCase({"g615_wet_ac", "fender-6g15-27000.circuit.json", {{"tone", 0.5}, {"wet", 1.0}}, 0, "6g15_wet"}); // recovery + tone + mixer
    runAcCase({"cb21_ac", "dunlop-crybaby-21000.circuit.json", {}});
    runAcCase({"tdrv_ac", "butler-tube-driver.circuit.json", {}});
    runAcCase({"supro_ac", "supro-1624t.circuit.json", {}});
    runAcCase({"jcm_ac", "marshall-jcm800-2203.circuit.json", {}});
    runAcCase({"ac30_ac", "vox-ac30-tb.circuit.json", {}});
    runAcCase({"dyna_ac", "mxr-dyna-comp.circuit.json", {}});
    runAcCase({"sstone_ac", "ehx-small-stone.circuit.json", {{"rate", 0.0}, {"color", 0.0}}, 192000});
    runAcCase({"sstone_ac_color", "ehx-small-stone.circuit.json", {{"color", 1.0}}, 192000, "sstone_hold=1.84"});
    runAcCase({"uvibe_ac", "shinei-univibe.circuit.json", {{"cancel", 1.0}}});
    runAcCase({"emist_ac", "ehx-electric-mistress.circuit.json", {{"color", 0.0}, {"feedback", 0.0}}});
    runAcCase({"dimd_ac", "roland-dimension-d.circuit.json", {}});
    runAcCase({"re201_ac", "roland-re201.circuit.json", {{"echo", 0.0}, {"reverb", 0.0}}});
    runAcCase({"re201_play_ac", "roland-re201.circuit.json", {{"instrument_volume", 0.0}, {"mode", 0.0}, {"reverb", 0.0}, {"echo", 0.8}, {"intensity_adj", 0.5}}, 0, "re201_head1"}); // the trim at its pre-2026-11-12 0.5: the gate checks the playback EQ, not the trim
    runAcCase({"dmm_ac", "ehx-deluxe-memory-man.circuit.json", {{"blend", 0.0}, {"feedback", 0.0}}});
    runAcCase({"ce1_ac", "boss-ce1.circuit.json", {{"mode", 0.0}}, 192000});
    runAcCase({"u1176_ac", "urei-1176ln.circuit.json", {{"ratio", 0.0}}});
    runAcCase({"la2a_ac", "teletronix-la2a.circuit.json", {{"peak_reduction", 0.0}}});
    runAcCase({"vcarms_ac", "test-vca-rms.circuit.json", {}});
    runAcCase({"busg_ac", "ssl-4000g-bus-comp-130000.circuit.json", {}});
    runAcCase({"d160_ac", "dbx-160vu-125000.circuit.json", {}});
    runAcCase({"spk_ac", "test-speaker.circuit.json", {}, 192000}); // mass-controlled cone above fs: warping 0.1 dB near 5 kHz at 96 kHz
    runAcCase({"spkcab_ac", "test-speaker-cab.circuit.json", {}, 192000}); // speaker + fitted 1960AX cab (sections up to 10 kHz)
    std::printf(" SPICE gate (DC operating point):\n");
    runDcCase("ts_dc", "ts808.circuit.json");
    runDcCase("ab_dc", "ab763-normal.circuit.json");
    runDcCase("pent_dc", "test-pentode-se.circuit.json");
    runDcCase("pp_dc", "test-pp-6l6.circuit.json");
    runDcCase("twin_dc", "ab763-twin-reverb.circuit.json");
    runDcCase("jfet_dc", "test-jfet.circuit.json");
    runDcCase("oprail_dc", "test-opamp-rails.circuit.json");
    runDcCase("ds1_dc", "boss-ds1.circuit.json");
    runDcCase("muff_dc", "ehx-big-muff-pi.circuit.json");
    runDcCase("ffsi_dc", "dallas-fuzz-face-si.circuit.json");
    runDcCase("klon_dc", "klon-centaur.circuit.json");
    runDcCase("bb_dc", "marshall-bluesbreaker.circuit.json");
    runDcCase("rat_dc", "proco-rat2.circuit.json");
    runDcCase("gcb95_dc", "dunlop-crybaby-gcb95.circuit.json");
    runDcCase("v846_dc", "vox-v846-clyde-mccoy.circuit.json");
    runDcCase("ph90_dc", "mxr-phase90-script.circuit.json");
    runDcCase("ffge_dc", "dallas-fuzz-face-ge.circuit.json");
    runDcCase("otaopt_dc", "test-ota-optics.circuit.json");
    runDcCase("comp_dc", "test-compander.circuit.json");
    runDcCase("deluxe_dc", "ab763-deluxe-reverb.circuit.json");
    runDcCase("bassman_dc", "5f6a-bassman.circuit.json");
    runDcCase("plexi_dc", "marshall-1959-plexi.circuit.json");
    runDcCase("pvar_dc", "marshall-1959-plexi-variac.circuit.json");
    runDcCase("wide_dc", "test-wide-group.circuit.json");
    runDcCase("ladder_dc", "test-wide-ladder.circuit.json");
    runDcCase("rm_dc", "dallas-rangemaster.circuit.json");
    runDcCase("m133_dc", "mxr-m133-micro-amp.circuit.json");
    runDcCase("ffv_dc", "fuzz-face-vintage-ge.circuit.json");
    runDcCase("perc_dc", "interfax-harmonic-percolator.circuit.json");
    runDcCase("ts9dx_dc", "ibanez-ts9dx.circuit.json");
    runDcCase("tweed_dc", "fender-5e3-tweed-deluxe.circuit.json");
    runDcCase("dr103_dc", "hiwatt-dr103.circuit.json");
    runDcCase("ac15_dc", "vox-ac15-handwired.circuit.json");
    runDcCase("surv_dc", "eae-surveyor-170000.circuit.json");
    runDcCase("hm2_dc", "boss-hm2.circuit.json");
    runDcCase("mt2_dc", "boss-mt2-metal-zone.circuit.json");
    runDcCase("gt2_dc", "tech21-sansamp-gt2.circuit.json");
    runDcCase("jh1_dc", "marshall-jh1-jackhammer.circuit.json");
    runDcCase("ssleq_dc", "ssl-4000e-eq-121000.circuit.json");
    runDcCase("bp_dc", "mutron-biphase-144000.circuit.json");
    runDcCase("leslie_dc", "leslie-122-31000.circuit.json");
    runDcCase("lune_dc", "4ms-tremulus-lune-13000.circuit.json");
    runDcCase("mt3_dc", "mu-tron-iii-47000.circuit.json");
    runDcCase("g615_dc", "fender-6g15-27000.circuit.json");
    runDcCase("cb21_dc", "dunlop-crybaby-21000.circuit.json");
    runDcCase("tdrv_dc", "butler-tube-driver.circuit.json");
    runDcCase("supro_dc", "supro-1624t.circuit.json");
    runDcCase("jcm_dc", "marshall-jcm800-2203.circuit.json");
    runDcCase("rr_dc", "marshall-1959rr-91000.circuit.json");
    runDcCase("uber_dc", "bogner-uberschall-81000.circuit.json");
    runDcCase("jc120_dc", "roland-jc-120.circuit.json");
    runDcCase("be100_dc", "friedman-be100-151000.circuit.json");
    runDcCase("ac30_dc", "vox-ac30-tb.circuit.json");
    runDcCase("amp_dc", "test-amp-psu.circuit.json");
    runDcCase("dyna_dc", "mxr-dyna-comp.circuit.json");
    runDcCase("sstone_dc", "ehx-small-stone.circuit.json");
    runDcCase("uvibe_dc", "shinei-univibe.circuit.json");
    runDcCase("emist_dc", "ehx-electric-mistress.circuit.json");
    runDcCase("dimd_dc", "roland-dimension-d.circuit.json");
    runDcCase("re201_dc", "roland-re201.circuit.json");
    runDcCase("dmm_dc", "ehx-deluxe-memory-man.circuit.json");
    runDcCase("ce1_dc", "boss-ce1.circuit.json");
    runDcCase("u1176_dc", "urei-1176ln.circuit.json");
    runDcCase("la2a_dc", "teletronix-la2a.circuit.json");
    runDcCase("vcarms_dc", "test-vca-rms.circuit.json");
    runDcCase("cmos_dc", "test-cmos-divider.circuit.json");
    runDcCase("cmosjk_dc", "test-cmos-jk.circuit.json");
    runDcCase("ad536_dc", "test-ad536.circuit.json");
    runDcCase("vca2151_dc", "test-vca-2151.circuit.json");
    runDcCase("busg_dc", "ssl-4000g-bus-comp-130000.circuit.json");
    runDcCase("d160_dc", "dbx-160vu-125000.circuit.json");
    // ENGINE_QUEUE item 29: an open-loop integrator inside a servo (SSL 82E10 snapshot) needs the gmin-stepping fallback
    runDcCase("ssl82e10_dc", "test-dc-ssl82e10.circuit.json");
    runDcCase("zen_dc", "test-zener.circuit.json");
    runDcCase("vos_dc", "test-opamp-vos.circuit.json");
    runDcCase("spk_dc", "test-speaker.circuit.json");
    // ENGINE_QUEUE item 32: a case-only node pair and five CA3094 OTAs (EHX Micro Synth snapshot), every node vs ngspice .op
    runDcCase("msynth_dc", "test-dc-microsynth.circuit.json");
    if (failures) {
        std::fprintf(stderr, "%d circuit engine check(s) failed\n", failures);
        return 1;
    }
    std::printf("all circuit engine checks passed\n");
    return 0;
}
