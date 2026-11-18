#pragma once
// Headroom circuit engine: compiler + real-time runtime (docs/CIRCUIT_ENGINE.md §3).
//
//   Netlist ──compile()──► primitives + NL devices ──► node classes ──► exact partition (union-find + SCC)
//           ──► per-group solver (linear SS | WDF | nodal DK + damped Newton) ──► process()
//
// Threading: compile()/reset() allocate and run on a control thread. setControlTarget(), process() and
// processBlock() never allocate and are safe on the audio thread.
#include <array>
#include <complex>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Blocks.hpp"
#include "Codegen.hpp"
#include "Devices.hpp"
#include "Logic.hpp"
#include "Netlist.hpp"

namespace rig::ce {

// LTE substep tolerance of the live units (CircuitUnit) and of `circuit_cli alias` (which measures the unit's path):
// 2, not the engine default 1. The aliasing gate's worst spur on the Plexi and variac sits at -58.7 / -58.9 dB for
// tolerances 0.5-1.25 and drops to -65.6 / -63.6 dB at 2, with the dimed SPICE accuracy nearly unchanged (docs §10.16).
inline constexpr double kLiveLteTol = 2.0;
// ...and the largest group the live units substep: every group, as in the engine (quality first; the CPU of the big
// power groups is in docs §10.16).
inline constexpr int kLiveLteMaxPorts = 96;
// Newton tolerances (relative step accepted as converged) where neither the caller nor the netlist sets one, measured
// against fine-step ngspice (docs §10.42). Every port: 1e-6. Junction, op-amp, rms and VCA control ports lose at 1e-3
// (dbx 160 detector 0.01 -> 1.2 % NRMSE, Small Stone LFO 10x further from ngspice, MT-2 / JH-1 aliasing over the gate),
// the rms detectors already at 1e-5. Tube ports (triode, pentode, vacuum diode): 1e-3. Their laws bend on a scale of
// volts; every tube unit's error against the truth is unchanged to its third significant digit, for up to 20 % less CPU
// (most amps 10-17 %). Circuits without tubes are byte-identical to the 1e-6 engine.
inline constexpr double kDefaultNewtonTol = 1e-6;
inline constexpr double kTubeNewtonTol = 1e-3;

// kMaxNlPorts (the largest coupled nonlinear system one solve group may hold): Devices.hpp.

class Circuit;
class WdfGroup;   // Wdf.hpp
class Halfband2x; // Multirate.hpp
class HalfbandStage; // Multirate.hpp
class SpringTank; // Blocks.hpp
class BbdLine;    // Blocks.hpp

enum class SolverKind : std::uint8_t { Linear, Wdf, Dk };
const char* solverName(SolverKind k) noexcept;

struct Prim {
    enum Kind : std::uint8_t { G, Cap, Ind, Vcvs, Nullor, Vccs, Winding }; // Winding: a,b electrical, c = core node, fixed = turns
    Kind kind = G;
    int a = 0, b = 0;  // terminals (node index, 0 = ground)
    int c = 0, d = 0;  // controlling nodes (Vcvs / Nullor / Vccs)
    int part = -1;     // netlist part index
    int slot = -1;     // value slot (R in ohms for G, C in farads, L in henries); -1 = `fixed`
    double fixed = 0;  // gain for Vcvs/Vccs; R for fixed internal resistors when slot < 0
    bool be = false;   // Cap/Ind: backward-Euler (L-stable) companion instead of trapezoidal, for stiff nodes
};

// How a value slot is computed from the controls.
struct ValueSlot {
    enum Kind : std::uint8_t { Const, Law, PotLower, PotUpper, Switch };
    Kind kind = Const;
    int part = -1;
    std::vector<int> controls; // control indices this slot reads
    double base = 0;
    double scale = 1.0;
    double value = 0;
};

struct Group {
    std::vector<int> unknowns; // node ids (free + owned driven)
    std::vector<int> branches; // winding prims: their currents are extra unknowns after the nodes
    std::vector<int> cutIn;    // cut nodes owned here whose consumers' current is injected (one sample late)
    struct CutTerm {
        std::uint8_t kind; // 0 G, 1 Cap, 2 Ind, 3 device port
        int index;         // prim id or local port offset
        int state;         // state index for Cap/Ind
        int node, other;   // cut node, other terminal (G)
        double sign;       // +1: current a→b leaves the cut node
    };
    std::vector<CutTerm> cutTerms; // consumer currents drawn from cut nodes by this group
    std::vector<int> inputs;   // node ids read from outside (known or upstream)
    std::vector<int> prims;    // prim ids stamped here
    std::vector<int> states;   // prim ids of Cap/Ind (one state each)
    std::vector<int> devices;  // NL device ids
    std::vector<int> exports;  // node ids published to the node-voltage array (subset of unknowns)
    std::vector<int> slots;    // value slots this group depends on
    std::vector<int> parts;    // netlist parts that live here (for the model page)
    int nn = 0;                // nonlinear port count
    std::vector<std::pair<int, int>> hyst; // magnetizing devices with hysteresis: (device id, port offset)
    std::vector<std::pair<int, int>> spk;  // speakers with a voice-coil thermal model: (Circuit::spkTh_ index, port offset)
    SolverKind solver = SolverKind::Dk;
    bool lowRate = false;      // multirate: runs at the host rate (sampleRate / 2)
    bool unread = false;       // docs §10.60: no node exported, no cut current: runGroup skips its transient samples
    int div = 1, phase = 0;    // slow partition: runs every div-th sample of its loop (staggered by phase)
    double T = 0;              // this group's sample period
    int level = 0;             // rate ladder: runs at sampleRate·2^level (CompileOptions::partLevels)
    int evos = 0;              // event-local oversampling (docs §10.48, CE_EVOS): the ladder level this base-rate group stands in for
    // Nodal charge states (docs §10.51, exactmisc; Engine.cpp "Nodal charge states"): per state, 1 if it is a co-tree
    // trapezoidal capacitor (its column of R is a ±1 combination of tree capacitors' columns: a capacitor loop), and per
    // state the combination (tree states nodT[nodOff[k] .. nodOff[k+1]) with signs nodS).
    std::vector<std::uint8_t> nodCo;
    std::vector<int> nodOff, nodT;
    std::vector<signed char> nodS;
};

struct Diagnostics {
    std::uint64_t samples = 0, newtonIterations = 0, homotopies = 0, failures = 0, refreshes = 0;
    std::uint64_t substeps = 0;
    std::uint64_t lteSubsteps = 0; // group-samples re-solved as 2 or 4 substeps by the truncation-error control
    std::uint64_t ptcRescues = 0;  // pseudo-transient continuation solved a sample Newton (and the homotopy) could not
    std::uint64_t activeSolves = 0, activeMisses = 0; // active-set Newton: solved there / fell back to the full port set // samples a group re-solved as two half steps after Newton failed at the full step
    int maxIterations = 0;
    double worstResidual = 0;
};

struct CompileOptions {
    double sampleRate = 96000;        // internal (oversampled) rate
    std::string solver = "";          // "" = netlist's choice; "dk" | "wdf" | "auto"
    bool exportAllNodes = false;      // analysis instances: every node probe-able
    bool exportNetlistProbes = false; // also publish the netlist's default probe nodes every sample
    std::vector<std::string> probes;  // extra nodes to export
    int maxIterations = 32;
    // Relative Newton step accepted as converged (docs §10.42). 0 = the default: the netlist's "newton.tol", else
    // kDefaultNewtonTol. A value > 0 here wins, and then applies to tube ports too unless tubeNewtonTol is set.
    double newtonTol = 0;
    // ...on the ports of tubes (triode, pentode, vacuum diode). 0 = the default: the netlist's "newton.tube", else
    // kTubeNewtonTol (newtonTol when that was set explicitly). Developer A/B: CE_TOL sets both, CE_TOLTUBE this one.
    double tubeNewtonTol = 0;
    // Set by compile (docs §10.55): newtonTol came from the caller or CE_TOL, so the netlist's group tolerances are ignored;
    // newtonTolResolved marks options that a compile already filled in (copied into a recompile or a helper).
    bool newtonTolForced = false, newtonTolResolved = false;
    bool predictor = true;            // extrapolate port voltages from the last two samples
    bool lazyClamps = true;           // solve without op-amp rail clamps while they are exactly off
    bool shermanMorrison = true;      // rank-1 updates for glided part values in groups with n >= 6
    bool multirate = false;           // honour the netlist's multirate partitions (sampleRate = 2x host)
    double glideMs = 15.0;            // control smoothing time constant
    // Mains-fed supplies start from their running (periodic) state instead of the DC operating point. 0 = start at the
    // DC operating point (the SPICE .op comparisons). > 0, rectifiers fed straight from mains sources: the DC point holds
    // the mains at its crest, which overstates B+ (a rectifier only tops the reservoir up near the crests), so reset()
    // runs at least this long with zero input, until the supply rails' per-period means stop moving (at most 2 s), then
    // re-solves the consumers' DC at those means. > 0, a transformer winding on a mains source (a supply behind a power
    // transformer): the running state is the periodic steady state itself, found from a cold start by extrapolating
    // the mains-period map (settlePeriodic, docs §3.10); the value only switches it on. A winding is a short at DC, so
    // that supply's DC point has every rail at 0 V, and its slowest modes (the core's DC flux, Lm/R ≈ 20 s; reservoir
    // balancers) are far too slow to run out.
    double settleMs = 0.0;
    // ≥ 0: a cold start instead of the above, the counterpart of ngspice's `.tran … uic`: every node, capacitor,
    // inductor and core flux at zero, sources at their t = 0 values, then this many ms of the mains with zero input
    // (a real duration). < 0: off.
    double coldMs = -1.0;
    // A circuit compiled from the same netlist at the same rate whose periodic state (settleMs, transformer-fed
    // supplies) starts this one's settle, at that circuit's control positions (set the controls afterwards as usual):
    // a second channel then settles in a few mains periods. Read during compile() only.
    const Circuit* settleFrom = nullptr;
    // Antiparallel diodes of the same model between the same two nodes become one pair port (shared Rs).
    // Exact up to the Rs drop of the non-conducting diode (< 1 µV); halves the Newton size of TS-style loops
    // and makes ideal-op-amp clipper loops WDF-eligible.
    bool mergeDiodePairs = true;
    // Bucket-brigade noise (BbdLine): on by default at the datasheet level (coordinator decision); off for tests and
    // for users who want a clean line.
    bool bbdNoise = true;
    // Rate ladder (docs §3.7): part ref → level. The group holding the part runs at sampleRate·2^level (level 0..5),
    // the rest at sampleRate; band-limited half-band cascades convert every node that crosses a rate. Groups joined by
    // a signal cut share the higher level. Off for circuits with CMOS logic cells and in the netlist multirate mode.
    std::vector<std::pair<std::string, int>> partLevels;
    // Noise and hum (docs §3.9; off by default so every SPICE gate and test is unchanged; on in the live unit):
    // Johnson current noise of every resistor and pot segment (4kT/R), shot noise 2qI on junction ports (diodes, BJT base
    // and collector, JFET gate leakage), space-charge-reduced tube plate noise (10·kT·gm, i.e. an equivalent noise
    // resistance of 2.5/gm at the grid) and grid-current shot noise; updated at up to 96 kHz per group and held
    // (white to 20 kHz within 0.4 dB). noiseSeed makes an instance's noise deterministic (part refs + seed).
    bool noise = false;
    double noiseTempK = 300.0;
    std::uint64_t noiseSeed = 0;
    // Heater-cathode hum: each tube's cathode sees its heater through rhk (leakage) ∥ chk (capacitance), the heater an
    // effective hkv volts rms at the mains frequency and phase (locked to the netlist's mains source). Model parameters
    // rhk / chk / hkv; defaults in ModelLibrary-independent constants (Engine.cpp). Needs a mains (45-65 Hz) source.
    bool hum = false;
    int ladderBaseStage = 0; // log2 of sampleRate over the host rate: ladder converters use the unit oversampler's stages
    // Local-truncation-error control (docs §3.1): each nonlinear DK group estimates the trapezoidal step's error from
    // its reactive states (T³x'''/12 from a third difference) and, when it is over tolerance while a device changes
    // regime (cutoff, grid conduction, flyback), re-solves the sample as 2 or 4 substeps: the first backward Euler,
    // the rest trapezoidal. Tolerance per state: lteTol·(1e-3·|x| + 1 mV or 1 µA). 0 = the fixed-step engine.
    double lteTol = 1.0;
    // ...only on groups of at most this many nonlinear ports (default: all). Substeps on the big power groups bring the
    // dimed 5150 from 3.3 to 1.2 % NRMSE against ngspice and the DR103 from 0.20 to 0.08 % (§10.16), at +35 % (DR103)
    // and +55 % (SLO-100) CPU dimed, −4 % on the 5150. The live units use the same (kLiveLteMaxPorts).
    int lteMaxPorts = 96;
    // ...the first substep after a step-size change (a full step before, or another substep count): 0 = backward Euler
    // (every substepped sample restarts on it), 1 = variable-step BDF2 through the last two samples' states, then plain
    // trapezoid, continuing without a restart while the substep count stays (§10.16). The netlist's "lte" key overrides.
    int lteStart = 0;
    // ...and not on groups the rate ladder raised (partLevels): those already run at the rate their aliasing needs, and
    // substeps there cost the Plexi's power group +54 % at 16x for 0.6 dB of alias level.
    // Open switch contacts as no edge (docs §10.29): a `switch` part on a switch or selector control that is open at the
    // compile's control positions is left out of the circuit instead of being stamped as its roff (1 GΩ), so the exact
    // partition can split the groups it joined (relay-switched amps: a channel relay's open contacts). A contact whose
    // removal would leave a node without a linear path to ground keeps its roff. The partition then depends on those
    // switches: reset() recompiles when they have moved (topologyStale()); between resets a contact compiled open stays
    // open (CircuitUnit rebuilds off the audio thread and hands the state over, adoptStateFrom). false (or CE_ROFFSW=1):
    // every contact is its ron / roff resistor, as before.
    bool openSwitches = true;
    // Control positions (netlist control order) the compile starts from instead of the defaults; they set the open
    // contacts above. Shorter vectors leave the remaining controls at their defaults.
    std::vector<double> controls;
    // Intra-unit lanes (docs §10.33): when the group dependency graph within a sample has independent branches worth a
    // second core, compile plans a helper lane (Circuit::beginLanes / laneHelper / endLanes). Bit-identical to the
    // sequential run. false (or CE_NOLANES=1, or the netlist's "lanes": false): no plan, always sequential.
    bool lanes = true;
    // Small-signal path (GR_LESSONS §4 P2, docs §10.37): while a group's ports stay inside a box around a frozen
    // linearisation whose remainder is certified below the Newton tolerance, the group solves that frozen linear
    // system instead of Newton. false (or CE_NOSMALLSIG=1): Newton on every sample, bit-identical to before.
    bool smallSignal = true;
    // Ahead-of-time specialised sparse kernels (Codegen.hpp, docs §10.44): bind generated SparseLin / SparseLu C++
    // kernels when their 128-bit structure signature is in the registry. false (or CE_NOCODEGEN=1): interpreter only,
    // bit-identical to the generated kernels.
    bool codegen = true;
};

class Circuit {
public:
    Circuit();
    ~Circuit();
    Circuit(const Circuit&) = delete;
    Circuit& operator=(const Circuit&) = delete;

