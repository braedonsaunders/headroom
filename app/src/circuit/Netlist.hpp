#pragma once
// Headroom circuit engine: netlist model (docs/CIRCUIT_ENGINE.md §2).
//
// A netlist is the single source of truth of a circuit unit: parts, values, device models, controls (with
// the legacy parameter ids they answer to), IO calibration, probes and schematic layout. JSON is the native
// format ("headroom.circuit.v1"); a SPICE subset can be imported and exported.
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "json.hpp"

namespace rig::ce {

inline constexpr const char* kSchemaV1 = "headroom.circuit.v1";

struct SourceRef {
    std::string title, url, licence, note;
};

// Device model: a kind plus named numeric parameters ("is", "n", "rs", ...). Unknown parameter names are a
// validation error so typos never silently fall back to defaults.
struct DeviceModel {
    std::string name, kind, note;
    std::map<std::string, double> p;
    double get(const std::string& k, double fallback) const {
        auto it = p.find(k);
        return it == p.end() ? fallback : it->second;
    }
};

// A component value: a constant, or a law driven by a control (any R / C / L can be continuously variable).
struct ValueSpec {
    double nominal = 0.0;
    std::string control;    // empty = constant
    double min = 0.0, max = 0.0;
    std::string law = "log"; // "lin" | "log"
    bool invert = false;
    bool variable() const noexcept { return !control.empty(); }
};

struct Taper {
    // "A" log (mid = fraction of track at 50 % rotation, 0.10), "B" linear, "C" reverse log, "W" (S-curve).
    char kind = 'B';
    double mid = 0.5;
    double operator()(double x) const noexcept;
};

struct ControlDef {
    std::string id, label, kind = "continuous", unit, group, note;
    double def = 0.5;
    std::vector<std::string> params;     // legacy ids this control answers to (.rig / imported racks)
    double paramMin = 0.0, paramMax = 1.0; // legacy raw range mapped onto 0..1
    std::vector<std::string> options;    // selector labels
    bool legacyOnly = false;             // stored for compatibility, no circuit meaning
    bool mod = false;                    // a real circuit change the original unit does not have (e.g. a mains variac)
    std::string modNote;                 // what the mod is and why it is offered
    bool face = false;                   // a panel switch / selector with no legacy parameter: CircuitUnit exports it as an
                                         // extra (stepped) parameter so the unit face shows it (raw value = option index)
};

struct Part {
    std::string ref, type, model, control, label, note;
    std::string family, package; // optional UI metadata overrides (e.g. "electrolytic", "octal")
    std::vector<std::string> nodes;
    ValueSpec value;
    Taper taper;
    double tolerance = 0.0;
    double rend = 0.0; // pot: end resistance at each end of the track (Ω; the wiper never reaches the pins)
    // pot: the part of the control's travel this track spans (default the whole of it). Outside [spanLo, spanHi] the
    // wiper rests at the near end. Two half-tracks with spans [0, 0.5] / [0.5, 1] plus two wiper switches make a
    // centre-tapped pot (the SSL 4000 boost/cut controls); JSON "span": [lo, hi].
    double spanLo = 0.0, spanHi = 1.0;
    double ron = 0.1, roff = 1e9; // switches
    std::vector<int> positions;   // switch: 0-based selector option indices where closed (empty = threshold at 0.5)
    double gain = 1.0;            // vcvs / vccs
    double acAmp = 0, acFreq = 0, acPhase = 90; // V source: DC + acAmp·sin(2π·acFreq·t + acPhase°) (mains)
    // V source scaled by a panel control (ENGINE_QUEUE item 7, e.g. a variac on the mains): the whole source value
    // (DC + AC) is multiplied by scale(position): scaleMin at 0, 1 at 0.5 (nominal), scaleMax at 1, linear between.
    // JSON: "scale": {"control": "variac", "min": 0.75, "max": 1.083}.
    std::string scaleControl;
    double scaleMin = 1.0, scaleMax = 1.0;
    std::vector<double> turns, windR, windL;   // transformer windings (nodes = a1 b1 a2 b2 …)
    double lm = 0, lsat = 0;                   // transformer magnetizing inductance and saturation flux (1 turn)
    double hc = 0, hw = 0;                     // transformer core hysteresis: coercive MMF (A, 1 turn) and play width (V·s, 1 turn)
    int parallel = 1;                          // triode/pentode: identical devices in parallel (exact for matched tubes)
    int series = 1;                            // speaker: identical drivers in series (with `parallel`: a series-parallel cab)
    std::string light;                         // optical emitters / cells: the light node they write or read
    double couple = 1.0;                       // optical cell: fraction of the light node it sees
    struct ClockRow {
        double at = 0.0;
        std::vector<std::pair<double, double>> points;
    };
    std::vector<std::pair<double, double>> clock; // bbd: VCO law, control volts -> clock Hz (piecewise linear)
    std::string clockControl;                     // bbd: optional panel control inside an above-Nyquist VCO (clock.byControl)
    std::vector<ClockRow> clockRows;              // bbd: sorted 2-D VCO rows {at, points} interpolated over clockControl
    double clockHz = 0;                           // bbd: fixed clock (when no table)
    bool invert = false;          // pot / switch: control runs the other way
    bool hasValue = false;
};

struct IoDef {
    std::string input = "in", output = "out";
    std::string outputR; // optional second output (a stereo circuit: one instance renders both sides)
    std::string inputR;  // optional second driven input (a stereo circuit fed true stereo: L drives input, R drives inputR)
    double inputVoltsPerFS = 1.0, outputVoltsPerFS = 1.0;
    double sourceImpedance = 0.0;
};

struct Netlist {
    std::string schema = kSchemaV1, id, name, description;
    int version = 1;
    std::vector<SourceRef> sources;
    IoDef io;
    int oversampling = 2;
    // Lowest internal rate this circuit runs at in HQ (Hz; 0 = just `oversampling`). Set from the aliasing audit:
    // circuits whose alias energy below 20 kHz drops measurably from 2x to 4x carry 176400, so they run at about 4x at
    // 44.1/48 kHz; circuits that measure the same at both rates (compressors, modulation) stay at 2x.
    double minRate = 0;
    // LTE substep control (docs §10.16): "lte": {"start": "be" | "bdf2", "tol": n}. start: the integrator of a substepped
    // sample's first substep when the step size changes (backward Euler, the default, or variable-step BDF2 followed by
    // plain trapezoid through continuation samples); tol: the LTE tolerance scale wherever LTE is on (0 = the caller's).
    std::string lteStart;
    double lteTol = 0;
    // Newton tolerances (docs §10.42): "newton": {"tol": x, "tube": y} replace the engine defaults for this circuit,
    // kDefaultNewtonTol on every port and kTubeNewtonTol on tube ports; 0 = the default. The caller's
    // CompileOptions::newtonTol / tubeNewtonTol still win.
    double newtonTol = 0, tubeNewtonTol = 0;
    // Per-group Newton tolerance (docs §10.55): "newton": {"groups": {"Q14_1": 1e-5}}: every non-tube port of the group
    // holding that part converges at that tolerance instead of newtonTol. CE_TOLGROUPS="ref=x,..." replaces the list.
    std::vector<std::pair<std::string, double>> newtonGroupTols;
    // Newton start and chord hints (docs §10.50): "newton": {"chordGate": g, "quadPorts": q, "subPred": b, "subPredPorts": s}
    // replace the engine's chord gate (kEnvChordGate, 30), the quadratic predictor's port floor (kPredQuadPorts, 24) and the
    // substep predictor's bits and port floor (kEnvSubPred 30, kSubPredPorts 19) for this circuit; 0 (subPred: -1) = the
    // engine's. The CE_CHORDGATE / CE_PREDQPORTS / CE_SUBPRED / CE_SUBPREDPORTS switches still win; CE_NONEWTONHINTS=1 ignores them.
    double newtonChordGate = 0;
    int newtonQuadPorts = 0, newtonSubPred = -1, newtonSubPredPorts = 0;
    // Event substeps' Newton tolerance (docs §10.64): "newton": {"substeps": k} converges the tube ports of every LTE
    // substep runtime (T/2, T/4, T/8 and their BDF2 / backward-Euler starts) at k times the full step's tube tolerance;
    // every other port, and the full step, keep theirs; 0 = 1 (the same). CE_SUBTOL=<k> replaces it (1: the engine before).
    double newtonSubsteps = 0;
    // Intra-unit lanes (docs §10.33): "lanes": false keeps this circuit on one core (CompileOptions::lanes).
    bool lanes = true;
    std::string solver = "auto"; // auto | dk | wdf
    std::map<std::string, DeviceModel> models; // local models (override / extend the built-in library)
    std::vector<ControlDef> controls;
    std::vector<Part> parts;
    std::vector<std::string> probes;
    // Multirate partitions: groups containing these parts run at the oversampled rate, all others at the
    // host rate (exact one-way boundaries + half-band converters). Empty = everything oversampled.
    std::vector<std::string> oversampledParts;
    // Slow partitions: groups owning these nodes run at about slowRate Hz (band-limited control-rate circuits such as a
    // tremolo oscillator and its lamp); their outputs hold between updates.
    std::vector<std::string> slowNodes;
    double slowRate = 0;
    // The node the unit's LFO readout reads ("readouts": {"lfo": {"node": ...}}, CircuitUnit): the mains-fed settle keeps
    // the group that holds it running instead of moving it to its averaged operating point (docs §10.49). The readouts
    // object itself is kept as it was for the round trip (toJson).
    std::string lfoNode;
    nlohmann::json readouts = nlohmann::json::object();
    // Rate ladder ("multirate": {"rates": {"VPA": 16}}): the group holding the part runs at that factor of a 44.1/48 kHz
    // host (halved at 88.2 kHz and up, like "oversampling"); the rest at the unit's own rate (docs §3.7).
    std::vector<std::pair<std::string, int>> partRates;
    // ..."events": true: those groups run at the unit's rate instead and take the ladder's rate as LTE substeps only
    // inside an event (a power tube cutting off into the output transformer), with a band-limited output stream
    // (event-local oversampling, docs §10.48).
    bool rateEvents = false;
    // Approximate cuts (docs §2.6): supply rails whose consumers see them as known voltages, with the consumers'
    // current fed back one sample late. Only for heavily decoupled nodes (B+ rails behind tens of µF).
    std::vector<std::string> cuts;
    // Signal cuts with explicit loads: the listed parts see the node as known; everything else owns it.
    std::map<std::string, std::vector<std::string>> cutLoads;
    std::vector<std::string> delayCuts; // explicit cuts whose loads read the previous sample (break a feedback loop)
    // Signal cuts that keep the pre-2026-11-13 C_hf estimate ("chfHold": "group"; docs §10.40): the cap-free load nodes
    // are held by 1e-9 × the load group's largest capacitance instead of their own capacitor network's.
    std::vector<std::string> chfGroupHold;
    nlohmann::json layout = nlohmann::json::object();