    // Control thread. Builds everything, sets controls to their defaults and solves the DC operating point.
    bool compile(const Netlist& n, const CompileOptions& opt, std::string& error);

    // Solve the DC operating point and initialise reactive states from it (control thread; no allocation
    // after compile, but iterative). Returns false if the operating point did not converge.
    bool reset();

    // Audio thread. Control positions 0..1 (netlist control order).
    void setControlTarget(int control, double value) noexcept;
    void setControlImmediate(int control, double value) noexcept; // no glide (prepare / analysis)
    double controlValue(int control) const noexcept { return ctlCur_[static_cast<std::size_t>(control)]; }
    int controlCount() const noexcept { return static_cast<int>(ctlCur_.size()); }
    // Audio thread: couple a following cabinet's speaker impedance profile (fsHz <= 0 restores the netlist's speaker).
    void setSpeakerLoad(double fsHz, double qRes = 1.4, double leScale = 1.0) noexcept;

    // One internal-rate sample: input volts at io.input → output volts at io.output.
    double process(double vin) noexcept;
    // Block helper (input / output in volts at the internal rate).
    void processBlock(const double* in, double* out, int n) noexcept;
    // Multirate circuits: one HOST-rate sample (sampleRate / 2) in and out; oversampled groups run twice with
    // half-band converters at their boundaries. Only valid when isMultirate().
    double processHost(double vin) noexcept;
    bool isMultirate() const noexcept { return multirate_; }
    // Stereo input (io.inputR): the right input for the next process() / processHost() call, in volts (held until
    // changed). Without inputR it is ignored. The two-argument forms set it and render one sample.
    bool hasInputR() const noexcept { return inputNodeR_ > 0; }
    void setInputR(double v) noexcept { vinR_ = std::isfinite(v) ? std::clamp(v, -100.0, 100.0) : 0.0; }
    double process(double vinL, double vinR) noexcept {
        setInputR(vinR);
        return process(vinL);
    }
    double processHost(double vinL, double vinR) noexcept {
        setInputR(vinR);
        return processHost(vinL);
    }

    // Node access (exported nodes are current after process()).
    int node(const std::string& name) const;
    double nodeVoltage(int node) const noexcept { return V_[static_cast<std::size_t>(node)]; }
    // Stereo circuits (io.outputR): the right output for the sample just rendered (host rate after processHost).
    bool hasOutputR() const noexcept { return outputNodeR_ > 0; }
    double outputR() const noexcept {
        const auto r = static_cast<std::size_t>(outputNodeR_);
        return multirate_ ? lo_[r] : V_[r];
    }
    bool isExported(int node) const noexcept;
    // Inductor currents (a → b, amperes) after the last sample, for seeding a SPICE deck's `.tran … uic`
    // (SpiceExportOptions::icL): "REF" for an L part, "REF#l<k>" for winding k's leakage of transformer REF.
    std::vector<std::pair<std::string, double>> inductorCurrents() const;
    const std::vector<std::string>& nodeNames() const noexcept { return nodeNames_; }

    // Adopt runtime state (reactive states, Newton warm starts, control positions) from a circuit compiled
    // from the same netlist structure. Audio thread safe (copies into preallocated storage).
    bool adoptStateFrom(const Circuit& other) noexcept;
    // Different structure (a part edit): carry reactive states (voltage and current), Newton warm starts, node
    // voltages and control positions by element / node / control name. Returns false if nothing matched.
    bool adoptStateByName(const Circuit& other) noexcept;
    std::uint64_t structureHash() const noexcept { return hash_; }
    // CompileOptions::openSwitches: at these control positions (netlist order; the current positions when null) a
    // contact the compile left out would close (it cannot until a recompile; reset() does it), or one compiled closed
    // would open (it might leave the partition). Contacts compiled open that kept their roff are exact either way.
    bool topologyStale(const std::vector<double>* controls = nullptr) const noexcept;
    // Contacts the compile left out (open, CompileOptions::openSwitches).
    int openContacts() const noexcept;
    // settlePeriodic has run (a transformer-fed supply): CompileOptions::settleFrom of a later compile reads this state.
    bool hasPeriodicState() const noexcept { return !pss_.empty(); }
    // reset() runs a mains supply to its running state (CompileOptions::settleMs with a source of 40 Hz or more, or
    // coldMs), not only a DC solve: the expensive part of a compile (CircuitUnit skips a second reset at the same
    // controls, docs §10.52). An LFO's start kick alone (a sub-40 Hz source) does not count.
    bool settlesSupply() const noexcept {
        if (coldMs_ >= 0) return true;
        if (settleMs_ <= 0) return false;
        for (const auto& a : acSrc_)
            if (a.amp != 0.0 && a.w >= 2.0 * 3.14159265358979323846 * 40.0) return true;
        return false;
    }
    // Control thread (tests, load-time A/B, docs §10.52): a 64-bit digest of every runtime state array (stateArrays),
    // the noise generators, the source phase and the sample count. Equal digests: the next sample is computed the same.
    std::uint64_t stateDigest();