    const ControlDef* control(std::string_view id) const;
    int controlIndex(std::string_view id) const;
    const Part* part(std::string_view ref) const;
    Part* part(std::string_view ref);
};

// Engineering-notation number: "4k7", "2u2", "47n", "1M" (mega), "220R", "1meg", "0.1uF", "10 Ω".
// spiceSuffixes: SPICE rules ("M" = milli, "MEG" = mega).
std::optional<double> parseValue(std::string_view text, bool spiceSuffixes = false);
std::string formatValue(double v, std::string_view unit);

// JSON <-> Netlist. parse() validates structure (types, node counts, models, controls); it never throws.
bool parseNetlist(const nlohmann::json& j, Netlist& out, std::string& error);
bool parseNetlistText(std::string_view text, Netlist& out, std::string& error);
nlohmann::json toJson(const Netlist& n);

// Resolve a device model by name: netlist-local first, then the built-in library (ModelLibrary.hpp).
std::optional<DeviceModel> resolveModel(const Netlist& n, const std::string& name);

// Apply user overrides {"parts": {"R7": {"value": "100k", "model": "TL072"}}, "models": {...}} (the
// component page's "circuit" edits). Transactional: on error `n` is unchanged.
bool applyOverrides(Netlist& n, const nlohmann::json& overrides, std::string& error);

// Values at a given control vector (positions 0..1 in netlist control order; empty = defaults). Shared by the
// compiler, the SPICE exporter and the model page so every view sees the same numbers.
double controlPosition(const Netlist& n, const std::string& id, const std::vector<double>& ctl);
double valueAt(const Netlist& n, const ValueSpec& v, const std::vector<double>& ctl);
// A V source's control scale factor (Part::scaleControl) at a control vector; 1 when not scaled.
double sourceScaleAt(const Netlist& n, const Part& p, const std::vector<double>& ctl);
inline double sourceScale(double pos, double lo, double hi) noexcept {
    return pos <= 0.5 ? lo + (1.0 - lo) * (pos * 2.0) : 1.0 + (hi - 1.0) * ((pos - 0.5) * 2.0);
}
// Pot track split: lower = pin1..wiper, upper = wiper..pin3 (each >= kPotEndOhms).
inline constexpr double kPotEndOhms = 1.0;
void potSegments(const Netlist& n, const Part& p, const std::vector<double>& ctl, double& lower, double& upper);
bool switchClosed(const Netlist& n, const Part& p, const std::vector<double>& ctl);
// The netlist ends in a cabinet (docs/CIRCUIT_ENGINE.md §10.21): it holds a `speaker` part and its output is none of a
// speaker's pins (so it is a cab block's microphone node). Such a unit already outputs a miked
// cab, so the rack adds no cabinet after it and a following cab's speaker profile does not load it.
bool netlistEndsInCab(const Netlist& n);
// Expected node count(s) per part type; 0 = unknown type.
int partArity(const std::string& type, int given);

// SPICE subset import / export (docs §2.4). Export emits B-sources for devices ngspice lacks.
struct SpiceImportReport {
    std::vector<std::string> warnings;
};
bool importSpice(std::string_view text, Netlist& out, SpiceImportReport& report, std::string& error);

// Authoring lint (docs §2, ENGINE_QUEUE items 4 and 11): mistakes that pass every numeric gate because ngspice is
// handed the same netlist. Returns human-readable warnings; empty = clean. `raw` is the netlist JSON as written
// (needed to see whether a value carried units).
//  - a capacitor written as a bare number below 1 ("0.01" parses as farads, not µF), or any C above 1 mF;
//  - an inductor (type L) above 100 H;
//  - a mains / AC-driven node shared with anything but rectifiers, transformers, fuses and low-value winding
//    resistors (a 270 kΩ screen feed on a mains node name put 848 V pp of raw AC on a 6973 screen).
std::vector<std::string> lintNetlist(const nlohmann::json& raw, const Netlist& n);
struct SpiceExportOptions {
    std::vector<double> controls; // control positions (0..1), defaults when empty
    std::string inputSource;      // full SPICE source spec for the input node, e.g. "SIN(0 0.1 1k)"
    std::string inputSourceR;     // io.inputR's source spec (empty: the same as inputSource, i.e. a mono feed)
    double logicRate = 0;         // engine internal rate (Hz) for CMOS logic: adds the band-limited edges' constant
                                  // group delay (Logic.hpp MinBlep::delay samples) to each output; 0 = none
    std::string inputFile;        // or: "time value" text file driven through XSPICE filesource (linear interpolation)
    std::string analysis;         // ".tran ..." etc. (control block written by caller if empty)
    // Convergence hints: initial guesses for op-amp swing nodes ("<ref>#x", engine node names), e.g. the engine's own
    // DC solution. Without one the mid-swing value is used. A .nodeset is released after the first DC phase, so
    // ngspice's operating point still satisfies its own equations.
    std::map<std::string, double> nodeset;
    // Initial state for `.tran … uic` (the SPICE gate's "settled" start): node voltages by engine node name (netlist
    // nodes and transformer cores' "REF#flux") as .ic, and inductor currents by Circuit::inductorCurrents() key as the
    // inductors' IC=. Capacitor voltages follow from the node voltages; nodes the deck lacks are skipped.
    std::map<std::string, double> ic, icL;
};
std::string exportSpice(const Netlist& n, const SpiceExportOptions& opt);
// The SPICE node name exportSpice() uses for a netlist node ("0" for ground).
std::string spiceNodeName(const std::string& node);
// The same for netlist n, with its case-collision renames (two node names that differ only in case get distinct
// SPICE nodes, see exportSpice). Use this one to address a deck's nodes.
std::string spiceNodeName(const Netlist& n, const std::string& node);

} // namespace rig::ce