    const Netlist& netlist() const noexcept { return net_; }
    const std::vector<Group>& groups() const noexcept { return groups_; }
    const std::vector<Prim>& prims() const noexcept { return prims_; }
    const std::vector<NlDevice>& devices() const noexcept { return devices_; }
    const Diagnostics& diagnostics() const noexcept { return diag_; }
    const std::vector<std::string>& warnings() const noexcept { return warnings_; }
    double sampleRate() const noexcept { return fs_; }
    double partValue(int part, int which = 0) const noexcept; // current physical value (pots: which=0 lower,1 upper)
    std::string describeGroups() const;
    const std::vector<double>& groupTimesNs() const noexcept { return groupNs_; } // CE_PROFILE only
    const std::vector<double>& newtonTimesNs() const noexcept { return newtonNs_; }
    // Exact small-signal response of the discretised engine (z = e^{jωT}) from the input port to `outNode`,
    // linearised at the current operating point (call after reset()). Includes trapezoidal frequency warping,
    // so it is what the engine actually plays; ngspice's .ac is the continuous-time reference.
    std::vector<std::complex<double>> smallSignalResponse(const std::vector<double>& freqsHz, int outNode) const;
    // Developer micro-benchmark (circuit_cli kbench): time group g's Newton solve on its last sample (restarted from
    // the predictor `reps` times) and the device evaluations alone; returns {ns per solve, iterations, ns per eval sweep}.
    std::vector<double> benchNewton(std::size_t g, int reps);
    // Estimated per-sample floating-point work of the compiled solve (for budgets / the model page).
    double flopsPerSample() const noexcept;
    // Ahead-of-time specialisation (Codegen.hpp, docs §10.44): the structure of every sparse linear update and sparse
    // nodal Newton solver in use (the full-step and substep runtimes), for tools/circuit/circuit_codegen; and how many
    // of them run a generated kernel.
    std::vector<cg::LinPattern> codegenLinPatterns() const;
    std::vector<cg::LuPattern> codegenLuPatterns() const;
    std::vector<cg::NewtonPattern> codegenNewtonPatterns() const;
    void codegenStats(int& bound, int& total) const noexcept;

    // ── Intra-unit lanes (docs §10.33) ──
    // compile() plans them when groups that do not read each other's values within a sample (linked only by
    // one-sample-lag cut currents, or not at all) can run on two cores with a shorter critical path. The caller runs a
    // block of process() calls as:
    //     if (c.beginLanes()) forked = pool.fork(join, &Circuit::laneHelper, &c);   // audio thread
    //     ... process() ...
    //     if (armed) { c.endLanes(); pool.join(join); }                               // releases the helper first
    // (RtPool::fork may refuse: call endLanes() at once then, or at the end, as above.) The helper attaches at the
    // next sample boundary and takes its lane's groups by claim; the audio thread runs every group the helper has
    // not claimed, so it never waits on a helper that is not running, and with no helper the block is sequential.
    // The helper detaches after a bounded idle spin. Output is bit-identical to process() without lanes.
    bool lanesPlanned() const noexcept { return lane_ != nullptr; }
    bool beginLanes() noexcept;            // audio thread; false: no plan (or CE_PROFILE / CE_DEBUG_FAIL): run sequentially
    void endLanes() noexcept;              // audio thread, after the block's last process(): the helper returns
    static void laneHelper(void* circuit) noexcept; // the helper lane (an RtPool task; returns at once after endLanes)
    std::string describeLanes() const;     // the plan: lanes, estimated costs and critical path
    struct LaneStats {
        std::uint64_t duoSamples = 0, soloSamples = 0, helperGroups = 0, steals = 0;
        std::uint64_t blocks = 0, attaches = 0, idleDetaches = 0, waitDetaches = 0; // helper attach / detach
        std::uint64_t soloBeforeAttach = 0; // one-lane samples of a block before its helper attached
    };
    LaneStats laneStats() const noexcept;

private:
    friend class WdfGroup;
    struct GroupRt;
    bool buildGroupsRuntime(std::string& error);
    void buildSparse(std::size_t g); // sparse nodal Newton step for groups above 24 ports (reset())
    void buildSparseLin(std::size_t g); // sparse linear update (SparseLin) where it beats the dense one (reset())
    void buildSubSparse(std::size_t g); // the same two for the group's substep runtimes (reset())
    void linPorts(GroupRt& r, const double* w, double* p) noexcept; // p = Dp·w + Dc·cut (+ noise)
    void linOut(GroupRt& r, const double* w, double* out) noexcept;  // [exports; state voltages] = H·w (+ noise)
    void refreshGroup(std::size_t g, bool dc) noexcept;
    void deriveFromZ(std::size_t g, bool dc) noexcept;
    bool smUpdate(std::size_t g) noexcept;
    double primConductance(const Group& g, int prim) const noexcept;
    bool solveGroupDc(std::size_t g) noexcept;
    void updateControls() noexcept;
    bool evalSlots(bool force) noexcept; // returns true if any slot changed
    double slotValue(const ValueSlot& s) const noexcept;
    void processGroup(std::size_t g) noexcept;
    void runGroup(std::size_t g) noexcept; // processGroup, or the held cut currents of a slow group between updates
    bool newton(GroupRt& r, const Group& gp, const double* p, int maxIt) noexcept;
    double dcGmin_ = 1e-12; // node-to-ground conductance of the DC matrices (raised only by the gmin-stepping fallback)
    bool newtonGeneric(GroupRt& r, const Group& gp, const double* p, int maxIt, int& iters, double& residual) noexcept;
    bool newtonPtc(GroupRt& r, const Group& gp, const double* p, int maxIt) noexcept;
    std::unique_ptr<GroupRt> makeGroupRt(std::size_t gi, std::string& error);
    bool solvePorts(GroupRt& r, const Group& g, const double* p, const double* pAnchor) noexcept; // Newton + homotopy
    // Re-solve this sample as m = 2 or 4 substeps (see processGroup). mode 0: trapezoidal half steps (the Newton-
    // failure rescue); 2: the first substep backward Euler, the rest trapezoidal (LTE control).
    bool substepGroup(std::size_t gi, int m, int mode) noexcept;
    GroupRt* subRuntime(std::size_t gi, int m, bool be) noexcept; // the group at T/m (trapezoidal or backward Euler), refreshed if stale
    // TR-BDF2 LTE substeps (docs §10.47, CE_TRBDF2): M macro steps of T/M, each a trapezoidal stage to γ and a BDF2 stage,
    // on one trapezoidal runtime at γT/M (tbRuntime, the unused [2] slots of rtTr_ / rtBe_); the embedded estimate sets
    // the next sample's decision (GroupRt::tbNext).
    bool substepTrBdf2(std::size_t gi, int m, int mode) noexcept;
    GroupRt* tbRuntime(std::size_t gi, int M) noexcept;
    int lteSubsteps(const GroupRt& r) const noexcept;             // 1 if the step just solved is within tolerance, else 2 or 4
    int lteSubstepsFromHistory(const GroupRt& r) const noexcept;
    void lteTally(const GroupRt& r, bool hist) const noexcept; // P1 profiling (CE_LTEWHO)
    static const double* lteSlot(const GroupRt& r, int j) noexcept; // LTE history slot j (0 = newest) of the ring  // the same estimate for the sample just finished
    bool nonlinearEvent(const GroupRt& r) const noexcept;         // a device port changed regime over the step
    bool nonlinearEventAt(const GroupRt& r, const double* a, const double* b) const noexcept; // ... between port voltages a and b
    void pushLteHistory(GroupRt& r) noexcept;                     // state variables at t[n], after the state update
    void updateNoise(GroupRt& r, const Group& g) noexcept;        // draw this update's noise currents (CompileOptions::noise)
    void escapeUnstableBranches();                                // reset: leave a positive-feedback loop's unstable middle solution

    Netlist net_;
    CompileOptions opt_;
    // CompileOptions::openSwitches: every switch part on a switch / selector control, its prim, the closed state it was
    // compiled at and whether the compile left it out (open, prim terminals grounded).
    struct SwitchState {
        int part = -1, prim = -1;
        bool closed = false, removed = false;
    };
    std::vector<SwitchState> sw_;
    bool recompiling_ = false;
    bool recompileTopology(); // reset(): compile again at the current control positions (topologyStale)
    // Same netlist, rate and options, another switch topology (adoptStateFrom): prim → (group, state slot) and
    // device → (group, first port) of this compile, and a hash of everything but the partition.
    std::uint64_t netHash_ = 0;
    std::vector<int> primStateG_, primStateK_, devG_, devPort_;
    bool adoptAcrossTopology(const Circuit& o) noexcept;
    double fs_ = 96000, T_ = 1.0 / 96000;
    std::vector<std::string> nodeNames_;
    std::vector<std::uint8_t> nodeKind_;   // 0 ground, 1 known, 2 driven, 3 free
    std::vector<double> knownValue_;       // DC value of known nodes
    int inputNode_ = -1, outputNode_ = -1, outputNodeR_ = -1;
    int inputNodeR_ = -1; // io.inputR (a second known input node), -1 = none
    double vinR_ = 0.0;   // its pending value (setInputR)
    bool isInputNode(int n) const noexcept { return n == inputNode_ || (inputNodeR_ > 0 && n == inputNodeR_); }
    std::vector<Prim> prims_;
    std::vector<NlDevice> devices_;
    std::vector<ValueSlot> slots_;
    std::vector<Group> groups_;
    std::vector<std::unique_ptr<GroupRt>> rt_;
    // Step-halving fallback: nonlinear groups keep a second runtime at T/2. When Newton (and the homotopy)
    // fails at the full step (a pentode cutting off into the OT's leakage, say: a flyback edge faster than
    // one sample), the sample is re-solved as two half steps with the states mapped between the two
    // companion sets. Refreshed lazily (halfStale_) when the group's matrices change.
    std::vector<std::unique_ptr<GroupRt>> rtHalf_;
    std::vector<std::uint8_t> halfStale_;
    // LTE control (CompileOptions::lteTol): the group at T/2 and T/4 with backward-Euler companions (rtBe_) and at T/4
    // trapezoidal (rtTr_; T/2 is rtHalf_). Allocated at compile, refreshed lazily like rtHalf_. Slot [2] (T/8) is unused,
    // except by TR-BDF2 substeps (CE_TRBDF2, docs §10.47): rtTr_[g][2] is the trapezoidal runtime at γT, rtBe_[g][2] at γT/2.
    std::vector<std::array<std::unique_ptr<GroupRt>, 3>> rtBe_, rtTr_;
    std::vector<std::array<std::uint8_t, 3>> beStale_, trStale_;
    std::vector<double> V_;                // node voltages (all nodes)
    std::vector<std::uint8_t> exported_;
    std::vector<double> ctlCur_, ctlTarget_;
    std::vector<std::uint8_t> groupDirty_;
    std::vector<std::vector<int>> slotGroups_;   // slot -> groups
    std::vector<std::vector<int>> controlSlots_; // control -> slots
    int subCounter_ = 0;
    double glideAlpha_ = 0.1;
    bool gliding_ = false;
    Diagnostics diag_;
    // Intra-unit lanes (docs §10.33; Engine.cpp "Intra-unit lanes"). laneDuo_: the block runs on two lanes, from the
    // first sample a helper was attached at to endLanes (set by the audio thread between samples): group-level counters
    // go to the group's own Diagnostics (merged into diag_ by endLanes) and cut currents to per-group records (added in
    // group order after each sample).
    struct LanePlan;
    struct alignas(64) LaneDiag {
        Diagnostics d;
    };
    std::unique_ptr<LanePlan> lane_;
    std::vector<LaneDiag> laneDiag_;
    bool lanesArmed_ = false, laneDuo_ = false;
    Diagnostics& dg(std::size_t gi) noexcept { return laneDuo_ ? laneDiag_[gi].d : diag_; }
    Diagnostics& dg(const Group& g) noexcept { return laneDuo_ ? laneDiag_[static_cast<std::size_t>(&g - groups_.data())].d : diag_; }
    void cutAdd(std::size_t gi, int node, double x) noexcept; // cutAcc_[node] += x, or this group's record (laneDuo_)
    void buildLanes();
    void runLaneSample() noexcept;
    bool laneEnsure(std::size_t gi, std::uint64_t s) noexcept; // audio thread: group gi done this sample (claim or wait)
    void mergeLaneDiag() noexcept;
    // ── Small-signal path (GR_LESSONS §4 P2, docs §10.37; Engine.cpp "Small-signal path") ──
    struct SmallSig;
    void buildSmallSignal();                                                    // compile: eligible groups, preallocated
    bool ssSolve(std::size_t gi, GroupRt& r, const double* p) noexcept;         // the certified frozen solve (false: Newton)
    void ssAfter(std::size_t gi, GroupRt& r, bool failed) noexcept;             // a Newton sample past the wait: freeze or back off
    bool ssFreeze(GroupRt& r) noexcept;                                         // linearise at the solution and certify a box
    std::vector<std::string> warnings_;
    std::uint64_t hash_ = 0;
    bool traceNewton_ = false, debugFail_ = false;
    struct AcSource {
        int node;
        double dc, amp, w, phase;
        int ctl = -1;                    // control scaling the whole source (Part::scaleControl), -1 = fixed
        double smin = 1.0, smax = 1.0;
    };
    double acScale(const AcSource& a) const noexcept {
        return a.ctl < 0 ? 1.0 : sourceScale(ctlCur_[static_cast<std::size_t>(a.ctl)], a.smin, a.smax);
    }
    void refreshScaledSources() noexcept; // known values of control-scaled DC sources (and AC sources at t = 0)
    std::vector<AcSource> acSrc_; // mains-type sources (known nodes with a time-varying value)
    double settleMs_ = 0;           // see CompileOptions::settleMs
    double coldMs_ = -1;            // see CompileOptions::coldMs
    // Transformer-fed mains supplies (CompileOptions::settleMs): winding terminal nodes (compile), the periodic steady
    // state of the last settlePeriodic (its warm start at the next reset) and the state arrays it extrapolates.
    std::vector<int> windingEnds_;
    std::vector<double> pss_;
    bool mainsCore() const noexcept;                                 // a transformer winding sits on a mains source node
    void stateArrays(std::vector<std::pair<double*, std::size_t>>& out); // the runtime state, array by array
    void coldStart() noexcept;                                       // every node and reactive state at zero, t = 0
    void runMains(long n) noexcept;                                  // n host samples with zero input
    bool settlePeriodic();                                           // periodic steady state of the mains cycle
    bool pssQuick_ = false; // (docs §10.53) settlePeriodic's cold run: a held group's repeated failures skip the rescues
    bool dcOnly_ = false;   // (docs §10.53) an averaged circuit solved at DC only: reset() skips the transient-only structures
    bool settleSlowRails(const Netlist& averaged, const std::vector<std::string>& rect, long periodSamples); // mains-fed: slow rails (§10.49)
    bool profile_ = false;          // CE_PROFILE: accumulate per-group process time
    mutable std::vector<double> groupNs_, newtonNs_;
    double phase_ = 0;            // seconds since reset (wrapped per source when evaluated)
    void advanceAc(double dt) noexcept;
    struct MagCore {
        int core, flux, device;
        double lm;
    };
    std::vector<MagCore> mags_;
    struct BlockRt {
        int senseA, senseB, src; // input coil resistor nodes, output source (known) node
        double rin;
        std::shared_ptr<SpringTank> tank;
        double y = 0;
        std::vector<double> params; // SpringTank::Params as {delay1, delay2, t60, disp, stages, lp, hp, gain}
    };
    std::vector<BlockRt> blocks_; // behavioural blocks (spring tanks): sense after the solve, drive next sample
    // Shared state this circuit owned before adoptStateFrom() took over its predecessor's (tank / bucket chain / tape):
    // parked here so the release happens with the circuit, on the control thread, never on the audio thread.
    std::vector<std::shared_ptr<void>> retired_;
    int refreshInterval_ = 32; // internal samples between control-glide refreshes (see compile)
    struct BbdRt {
        int in, ctl, clockCtl, src1, src2; // sensed input, clock-control node, optional VCO panel control; output sources
        std::shared_ptr<BbdLine> line;
    };
    std::vector<BbdRt> bbds_;
    struct TapeRt {
        int recA, recX;  // record-head resistor nodes (current sense)
        double rrec;
        int ctl;         // Repeat Rate control index (-1: full speed)
        int src[3];      // playback-head sources (known nodes; -1 = none)
        std::shared_ptr<TapeLoop> loop;
    };
    std::vector<TapeRt> tapes_;
    std::vector<TapeLoop::Params> tapeParams_; // for the multirate re-prepare
    bool settleBbdDc(const std::vector<std::uint8_t>* skip = nullptr) noexcept; // DC: block outputs at their DC map, re-solve
    // CMOS logic cells (Logic.hpp, docs §10.14): logicAt_[g] lists the cells evaluated just before group g (their inputs
    // are solved earlier in the sample: exact timing); logicAt_[groups] runs after the sample (cells in a loop or in a
    // multirate circuit: a one-sample delay). Topological order within each list.
    std::vector<LogicCell> logic_;
    std::vector<std::vector<int>> logicAt_;
    std::shared_ptr<const MinBlep> blep_;
    bool settleLogicDc();
    void runLogic(std::size_t slot) noexcept {
        for (int c : logicAt_[slot]) logic_[static_cast<std::size_t>(c)].step(V_.data());
    }
    bool hasDelayCuts_ = false;  // netlist has delay cuts (feedback loops broken by a one-sample read)
    void runBlocks() noexcept;        // transformer cores (DC: v_core = 0, λ set by the magnetizing MMF)
    std::vector<std::uint8_t> cut_;    // node is a cut supply node
    std::vector<std::uint8_t> supplyCut_; // cut node by the supply rule (not a signal cut with explicit loads)
    std::vector<double> cutI_, cutAcc_; // consumer current per cut node (last sample / accumulating)
    double cutScale_ = 1.0;
    std::uint64_t tick_ = 0;            // samples of the group loop (slow partitions)
    std::vector<std::vector<std::pair<int, double>>> cutHold_; // slow groups: last cut currents, re-added while held             // multirate: weight of an oversampled group's cut currents
    std::vector<double> dcLoadG_;       // DC: resistive load conductance at each cut node (interface Newton)
    std::vector<double> tranLoadG_;     // signal cuts: loads' companion conductance stamped into the owner (only the
                                        // residual current lags a sample, so the explicit coupling stays a contraction)
    void accumulateCutTerms(std::size_t g, bool dc) noexcept;
    void initGroupStates(std::size_t gi) noexcept;  // reactive states at the group's current DC solution
    bool dcCutIterations(const std::vector<std::uint8_t>* skip); // alternate groups until cut currents settle
    void finishCutSample() noexcept;
    // Op-amps whose supply pins are circuit nodes (rail-node mode): the output stays an ideal source (exact
    // partition); its load current, measured like a cut node's consumer current, is drawn from the rails one sample
    // late (sourcing from V+, sinking into V−, plus the quiescent current), gated by the rail headroom.
    struct RailAmp {
        int out, vp, vn;
        double iq;
    };
    std::vector<RailAmp> railAmps_;
    void railDraws() noexcept;
    bool drawsDone_ = false; // railDraws() already added this sample's / iteration's rail currents
    // multirate
    bool multirate_ = false;
    std::vector<double> lo_, hi0_, hi1_;           // per node: host-rate value, two oversampled values
    std::vector<int> upOf_, downOf_;               // node -> converter index (-1 = none)
    std::vector<Halfband2x> conv_;                // up converters then down converters (indexed by upOf_/downOf_)
    // Rate ladder (CompileOptions::partLevels): one base-rate sample runs each group 2^level times in topological order;
    // a node read at another level than its producer's goes through a half-band cascade (|Δlevel| stages).
    struct LadderConv {
        int node = 0, from = 0, to = 0;
        std::vector<HalfbandStage> st;            // in processing order
        std::array<double, 32> buf{};
    };
    bool ladder_ = false;
    int maxLevel_ = 0;
    std::vector<double> blk_;                      // per node, 32 slots: its producer's samples in this base sample
    std::vector<int> levelOfNode_;                 // producer level per node (-1: held: rails, sources)
    std::vector<LadderConv> lconv_;
    std::vector<std::vector<std::pair<int, int>>> ladderIn_; // per group: (input node, converter or -1 at the same level)
    int outConv_ = -1, outConvR_ = -1;
    std::vector<std::uint8_t> ladderAc_;           // per group: reads an AC (mains) source
    std::vector<std::vector<int>> ladderAcIdx_;   // per group: the AC sources it reads (indices into acSrc_)
    double processLadder(double vin) noexcept;
    void runLadderConv(LadderConv& c, const double* src) noexcept;
    // Cut currents across the ladder: each consumer group's per-sub-sample contribution to a cut node, resampled to the
    // owner's level through the same half-band cascades, and played to the owner one base sample later, sub-sample by
    // sub-sample. (Averaging them over the base sample and holding the mean folded the consumers' currents and imaged
    // them back: per-group 16x measured -56 dB where the whole circuit at 16x measures -75 dB on the JCM800.)
    struct LadderCut {
        int group = 0, node = 0, owner = 0, conv = -1; // conv: index into lcutConv_ (-1: the same level)
        std::array<double, 32> buf{};
    };
    bool ladderCuts_ = false;
    int ladderSub_ = 0;                            // sub-sample of the group being solved (processLadder)
    std::vector<double> cutNext_, cutPlay_;        // per node, 32 slots at its owner's level
    std::vector<LadderCut> lcut_;
    std::vector<std::vector<int>> lcutOf_;         // per group: its entries in lcut_
    std::vector<int> lcutNodes_;                   // the cut nodes involved
    std::vector<LadderConv> lcutConv_;
    std::vector<double> lcutSave_;                 // per lcut_ entry: cutAcc_ before the consumer's sub-sample
    std::vector<double> cutKeep_;                  // primeLadderCuts: cutAcc_ kept across the priming (sized at compile)
    void primeLadderCuts() noexcept;
    std::vector<std::uint8_t> nodeLow_;            // node produced at host rate
    void primeConverters() noexcept;
    // ── Event-local oversampling (docs §10.48; CE_EVOS) ──
    // A group the rate ladder would raise to level L (netlist "multirate": {"events": true}) runs at the base rate with LTE
    // substeps, forced to M = 2^L substeps whenever it substeps. The output node's M× stream is the true substep values
    // inside such a sample and a windowed-sinc interpolation of the base-rate samples elsewhere (P samples of lookahead),
    // decimated by a linear-phase FIR (Q base samples each side): the output is delayed by exactly P − 1 + Q base samples.
    struct EvosOut {
        static constexpr int kRing = 32, kMaxM = 8;
        int node = -1, group = -1, exportIdx = -1;
        std::array<double, kRing> s{};                 // base-rate samples (ring)
        std::array<double, kRing * kMaxM> sub{};       // per slot: the event's sub-values 1..M−1
        std::array<std::uint8_t, kRing> ev{};          // per slot: the sample was substepped at M
        std::array<double, kMaxM> rec{};               // the running sample's substep values (substepGroup)
        int head = 0;
        std::vector<HalfbandStage> st;                 // CE_EVOS_IIR=1: the ladder's down stages (sized at compile)
        int nst = 0;
        std::array<double, 128> zh{};                  // the M× stream (ring) for the linear-phase decimator
        int zhHead = 0;
    };
    std::vector<EvosOut> evo_;                         // the output nodes (sized at compile)
    int nEvo_ = 0, evosM_ = 1, evosP_ = 4, evosQ_ = 5;
    std::vector<double> evosH_;                        // interpolation taps: (M − 1) phases × 2P
    std::vector<double> evosD_;                        // decimator taps: 2·M·Q + 1 at M× (delay Q base samples)
    std::vector<std::uint8_t> evosGroup_;              // per group: substeps forced to evosM_
    void evosPlan(const CompileOptions& opt);
    // ── Nodal charge states (docs §10.51, exactmisc; Engine.cpp "Nodal charge states") ──
    void nodalPlan();           // compile: per group, the spanning forest of its trapezoidal capacitors and the co-tree loops
    void nodalInfo() const;     // CE_NODALINFO=1: per group, states, loops and the linear-update operations they cost
    void evosPrime() noexcept;
    void evosRecord(std::size_t gi, GroupRt& h, const double* w, int step, int m) noexcept;
    double evosEmit(EvosOut& e) noexcept;
    // Electro-mechanical speakers (docs §10.21): the voice-coil / magnet thermal network, integrated after each solved sample
    // (trapezoidal, from the converged coil current) and fed back as the device's ΔRe for the next sample. hs[1..2] of the
    // device hold the temperatures, so a state hand-off carries them.
    struct SpkThermal {
        int dev;
        double re, alpha, rtv, ctv, rtm, ctm, coils;
        double pPrev = 0;
    };
    std::vector<SpkThermal> spkTh_;
    void advanceSpeakers(const Group& g, const double* vp, double dt) noexcept;
    struct SpeakerSlots {
        double fs0 = 0;
        int lces = -1, cmes = -1, res = -1, le = -1, le2 = -1;
    };
    std::vector<SpeakerSlots> speakers_;
    // Electro-mechanical speakers in a netlist that does not end in a cab (§10.21): a following cab's profile moves the
    // device's resonance and impedance peak through Mms and Rms, and its coil inductance through Le / L2 (the law's
    // reluctance term follows Le). fs0 is the driver's free-air resonance.
    struct SpeakerDevSlots {
        int dev = -1, mms = -1, rms = -1, le = -1, l2 = -1;
        double fs0 = 0, p13 = 0;
    };
    std::vector<SpeakerDevSlots> spkDevs_;
    void syncSpeakerDevices() noexcept; // the devices' Le-dependent law term from the Le slots' scale
    double spkFs_ = 0, spkQ_ = 1.4, spkLe_ = 1.0;
};

// A transformer-fed netlist's averaged circuit (settlePeriodic, docs §3.10): every part on the mains side (reached from a
// mains source through transformers and passives) removed, each rectifier output driven by a DC source "VSETTLE<k>"
// (value 0; rectNodes[k] is its node) and dropped from the cuts. False if the netlist has no such supply.
// capsBound (docs §10.49, the mains-fed settle): capacitors bound the mains side instead of joining it (they are open at
// DC), sources controlled from it join it, and cuts on it are dropped.
bool mainsAveragedNetlist(const Netlist& n, Netlist& out, std::vector<std::string>& rectNodes, bool capsBound = false,
                          std::vector<std::string>* mainsSide = nullptr);

} // namespace rig::ce
