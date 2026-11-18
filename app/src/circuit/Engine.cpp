#include "Engine.hpp"

#include <chrono>

#include "Denormals.hpp"
#include "ModelLibrary.hpp"
#include "Blocks.hpp"
#include "Codegen.hpp"
#include "NewtonKernel.hpp"
#include "Multirate.hpp"
#include "Wdf.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <unordered_map>
#include <numeric>
#include <set>
#include <sstream>
#include <type_traits>
#include <array>
#include <atomic>
#include <utility>

namespace rig::ce {
using namespace nk;

// Developer A/B switches, read once at load. They used to be function-local statics on the audio path: the first
// call after a knob edit in HQ mode ran their guarded initialisation (a lock, plus getenv) on the audio thread
// (test_param_drag's rtguard caught 2 locks).
namespace {
const bool kEnvBeCaps = std::getenv("CE_BECAPS") != nullptr;
const bool kEnvKcolOn = std::getenv("CE_NOLOWRANK") == nullptr;
const bool kEnvChordOn = std::getenv("CE_NOCHORD") == nullptr;
// Predictor input correction (processGroup) when p left its linear trend by more than this many tolerances (relative to
// the port voltage). 0 (2026-11-11, COST_BREAKDOWN T4): every sample. With the units' noise and hum on, the held noise
// draws step p every few samples and the plain extrapolation misses them; correcting every sample saves 3-15 % of the
// amps' instructions (Plexi -14 %, DR103 -15 %, Twin -8 %, 5150 -3 %) and up to 14 % on studio units, with no SPICE gate
// case or alias figure moving (it only changes the Newton start). The old attack-only gate was 1000; CE_PREDTH=1e30 turns
// it off (developer A/B).
const double kEnvPredTh = std::getenv("CE_PREDTH") ? std::atof(std::getenv("CE_PREDTH")) : 0.0;
// ...except on small groups (at most kPredSmallPorts ports) that already average under kPredEveryAboveIt Newton
// iterations per solve (the 768 kHz fuzz faces: 1.04): there the correction's own solve costs more than it saves
// (+5-7 % on the Fuzz Faces), so they keep the attack-only gate. CE_PREDNN / CE_PREDIT for A/B.
const double kPredAttackTh = std::getenv("CE_PREDTH") ? kEnvPredTh : 1000.0;
const int kPredSmallPorts = std::getenv("CE_PREDNN") ? std::atoi(std::getenv("CE_PREDNN")) : 4;
// Large groups (above kPredQuadPorts ports, 2026-11-12): the predictor extrapolates the port voltages
// and p quadratically (3·x1 − 3·x2 + x3) instead of linearly once three samples of history exist; the predictor correction
// then takes p's deviation from its quadratic trend. Only Newton's start moves (DI nulls −162 to −195 dB); on the
// Uberschall, 5150, SLO-100 MK II, Ecstasy and both Rectifiers it saves 7-11 % of instructions. CE_NOPREDQ=1: linear.
const bool kEnvNoPredQ = std::getenv("CE_NOPREDQ") != nullptr;
const int kPredQuadPorts = std::getenv("CE_PREDQPORTS") ? std::atoi(std::getenv("CE_PREDQPORTS")) : 24; // P1 A/B: CE_PREDQPORTS
const bool kEnvLteWho = std::getenv("CE_LTEWHO") != nullptr; // P1 profiling: which states trip the LTE control (describeGroups)
// LTE event statistics (CE_LTESTAT=1, describeGroups; docs §10.46): substep counts of detected and continued events, the
// estimate's ratio at each decision, chain lengths, and whether a continued sample still changed a device's regime.
const bool kEnvLteStat = std::getenv("CE_LTESTAT") != nullptr;
// Mains-fed settle: slow rails brought to their running level by the averaged circuit (docs §10.49, Circuit::settleSlowRails).
// CE_SLOWSETTLE=0: the settle before (the run's per-period test alone).
const bool kEnvSlowSettle = !(std::getenv("CE_SLOWSETTLE") && std::atoi(std::getenv("CE_SLOWSETTLE")) == 0);
// ...and where that settle falls back (its averaged circuit does not compile, has no DC point, or its steps never
// shrink), the process remembers it per netlist, partition, rate and options, and later settles of the same circuit go
// straight to the fallback (docs §10.52). CE_SLOWMEMO=0: every settle tries again, as before.
const bool kEnvSlowMemo = !(std::getenv("CE_SLOWMEMO") && std::atoi(std::getenv("CE_SLOWMEMO")) == 0);
// Unread groups (docs §10.60, pedeng): a group that exports no node (no other group, logic cell, probe or the output reads
// any of its nodes), draws no current from a cut node and owns none has no effect on anything outside itself (its
// states, ports and noise generator are its own), so its transient samples are skipped (Group::unread, runGroup); its DC point is
// still solved. A 4013 output pin wired to nothing (the OC-2's U3A Q) is one. CE_NOUNREAD=1: every group is solved every
// sample (the engine before §10.60). CE_UNREADINFO=1 lists them at compile.
const bool kEnvNoUnread = std::getenv("CE_NOUNREAD") && std::atoi(std::getenv("CE_NOUNREAD")) > 0;
const bool kEnvUnreadInfo = std::getenv("CE_UNREADINFO") && std::atoi(std::getenv("CE_UNREADINFO")) > 0;
std::mutex gSlowMemoMutex;
// Transformer-fed settle from a cold start (docs §10.53, Circuit::settlePeriodic), the same state, bit for bit, sooner:
//  - in the cold run, a group sample whose Newton fails right after a failed (held) one goes straight to the failure
//    exit, without newton()'s PTC fallback and the homotopy, half-step and PTC rescues;
//  - the averaged circuits (here and in settleSlowRails) are only ever solved at DC: their resets skip the sparse and
//    substep structures (buildSparse, buildSparseLin, buildSubSparse) that only a transient sample reads;
// CE_PSSFAST=0: the settle before.
const bool kEnvPssFast = !(std::getenv("CE_PSSFAST") && std::atoi(std::getenv("CE_PSSFAST")) == 0);
std::unordered_map<std::uint64_t, int> gSlowMemo; // key -> 1 the averaged circuit does not compile, 2 its steps fail
// Series rectifier diodes (docs §10.56, mainsAveragedNetlist): a node that only diodes touch (between two diodes of one
// rectifier leg: the OR120's and SLO-100 MK II's 2 × 1N400x per leg) is part of the rectifier, and the rectifier output is
// past the chain, at the reservoir. Before, the chain's midpoint was taken as the output: it floats when its diodes are
// off, its period mean is a few hundred volts under the reservoir, and the averaged circuit pinned there put the rails
// near 0 V each step (moves of 110-260 V that never shrank: the fallback). CE_DIODECHAIN=0: the midpoint, as before.
const bool kEnvDiodeChain = !(std::getenv("CE_DIODECHAIN") && std::atoi(std::getenv("CE_DIODECHAIN")) == 0);
// LTE substeps' Newton start (docs §10.46). Only Newton's start moves: same equations, tolerance and acceptance test.
// Bits of CE_SUBPRED (default 30 = 2 | 4 | 8 | 16; 0 = the zero-slope start of every substep sequence, as before):
//   1  the first substep extrapolates along the last two samples' slope (the shoot-out's "mode 1");
//      a detected event or an exact grid (16, 2) takes precedence. Not in the default: it costs the 5150 1-2 more
//      iterations on its BDF2 T/4 starts.
//   2  a sample that continues the last one's substep count on the same grid starts from the last substeps' own port
//      voltages (still in the trapezoidal runtime), not from a zero slope;
//   4  quadratic extrapolation along the substep grid once three real grid points exist;
//   8  the full step's predictor correction along the runtime's own p history (the part of p's change off the
//      extrapolated trend, through the runtime's kept factorisation), wherever that history is the same runtime's;
//  16  a detected event's first substep heads for the rejected full step's solution (v[n] + (v_full − v[n])/m).
// Only groups of at least kSubPredPorts ports (CE_SUBPREDPORTS): the history copies and the correction step cost about the
// iteration they save on small groups (the JC-120's 8-18-port groups: +1.5 / +3.3 % real / driven on the 9900X, +1.7 %
// driven instructions), and pay where an iteration is a 19-39 port factorisation (JCM800, 5150, Uberschall, SLO-100).
const int kEnvSubPred = std::getenv("CE_SUBPRED") ? std::atoi(std::getenv("CE_SUBPRED")) : 30;
const int kSubPredPorts = std::getenv("CE_SUBPREDPORTS") ? std::atoi(std::getenv("CE_SUBPREDPORTS")) : 19;
// ───────────────────────────── Newton hints per circuit (docs §10.50; 2026-11-14) ─────────────────────────────
// A netlist's "newton": {"chordGate", "quadPorts", "subPred", "subPredPorts"} (Netlist::newtonChordGate ...) replaces the
// in-solve chord gate (kEnvChordGate), the quadratic predictor's port floor (kPredQuadPorts) and the substep predictor's bits
// and port floor (kEnvSubPred, kSubPredPorts) for that circuit. Only Newton's start and its chord-or-refactorise choice move:
// the equations, the tolerances and the acceptance test (step within tolerance, residual gate) are the same. Each developer
// switch (CE_CHORDGATE, CE_PREDQPORTS, CE_SUBPRED, CE_SUBPREDPORTS) still wins; CE_NONEWTONHINTS=1 ignores the hints.
const bool kEnvNoNewtonHints = std::getenv("CE_NONEWTONHINTS") != nullptr;
struct NewtonHints {
    double chordGate = 0; // 0: kEnvChordGate (the kernel's own)
    int quadPorts, subPred, subPredPorts;
};
static NewtonHints newtonHints(const Netlist& n) {
    NewtonHints h{0, kPredQuadPorts, kEnvSubPred, kSubPredPorts};
    if (kEnvNoNewtonHints) return h;
    if (n.newtonChordGate > 0 && !std::getenv("CE_CHORDGATE")) h.chordGate = n.newtonChordGate;
    if (n.newtonQuadPorts > 0 && !std::getenv("CE_PREDQPORTS")) h.quadPorts = n.newtonQuadPorts;
    if (n.newtonSubPred >= 0 && !std::getenv("CE_SUBPRED")) h.subPred = n.newtonSubPred;
    if (n.newtonSubPredPorts > 0 && !std::getenv("CE_SUBPREDPORTS")) h.subPredPorts = n.newtonSubPredPorts;
    return h;
}
// Event substeps' Newton tolerance (docs §10.64; 2026-11-16): the tube ports of every LTE substep runtime
// of a circuit whose netlist sets "newton": {"substeps": k} converge at k times the full step's tube tolerance (step test
// and residual gate); every other port keeps its own. A substep's Newton starts a T/8 (T/4, T/2) step away on the substep
// predictor, so its first Newton step lands far inside the full step's tolerance; the tighter test mostly bought a
// confirming iteration. CE_SUBTOL=<k> replaces the netlist's factor everywhere (CE_SUBTOL=1: the engine before §10.64).
const double kEnvSubTol = std::getenv("CE_SUBTOL") ? std::atof(std::getenv("CE_SUBTOL")) : 0.0;
// ───────────────────────────── end of Newton hints ─────────────────────────────
constexpr int kSpSlope = 1, kSpGrid = 2, kSpQuad = 4, kSpCorr = 8, kSpToward = 16;
// TR-BDF2 LTE substeps (docs §10.47; Circuit::substepTrBdf2). CE_TRBDF2=1 on every LTE group without per-part backward-
// Euler companions, 0 off, unset: the netlist's "lte": {"start": "trbdf2"}. CE_TBSCALE scales the trapezoid-equivalent
// ratio that picks one or two macro steps (lteCount's 8), CE_TBFULL the one that returns to the full step (1), CE_TBMAX
// caps the macro steps (2), CE_TBPRED the Newton start bits (1 toward the rejected full step, 2 the predictor correction
// along the stage grid, 4 a continued sample extrapolates from the last sample's stages; default 7).
const int kEnvTrBdf2 = std::getenv("CE_TRBDF2") ? std::atoi(std::getenv("CE_TRBDF2")) : -1;
const double kEnvTbScale = std::getenv("CE_TBSCALE") ? std::atof(std::getenv("CE_TBSCALE")) : 1.0;
const double kEnvTbFull = std::getenv("CE_TBFULL") ? std::atof(std::getenv("CE_TBFULL")) : 1.0;
const int kEnvTbMax = std::getenv("CE_TBMAX") ? std::atoi(std::getenv("CE_TBMAX")) : 2;
const int kEnvTbPred = std::getenv("CE_TBPRED") ? std::atoi(std::getenv("CE_TBPRED")) : 7;
const double kPredEveryAboveIt = std::getenv("CE_PREDIT") ? std::atof(std::getenv("CE_PREDIT")) : 1.3;
// Lazy-clamp groups (op-amp rail clamps) keep their factorisations for the predictor correction too (2026-11-12): it
// only changes the Newton start. CE_NOCLAMPKS=1: as before (no correction on those groups).
const bool kEnvNoClampKs = std::getenv("CE_NOCLAMPKS") != nullptr;
const double kEnvSpFullRatio = std::getenv("CE_SPFULL") ? std::atof(std::getenv("CE_SPFULL")) : 4.2; // developer: min full-port dense/sparse ratio for SparseLu
const int kEnvSpMinPorts = std::getenv("CE_SPMINPORTS") ? std::atoi(std::getenv("CE_SPMINPORTS")) : 16; // developer: min ports for SparseLu
const bool kEnvDenseLin = std::getenv("CE_DENSELIN") != nullptr; // developer A/B: dense linear update everywhere
const double kEnvLinWeight = std::getenv("CE_LINWEIGHT") ? std::atof(std::getenv("CE_LINWEIGHT")) : 3.0; // developer: SparseLin cost weight
const bool kEnvCgDiag = std::getenv("CE_CGDIAG") != nullptr; // print each SparseLin / SparseLu signature and whether a kernel bound
const bool kEnvNoSparse = std::getenv("CE_NOSPARSE") != nullptr; // developer A/B: dense Newton steps only
const bool kEnvNoPortStates = std::getenv("CE_NOPORTSTATES") != nullptr; // developer A/B: every export / state row from H (GroupRt::pdRow)
const bool kEnvGenericNewton = std::getenv("CE_GENERIC") != nullptr; // developer A/B: groups above 24 ports on newtonGeneric
// Cross-sample chord (2026-11-12; OFF by default, developer opt-in): a group first tries the last
// sample's kept factorisation as a chord step, accepted only by the in-solve chord's test with the residual gate divided
// by CE_XCHORDGATE (1000) and only while the device Jacobians moved by at most CE_XCHORDDJ (0.1, Σ max|ΔJd|·max|K col|).
// CE_XCHORD=1: groups on a raised ladder level; big: those and groups above 24 ports; all: every DK group; 0 (default): off.
// Why off (docs §10.32): on ordinary DIs it nulls at -158..-180 dB against off for -3 % (AC30) / -7 % (Plexi)
// instructions, but it moves Newton's path at the tolerance level, and the AC30 dimed with a hot DI has solver events
// (Newton failures + homotopy, 28 in 20 s) whose outcome depends on that path: guarded at 0.1 it moved the output there by
// -17.5 dB peak (unguarded -147 dB, but unguarded it glitched the Uberschall's DI to -60 dB peak). The shipped predictor
// setting (CE_PREDTH=1e30 vs default) moves the same events by -11.6 dB peak, so the sensitivity is the events', not the
// chord's, but no setting of it is safe against every input.
const int kEnvXChord = std::getenv("CE_XCHORD") ? (std::string(std::getenv("CE_XCHORD")) == "all" ? 2 : std::string(std::getenv("CE_XCHORD")) == "big" ? 3 : std::atoi(std::getenv("CE_XCHORD")) ? 1 : 0) : 0;
// Event-local oversampling (docs §10.48). CE_EVOS=1: every group the rate ladder would raise (partLevels) runs at
// the base rate with LTE substeps forced to 2^level (Circuit::evosPlan); 0: the ladder everywhere; unset: the netlist's
// "multirate": {"events": true}.
// CE_EVOS_P: the output interpolator's half-width in base samples (lookahead; default 4); CE_EVOS_FC: its cutoff as a
// fraction of the base Nyquist (default 0.9); CE_EVOS_BETA: its Kaiser window β (default 7); CE_EVOS_FORCE=1: every sample
// substepped (a diagnostic: the stream then holds no interpolated value); CE_EVOS_NOGATE=1: a detected estimate over
// tolerance substeps without a device changing regime too (the leakage ring itself).
const int kEnvEvos = std::getenv("CE_EVOS") ? std::atoi(std::getenv("CE_EVOS")) : -1;
const int kEnvEvosP = std::getenv("CE_EVOS_P") ? std::atoi(std::getenv("CE_EVOS_P")) : 4;
const double kEnvEvosFc = std::getenv("CE_EVOS_FC") ? std::atof(std::getenv("CE_EVOS_FC")) : 0.9;
const double kEnvEvosBeta = std::getenv("CE_EVOS_BETA") ? std::atof(std::getenv("CE_EVOS_BETA")) : 7.0;
const bool kEnvEvosForce = std::getenv("CE_EVOS_FORCE") && std::atoi(std::getenv("CE_EVOS_FORCE")) > 0;
const bool kEnvEvosNoGate = std::getenv("CE_EVOS_NOGATE") && std::atoi(std::getenv("CE_EVOS_NOGATE")) > 0;
// ...the decimator: a linear-phase Kaiser FIR of half-length CE_EVOS_Q base samples (default 5; the output's delay is then
// the integer P − 1 + Q base samples), cutoff at the base Nyquist, β CE_EVOS_DBETA (default 11); CE_EVOS_IIR=1: the
// ladder's half-band stages instead (a fractional, frequency-dependent delay).
const int kEnvEvosQ = std::getenv("CE_EVOS_Q") ? std::atoi(std::getenv("CE_EVOS_Q")) : 5;
const double kEnvEvosDBeta = std::getenv("CE_EVOS_DBETA") ? std::atof(std::getenv("CE_EVOS_DBETA")) : 11.0;
const double kEnvEvosDFc = std::getenv("CE_EVOS_DFC") ? std::atof(std::getenv("CE_EVOS_DFC")) : 1.0; // its cutoff, fraction of the base Nyquist
const bool kEnvEvosIir = std::getenv("CE_EVOS_IIR") && std::atoi(std::getenv("CE_EVOS_IIR")) > 0;
// ...CE_EVOS_CONTM=1 (experiment): a sample that continues an event takes the estimate's own count (2 or 4), not M.
const bool kEnvEvosContM = std::getenv("CE_EVOS_CONTM") && std::atoi(std::getenv("CE_EVOS_CONTM")) > 0;
// ...CE_EVOS_REGIME=1 (experiment): an event continues only while a device still changed regime over the last sample.
const bool kEnvEvosRegime = std::getenv("CE_EVOS_REGIME") && std::atoi(std::getenv("CE_EVOS_REGIME")) > 0;
// ...CE_EVOS_MAX8=0 restores the cap of 4 substeps per event on every netlist (docs §10.62: a netlist's events may take 8)
const bool kEnvEvosMax8 = !(std::getenv("CE_EVOS_MAX8") && std::atoi(std::getenv("CE_EVOS_MAX8")) == 0);
} // namespace

const KorenTables* korenTables(const double* p) {
    static std::mutex m;
    static std::vector<std::unique_ptr<KorenTables>> cache;
    std::lock_guard<std::mutex> lock(m);
    for (const auto& t : cache)
        if (t->mu == p[0] && t->ex == p[1] && t->kg1 == p[2] && t->kp == p[3] && t->kvb == p[4] && t->gs == p[5] && t->gk == p[6]) return t.get();
    auto t = std::make_unique<KorenTables>();
    t->mu = p[0];
    t->ex = p[1];
    t->kg1 = p[2];
    t->kp = p[3];
    t->kvb = p[4];
    t->gs = p[5];
    t->gk = p[6];
    const double ex = t->ex;
    t->h.resize(KorenTables::NZ);
    t->dh.resize(KorenTables::NZ);
    t->d2h.resize(KorenTables::NZ);
    for (int i = 0; i < KorenTables::NZ; ++i) {
        const double z = KorenTables::ZMIN + i * (KorenTables::ZMAX - KorenTables::ZMIN) / (KorenTables::NZ - 1);
        double sig;
        const double sp = softplus(z, sig);
        const auto u = static_cast<std::size_t>(i);
        t->h[u] = std::pow(sp, ex);
        t->dh[u] = ex * std::pow(sp, ex - 1) * sig;
        t->d2h[u] = ex * ((ex - 1) * std::pow(sp, ex - 2) * sig * sig + std::pow(sp, ex - 1) * sig * (1 - sig));
    }
    t->a.resize(KorenTables::NV);
    t->da.resize(KorenTables::NV);
    t->c.resize(KorenTables::NV);
    t->dc.resize(KorenTables::NV);
    for (int i = 0; i < KorenTables::NV; ++i) {
        const double v = i * KorenTables::VMAX / (KorenTables::NV - 1);
        const auto u = static_cast<std::size_t>(i);
        t->a[u] = v > 0 ? 2.0 / t->kg1 * std::pow(v / t->kp, ex) : 0.0;
        t->da[u] = v > 0 ? 2.0 / t->kg1 * ex / t->kp * std::pow(v / t->kp, ex - 1) : 0.0;
        const double q2 = t->kvb + v * v, q = std::sqrt(q2);
        t->c[u] = 1.0 / q;
        t->dc[u] = -v / (q2 * q);
    }
    t->at.resize(KorenTables::NA);
    t->dat.resize(KorenTables::NA);
    t->d2at.resize(KorenTables::NA);
    for (int i = 0; i < KorenTables::NA; ++i) { // pentode plate collection atan(vpk/kvb)
        const double v = KorenTables::AMIN + i * (KorenTables::AMAX - KorenTables::AMIN) / (KorenTables::NA - 1);
        const double u = v / t->kvb, q = 1.0 / (1.0 + u * u);
        const auto k = static_cast<std::size_t>(i);
        t->at[k] = std::atan(u);
        t->dat[k] = q / t->kvb;
        t->d2at[k] = -2.0 * u * q * q / (t->kvb * t->kvb);
    }
    t->g.resize(KorenTables::NG);
    t->dg.resize(KorenTables::NG);
    t->d2g.resize(KorenTables::NG);
    for (int i = 0; i < KorenTables::NG; ++i) {
        const double vg = KorenTables::GMIN + i * (KorenTables::GMAX - KorenTables::GMIN) / (KorenTables::NG - 1);
        const double z = vg / t->gk;
        double sg;
        const double s = t->gk * softplus(z, sg);
        const double rs = std::sqrt(s);
        const auto u = static_cast<std::size_t>(i);
        t->g[u] = t->gs * s * rs;
        t->dg[u] = 1.5 * t->gs * rs * sg;
        t->d2g[u] = s > 1e-300 ? 1.5 * t->gs * (0.5 * sg * sg / rs + rs * sg * (1 - sg) / t->gk) : 0.0;
    }
    // Two-lane tables (docs §10.51, exactmisc): the same values, interleaved per knot (NlDevice::evalTriodeLut2)
    auto knots = [](std::vector<double>& K, const std::vector<double>& x, const std::vector<double>& y, const std::vector<double>& z,
                    const std::vector<double>* w) {
        K.assign(4 * x.size(), 0.0);
        for (std::size_t i = 0; i < x.size(); ++i) K[4 * i] = x[i], K[4 * i + 1] = y[i], K[4 * i + 2] = z[i], K[4 * i + 3] = w ? (*w)[i] : 0.0;
    };
    knots(t->ac, t->a, t->c, t->da, &t->dc);
    knots(t->gK, t->g, t->dg, t->d2g, nullptr);
    knots(t->hK, t->h, t->dh, t->d2h, nullptr);
    knots(t->atK, t->at, t->dat, t->d2at, nullptr);
    t->lanes = std::getenv("CE_NOTUBELANES") == nullptr;
    cache.push_back(std::move(t));
    return cache.back().get();
}

const char* solverName(SolverKind k) noexcept {
    switch (k) {
        case SolverKind::Linear: return "linear state-space";
        case SolverKind::Wdf: return "wave digital filter";
        case SolverKind::Dk: return "nodal DK + Newton";
    }
    return "?";
}

namespace {
enum NodeKind : std::uint8_t { kGround = 0, kKnown = 1, kDriven = 2, kFree = 3 };
constexpr int kRefreshInterval = 32;    // internal samples between control-glide matrix refreshes
constexpr double kDcInductorG = 1e3;    // inductor conductance in the DC solve (S)
constexpr double kDcGmin = 1e-12;       // node-to-ground conductance in the DC solve (S)
constexpr double kLteRel = 1e-3;        // LTE control: relative tolerance per state (scaled by CompileOptions::lteTol)
constexpr double kLteAbsV = 1e-3, kLteAbsA = 1e-6; // ... and absolute (capacitor volts, inductor amps)
constexpr double kRegenJump = 0.5;     // regenerative groups: a port moving this far (V) in one sample is a switch
static const bool kEnvOldStagger = std::getenv("CE_OLDSTAGGER") != nullptr; // developer A/B: slow groups staggered by group index (before 2026-11-12)
static const bool kEnvForceTopo = std::getenv("CE_FORCETOPO") != nullptr; // developer A/B: same-structure hand-offs through adoptAcrossTopology
constexpr int kLteMaxSubsteps = 4;      // ... at most 4 substeps per sample (8 cost 10-15 % more CPU dimed for < 0.1 % NRMSE)
constexpr double kLteGate = 0.2;        // ... substeps only where a port's incremental conductance moved this much
// Heater-cathode hum (CompileOptions::hum): the effective heater AC seen by the cathode, V rms. A 6.3 V centre-tapped
// heater is balanced to ground, so only its imbalance couples; 5 % of 6.3 V is assumed (model parameter hkv).
constexpr double kHumHkv = 0.315;
// Typical (healthy-tube) heater-cathode leakage, the hum defaults (model parameter rhk; docs §3.9 for the sources). The
// datasheet / MIL acceptance worst case (Ihk 10 µA at 100 V, rhk = 10 MΩ) is reachable through the model card.
constexpr double kHumRhkTriode = 1e9, kHumRhkPentode = 1e8;
constexpr int kNoiseTaps = 7;        // noise hold compensation: symmetric FIR length (updateNoise)
// Noise draws (P3, 2026-11-12; docs §10.34). The thermal sources' node scatter is folded into the response matrix (one
// column per source instead of per node), so a draw copies its Gaussians instead of scattering them. The folded matrix
// is then multiplied by 4×4 blocks, skipping a block whose every entry adds less than kNoiseChunkEps of the noise power
// of each export, state and port that reads its rows (about half the blocks of the one-group heavy amps; an output
// moves by at most 2·sources·kNoiseChunkEps of its noise power, 2e-8 at 100 sources). The tube plates' gm ∝ Ip^(1/3) takes a float cube root (cbrtNoise)
// instead of the libm call. Same draws, same sources, same seeds: the noise realisation is the old one to float
// rounding. CE_NOISELEGACY=1: the per-node scatter, every block, std::cbrt (A/B); CE_NOISECHUNK=<eps> (0: every block).
static const bool kNoiseFold = std::getenv("CE_NOISELEGACY") == nullptr;
static const double kNoiseChunkEps = std::getenv("CE_NOISECHUNK") ? std::atof(std::getenv("CE_NOISECHUNK")) : 1e-10;
constexpr int kFlickMax = 8;         // 1/f noise: AR(1) sections per source, half a decade apart (buildGroupsRuntime)
constexpr double kNoisePrune = 1e-4; // noise columns under −40 dB of every row's noise power are dropped (≤ 0.02 dB each)
constexpr double kOpenLeak = 1e-6;       // open switch contacts leave the circuit only where R_end ≤ this · roff (compile)
constexpr double kCutLoopGainMax = 0.5;  // supply cuts above this per-sample loop gain are solved implicitly (compile)
constexpr double kCutReservoirMax = 0.5; // ... and those whose supply caps do not at least halve Z_owner (no reservoir)

struct UnionFind {
    std::vector<int> p;
    explicit UnionFind(int n) : p(static_cast<std::size_t>(n)) { std::iota(p.begin(), p.end(), 0); }
    int find(int x) {
        while (p[static_cast<std::size_t>(x)] != x) x = p[static_cast<std::size_t>(x)] = p[static_cast<std::size_t>(p[static_cast<std::size_t>(x)])];
        return x;
    }
    void unite(int a, int b) { p[static_cast<std::size_t>(find(a))] = find(b); }
};

std::uint64_t fnv(std::uint64_t h, const std::string& s) {
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}
} // namespace

// Sparse linear update: H·w and Dp·w are C·M⁻¹·R·w (exports / state voltages / port voltages are differences of node
// voltages, and inputs enter directly), so for a large sparse group they are one solve with the sparse LU of M (pattern
// and pivot order fixed at reset, values refactored at every refresh) instead of the dense (ny+ns)×W and nn×(ns+nu)
// products. Same arithmetic up to rounding order; engaged only where its operation count wins (10x weight).
struct SparseLin {
    bool on = false, valid = false;    // built (cheaper), and factorised for the current transient M
    SparseLu lu;                       // M alone (lu.Mraw = GroupRt::Mraw)
    std::vector<std::uint8_t> rStruct; // n×W: R's pattern at reset (a refresh outside it turns the group dense)
    std::vector<int> cOff, cRow, cIdx; // R by column: permuted row, index into R
    std::vector<int> oA, oB, oJA, oJB; // per output row (exports, then states): permuted columns (+) / (−), inputs (+) / (−)
    std::vector<int> pA, pB, pJA, pJB; // per port
    std::vector<double> lVal, uVal, rVal; // L by column, U by row, R by column: packed in solve order at each factorisation
    std::vector<int> bPorts, bOut;     // back substitution rows linPorts / linOut read (descending; see spNeeded)
    std::vector<double> bPart;         // linPorts' right-hand side after the state and input columns (linOut starts from it)
    const double* bPartW = nullptr;    // the w it was summed from (null: none; linOut consumes it)
    // Specialised kernel for this exact structure (Codegen.hpp; null: none generated, or CE_NOCODEGEN). cgOk: bound and
    // its baked R entries equal the packed values (checked at every pack, so after every refresh).
    const cg::LinKernel* cg = nullptr;
    bool cgOk = false;
    bool pack(const double* R) noexcept {  // after spFactor: contiguous values for the per-sample solves
        bPartW = nullptr;
        const auto nL = lu.lIdx.size(), nU = lu.uIdx.size();
        std::copy(lu.v.begin(), lu.v.begin() + static_cast<std::ptrdiff_t>(nL), lVal.begin());
        std::copy(lu.v.begin() + static_cast<std::ptrdiff_t>(nL), lu.v.begin() + static_cast<std::ptrdiff_t>(nL + nU), uVal.begin());
        for (std::size_t e = 0; e < cIdx.size(); ++e) rVal[e] = R[cIdx[e]];
        cgOk = cg != nullptr;
        for (int i = 0; cgOk && i < cg->nBaked; ++i) cgOk = rVal[static_cast<std::size_t>(cg->bakedIdx[i])] == cg->bakedVal[i];
        if (cg && !cgOk && kEnvCgDiag) std::fprintf(stderr, "codegen: a baked R entry changed: interpreter until the next refresh\n");
        return true;
    }
};
// Small-signal path (GR_LESSONS §4 P2, docs §10.37; the code is in the "Small-signal path" section after processGroup):
// a group's devices linearised at v0 (currents i0, Jacobian Jd0), the frozen port-space solve v = v0 + S·(p + q0) with
// S = (I − K·Jd0)⁻¹ and q0 = K·i0 − v0, and the box [lo, hi] inside which that solve is certified within the Newton
// tolerance. Allocated at compile for eligible groups only; everything else is preallocated.
struct Circuit::SmallSig {
    bool on = false;                       // a certified linearisation is frozen: ssSolve tries it every sample
    bool hadBox = false;                   // rho holds the last box's half-widths (the cheap pre-check before a freeze)
    std::vector<double> v0, i0, Jd0, q0, S, lo, hi, rho;
    std::vector<double> A, SK, G, rinv, eps; // freeze scratch (G: the exports' and states' rows, (ny + ns) × nn)
    std::vector<int> piv;
    int wait = 0, backoff = 16;            // Newton samples until the next freeze attempt; its growth on short stretches
    std::uint64_t run = 0;                 // samples of the current frozen stretch
    std::uint64_t hits = 0, freezes = 0, tries = 0, exits = 0, rejects = 0;
    double checkWorst = 0.0;               // CE_SSCHECK: worst |v_L − v_Newton| / (tol·(1 + |v|)) over the hits
    double freezeNs = 0.0, freezeMaxNs = 0.0; // CE_PROFILE: time in freeze attempts (total, longest)
};
struct Circuit::GroupRt {
    std::uint8_t pssHeld = 0;      // (docs §10.53) the last sample of settlePeriodic's cold run failed and kept its ports
    std::uint8_t pssNoPtc = 0;     // (docs §10.53) newton() without its PTC fallback (that held group's next Newton)
    std::unique_ptr<SmallSig> ss;  // small-signal path (null: not eligible or switched off)
    std::unique_ptr<SparseLin> sl;
    std::unique_ptr<SparseLu> sp;
    std::vector<double> Mraw;          // M before factorisation (sparse Newton path; groups above 24 ports)
    int n = 0, nu = 0, ns = 0, nn = 0, ny = 0, W = 0;
    std::vector<int> unkOf, inOf;  // global node -> local unknown / input index (-1 = none)
    std::vector<std::uint8_t> kcl; // unknown has a KCL row
    std::vector<double> M, R, Z;   // n×n system, n×W right-hand sides, Z = M⁻¹R
    std::vector<int> piv;
    std::vector<double> Dp, K, H;  // nn×(ns+nu), nn×nn, (ny+ns)×W
    // Port-defined states (COST_BREAKDOWN T10): a state (or an exported node) across one port's sense nodes, or across
    // two ports' sense nodes that share their other end (a tube's interelectrode capacitors), has its voltage in the accepted Newton solution,
    // vc = vp[pdA] − vp[pdB] (−1: none), which is its row of H up to the Newton residual (inside newtonTol). The dense
    // linOut then computes the other rows only: Hc holds the export rows and the other states' rows, hcOut their place
    // in out. pdOk: this sample's vp is the accepted solution (processGroup), so the rows may be read from it.
    std::vector<int> pdRow, pdA, pdB, hcOut; // pdRow: index into out (exports, then states)
    std::vector<double> Hc, hcY;
    int hcY0 = 0; // Hc rows that are exports (they come first)
    bool pdOk = false;
    std::unique_ptr<KaCache> kac, kacR; // gathered live columns of K / Kr (newtonKernel)
    std::vector<double> kcol, kcolR; // per port: max_k |K[k][port]| (full / lazy-clamp reduced): drops dead Jacobian rows
    std::vector<double> Dc;        // nn×nuc (cut-current columns into p)
    int nuc = 0;
    std::vector<double> w;         // [states; inputs; NL currents]
    std::vector<double> gc, G2, sgn, vcLast;
    std::vector<double> uPrev;     // last sample's inputs and cut injections (for step halving)
    // LTE control: state variables (capacitor voltage, inductor current) at t[n-1] … t[n-5] (5·ns, newest first), which
    // states are inductors, their absolute tolerances, how many valid history samples there are, and the substep count
    // the next sample starts with (1 = try the full step). beAll / iLast: the backward-Euler substep runtimes.
    bool lteOn = false, beAll = false;
    bool bdf2 = false; // LTE substeps start on variable-step BDF2 (CompileOptions::lteStart): the rtBe_ slots are trapezoidal runtimes at 2βT/m
    // Noise (CompileOptions::noise): injection columns, their response, this draw's offsets, the generator.
    bool noiseOn = false;
    int nNoise = 0, noiseDiv = 1, noiseCount = 0;
    std::vector<int> noiseRow;                 // column -> KCL row (unknown index)
    struct NoiseR {
        int prim, ca, cb;                      // resistor prim, its ends' columns (-1: no unknown with a KCL row)
        double scale;                          // sqrt(4kT/R·band): refreshed with the matrices (deriveFromZ)
        int ka, kb;                            // its ends' kept-column positions in xf (-1: none or pruned)
        double dens = 0.0;                     // prim < 0: a fixed current-noise density (A/√Hz; op-amp input pin)
        int fl = -1;                           // its 1/f bank (flick), -1: white only
    };
    // 1/f noise (op-amp en/in corners, tube flicker): a bank of unit-variance AR(1) sections s ← a·s + c·g whose
    // weighted sum has the PSD fc/f relative to the source's white density (designFlicker); added to the white draw.
    struct Flick {
        int n = 0;
        float a[kFlickMax] = {}, c[kFlickMax] = {}, w[kFlickMax] = {}, s[kFlickMax] = {};
        double fc = 0.0;
    };
    std::vector<Flick> flick;
    struct PortV {                             // a voltage noise in series with a sensing port (op-amp en)
        int port;
        double dens, scale;                    // V/√Hz; dens·√band (deriveFromZ)
        int fl;
    };
    std::vector<PortV> portV;
    std::vector<NoiseR> noiseR;
    std::vector<int> rAct;                     // resistors with a kept column (deriveFromZ)
    int nRAct = 0;
    struct ShotDev {
        int o, kind;                           // first port; 0 one junction port, 1 BJT, 2 tube
        int fl = -1;                           // tube plate flicker bank (model "fcf"), -1: none
        double var = 1.0;                      // kind 0: variance factor (1/m on a series diode string of m junctions)
    };
    std::vector<ShotDev> shotDev;
    std::vector<double> Pn, Hn, Zd, zcol, zv, dP, dOut, dI;
    std::vector<double> noiseI0, noiseGm0;     // per port: tube plate current and gm at the operating point (reset)
    int nAct = 0, naP = 0, nxP = 0;            // kept noise columns (deriveFromZ prunes the negligible ones), padded
    std::vector<int> actCol;
    std::vector<std::uint8_t> colKeep;
    std::vector<double> colVar;
    // The per-draw product is the node-voltage response z = Zf·x over the nz unknowns the exports, states and ports read
    // (Zd: their rows of M⁻¹ at the noise columns); exports, states and ports then gather from z (index nz reads 0).
    // In float (the noise needs no more): Zf (nz × naP, kept columns), Kf = K (nn × nnP) for the shot currents,
    // xf = [column injections | port shot currents] after the hold compensation, xr = the last kNoiseTaps raw draws
    // of it (ring, xHead), comp = the compensation's half taps (comp[0] the centre), gz = this update's Gaussians.
    int nz = 0, nDraw = 0, xHead = 0;
    std::vector<int> zNeed, gSt, gPort;        // z rows: the exports' unknowns (rows 0..ny-1, -1: none), then the rest;
                                               // gathers: (a, b) index pairs into z per state and per port
    std::vector<float> Zf, Kf, xf, xr, gz;
    // Folded draws (kNoiseFold): Zf's columns are the kept sources (rAct order), xf / xr's head their unit draws. Per
    // block of Zf rows (4 rows; the last 0-3 rows one block each) the kept 4-column chunks (zcCol[zcOff[b] .. zcOff[b+1])),
    // per z row the least noise power of an export, state or port reading it (scratch), and whether a kept source has a
    // 1/f bank (then the draws are composed one by one).
    std::vector<int> zcOff, zcCol;
    std::vector<double> zRowMin;
    bool thFl = false;
    float comp[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    std::uint64_t rng[8] = {};                 // two interleaved xoshiro256** states (one per vector lane)
    // A positive-feedback (regenerative) loop: its operating point was the unstable middle solution (reset). Each
    // sample's switch then waits until the old branch is gone (see processGroup); vpAlt / curAlt hold the jump's solution.
    bool regen = false;
    std::vector<double> vpAlt, curAlt;
    int lteWarm = 0, subNext = 1;
    int subPrev = 1; // substeps per sample of the last accepted sample (1 = a full step): BDF2 starts only when it changes
    int evosM = 0;   // event-local oversampling (docs §10.48): every LTE decision over 1 substeps this many times
    int subGrid = 0; // substep runtimes: real substep-grid points in vpPrev, vpPrev2, vpPrev3 after the last substep (kEnvSubPred)
    int subPGrid = 0; // ... and consecutive solves of this runtime whose p is in pPrev, pPrev2, pPrev3 (kEnvSubPred)
    bool subDetect = false; // substepGroup is re-solving a rejected full step (its solution is in vp)
    bool tb = false;        // TR-BDF2 LTE substeps (docs §10.47, CE_TRBDF2): substepTrBdf2 replaces substepGroup's LTE sequences
    int tbNext = 1;         // ... the next sample's decision from the embedded estimate (1 full step, 2 / 4: one / two macro steps)
    std::vector<double> xh, lteAbs, iLast;
    int xhHead = 0; // LTE history ring: slot j (0 = newest) of xh starts at ((xhHead + j) % 5)·ns
    std::vector<std::uint8_t> xInd;
    // Active-set Newton: per port, its device and first port, whether it can drop out (grid / control-only), the
    // cut-in threshold for grids, and the current history for the predictor.
    bool activeSet = false;
    std::vector<int> devOfPort, baseOfPort;
    std::vector<std::uint8_t> dropKind; // 0 never, 1 control-only (always), 2 grid (when below cut-in)
    std::vector<double> dropBelow, curPrev1, curPrev2;
    std::vector<double> p, pPrev, vp, vpPrev, vpPrev2, F, J, Jf, delta, vloc, out;
    std::vector<double> pPrev3, vpPrev3; // quadratic predictor (kPredQuadPorts): three samples back
    std::vector<double> pPrev2, dpPred, KJd; // KJd: newtonPtc scratch (nn²) // predictor: p two samples back; p minus its linear extrapolation
    KernelState ks, ksR; // ksR: a lazy-clamp group's clamp-free solve (Circuit::newton)
    int quadPorts = 24, subPred = 30, subPredPorts = 19; // Newton hints (docs §10.50): kPredQuadPorts, kEnvSubPred, kSubPredPorts or the netlist's
    const double* predDp = nullptr; // set for the sample's first Newton call only (processGroup)
    int predHist = 0;               // consecutive samples completed in processGroup (the p history is valid at 2)
    std::vector<int> pivN;
    std::vector<std::uint8_t> lim; // per port
    std::vector<double> limA, limB, limC;
    // Newton tolerance (docs §10.42): tol is the group's smallest port tolerance; ptol holds each port's own when they
    // differ (tube ports at CompileOptions::tubeNewtonTol beside junction ports), else it is empty and every port uses tol.
    double tol = 1e-6;
    std::vector<double> ptol;
    std::vector<int> portDev;      // port -> device id (first port of each device gets the eval)
    std::unique_ptr<WdfGroup> wdf;
    bool dcMode = false;
    std::uint64_t iters = 0, solves = 0;
    std::uint64_t lteEvents = 0; // P1 (GR_LESSONS §4): samples this group re-solved as LTE substeps (profiling)
    std::uint64_t lteDetect = 0; // P1 profiling: LTE events found by rejecting a solved full step (the rest continue one)
    double subNs = 0;            // P1 profiling (CE_PROFILE): time in LTE substeps (ns)
    mutable std::vector<std::uint32_t> lteWho; // P1 profiling (CE_LTEWHO=1): per state, LTE decisions it was over tolerance in
    // CE_LTESTAT (docs §10.46): the last estimate's worst ratio (÷ 12, as lteCount reads it), and per decision kind
    // (0 detected on a full step, 1 continued, 2 the history decision after a substepped sample) the substep count and
    // the ratio bucket (<=1, <=2, <=4, <=8, <=27, <=64, <=512, more); chain lengths (1..63, 64+); continued samples with /
    // without a regime change over the sample.
    mutable double lteLastRatio = 0;
    std::uint64_t stM[3][5] = {}, stR[3][8] = {}, stChain[65] = {}, stRegime[2] = {};
    int stLen = 0;
    std::uint64_t itHist[6] = {}; // solves needing 1, 2, 3, 4-6, 7-12, >12 iterations (profiling)
    int maxIt = 0;
    double itAvg = 2.0; // smoothed Newton iterations per solve (predictor gate: kPredEveryBelowIt)
    void count(int its) noexcept {
        itAvg += (static_cast<double>(its) - itAvg) * (1.0 / 256.0);
        iters += static_cast<std::uint64_t>(its);
        ++solves;
        maxIt = std::max(maxIt, its);
        ++itHist[its <= 3 ? std::max(its, 1) - 1 : its <= 6 ? 3 : its <= 12 ? 4 : 5];
    }
    std::vector<const NlDevice*> dev; // devices in port order
    std::vector<int> bs, bn;          // per port: first port of its device, device port count
    // The Newton kernel specialised to this group's structure (docs §10.45; null: none generated, or CE_NOCODEGEN): the
    // full port set and the lazy-clamp reduced set (devR).
    cg::NewtonFn nkFull = nullptr, nkRed = nullptr;
    // Lazy rail clamps: inside the op-amp swing a clamp's current is exactly zero, so the group is solved
    // without its clamp ports and falls back to the full system only when a clamp would conduct.
    bool lazyClamps = false;
    std::vector<int> red, clampPort;
    std::vector<double> clampLo, clampHi, Kr, limAr, limBr, limCr;
    std::vector<const NlDevice*> devR;
    std::vector<int> bsR, bnR;
    std::vector<std::uint8_t> limR;
    std::uint64_t fullSolves = 0;
    // Sherman–Morrison
    bool sm = false;
    int smCount = 0;
    std::vector<double> X, gStamp, colTmp, rowTmp, rowTmp2;
};

// Intra-unit lanes (docs §10.33): the compile-time plan and the per-sample claim protocol (see "Intra-unit lanes" at
// the end of this file). Sample numbers s count up from 1 per circuit; a group's claim / done word holds the last duo
// sample it was claimed / finished in.
struct Circuit::LanePlan {
    struct alignas(64) Word {
        std::atomic<std::uint64_t> v{0};
    };
    static constexpr std::uint64_t kClosed = ~std::uint64_t{0};
    std::vector<int> laneA, laneB;           // each lane's groups in group order (A: the audio thread, B: the helper)
    std::vector<std::uint8_t> inB;           // per group: in lane B (claimable by either thread)
    std::vector<std::vector<int>> deps;      // per group: earlier groups it shares a node with (must finish first)
    std::vector<std::vector<int>> depsB;     // per lane-A group: its deps in lane B
    std::vector<double> cost;                // per group: estimated ns per sample
    double seqCost = 0, parCost = 0;         // estimated sample: sequential, two lanes (incl. synchronisation)
    std::vector<std::uint8_t> needDone;      // per lane-A group: a lane-B group depends on it (publish its done word)
    std::unique_ptr<Word[]> claim, done;
    // Cut-current records: group g's region starts at rec + recOff[g]: its count (first) and up to recCap[g] entries,
    // on cache lines of its own (the two lanes write them at once). Lane B's regions come last, together: the audio
    // thread's replay streams them.
    struct alignas(64) RecLine {
        std::pair<int, double> e[4];
    };
    std::vector<RecLine> recLines;
    std::pair<int, double>* rec = nullptr;
    std::vector<int> recOff, recCap;
    alignas(64) std::atomic<std::uint64_t> stamp{kClosed}; // (sample << 1) | duo while a block runs, else kClosed
    alignas(64) std::atomic<std::uint64_t> doneB{0};       // the helper went through lane B for this sample
    // (sample << 8) | k: the helper is at laneB[k] (every earlier lane-B group is finished, or claimed by the audio thread)
    alignas(64) std::atomic<std::uint64_t> cursorB{0};
    alignas(64) std::atomic<int> present{0};               // a helper is attached (read by the audio thread per sample)
    alignas(64) std::atomic<std::uint64_t> helperGroups{0}, attaches{0}, idleDetaches{0}, waitDetaches{0};
    std::uint64_t seq = 0, duo = 0, solo = 0, steals = 0;  // audio thread only
    std::uint64_t blocks = 0, soloLead = 0, blockSolo = 0; // audio thread: blocks, solo samples before the first duo one
    bool blockDuo = false;
    std::uint64_t fpMode = 0;                              // the audio thread's FP control word (the helper copies it)
};

Circuit::Circuit() = default;
Circuit::~Circuit() = default;

int Circuit::node(const std::string& name) const {
    std::string l = name;
    for (auto& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (l == "0" || l == "gnd" || l == "ground") return 0;
    for (std::size_t i = 0; i < nodeNames_.size(); ++i)
        if (nodeNames_[i] == name) return static_cast<int>(i);
    return -1;
}

bool Circuit::isExported(int nd) const noexcept {
    return nd >= 0 && static_cast<std::size_t>(nd) < exported_.size() && exported_[static_cast<std::size_t>(nd)];
}

double Circuit::slotValue(const ValueSlot& s) const noexcept {
    switch (s.kind) {
        case ValueSlot::Const: return s.base * s.scale;
        case ValueSlot::Law: return s.scale * valueAt(net_, net_.parts[static_cast<std::size_t>(s.part)].value, ctlCur_);
        case ValueSlot::PotLower:
        case ValueSlot::PotUpper: {
            double lo, up;
            potSegments(net_, net_.parts[static_cast<std::size_t>(s.part)], ctlCur_, lo, up);
            return (s.kind == ValueSlot::PotLower ? lo : up) * s.scale;
        }
        case ValueSlot::Switch: {
            const auto& p = net_.parts[static_cast<std::size_t>(s.part)];
            return switchClosed(net_, p, ctlCur_) ? p.ron : p.roff;
        }
    }
    return s.value;
}

double Circuit::partValue(int part, int which) const noexcept {
    for (const auto& pr : prims_) {
        if (pr.part != part || pr.slot < 0) continue;
        const auto& s = slots_[static_cast<std::size_t>(pr.slot)];
        if (s.kind == ValueSlot::PotUpper && which == 0) continue;
        if (s.kind == ValueSlot::PotLower && which == 1) continue;
        return s.value;
    }
    return 0.0;
}

// ───────────────────────────── compile ─────────────────────────────
bool Circuit::compile(const Netlist& n, const CompileOptions& opt, std::string& error) {
    net_ = n;
    opt_ = opt;
    debugFail_ = std::getenv("CE_DEBUG_FAIL") != nullptr; // developer trace of Newton failures (stderr)
    fs_ = opt.sampleRate;
    settleMs_ = opt.settleMs;
    coldMs_ = opt.coldMs;
    windingEnds_.clear();
    pss_.clear();
    railAmps_.clear();
    if (n.lteTol > 0 && opt_.lteTol > 0) opt_.lteTol = n.lteTol; // the netlist's LTE tolerance where the caller has LTE on
    if (n.lteStart == "bdf2") opt_.lteStart = 1;
    else if (n.lteStart == "be") opt_.lteStart = 0;
    { // Newton tolerances (docs §10.42): the caller's, else CE_TOL (both, developer A/B), else the netlist's, else the defaults
        const char* tt = std::getenv("CE_TOL");
        const char* tu = std::getenv("CE_TOLTUBE");
        const double envTol = tt ? std::atof(tt) : 0.0, envTube = tu ? std::atof(tu) : 0.0;
        // one tolerance for every port, and then no per-group tolerance (docs §10.55); options copied from an earlier
        // compile (a recompile, a settle helper) keep that compile's answer
        const bool forced = opt_.newtonTolResolved ? opt_.newtonTolForced : opt_.newtonTol > 0 || envTol > 0;
        opt_.newtonTolForced = forced;
        opt_.newtonTolResolved = true;
        if (!(opt_.newtonTol > 0)) opt_.newtonTol = envTol > 0 ? envTol : n.newtonTol > 0 ? n.newtonTol : kDefaultNewtonTol;
        if (!(opt_.tubeNewtonTol > 0))
            opt_.tubeNewtonTol = envTube > 0 ? envTube : forced ? opt_.newtonTol : n.tubeNewtonTol > 0 ? n.tubeNewtonTol : kTubeNewtonTol;
    }
    if (const char* lt = std::getenv("CE_LTE")) opt_.lteTol = std::atof(lt); // developer A/B: LTE tolerance scale, 0 = fixed step
    if (const char* ls = std::getenv("CE_LTESTART")) opt_.lteStart = std::string(ls) == "bdf2" ? 1 : 0; // developer A/B: substep start
    if (const char* lp = std::getenv("CE_LTEPORTS")) opt_.lteMaxPorts = std::atoi(lp); // developer A/B: largest group with LTE
    profile_ = std::getenv("CE_PROFILE") != nullptr; // developer: per-group time (circuit_cli bench)
    T_ = 1.0 / fs_;
    nodeNames_.assign(1, "0");
    prims_.clear();
    devices_.clear();
    mags_.clear();
    spkTh_.clear();
    spkDevs_.clear();
    const bool endsInCab = netlistEndsInCab(n);
    blocks_.clear();
    bbds_.clear();
    tapes_.clear();
    logic_.clear();
    logicAt_.clear();
    retired_.clear();
    tapeParams_.clear();
    hasDelayCuts_ = false;
    acSrc_.clear();
    phase_ = 0;
    slots_.clear();
    speakers_.clear();
    spkFs_ = 0;
    spkQ_ = 1.4;
    spkLe_ = 1.0;
    groups_.clear();
    rt_.clear();
    lane_.reset(); // intra-unit lanes: planned again below (buildLanes)
    laneDiag_.clear();
    warnings_.clear();
    diag_ = {};
    // The control positions this compile starts from (CompileOptions::controls, else the defaults): they decide which
    // switch contacts are open (CompileOptions::openSwitches).
    sw_.clear();
    std::vector<double> compileCtl(n.controls.size(), 0.5);
    for (std::size_t c = 0; c < n.controls.size(); ++c)
        compileCtl[c] = std::clamp(c < opt.controls.size() && std::isfinite(opt.controls[c]) ? opt.controls[c] : n.controls[c].def, 0.0, 1.0);
    const bool openSwOff = std::getenv("CE_ROFFSW") != nullptr; // developer A/B: every open contact stamped as its roff
    std::map<std::string, int> idx;
    auto nodeId = [&](const std::string& name) {
        std::string l = name;
        for (auto& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (l == "0" || l == "gnd" || l == "ground") return 0;
        auto it = idx.find(name);
        if (it != idx.end()) return it->second;
        const int id = static_cast<int>(nodeNames_.size());
        nodeNames_.push_back(name);
        idx[name] = id;
        return id;
    };
    auto constSlot = [&](double v, int part) {
        ValueSlot s;
        s.kind = ValueSlot::Const;
        s.base = s.value = v;
        s.part = part;
        slots_.push_back(s);
        return static_cast<int>(slots_.size()) - 1;
    };
    auto partSlot = [&](ValueSlot::Kind k, int part) {
        ValueSlot s;
        s.kind = k;
        s.part = part;
        const auto& p = n.parts[static_cast<std::size_t>(part)];
        for (const std::string* c : {&p.control, &p.value.control})
            if (!c->empty()) s.controls.push_back(n.controlIndex(*c));
        if (k == ValueSlot::Law && !p.value.variable()) {
            s.kind = ValueSlot::Const;
            s.base = s.value = p.value.nominal;
        }
        slots_.push_back(s);
        return static_cast<int>(slots_.size()) - 1;
    };
    auto addPrim = [&](Prim::Kind k, int a, int b, int slot, int part, double fixed = 0, int c = 0, int d = 0) {
        Prim p;
        p.kind = k;
        p.a = a;
        p.b = b;
        p.c = c;
        p.d = d;
        p.slot = slot;
        p.part = part;
        p.fixed = fixed;
        prims_.push_back(p);
    };

    // Known nodes (DC sources + the input port).
    std::map<int, double> known;
    struct HumTube {
        int k, part;
        double rhk, chk, hkv;
    };
    std::vector<HumTube> humTubes; // CompileOptions::hum: heater-cathode coupling, stamped after the parts
    std::map<int, int> driven; // node -> prim
    inputNode_ = nodeId(n.io.input);
    if (inputNode_ == 0) {
        error = "the input port cannot be ground";
        return false;
    }
    int inputDrive = inputNode_;
    if (n.io.sourceImpedance > 0) {
        inputNode_ = nodeId(n.io.input + "#src");
        addPrim(Prim::G, inputNode_, inputDrive, constSlot(n.io.sourceImpedance, -1), -1);
    }
    known[inputNode_] = 0.0;
    // io.inputR: a second driven input (stereo circuits fed true stereo), the same kind of implicit source.
    inputNodeR_ = -1;
    vinR_ = 0.0;
    if (!n.io.inputR.empty()) {
        if (n.io.inputR == n.io.input) {
            error = "inputR must differ from the input node";
            return false;
        }
        inputNodeR_ = nodeId(n.io.inputR);
        if (inputNodeR_ == 0) {
            error = "the inputR port cannot be ground";
            return false;
        }
        if (n.io.sourceImpedance > 0) {
            const int drive = inputNodeR_;
            inputNodeR_ = nodeId(n.io.inputR + "#src");
            addPrim(Prim::G, inputNodeR_, drive, constSlot(n.io.sourceImpedance, -1), -1);
        }
        known[inputNodeR_] = 0.0;
    }

    // Antiparallel same-model diode pairs → one pair port.
    std::vector<char> skipPart(n.parts.size(), 0), asPair(n.parts.size(), 0);
    if (opt.mergeDiodePairs) {
        for (std::size_t i = 0; i < n.parts.size(); ++i) {
            const auto& a = n.parts[i];
            if (a.type != "D" || skipPart[i] || asPair[i]) continue;
            for (std::size_t j = i + 1; j < n.parts.size(); ++j) {
                const auto& b = n.parts[j];
                if (b.type != "D" || skipPart[j] || asPair[j] || b.model != a.model) continue;
                if (const auto zm = resolveModel(n, a.model); zm && zm->get("bv", 0.0) > 0) continue; // a zener pair is not symmetric
                if (b.nodes[0] == a.nodes[1] && b.nodes[1] == a.nodes[0]) {
                    asPair[i] = 1;
                    skipPart[j] = 1;
                    warnings_.push_back(a.ref + " + " + b.ref + " solved as one antiparallel pair (shared Rs)");
                    break;
                }
            }
        }
    }
    // NlKind::Zener's parameters p[0..5] from a model with bv (Devices.hpp zenerLaw)
    auto zenerParams = [&](const DeviceModel& m, const std::string& ref, double* q) {
        const double is = m.get("is", 1e-14), nn = m.get("n", 1.0), bv = m.get("bv", 0.0), ibv = m.get("ibv", 1e-3);
        const double nbv = m.get("nbv", 0.0) > 0 ? m.get("nbv", 0.0) : nn; // ngspice: NBV defaults to N
        // ngspice's BV adjustment (diotemp.c): the breakdown knee placed so the current at −BV is IBV, at its own
        // thermal voltage and the deck's RELTOL 1e-5, so the export is the plain .model with BV / IBV / NBV
        constexpr double vts = 0.02586491704;
        double xbv = bv;
        if (ibv >= is * bv / vts) {
            xbv = bv - nbv * vts * std::log(1.0 + ibv / is);
            for (int it = 0; it < 25; ++it) {
                xbv = bv - nbv * vts * std::log(ibv / is + 1.0 - xbv / vts);
                const double xc = is * (std::exp((bv - xbv) / (nbv * vts)) - 1.0 + xbv / vts);
                if (std::abs(xc - ibv) <= 1e-5 * ibv) break;
            }
        }
        q[0] = is;
        q[1] = nn * kVt;
        q[2] = 1.0 / q[1];
        if (xbv < 3.0 * nn * kVt)
            warnings_.push_back(ref + ": breakdown knee " + std::to_string(xbv) + " V is inside the forward region (lower nbv or raise rs): "
                                      "the law steps at −3·n·Vt (as ngspice's)");
        q[3] = xbv;                           // the breakdown exponent's knee (ngspice DIOtBrkdwnV)
        q[4] = 1.0 / (nbv * vts);
        q[5] = std::max(xbv, 3.0 * nn * kVt); // where breakdown takes over: ngspice's forward region wins above −3·n·Vt
    };
    // Back-to-back zener pairs (docs §10.29): two zener diodes joined anode to anode or cathode to cathode at a node that
    // nothing else touches (no part, port, probe, cut or readout) become one two-terminal port (NlKind::ZenerPair) with
    // the midpoint eliminated: series elements commute, so the pair's series resistances go into one resistor at the
    // first outer end. The pair is the two junctions exactly; the midpoint node is not simulated (it reads 0 V).
    // zPair[i] = j: part i carries the pair with part j (skipped). CE_NOZPAIR=1: two zener ports, as before.
    std::vector<int> zPair(n.parts.size(), -1);
    if (!std::getenv("CE_NOZPAIR")) {
        auto isZener = [&](std::size_t i) {
            const auto& q = n.parts[i];
            if (q.type != "D" || skipPart[i] || asPair[i] || q.nodes.size() != 2 || q.nodes[0] == q.nodes[1]) return false;
            const auto m = resolveModel(n, q.model);
            return m && m->kind != "vacdiode" && m->get("bv", 0.0) > 0;
        };
        std::set<std::string> held{n.io.input, n.io.output, n.io.inputR, n.io.outputR};
        for (const auto& x : n.cuts) held.insert(x);
        for (const auto& x : n.delayCuts) held.insert(x);
        for (const auto& x : n.slowNodes) held.insert(x);
        for (const auto& [x, refs] : n.cutLoads) held.insert(x);
        for (const auto& x : n.probes) held.insert(x);
        for (const auto& x : opt.probes) held.insert(x);
        std::map<std::string, std::vector<std::size_t>> at; // node name -> parts touching it
        for (std::size_t i = 0; i < n.parts.size(); ++i)
            for (const auto& nm : std::set<std::string>(n.parts[i].nodes.begin(), n.parts[i].nodes.end())) at[nm].push_back(i);
        for (const auto& [nm, ps] : at) {
            std::string l = nm;
            for (auto& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ps.size() != 2 || held.count(nm) || l == "0" || l == "gnd" || l == "ground" || !isZener(ps[0]) || !isZener(ps[1])) continue;
            const auto& a = n.parts[ps[0]];
            const auto& b = n.parts[ps[1]];
            const int ia = a.nodes[0] == nm ? 0 : 1, ib = b.nodes[0] == nm ? 0 : 1;
            if (ia != ib || a.nodes[1 - ia] == b.nodes[1 - ib] || zPair[ps[0]] >= 0 || zPair[ps[1]] >= 0 || skipPart[ps[0]] || skipPart[ps[1]]) continue; // in series the same way: no clamp
            zPair[ps[0]] = static_cast<int>(ps[1]);
            skipPart[ps[1]] = 1;
        }
    }
    // Series diode strings (docs §10.30): m identical plain junction diodes (one model, no breakdown) joined cathode to
    // anode at nodes that nothing else touches (no part, port, probe, cut or readout) carry one current, and a strictly
    // monotonic law then puts 1/m of the string's voltage on each junction. One port with n·m, Rs·m, gmin/m and 1/m of
    // the shot-noise variance is the same circuit exactly, without the midpoints: those islands are held by junctions
    // alone, so Newton has to converge them on their leakage currents (the JC-120's D4/D5 reverb-driver bias string:
    // 3.9 iterations a sample, 2.2 folded). The midpoints are not simulated. dStr[i] = m on the string's first diode
    // (anode end), dStrEnd[i] its cathode end; the others are skipped. CE_NOSERIESD=1: one port per diode, as before.
    std::vector<int> dStr(n.parts.size(), 0);
    std::vector<std::string> dStrEnd(n.parts.size());
    if (!std::getenv("CE_NOSERIESD")) {
        auto plainD = [&](std::size_t i) {
            const auto& q = n.parts[i];
            if (q.type != "D" || skipPart[i] || asPair[i] || zPair[i] >= 0 || q.nodes.size() != 2 || q.nodes[0] == q.nodes[1]) return false;
            const auto m = resolveModel(n, q.model);
            return m && m->kind != "vacdiode" && !(m->get("bv", 0.0) > 0);
        };
        std::set<std::string> held{n.io.input, n.io.output, n.io.inputR, n.io.outputR};
        for (const auto& x : n.cuts) held.insert(x);
        for (const auto& x : n.delayCuts) held.insert(x);
        for (const auto& x : n.slowNodes) held.insert(x);
        for (const auto& [x, refs] : n.cutLoads) held.insert(x);
        for (const auto& x : n.probes) held.insert(x);
        for (const auto& x : opt.probes) held.insert(x); // (the unit's face readouts come in as probes)
        std::map<std::string, std::vector<std::size_t>> at; // node name -> parts touching it
        for (std::size_t i = 0; i < n.parts.size(); ++i)
            for (const auto& nm : std::set<std::string>(n.parts[i].nodes.begin(), n.parts[i].nodes.end())) at[nm].push_back(i);
        std::vector<int> next(n.parts.size(), -1), prev(n.parts.size(), -1); // along the string, anode to cathode
        for (const auto& [nm, ps] : at) {
            std::string l = nm;
            for (auto& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ps.size() != 2 || held.count(nm) || l == "0" || l == "gnd" || l == "ground" || !plainD(ps[0]) || !plainD(ps[1])) continue;
            const auto& a = n.parts[ps[0]];
            const auto& b = n.parts[ps[1]];
            if (a.model != b.model) continue;
            const std::size_t up = a.nodes[1] == nm && b.nodes[0] == nm ? ps[0] : b.nodes[1] == nm && a.nodes[0] == nm ? ps[1] : n.parts.size();
            if (up == n.parts.size()) continue; // joined anode to anode or cathode to cathode: not a string
            const std::size_t dn = up == ps[0] ? ps[1] : ps[0];
            next[up] = static_cast<int>(dn);
            prev[dn] = static_cast<int>(up);
        }
        for (std::size_t i = 0; i < n.parts.size(); ++i) {
            if (next[i] < 0 || prev[i] >= 0) continue; // strings start at a diode with no predecessor (a ring has none)
            std::vector<std::size_t> s{i};
            while (next[s.back()] >= 0 && s.size() <= n.parts.size()) s.push_back(static_cast<std::size_t>(next[s.back()]));
            if (n.parts[s.front()].nodes[0] == n.parts[s.back()].nodes[1]) continue; // a shorted string carries nothing
            // A string ending on a cut node is left alone (2026-11-13): with both ends on supply cuts (the OR120's
            // stacked rectifier, hra/raw) the folded port joins two known voltages and the cut bookkeeping that charged
            // the second diode's current to `raw` no longer sees it (or120_* gate 12-740 % NRMSE). Not exact there.
            {
                std::set<std::string> cutNodes(n.cuts.begin(), n.cuts.end());
                for (const auto& x : n.delayCuts) cutNodes.insert(x);
                for (const auto& [x, refs] : n.cutLoads) cutNodes.insert(x);
                if (cutNodes.count(n.parts[s.front()].nodes[0]) || cutNodes.count(n.parts[s.back()].nodes[1])) continue;
            }
            dStr[i] = static_cast<int>(s.size());
            dStrEnd[i] = n.parts[s.back()].nodes[1];
            std::string refs = n.parts[i].ref;
            for (std::size_t k = 1; k < s.size(); ++k) {
                skipPart[s[k]] = 1;
                refs += " + " + n.parts[s[k]].ref;
            }
            warnings_.push_back(refs + " solved as one series diode string (" + std::to_string(s.size()) + " junctions; midpoints eliminated)");
        }
    }
    std::map<int, double> lightTau;                     // light node -> τ_light (optical emitters)
    std::vector<std::pair<int, std::string>> lightReaders; // cells: light node, part ref
    for (std::size_t pi = 0; pi < n.parts.size(); ++pi) {
        const Part& p = n.parts[pi];
        const int part = static_cast<int>(pi);
        if (skipPart[pi]) continue;
        if (zPair[pi] >= 0) { // a back-to-back zener pair (see above): one port between the two outer ends
            const Part& q = n.parts[static_cast<std::size_t>(zPair[pi])];
            const auto ma = *resolveModel(n, p.model), mb = *resolveModel(n, q.model);
            const bool anodes = p.nodes[0] == q.nodes[0]; // joined at their anodes (the outer ends differ: detection)
            // first = the junction whose voltage u is solved for: anode-joined, p's (cathode at the port's + end);
            // cathode-joined, q's (Devices.hpp ZenerPair: f(u) = I1(u) + I2(u + v), i = I2(u + v) into the + end)
            const int plus = nodeId(anodes ? p.nodes[1] : p.nodes[0]), minus = nodeId(anodes ? q.nodes[1] : q.nodes[0]);
            int from = plus;
            const double rs = ma.get("rs", 0.0) + mb.get("rs", 0.0);
            if (rs > 0) {
                from = nodeId(p.ref + "#j");
                addPrim(Prim::G, plus, from, constSlot(rs, part), part);
            }
            NlDevice d;
            d.kind = NlKind::ZenerPair;
            d.part = part;
            d.nports = 1;
            d.port[0] = {from, minus, from, minus, 0};
            zenerParams(anodes ? ma : mb, anodes ? p.ref : q.ref, d.p);
            zenerParams(anodes ? mb : ma, anodes ? q.ref : p.ref, d.p + 6);
            devices_.push_back(d);
            warnings_.push_back(p.ref + " + " + q.ref + " solved as one back-to-back zener pair (midpoint " + (anodes ? p.nodes[0] : p.nodes[1]) + " eliminated)");
            continue;
        }
        std::vector<int> nd;
        for (std::size_t k = 0; k < p.nodes.size(); ++k) nd.push_back(nodeId(k == 1 && dStr[pi] > 1 ? dStrEnd[pi] : p.nodes[k])); // a string: its cathode end
        auto model = [&]() { return *resolveModel(n, p.model); };
        if (p.type == "V") {
            int node = nd[0];
            double v = p.value.nominal;
            if (nd[1] != 0 && nd[0] == 0) {
                node = nd[1];
                v = -v;
            } else if (nd[1] != 0) {
                error = "part " + p.ref + ": voltage sources must have one terminal on ground (v1)";
                return false;
            }
            if (node == 0) {
                error = "part " + p.ref + ": voltage source shorted to ground";
                return false;
            }
            if (known.count(node)) {
                error = "part " + p.ref + ": node " + p.nodes[0] + " already driven by another source";
                return false;
            }
            known[node] = v;
            const int sctl = p.scaleControl.empty() ? -1 : n.controlIndex(p.scaleControl);
            if (p.acAmp != 0 || sctl >= 0) {
                AcSource a{node, v, (nd[1] != 0 && nd[0] == 0 ? -1.0 : 1.0) * p.acAmp, 2.0 * M_PI * p.acFreq, p.acPhase * M_PI / 180.0};
                a.ctl = sctl;
                a.smin = p.scaleMin;
                a.smax = p.scaleMax;
                acSrc_.push_back(a);
            }
        } else if (p.type == "R") {
            addPrim(Prim::G, nd[0], nd[1], partSlot(ValueSlot::Law, part), part);
        } else if (p.type == "C") {
            addPrim(Prim::Cap, nd[0], nd[1], partSlot(ValueSlot::Law, part), part);
        } else if (p.type == "L") {
            addPrim(Prim::Ind, nd[0], nd[1], partSlot(ValueSlot::Law, part), part);
        } else if (p.type == "pot") {
            if (nd[0] != nd[1]) addPrim(Prim::G, nd[0], nd[1], partSlot(ValueSlot::PotLower, part), part);
            if (nd[1] != nd[2]) addPrim(Prim::G, nd[1], nd[2], partSlot(ValueSlot::PotUpper, part), part);
        } else if (p.type == "switch") {
            addPrim(Prim::G, nd[0], nd[1], partSlot(ValueSlot::Switch, part), part);
            // A contact on a switch or selector control (no glide: it moves only when the control is set) may leave the
            // partition while open (CompileOptions::openSwitches; decided below, once every node's kind is known). A
            // contact on a continuous control (a pot's switch) keeps its roff: a glide can close it at any sample.
            const ControlDef* cd = n.control(p.control);
            if (opt.openSwitches && !openSwOff && cd && cd->kind != "continuous" && nd[0] != nd[1])
                sw_.push_back({part, static_cast<int>(prims_.size()) - 1, switchClosed(n, p, compileCtl), false});
        } else if (p.type == "vcvs") {
            if (nd[1] != 0) {
                error = "part " + p.ref + ": vcvs output must be referenced to ground (v1)";
                return false;
            }
            addPrim(Prim::Vcvs, nd[0], 0, -1, part, p.gain, nd[2], nd[3]);
        } else if (p.type == "vccs") {
            addPrim(Prim::Vccs, nd[0], nd[1], -1, part, p.gain, nd[2], nd[3]);
        } else if ((p.type == "D" || p.type == "Dpair") && model().kind == "vacdiode") {
            const auto m = model();
            NlDevice d;
            d.kind = NlKind::VacDiode;
            d.part = part;
            d.nports = 1;
            d.port[0] = {nd[0], nd[1], nd[0], nd[1], 0};
            d.p[0] = m.get("k", 3.6e-3);
            d.p[1] = m.get("knee", 0.5);
            devices_.push_back(d);
        } else if ((p.type == "D" || p.type == "Dpair") && model().get("bv", 0.0) > 0) {
            // Zener / avalanche breakdown (ngspice level-1 diode BV / IBV / NBV; Devices.hpp NlKind::Zener)
            const auto m = model();
            if (p.type == "Dpair") {
                error = "part " + p.ref + ": a Dpair cannot carry breakdown (bv); use two D parts";
                return false;
            }
            const double rs = m.get("rs", 0.0);
            int anode = nd[0];
            if (rs > 0) {
                const int j = nodeId(p.ref + "#j");
                addPrim(Prim::G, nd[0], j, constSlot(rs, part), part);
                anode = j;
            }
            NlDevice d;
            d.kind = NlKind::Zener;
            d.part = part;
            d.nports = 1;
            d.port[0] = {anode, nd[1], anode, nd[1], 0};
            zenerParams(m, p.ref, d.p);
            devices_.push_back(d);
        } else if (p.type == "D" || p.type == "Dpair") {
            const auto m = model();
            const int ms = std::max(dStr[pi], 1); // a series string (see above): m junctions, nd[1] its cathode end
            const double rs = m.get("rs", 0.0) * ms;
            int anode = nd[0];
            if (rs > 0) {
                const int j = nodeId(p.ref + "#j");
                addPrim(Prim::G, nd[0], j, constSlot(rs, part), part);
                anode = j;
            }
            NlDevice d;
            d.kind = (p.type == "D" && !asPair[pi]) ? NlKind::Diode : NlKind::DiodePair;
            d.part = part;
            d.nports = 1;
            d.port[0] = {anode, nd[1], anode, nd[1], 0};
            d.p[0] = m.get("is", 1e-14);
            d.p[1] = m.get("n", 1.0) * kVt * ms;
            d.p[2] = 1.0 / d.p[1];
            d.p[3] = 1.0 - 1.0 / ms; // Devices.hpp: gmin·(1 − p[3]), the string's gmin/m
            devices_.push_back(d);
        } else if (p.type == "npn" || p.type == "pnp") {
            const auto m = model();
            NlDevice d;
            d.kind = NlKind::Bjt;
            d.part = part;
            d.nports = 2;
            d.pnp = p.type == "pnp";
            const int c = nd[0], e = nd[2];
            int b = nd[1];
            if (m.get("rb", 0) > 0) { // base spreading resistance: internal base node
                const int bi = nodeId(p.ref + "#b");
                addPrim(Prim::G, b, bi, constSlot(m.get("rb", 0), part), part);
                b = bi;
            }
            d.port[0] = {b, e, b, e, 0};
            d.port[1] = {b, c, b, c, 0};
            d.p[0] = m.get("is", 1e-14);
            d.p[1] = m.get("bf", 100);
            d.p[2] = m.get("br", 1);
            d.p[3] = 1.0 / d.p[1];
            d.p[4] = 1.0 / d.p[2];
            d.p[5] = d.p[0] / kVt;
            // Gummel-Poon extensions (0 = off): ISE/NE, ISC/NC non-ideal base currents, VAF Early voltage
            d.p[6] = m.get("ise", 0.0);
            d.p[7] = 1.0 / (m.get("ne", 1.5) * kVt);
            d.p[8] = m.get("isc", 0.0);
            d.p[9] = 1.0 / (m.get("nc", 2.0) * kVt);
            d.p[10] = m.get("vaf", 0.0) > 0 ? 1.0 / m.get("vaf", 0.0) : 0.0;
            d.p[11] = m.get("icbo", 0.0);
            d.p[12] = std::max(m.get("vleak", 0.2), 1e-3);
            devices_.push_back(d);
            // junction capacitances as linear caps at their zero-bias values (optional)
            if (m.get("cje", 0) > 0) addPrim(Prim::Cap, b, e, constSlot(m.get("cje", 0), part), part);
            if (m.get("cjc", 0) > 0) addPrim(Prim::Cap, b, c, constSlot(m.get("cjc", 0), part), part);
        } else if (p.type == "njf" || p.type == "pjf") {
            // SPICE JFET level 1 (Devices.hpp NlKind::Jfet). nodes: drain, gate, source
            const auto m = model();
            NlDevice d;
            d.kind = NlKind::Jfet;
            d.part = part;
            d.nports = 2;
            d.pnp = p.type == "pjf";
            const int dr = nd[0], g = nd[1], so = nd[2];
            d.port[0] = {g, so, g, so, 0};
            d.port[1] = {dr, so, dr, so, 0};
            d.p[0] = m.get("vto", -2.0);
            d.p[1] = m.get("beta", 1e-4);
            d.p[2] = m.get("lambda", 0.0);
            d.p[3] = m.get("is", 1e-14);
            d.p[4] = 1.0 / (m.get("n", 1.0) * kVt);
            d.p[5] = std::max(m.get("ksub", 0.0), 0.0); // subthreshold smoothing κ (0 = exact level 1)
            devices_.push_back(d);
            if (m.get("cgs", 0) > 0) addPrim(Prim::Cap, g, so, constSlot(m.get("cgs", 0), part), part);
            if (m.get("cgd", 0) > 0) addPrim(Prim::Cap, g, dr, constSlot(m.get("cgd", 0), part), part);
        } else if (p.type == "opamp") {
            const auto m = model();
            const int inn = nd[1], out = nd[2];
            // Input errors (ENGINE_QUEUE 31; datasheet VIO, IIB, IIO; all 0 by default, so the macro is unchanged).
            // vos: a series voltage at the + input, V(+') = V(+) + vos, an ideal source in front of the input stage
            // (the stage draws no current, so it is the same as an offset in the law). ib/ios: DC currents into the
            // pins, I(+) = ib + ios/2 and I(−) = ib − ios/2 (positive into the op-amp; a PNP input stage has ib < 0).
            // They make a stage without a DC feedback path (series-capacitor feedback, a clamp diode) determinate.
            const double vos = m.get("vos", 0.0), ib = m.get("ib", 0.0), ios = m.get("ios", 0.0);
            int inp = nd[0];
            if (ib + 0.5 * ios != 0.0 || ib - 0.5 * ios != 0.0) {
                const int one = nodeId(p.ref + "#ib1");
                known[one] = 1.0;
                if (ib + 0.5 * ios != 0.0 && nd[0] != 0) addPrim(Prim::Vccs, nd[0], 0, -1, part, ib + 0.5 * ios, one, 0);
                if (ib - 0.5 * ios != 0.0 && inn != 0) addPrim(Prim::Vccs, inn, 0, -1, part, ib - 0.5 * ios, one, 0);
            }
            if (vos != 0.0) {
                const int kv = nodeId(p.ref + "#kvos"), ipv = nodeId(p.ref + "#ip");
                known[kv] = -vos;
                addPrim(Prim::Vcvs, ipv, 0, -1, part, 1.0, nd[0], kv); // V(ip) = V(+) − (−vos)
                inp = ipv;
            }
            if (m.get("level", 1) < 0.5) {
                addPrim(Prim::Nullor, out, 0, -1, part, 1.0, inp, inn);
                continue;
            }
            double vhi = m.get("vhi", 13.5), vlo = m.get("vlo", -13.5);
            bool railNodes = false; // supply pins on circuit nodes (a charge pump, a filtered or diode-fed rail)
            if (nd.size() == 5) {
                auto vk = [&](int node) -> std::optional<double> {
                    if (node == 0) return 0.0;
                    auto it = known.find(node);
                    if (it == known.end()) return std::nullopt;
                    return it->second;
                };
                // Supply sources may be declared after the op-amp: resolve from the parts list directly.
                auto supply = [&](const std::string& name) -> std::optional<double> {
                    const int id = nodeId(name);
                    if (auto v = vk(id)) return v;
                    for (const auto& q : n.parts)
                        if (q.type == "V" && q.nodes[0] == name && (q.nodes[1] == "0" || q.nodes[1] == "gnd") && q.acAmp == 0) return q.value.nominal;
                    return std::nullopt;
                };
                auto vp = supply(p.nodes[3]), vn = supply(p.nodes[4]);
                if (vp && vn) {
                    vhi = *vp - m.get("swing_hi", 1.5);
                    vlo = *vn + m.get("swing_lo", 1.5);
                } else
                    railNodes = true;
            }
            const double c1 = m.get("cdom", 30e-12);
            const double gm = 2.0 * M_PI * m.get("gbw", 1e6) * c1;
            const double r1 = m.get("a0", 1e5) / gm;
            const double imax = m.get("slew", 1e6) * c1;
            const int x = nodeId(p.ref + "#x");
            NlDevice ota;
            ota.kind = NlKind::Ota;
            ota.part = part;
            ota.port[0] = {inp, inn, 0, x, 1e30};
            ota.p[0] = imax;
            ota.p[1] = gm / imax;
            devices_.push_back(ota);
            addPrim(Prim::G, x, 0, constSlot(r1, part), part);
            addPrim(Prim::Cap, x, 0, constSlot(c1, part), part);
            prims_.back().be = m.get("be", 0.0) > 0.5; // optional L-stable compensation node (model param "be")
            if (!railNodes) {
                NlDevice cl;
                cl.kind = NlKind::RailClamp;
                cl.part = part;
                cl.port[0] = {x, 0, x, 0, 0};
                cl.p[0] = imax * 1e-3;
                cl.p[1] = vhi;
                cl.p[2] = vlo;
                cl.p[3] = 0.05;
                devices_.push_back(cl);
                if (m.get("ilim", 0.0) > 0) {
                    // Output current limit (model "ilim", with the open-loop output resistance "rout"): the ideal output
                    // source drives an internal node, and the pin is fed through i = ilim·tanh(v/(ilim·rout)). The pin
                    // is no longer an ideal output, so an exact linear split (1/rout, subtracted from the law) keeps a
                    // pin loaded only by capacitance or sensing regular.
                    const double il = m.get("ilim", 0.0), ro = std::max(m.get("rout", 100.0), 1e-3);
                    const int xb = nodeId(p.ref + "#xb");
                    addPrim(Prim::Vcvs, xb, 0, -1, part, 1.0, x, 0);
                    NlDevice lim;
                    lim.kind = NlKind::OutLim;
                    lim.part = part;
                    lim.port[0] = {xb, out, xb, out, 0};
                    lim.p[0] = il;
                    lim.p[1] = 1.0 / (il * ro);
                    addPrim(Prim::G, xb, out, -1, part, ro);
                    lim.gShift[0] = 1.0 / ro;
                    lim.hasShift = true;
                    devices_.push_back(lim);
                } else {
                    addPrim(Prim::Vcvs, out, 0, -1, part, 1.0, x, 0);
                }
            } else {
                if (m.get("ilim", 0.0) > 0) {
                    error = "part " + p.ref + ": ilim needs fixed supply rails (supply pins on circuit nodes are not supported yet)";
                    return false;
                }
                // Supply pins on circuit nodes. The internal swing node is clamped relative to the actual rails (two
                // one-sided clamps sensing x − V+ and x − V−: fixed bands in their port voltage, so the lazy-clamp
                // kernel still applies). The output stays an ideal source (so stages still partition exactly); its
                // load current is drawn from the rails one sample late (Circuit::railDraws), and the rail pins must
                // be cut nodes (checked below).
                const int vpn = nd[3], vnn = nd[4];
                for (int side = 0; side < 2; ++side) {
                    NlDevice cl;
                    cl.kind = NlKind::RailClamp;
                    cl.part = part;
                    cl.port[0] = {x, side == 0 ? vpn : vnn, x, 0, 0};
                    cl.p[0] = imax * 1e-3;
                    cl.p[1] = side == 0 ? -m.get("swing_hi", 1.5) : 1e9;
                    cl.p[2] = side == 0 ? -1e9 : m.get("swing_lo", 1.5);
                    cl.p[3] = 0.05;
                    devices_.push_back(cl);
                }
                addPrim(Prim::Vcvs, out, 0, -1, part, 1.0, x, 0);
                railAmps_.push_back({out, vpn, vnn, std::max(m.get("iq", 1e-3), 0.0)});
            }
        } else if (p.type == "spring") {
            // nodes: input coil (a, b), output (out, ref = ground). Input coil = Rin + Lin; output = source + Rout.
            const auto m = model();
            if (nd[3] != 0) {
                error = "part " + p.ref + ": the spring tank output must be referenced to ground";
                return false;
            }
            const int c = nodeId(p.ref + "#coil"), src = nodeId(p.ref + "#src");
            addPrim(Prim::G, nd[0], c, constSlot(m.get("rin", 8), part), part);
            addPrim(Prim::Ind, c, nd[1], constSlot(m.get("lin", 1e-3), part), part);
            addPrim(Prim::G, src, nd[2], constSlot(m.get("rout", 2575), part), part);
            known[src] = 0.0;
            SpringTank::Params sp;
            sp.delay1 = m.get("delay1", 0.0334);
            sp.delay2 = m.get("delay2", 0.0392);
            sp.t60 = m.get("t60", 2.9);
            sp.disp = m.get("disp", 0.62);
            sp.stages = static_cast<int>(m.get("stages", 36));
            sp.lp = m.get("lp", 4300);
            sp.hp = m.get("hp", 90);
            sp.gain = m.get("gain", 0.08);
            sp.delay3 = m.get("delay3", 0.0);
            BlockRt b{nd[0], c, src, m.get("rin", 8), std::make_shared<SpringTank>(), 0.0,
                      {sp.delay1, sp.delay2, sp.t60, sp.disp, static_cast<double>(sp.stages), sp.lp, sp.hp, sp.gain, sp.delay3}};
            b.tank->prepare(opt.sampleRate, sp);
            blocks_.push_back(b);
        } else if (p.type == "ldr") {
            // Neon lamp (la–lb) → light node (1 F, release R = τ) → photoresistor (a–b) with G = 1/Rdark + λ^γ/Ron.
            const auto m = model();
            const int a = nd[0], b = nd[1], la = nd[2], lb = nd[3];
            const int light = nodeId(p.ref + "#light");
            NlDevice ne;
            ne.kind = NlKind::Neon;
            ne.part = part;
            ne.port[0] = {la, lb, la, lb, 0};
            ne.p[0] = m.get("vstrike", 65);
            ne.p[1] = m.get("rlamp", 30e3);
            ne.p[2] = m.get("smooth", 2);
            devices_.push_back(ne);
            NlDevice ld = ne;
            ld.kind = NlKind::LightDrive;
            ld.port[0] = {la, lb, 0, light, 0};
            ld.p[3] = m.get("drive", 150);
            devices_.push_back(ld);
            addPrim(Prim::Cap, light, 0, constSlot(1.0, part), part);
            addPrim(Prim::G, light, 0, constSlot(m.get("release", 0.08), part), part);
            NlDevice d;
            d.kind = NlKind::Ldr;
            d.part = part;
            d.nports = 2;
            d.port[0] = {a, b, a, b, 0};
            d.port[1] = {light, 0, 0, 0, 0};
            d.p[0] = m.get("rdark", 10e6);
            d.p[1] = m.get("ron", 8e3);
            d.p[2] = m.get("gamma", 0.9);
            devices_.push_back(d);
        } else if (p.type == "ota") {
            // Real OTA (DEVICES.md §1.3). nodes: out, inp, inn, abc, V−, V+. The abc pin is a junction to V− (one or two
            // in series: n·junctions); the output is two OtaHalf devices (sourcing from V+, sinking into V−) whose
            // difference is IABC·tanh(vd/2Vt), with the mirror's IABC taken from that same junction law.
            const auto m = model();
            const int out = nd[0], inp = nd[1], inn = nd[2], abc = nd[3], vm = nd[4], vp = nd[5];
            const double nvt = m.get("n", 1.0) * std::max(1.0, m.get("junctions", 1.0)) * kVt, is = m.get("is", 5.9e-16);
            NlDevice j;
            j.kind = NlKind::Diode;
            j.part = part;
            j.nports = 1;
            j.port[0] = {abc, vm, abc, vm, 0};
            j.p[0] = is;
            j.p[1] = nvt;
            j.p[2] = 1.0 / nvt;
            devices_.push_back(j);
            const double bf = m.get("bf", 0.0), va = m.get("va", 0.0);
            for (int half = 0; half < 2; ++half) {
                NlDevice h;
                h.kind = NlKind::OtaHalf;
                h.part = part;
                h.nports = 3;
                const int base = half == 0 ? inp : inn;
                h.port[0] = {inp, inn, bf > 0 ? base : 0, bf > 0 ? vm : 0, 0};
                h.port[1] = {abc, vm, 0, 0, 0};
                h.port[2] = half == 0 ? NlPort{vp, out, vp, out, 0} : NlPort{out, vm, out, vm, 0};
                h.p[0] = is * m.get("mirror", 1.0);
                h.p[1] = 1.0 / nvt;
                h.p[2] = half == 0 ? 1.0 : -1.0;
                h.p[3] = 1.0 / kVt;
                h.p[4] = half == 0 ? m.get("usat_hi", 1.5) : m.get("usat_lo", 0.6);
                h.p[5] = 1.0 / std::max(m.get("w", 0.1), 1e-3);
                h.p[6] = va > 0 ? 1.0 / va : 0.0;
                h.p[7] = bf > 0 ? 1.0 / bf : 0.0;
                devices_.push_back(h);
            }
        } else if (p.type == "bbd") {
            // Bucket-brigade delay (Blocks.hpp BbdLine). nodes: in (sensed, no current), out1, out2 (each a source
            // behind rout), clock control (sensed). The clock law maps the control voltage to f_clk.
            const auto m = model();
            const int in = nd[0], o1 = nd[1], o2 = nd[2], ctl = nd[3];
            if (in == 0 || o1 == 0) {
                error = "part " + p.ref + ": a bbd needs a signal input and a first output (not ground)";
                return false;
            }
            const double rout = std::max(m.get("rout", 2e3), 1.0);
            const int s1 = nodeId(p.ref + "#src1");
            addPrim(Prim::G, s1, o1, constSlot(rout, part), part);
            known[s1] = 0.0;
            int s2 = -1;
            if (o2 != 0 && o2 != o1) {
                s2 = nodeId(p.ref + "#src2");
                addPrim(Prim::G, s2, o2, constSlot(rout, part), part);
                known[s2] = 0.0;
            }
            if (m.get("cin", 0) > 0) addPrim(Prim::Cap, in, 0, constSlot(m.get("cin", 0), part), part);
            BbdLine::Params bp;
            bp.stages = std::max(2, static_cast<int>(std::lround(m.get("stages", 1024))));
            bp.eps = m.get("eps", 0.1);
            bp.a2 = m.get("a2", 0.0);
            bp.vlo = m.get("vlo", -1e9);
            bp.vhi = m.get("vhi", 1e9);
            bp.vshift = m.get("vshift", 0.0);
            bp.noise = opt.bbdNoise ? m.get("noise", 0.0) : 0.0;
            bp.seed = static_cast<std::uint32_t>((fnv(1469598103934665603ull, p.ref) ^ opt.noiseSeed ^ (opt.noiseSeed >> 32)) & 0xffffffffu) | 1u; // + the unit's stream
            BbdLine::ControlRows rows;
            for (const auto& r : p.clockRows) rows.emplace_back(r.at, r.points);
            auto line = std::make_shared<BbdLine>();
            line->prepare(opt.sampleRate, bp, p.clock, p.clockHz, std::move(rows));
            bbds_.push_back({in, ctl, p.clockControl.empty() ? -1 : net_.controlIndex(p.clockControl), s1, s2, line});
        } else if (p.type == "tape") {
            // Tape loop (Blocks.hpp TapeLoop). nodes: record head a, b (R + L, current sensed), playback heads 1..3 (each a
            // source behind R + L to its node; "0" = no head). part "control" = the Repeat Rate (speed) control.
            const auto m = model();
            const double rrec = std::max(m.get("rrec", 100), 1e-3);
            const int rx = nodeId(p.ref + "#rx");
            addPrim(Prim::G, nd[0], rx, constSlot(rrec, part), part);
            addPrim(Prim::Ind, rx, nd[1], constSlot(std::max(m.get("lrec", 10e-3), 1e-9), part), part);
            TapeRt t{nd[0], rx, rrec, p.control.empty() ? -1 : n.controlIndex(p.control), {-1, -1, -1}, std::make_shared<TapeLoop>()};
            TapeLoop::Params tp;
            tp.heads = 0;
            for (int h = 0; h < 3; ++h) {
                tp.delays[h] = m.get("d" + std::to_string(h + 1), tp.delays[h]);
                if (nd[2 + h] == 0) continue;
                tp.heads = h + 1;
                const int src = nodeId(p.ref + "#src" + std::to_string(h + 1)), lx = nodeId(p.ref + "#pl" + std::to_string(h + 1));
                addPrim(Prim::G, src, lx, constSlot(std::max(m.get("rpb", 300), 1e-3), part), part);
                addPrim(Prim::Ind, lx, nd[2 + h], constSlot(std::max(m.get("lpb", 0.1), 1e-9), part), part);
                known[src] = 0.0;
                t.src[h] = src;
            }
            tp.range = m.get("range", 2.57);
            tp.ms = m.get("ms", 3.5e5);
            tp.a = m.get("a", 22e3);
            tp.alpha = m.get("alpha", 1.6e-3);
            tp.drive = m.get("drive", 4e8);
            tp.gain = m.get("gain", 5e-7);
            tp.v0 = m.get("v0", 0.19);
            tp.gap = m.get("gap", 5.0) * 1e-6;
            tp.spacing = m.get("spacing", 3.0) * 1e-6;
            tp.thickness = m.get("thickness", 10.0) * 1e-6;
            tp.wow = m.get("wow", 1e-3);
            tp.flutter = m.get("flutter", 5e-4);
            tp.servo = m.get("servo", 0.25);
            tp.seed = static_cast<std::uint32_t>(fnv(1469598103934665603ull, p.ref) & 0xffffffffu) | 1u;
            t.loop->prepare(opt.sampleRate, tp);
            tapes_.push_back(t);
            tapeParams_.push_back(tp);
        } else if (p.type == "absi") {
            // NE570 rectifier law (lib:ne570): senses nodes[0] − nodes[1], injects into nodes[2] (from nodes[3]).
            const auto m = model();
            NlDevice d;
            d.kind = NlKind::AbsI;
            d.part = part;
            d.nports = 1;
            d.port[0] = {nd[0], nd[1], nd[3], nd[2], 0};
            d.p[0] = 1.0 / m.get("r1", 10e3);
            d.p[1] = std::max(m.get("delta", 1e-3), 1e-9);
            devices_.push_back(d);
        } else if (p.type == "gaincell") {
            // NE570 ΔG cell (lib:ne570): nodes gin, vref, r5a, r5b, inv, from.
            const auto m = model();
            NlDevice d;
            d.kind = NlKind::GainCell;
            d.part = part;
            d.nports = 3;
            d.port[0] = {nd[0], nd[1], 0, 0, 0};
            d.port[1] = {nd[2], nd[3], 0, 0, 0};
            d.port[2] = {nd[4], 0, nd[5], nd[4], 0};
            d.p[0] = 2.0 / m.get("r5", 10e3);
            d.p[1] = 1.0 / m.get("r2", 20e3);
            d.p[2] = 1.0 / m.get("i1", 140e-6);
            devices_.push_back(d);
        } else if (p.type == "vca") {
            // Blackmer log/antilog VCA (THAT 2180 Fig. 5; dbx 202; docs §"vca and rms"). nodes (2180 SIP order): in, ec+,
            // ec-, sym, v-, gnd, v+, out. The internal amplifier holds IN at GND by driving the log node #x (a nullor);
            // the bias generator holds #v3 = #x − Vb. Four two-junction paths (transistor + series diode, β → ∞), each a
            // Shockley law in its own junction voltage with an ohmic series resistance:
            //   Q1/D1 (pnp, input):  #x → in,   base ec+      Q2/D2 (pnp, output): #x → out,  base ec-
            //   Q3/D3 (npn, input):  in → #v3,  base ec-      Q4/D4 (npn, output): out → #v3, base sym (25 Ω to ec+)
            // so I_out = g·I_in with g = exp((ec+ − ec-)/2VT), 2VT·ln10/20 = mvdb. Nonidealities: vsym (output-npn
            // offset: even harmonics, √g control feedthrough), rs (ohmic drop: odd harmonics growing with |g − 1|), rd
            // (output-path ohmic asymmetry: even harmonics growing with level), rc (control-port resistance to gnd),
            // rsym (sym-pin resistance to ec+), icc (quiescent supply current V+ → V−).
            const auto m = model();
            const int in = nd[0], ecp = nd[1], ecn = nd[2], sym = nd[3], vm = nd[4], gnd = nd[5], vp = nd[6], out = nd[7];
            if (in == 0 || out == 0 || in == out) {
                error = "part " + p.ref + ": a vca needs distinct, non-ground in and out pins";
                return false;
            }
            const double v2 = std::max(m.get("mvdb", 6.1), 1e-3) * 1e-3 * 20.0 / std::log(10.0); // 2·VT
            const double iq = std::max(m.get("iq", 20e-6), 1e-12), v0 = 1.3;                  // idle path current, drop
            const double rs = std::max(m.get("rs", 0.0), 0.0), rd = m.get("rd", 0.0);
            const double is = iq * std::exp(-v0 / v2);
            const int x = nodeId(p.ref + "#x"), kb = nodeId(p.ref + "#kb"), v3 = nodeId(p.ref + "#v3");
            addPrim(Prim::Nullor, x, 0, -1, part, 1.0, gnd, in);
            known[kb] = 2.0 * (v0 + iq * rs); // bias generator: every path idles at iq at 0 dB
            addPrim(Prim::Vcvs, v3, 0, -1, part, 1.0, x, kb);
            if (m.get("rsym", 25.0) > 0 && sym != ecp) addPrim(Prim::G, sym, ecp, constSlot(m.get("rsym", 25.0), part), part);
            if (m.get("rc", 0.0) > 0) {
                if (ecp != gnd) addPrim(Prim::G, ecp, gnd, constSlot(m.get("rc", 0.0), part), part);
                if (ecn != gnd) addPrim(Prim::G, ecn, gnd, constSlot(m.get("rc", 0.0), part), part);
            }
            struct Path {
                const char* tag;
                int top, base, coll;
                bool pnp;
                double r, is;
            };
            // vczero: the control offset at Ec− (gain = (Ec+ − Ec− − vczero)/mvdb; datasheet "gain at 0 V control"), applied
            // to the two Ec− bases as a saturation-current scale
            const double vcz = m.get("vczero", 0.0);
            const Path paths[4] = {{"#e1", x, ecp, in, true, rs, is},
                                   {"#e2", x, ecn, out, true, std::max(rs - 0.5 * rd, 0.0), is * std::exp(-vcz / v2)},
                                   {"#e3", v3, ecn, in, false, rs, is * std::exp(vcz / v2)},
                                   {"#e4", v3, sym, out, false, std::max(rs + 0.5 * rd, 0.0), is * std::exp(m.get("vsym", 0.0) / v2)}};
            const double bfc = m.get("bf", 0.0); // core β: the control pins are transistor bases and draw I/β (0 = ideal)
            for (const auto& q : paths) {
                int e = q.top;
                if (q.r > 0) {
                    e = nodeId(p.ref + q.tag);
                    addPrim(Prim::G, q.top, e, constSlot(q.r, part), part);
                }
                NlDevice d;
                d.kind = NlKind::DiodeLight; // a Shockley transfer port: senses the junction pair, injects elsewhere
                d.part = part;
                d.nports = 1;
                d.port[0] = q.pnp ? NlPort{e, q.base, e, q.coll, 0} : NlPort{q.base, e, q.coll, e, 0};
                d.p[0] = q.is;
                d.p[1] = v2;
                d.p[2] = 1.0 / v2;
                d.p[3] = 1.0;
                if (q.coll == in) {
                    // Exact linear split: the input paths are the internal amplifier's only feedback, so the linear
                    // matrix would leave #x undetermined. A transconductance g0 (the idle gm) is stamped as a VCCS on
                    // the same sense / injection terminals and subtracted from the law: the total current is unchanged.
                    const double g0 = iq / v2;
                    addPrim(Prim::Vccs, d.port[0].from, d.port[0].to, -1, part, g0, d.port[0].cp, d.port[0].cn);
                    d.gShift[0] = g0;
                    d.hasShift = true;
                }
                devices_.push_back(d);
                if (bfc > 0) { // base current: into an npn base (to its emitter), out of a pnp base (from its emitter)
                    NlDevice b = d;
                    b.hasShift = false;
                    b.gShift[0] = 0.0;
                    b.p[3] = 1.0 / bfc;
                    b.port[0] = q.pnp ? NlPort{e, q.base, e, q.base, 0} : NlPort{q.base, e, q.base, e, 0};
                    devices_.push_back(b);
                }
            }
            if (m.get("icc", 0.0) > 0 && vp != vm) {
                const int one = nodeId(p.ref + "#one");
                known[one] = 1.0;
                addPrim(Prim::Vccs, vp, vm, -1, part, m.get("icc", 0.0), one, 0);
            }
            if (m.get("isc", 0.0) > 0) {
                // Output compliance: the collector-base junctions of the output transistors (Q2 pnp: OUT → its base Ec−;
                // Q4 npn: its base SYM → OUT). At a virtual-ground OUT they carry nothing; pulled away (an output op-amp in
                // clipping) they saturate the transistor and hold OUT within a junction drop of the bases.
                for (int side = 0; side < 2; ++side) {
                    NlDevice j;
                    j.kind = NlKind::Diode;
                    j.part = part;
                    j.nports = 1;
                    j.port[0] = side == 0 ? NlPort{out, ecn, out, ecn, 0} : NlPort{sym, out, sym, out, 0};
                    j.p[0] = m.get("isc", 0.0);
                    j.p[1] = kVt;
                    j.p[2] = 1.0 / kVt;
                    devices_.push_back(j);
                }
            }
            if (m.get("v5", 0.0) > 0 && vm != 0 && vm != gnd) {
                // Pin 5 (V−): the bias network holds it about four junction drops below GND (2180: −2.85 V; dbx AN103:
                // "normally four diode drops below ground"), so the external resistor to VEE sets the bias / supply
                // current, drawn from V+ (r5: the pin's dynamic resistance). This is also what AN103's 10 kΩ "DC offset
                // nulling" resistor from the control port to pin 5 sees.
                const int k5 = nodeId(p.ref + "#k5"), n5 = nodeId(p.ref + "#v5");
                known[k5] = m.get("v5", 0.0);
                addPrim(Prim::Vcvs, n5, 0, -1, part, 1.0, gnd, k5);
                const double r5 = std::max(m.get("r5", 20.0), 1e-3);
                addPrim(Prim::G, n5, vm, constSlot(r5, part), part);
                if (vp != 0) addPrim(Prim::Vccs, vp, n5, -1, part, 1.0 / r5, n5, vm); // the same current, from V+
            }
        } else if (p.type == "dff" || p.type == "jkff" || p.type == "nand" || p.type == "inv") {
            // CMOS 4000B logic (Logic.hpp, Devices.hpp CmosOut / MosHalf; docs §10.14). nodes: dff [d, clk, set, reset, q, qb,
            // vdd, vss]; nand [a, b, y, vdd, vss]; inv [a, y, vdd, vss]. Inputs are high impedance with CIN to VSS.
            const auto m = model();
            const int nin = p.type == "jkff" ? 5 : p.type == "dff" ? 4 : (p.type == "nand" ? 2 : 1), nout = p.type == "dff" || p.type == "jkff" ? 2 : 1;
            const int vdd = nd[static_cast<std::size_t>(nin + nout)], vss = nd[static_cast<std::size_t>(nin + nout + 1)];
            if (vdd == vss) {
                error = "part " + p.ref + ": VDD and VSS must be different nodes";
                return false;
            }
            const double mos[5] = {m.get("beta", 1.215e-3), m.get("vt", 1.659), m.get("theta", 0.2016), m.get("lambda", 0.01527), std::max(m.get("kappa", 0.05), 1e-3)};
            if (m.get("cin", 0) > 0)
                for (int k = 0; k < nin; ++k)
                    if (nd[static_cast<std::size_t>(k)] != vss) addPrim(Prim::Cap, nd[static_cast<std::size_t>(k)], vss, constSlot(m.get("cin", 0), part), part);
            if (p.type == "inv" && m.get("ub", 0) > 0.5) {
                // unbuffered inverter: the complementary pair itself (the transfer characteristic is the channel law)
                const int a = nd[0], y = nd[1];
                for (int ch = 0; ch < 2; ++ch) {
                    NlDevice d;
                    d.kind = NlKind::MosHalf;
                    d.part = part;
                    d.nports = 2;
                    d.port[0] = ch == 0 ? NlPort{a, vss, 0, 0, 0} : NlPort{vdd, a, 0, 0, 0};
                    d.port[1] = ch == 0 ? NlPort{y, vss, y, vss, 0} : NlPort{vdd, y, vdd, y, 0};
                    for (int q = 0; q < 5; ++q) d.p[q] = mos[q];
                    d.p[0] = ch == 0 ? m.get("betan", mos[0]) : m.get("betap", mos[0]);
                    // exact linear split on the drain port (the channel's conductance scale), so an output pin loaded only by
                    // capacitance is regular at DC and Newton is well scaled; the law subtracts it again
                    const double gs = d.p[0] * 1.0;
                    addPrim(Prim::G, d.port[1].cp, d.port[1].cn, -1, part, 1.0 / gs);
                    d.gShift[1] = gs;
                    d.hasShift = true;
                    devices_.push_back(d);
                }
                continue;
            }
            LogicCell c;
            c.type = p.type == "jkff" ? LogicCell::Jkff : p.type == "dff" ? LogicCell::Dff : (p.type == "nand" ? LogicCell::Nand : LogicCell::Inv);
            c.part = part;
            c.nin = nin;
            c.nout = nout;
            for (int k = 0; k < nin; ++k) c.in[k] = nd[static_cast<std::size_t>(k)];
            c.vdd = vdd;
            c.vss = vss;
            c.vth = m.get("vth", 0.5);
            c.tpd[0] = std::max(m.get("tpd5", 125e-9), 1e-12);
            c.tpd[1] = std::max(m.get("tpd10", 60e-9), 1e-12);
            c.tpd[2] = std::max(m.get("tpd15", 45e-9), 1e-12);
            c.blep = m.get("blep", 1) > 0.5;
            c.q0 = m.get("q0", 0.0);
            for (int j = 0; j < nout; ++j) {
                const int pin = nd[static_cast<std::size_t>(nin + j)];
                const int lam = nodeId(p.ref + (j == 0 ? "#l" : "#lb"));
                known[lam] = 0.0;
                c.out[j].pin = pin;
                c.out[j].node = lam;
                NlDevice d;
                d.kind = NlKind::CmosOut;
                d.part = part;
                d.nports = 3;
                d.port[0] = {lam, 0, 0, 0, 0};
                d.port[1] = {vdd, pin, vdd, pin, 0};
                d.port[2] = {pin, vss, pin, vss, 0};
                for (int q = 0; q < 5; ++q) d.p[q] = mos[q];
                d.p[5] = std::max(2.0 * m.get("iddq", 5e-9), 1e-12);
                // exact linear split on the sink port (the on-conductance scale): see the unbuffered case
                const double gs = mos[0] * 1.0;
                addPrim(Prim::G, pin, vss, -1, part, 1.0 / gs);
                d.gShift[2] = gs;
                d.hasShift = true;
                devices_.push_back(d);
            }
            logic_.push_back(c);
        } else if (p.type == "ad536") {
            // AD536A rms-to-DC converter (datasheet Fig. 6 / Table 3; Devices.hpp NlKind::Ad536; docs §10.17). nodes, D-14 pin
            // order: VIN 1, NC 2, −VS 3, CAV 4, dB 5, BUF OUT 6, BUF IN 7, IOUT 8, RL 9, COM 10, NC 11, NC 12, NC 13, +VS 14.
            const auto m = model();
            const int vin = nd[0], vneg = nd[2], cav = nd[3], db = nd[4], bo = nd[5], bi = nd[6], iout = nd[7], rl = nd[8], com = nd[9], vpos = nd[13];
            if (vin == 0 || cav == 0 || vpos == 0 || cav == vpos) {
                error = "part " + p.ref + ": an ad536 needs VIN, CAV and +VS on their own nodes";
                return false;
            }
            const double r1 = std::max(m.get("r1", 25e3), 1.0), r4 = std::max(m.get("r4", 50e3), 1.0);
            addPrim(Prim::G, vin, com, constSlot(std::max(m.get("rin", 16.67e3), 1.0), part), part); // input resistance
            for (int k : {1, 10, 11, 12}) // NC pins may be left open (bare stubs): 1 GΩ to COM keeps such a node defined
                if (nd[static_cast<std::size_t>(k)] != com && nd[static_cast<std::size_t>(k)] != 0) addPrim(Prim::G, nd[static_cast<std::size_t>(k)], com, constSlot(1e9, part), part);
            addPrim(Prim::G, vpos, cav, constSlot(r1, part), part);                                  // R1: the averaging resistor
            const int dbv = nodeId(p.ref + "#db");
            addPrim(Prim::G, dbv, com, constSlot(1.0, part), part);
            NlDevice d;
            d.kind = NlKind::Ad536;
            d.part = part;
            d.nports = 3;
            d.port[0] = {vin, com, 0, 0, 0};
            d.port[1] = {vpos, cav, cav, vneg, 0};
            d.port[2] = {dbv, com, com, dbv, 0};
            const double i0 = std::max(m.get("vfloor", 1e-3), 1e-9) / r4;
            const double pv[7] = {1.0 / (r4 * r4), i0 * i0, 1.0 / r1, 0.01 * i0, std::max(m.get("i4max", 0.0), 0.0),
                                  std::max(m.get("mvdb", 3.0), 1e-3) * 1e-3 * 20.0 / std::log(10.0), std::max(m.get("iref", 20e-6), 1e-12)};
            for (int q = 0; q < 7; ++q) d.p[q] = pv[q];
            devices_.push_back(d);
            addPrim(Prim::Vcvs, db, 0, -1, part, 1.0, dbv, 0);                       // Q5 emitter follower (ideal)
            addPrim(Prim::Vccs, vpos, iout, -1, part, 2.0 / r1, vpos, cav);          // IOUT = 2·I3 (the mirror's second output)
            if (iout != rl) addPrim(Prim::G, iout, rl, constSlot(std::max(m.get("r2", 25e3), 1.0), part), part); // R2
            if (bo != 0 && bo != bi) {
                if (bi != com) addPrim(Prim::G, bi, com, constSlot(1e8, part), part); // buffer input resistance
                addPrim(Prim::Vcvs, bo, 0, -1, part, 1.0, bi, 0);                     // A4, a unity follower (ideal)
            }
            if (m.get("icc", 0.0) > 0 && vpos != vneg) {
                const int one = nodeId(p.ref + "#one");
                known[one] = 1.0;
                addPrim(Prim::Vccs, vpos, vneg, -1, part, m.get("icc", 0.0), one, 0);
            }
        } else if (p.type == "rms") {
            // RMS-level detector (THAT 2252 Fig. 3; dbx RMS; docs §"vca and rms"). nodes (2252 SIP order): in, ibias, gnd,
            // sym, v-, cap, out, v+. OA1 holds IN at GND (nullor; Iin read across r0 = 1 kΩ at #x); RmsSq writes the
            // rectified, squared current to #sq (1 Ω); RmsLog is Q6 charging the external CAP from V+; the pin-2 diode
            // string (nb junctions to V−) sets IBIAS; OA3 buffers OUT = V(cap) − vofs.
            const auto m = model();
            const int in = nd[0], ib = nd[1], gnd = nd[2], sym = nd[3], vm = nd[4], cap = nd[5], out = nd[6], vp = nd[7];
            if (in == 0 || out == 0 || cap == 0 || ib == 0 || ib == vm) {
                error = "part " + p.ref + ": an rms detector needs in, ibias, cap and out on their own nodes";
                return false;
            }
            // The junctions run at the physical thermal voltage vtj (default: the 2252's, 6.1 mV/dB at a 35 °C chip); mvdb
            // is only the output slope, an output gain mvdb / (2·vtj·ln10/20) inside the OA3 buffer (exactly 1 for 6.1).
            const double vt = m.get("vtj", 0.0) > 0 ? m.get("vtj", 0.0) : 6.1e-3 * 10.0 / std::log(10.0);
            double gain = std::max(m.get("mvdb", 6.1), 1e-3) / (vt * 1e3 * std::log(10.0) / 10.0);
            if (std::abs(gain - 1.0) < 1e-12) gain = 1.0;
            const double nb = std::max(m.get("nb", 3.0), 1.0), r0 = 1e3, iref = 1e-3;
            const double isb = std::max(m.get("ibref", 24e-6), 1e-12) * std::exp(-m.get("vbias", 2.1) / (nb * vt));
            const double vofs = m.get("vofs", 1.4), kgeo = std::max(m.get("kgeo", 2.9), 1e-6);
            const int x = nodeId(p.ref + "#x"), sq = nodeId(p.ref + "#sq"), ko = nodeId(p.ref + "#ko");
            const int km = nodeId(p.ref + "#km"), kv = nodeId(p.ref + "#kv");
            addPrim(Prim::Nullor, x, 0, -1, part, 1.0, gnd, in);
            addPrim(Prim::G, in, x, constSlot(r0, part), part);
            addPrim(Prim::G, sq, 0, constSlot(1.0, part), part);
            if (sym != gnd) addPrim(Prim::G, sym, gnd, constSlot(1e7, part), part); // Q2 base: keeps an open SYM defined
            NlDevice bj; // pin 2: IBIAS through nb diode junctions to V−
            bj.kind = NlKind::Diode;
            bj.part = part;
            bj.nports = 1;
            bj.port[0] = {ib, vm, ib, vm, 0};
            bj.p[0] = isb;
            bj.p[1] = nb * vt;
            bj.p[2] = 1.0 / (nb * vt);
            devices_.push_back(bj);
            NlDevice d;
            d.kind = NlKind::RmsSq;
            d.part = part;
            d.nports = 2;
            d.port[0] = {x, gnd, 0, sq, 0};
            d.port[1] = {sym, in, 0, 0, 0};
            const double iflr = std::max(m.get("iflr", 16e-9), 0.0);
            const double ps[5] = {1.0 / r0, m.get("vsym", 0.0), 1.0 / vt, iflr * iflr, 1.0 / (iref * iref)};
            for (int q = 0; q < 5; ++q) d.p[q] = ps[q];
            devices_.push_back(d);
            NlDevice l;
            l.kind = NlKind::RmsLog;
            l.part = part;
            l.nports = 3;
            l.port[0] = {sq, 0, 0, 0, 0};
            l.port[1] = {ko, cap, vp, cap, 0}; // y = (gnd + vofs) − cap: a forward-junction coordinate for pnjlim
            l.port[2] = {ib, vm, 0, 0, 0};
            const double pl[5] = {iref * iref * kgeo * kgeo, vofs, 1.0 / vt, isb, 1.0 / (nb * vt)};
            for (int q = 0; q < 5; ++q) l.p[q] = pl[q];
            devices_.push_back(l);
            known[km] = -vofs; // #ko = gnd + vofs (the CAP level at 0 V out, relative to the chip's GND)
            addPrim(Prim::Vcvs, ko, 0, -1, part, 1.0, gnd, km);
            known[kv] = vofs; // OA3: OUT = V(cap) − vofs
            addPrim(Prim::Vcvs, out, 0, -1, part, gain, cap, kv);
            if (m.get("icc", 0.0) > 0 && vp != vm) {
                const int one = nodeId(p.ref + "#one");
                known[one] = 1.0;
                addPrim(Prim::Vccs, vp, vm, -1, part, m.get("icc", 0.0), one, 0);
            }
        } else if (p.type == "lamp" || p.type == "led" || p.type == "neon" || p.type == "cds" || p.type == "elpanel") {
            // Optical parts (DEVICES.md §4.2): emitters write a normalised light node (λ = 1 at full drive; 1 F with a
            // leak of 1/τ_light, so the emitter injects Φ/τ_light), cells read it.
            const auto m = model();
            const int light = nodeId(p.light);
            if (light == 0) {
                error = "part " + p.ref + ": the light node cannot be ground";
                return false;
            }
            const bool emitter = p.type != "cds";
            if (emitter && !lightTau.count(light)) lightTau[light] = std::max(m.get("tau_light", 1e-4), 1e-6);
            if (!emitter) lightReaders.push_back({light, p.ref});
            const double gl = emitter ? 1.0 / lightTau[light] : 0.0;
            const int a = nd[0], b = nd[1];
            if (p.type == "lamp") {
                // Incandescent filament with a thermal state θ = T/T_hot (NlKind::LampTh).
                const double vr = m.get("vrated", 28), ir = m.get("irated", 0.04), al = m.get("alpha", 1.214);
                const double tha = std::pow(std::clamp(m.get("rcold_ratio", 1.0 / 13.5), 1e-3, 1.0), 1.0 / al);
                const double rad = 1.0 / (1.0 - std::pow(tha, 4.0));
                const double tau0 = std::max(m.get("tau", 0.02), 1e-5) * (al + 4.0 * rad);
                const int th = nodeId(p.ref + "#theta");
                addPrim(Prim::Cap, th, 0, constSlot(1.0, part), part);
                NlDevice d;
                d.kind = NlKind::LampTh;
                d.part = part;
                d.nports = 3;
                d.port[0] = {a, b, a, b, 0};
                d.port[1] = {th, 0, th, 0, 0};
                d.port[2] = {light, 0, 0, light, 0};
                const double pv[8] = {ir / vr, al, tha, 1.0 / tau0, 1.0 / (vr * ir), m.get("tlambda", 20250) / m.get("thot", 2500), gl, rad};
                for (int q = 0; q < 8; ++q) d.p[q] = pv[q];
                // θ has only its 1 F and this law: an exact linear split (the radiation slope at θ = 1) anchors it at DC.
                const double g0 = 4.0 * rad / tau0;
                addPrim(Prim::G, th, 0, -1, part, 1.0 / g0);
                d.gShift[1] = g0;
                d.hasShift = true;
                devices_.push_back(d);
            } else if (p.type == "led") {
                const double rs = m.get("rs", 0.0);
                int an = a;
                if (rs > 0) {
                    const int jn = nodeId(p.ref + "#j");
                    addPrim(Prim::G, a, jn, constSlot(rs, part), part);
                    an = jn;
                }
                NlDevice d;
                d.kind = NlKind::Diode;
                d.part = part;
                d.nports = 1;
                d.port[0] = {an, b, an, b, 0};
                d.p[0] = m.get("is", 1e-19);
                d.p[1] = m.get("n", 1.6) * kVt;
                d.p[2] = 1.0 / d.p[1];
                devices_.push_back(d);
                NlDevice e = d;
                e.kind = NlKind::DiodeLight;
                e.port[0] = {an, b, 0, light, 0};
                e.p[3] = gl / std::max(m.get("iref", 0.01), 1e-9);
                devices_.push_back(e);
            } else if (p.type == "elpanel") {
                // Electroluminescent panel: a → Rs → m → Cp → b (linear parts) + ElLight into the light node, normalised so
                // λ = 1 for a vref-rms sine at fref across the panel.
                const double cp = m.get("cp", 3e-9), rs = m.get("rs", 1e4), bb = m.get("b", 20.0), ve = m.get("veps", 0.5), dl = 1e-3;
                const int mid = nodeId(p.ref + "#m");
                addPrim(Prim::G, a, mid, constSlot(rs, part), part);
                addPrim(Prim::Cap, mid, b, constSlot(cp, part), part);
                const double w = 2.0 * M_PI * m.get("fref", 1000.0), vpk = std::sqrt(2.0) * m.get("vref", 60.0);
                const std::complex<double> z(rs, -1.0 / (w * cp));
                const std::complex<double> ipk = vpk / z, vcpk = ipk / std::complex<double>(0.0, w * cp);
                double avg = 0;
                for (int q = 0; q < 512; ++q) { // period average of the light law at the reference drive
                    const double ph = 2.0 * M_PI * (q + 0.5) / 512.0;
                    const double iv = std::abs(ipk) * std::sin(ph), vc = std::abs(vcpk) * std::sin(ph + std::arg(vcpk) - std::arg(ipk));
                    avg += std::sqrt(iv * iv * rs * rs + dl * dl) / rs * std::exp(-bb / (std::sqrt(std::sqrt(vc * vc + dl * dl) + ve)));
                }
                avg /= 512.0;
                NlDevice d;
                d.kind = NlKind::ElLight;
                d.part = part;
                d.nports = 3;
                d.port[0] = {a, mid, 0, 0, 0};
                d.port[1] = {mid, b, 0, 0, 0};
                d.port[2] = {light, 0, 0, light, 0};
                d.p[0] = gl / std::max(avg, 1e-30) / rs;
                d.p[1] = bb;
                d.p[2] = ve;
                d.p[3] = dl;
                devices_.push_back(d);
            } else if (p.type == "neon") {
                NlDevice ne;
                ne.kind = NlKind::Neon;
                ne.part = part;
                ne.port[0] = {a, b, a, b, 0};
                ne.p[0] = m.get("vstrike", 65);
                ne.p[1] = m.get("rlamp", 30e3);
                ne.p[2] = m.get("smooth", 2);
                devices_.push_back(ne);
                NlDevice ld = ne;
                ld.kind = NlKind::LightDrive;
                ld.port[0] = {a, b, 0, light, 0};
                ld.p[3] = gl / std::max(m.get("iref", 1e-3), 1e-9);
                devices_.push_back(ld);
            } else { // cds
                const double taul = std::max(m.get("tau_l", 2.0), 1e-3), tauh = m.get("tau_h", 0.0), mh = m.get("mh", 0.0), mr = m.get("mr", 0.0);
                const bool hist = tauh > 0 && (mh > 0 || mr > 0);
                const int nn = nodeId(p.ref + "#n");
                addPrim(Prim::Cap, nn, 0, constSlot(1.0, part), part);
                addPrim(Prim::G, nn, 0, constSlot(taul, part), part); // dark leak −n/τl (1 F, so R = τl)
                int hn = 0;
                if (hist) { // history: dh/dt = (n − h)/τh
                    hn = nodeId(p.ref + "#h");
                    addPrim(Prim::Cap, hn, 0, constSlot(1.0, part), part);
                    addPrim(Prim::G, hn, 0, constSlot(tauh, part), part);
                    addPrim(Prim::Vccs, 0, hn, -1, part, 1.0 / tauh, nn, 0);
                }
                NlDevice r;
                r.kind = NlKind::CdsRate;
                r.part = part;
                r.nports = hist ? 3 : 2;
                r.port[0] = {light, 0, 0, 0, 0};
                r.port[1] = {nn, 0, nn, 0, 0};
                if (hist) r.port[2] = {hn, 0, 0, 0, 0};
                r.p[0] = m.get("kr", 200);
                r.p[1] = std::max(p.couple, 0.0);
                r.p[3] = hist ? mr : 0.0;
                devices_.push_back(r);
                NlDevice c;
                c.kind = NlKind::CdsCell;
                c.part = part;
                c.nports = hist ? 3 : 2;
                c.port[0] = {a, b, a, b, 0};
                c.port[1] = {nn, 0, 0, 0, 0};
                if (hist) c.port[2] = {hn, 0, 0, 0, 0};
                c.p[0] = 1.0 / std::max(m.get("rdark", 10e6), 1.0);
                c.p[1] = 1.0 / std::max(m.get("ron", 1e3), 1e-3);
                c.p[2] = 2.0 * m.get("gamma", 1.0);
                c.p[3] = hist ? mh : 0.0;
                devices_.push_back(c);
            }
        } else if (p.type == "transformer") {
            // Windings k: (a_k, b_k) with turns N_k, series R_k and leakage L_k. Core node: MMF balance
            // Σ N_k i_k = f(λ); flux node λ' = v_core (1 F); f = (λsat/Lm)·sinh(λ/λsat) (Lm, λsat referred to 1 turn).
            const int core = nodeId(p.ref + "#core");
            const std::size_t W = p.turns.size();
            auto isCutName = [&](const std::string& nm) { return std::find(n.cuts.begin(), n.cuts.end(), nm) != n.cuts.end(); };
            for (std::size_t k = 0; k < W; ++k) {
                int a = nd[2 * k];
                int b = nd[2 * k + 1];
                windingEnds_.push_back(a), windingEnds_.push_back(b); // mainsCore(): a winding on a mains source
                const double rw = k < p.windR.size() ? p.windR[k] : 0.0, lk = k < p.windL.size() ? p.windL[k] : 0.0;
                // The winding resistance goes on the end that sits on a cut supply node (the consumer current is
                // then a resistor current); otherwise on the 'a' end.
                const bool rAtB = rw > 0 && isCutName(p.nodes[2 * k + 1]);
                if (rAtB) {
                    const int x = nodeId(p.ref + "#rb" + std::to_string(k));
                    addPrim(Prim::G, x, b, constSlot(rw, part), part);
                    b = x;
                } else if (rw > 0) {
                    const int x = nodeId(p.ref + "#r" + std::to_string(k));
                    addPrim(Prim::G, a, x, constSlot(rw, part), part);
                    a = x;
                }
                if (lk > 0) {
                    const int y = nodeId(p.ref + "#l" + std::to_string(k));
                    addPrim(Prim::Ind, a, y, constSlot(lk, part), part);
                    a = y;
                }
                addPrim(Prim::Winding, a, b, -1, part, p.turns[k], core, 0);
            }
            if (p.lm > 0) {
                const int flux = nodeId(p.ref + "#flux");
                addPrim(Prim::Vccs, 0, flux, -1, part, 1.0, core, 0);
                addPrim(Prim::Cap, flux, 0, constSlot(1.0, part), part);
                NlDevice d;
                d.kind = NlKind::Magnetizing;
                d.part = part;
                d.nports = 1;
                d.port[0] = {flux, 0, 0, core, 0};
                d.p[0] = p.lm;
                d.p[1] = p.lsat > 0 ? p.lsat : 1e6;
                d.p[2] = p.hc;
                d.p[3] = p.hw > 0 ? p.hw : 0.05 * d.p[1]; // default play width: 5 % of the saturation flux
                devices_.push_back(d);
                mags_.push_back({core, flux, static_cast<int>(devices_.size()) - 1, p.lm});
            }
        } else if (p.type == "speaker") {
            // Electro-mechanical loudspeaker (docs §10.21; ModelLibrary.hpp speakerDerived; law: Devices.hpp NlKind::Speaker).
            // Electrical: pin+ → Re → (L2 ∥ R2) → a; the motional coil a → pin− carries i = ψ·Le0/Le(x), where the flux node
            // ψ (Le0 farads, volts read as amps) integrates the coil voltage minus the EMF Bl(x)·u and the thermal ΔRe·i.
            // Mechanical (mobility analogue, force = current, velocity = voltage): vel carries Mms (C), Rms (G), the force
            // Bl(x)·i + ½i²·dLe/dx − Kms(x)·x; the excursion node xm integrates 1000·u (mm). The rest-point linear parts
            // are primitives here, the remainder is the 3-port law.
            const SpeakerDerived sp = speakerDerived(model(), p.series, p.parallel);
            const int vel = nd[2], neg = nd[1];
            const int n1 = nodeId(p.ref + "#1"), psi = nodeId(p.ref + "#psi"), xm = nodeId(p.ref + "#xm");
            int a = n1;
            SpeakerDevSlots ds;
            addPrim(Prim::G, nd[0], n1, constSlot(sp.re, part), part);
            if (sp.l2 > 0) {
                a = nodeId(p.ref + "#a");
                ds.l2 = constSlot(sp.l2, part);
                addPrim(Prim::Ind, n1, a, ds.l2, part);
                if (sp.r2 > 0) addPrim(Prim::G, n1, a, constSlot(sp.r2, part), part);
            }
            ds.le = constSlot(sp.le, part);
            addPrim(Prim::Cap, psi, 0, ds.le, part);
            addPrim(Prim::Vccs, 0, psi, -1, part, 1.0, a, neg);            // coil voltage into ψ
            addPrim(Prim::Vccs, psi, 0, -1, part, sp.bl, vel, 0);          // EMF Bl0·u
            addPrim(Prim::Vccs, a, neg, -1, part, 1.0, psi, 0);            // coil current at rest: ψ
            ds.mms = constSlot(sp.mms, part);
            ds.rms = constSlot(1.0 / sp.rms, part);
            addPrim(Prim::Cap, vel, 0, ds.mms, part);
            addPrim(Prim::G, vel, 0, ds.rms, part);
            addPrim(Prim::Vccs, 0, vel, -1, part, sp.bl, psi, 0);          // force Bl0·i
            addPrim(Prim::Vccs, vel, 0, -1, part, sp.kms * 1e-3, xm, 0);   // suspension Kms0·x (x in mm)
            addPrim(Prim::Vccs, 0, xm, -1, part, 1000.0, vel, 0);          // dx/dt = u (mm)
            addPrim(Prim::Cap, xm, 0, constSlot(1.0, part), part);
            NlDevice d;
            d.kind = NlKind::Speaker;
            d.part = part;
            d.nports = 3;
            d.port[0] = {xm, 0, vel, 0, 0};
            d.port[1] = {vel, 0, psi, 0, 0};
            d.port[2] = {psi, 0, a, neg, 0};
            const double pv[14] = {sp.bl, sp.b[0], sp.b[1], sp.b[2], sp.b[3], sp.xlo, sp.xhi, sp.kms * 1e-3, sp.k1, sp.k2, sp.k4, sp.lx1, sp.lx2, 500.0 * sp.le};
            for (int q = 0; q < 14; ++q) d.p[q] = pv[q];
            devices_.push_back(d);
            if (!endsInCab) { // a plain load: a following cab unit's speaker profile may drive it (setSpeakerLoad)
                ds.dev = static_cast<int>(devices_.size()) - 1;
                ds.fs0 = std::sqrt(sp.kms / sp.mms) / (2.0 * M_PI);
                ds.p13 = d.p[13];
                spkDevs_.push_back(ds);
            }
            if (sp.thermal)
                spkTh_.push_back({static_cast<int>(devices_.size()) - 1, sp.re, sp.alpha, sp.rtv, sp.ctv, sp.rtm, sp.ctm, static_cast<double>(sp.coils)});
        } else if (p.type == "pentode") {
            const auto m = model();
            const int pl = nd[0], g2 = nd[1], g1 = nd[2], k = nd[3];
            NlDevice d;
            d.kind = NlKind::Pentode;
            d.part = part;
            d.nports = 3;
            d.port[0] = {g1, k, g1, k, 0};
            d.port[1] = {pl, k, pl, k, 0};
            d.port[2] = {g2, k, g2, k, 0};
            const char* keys[] = {"mu", "ex", "kg1", "kg2", "kp", "kvb", "gscale", "gknee"};
            for (int i = 0; i < 8; ++i) d.p[i] = m.get(keys[i], 0);
            const double par = p.parallel; // matched tubes in parallel: currents and capacitances scale by the count
            d.p[2] /= par;
            d.p[3] /= par;
            d.p[6] *= par;
            if (m.get("lut", 1.0) > 0.5 && !std::getenv("CE_NOPENTLUT")) { // tables in the triode parameter order
                const double tp[7] = {d.p[0], d.p[1], d.p[2], d.p[4], d.p[5], d.p[6], d.p[7]};
                d.lut = korenTables(tp);
            }
            devices_.push_back(d);
            if (m.get("cg1k", 0) > 0) addPrim(Prim::Cap, g1, k, constSlot(par * m.get("cg1k", 0), part), part);
            if (m.get("cg1p", 0) > 0) addPrim(Prim::Cap, g1, pl, constSlot(par * m.get("cg1p", 0), part), part);
            if (m.get("cpk", 0) > 0) addPrim(Prim::Cap, pl, k, constSlot(par * m.get("cpk", 0), part), part);
            if (opt.hum && k > 0) // power pentodes: typical rhk 100 MΩ, chk 10 pF (assumed; docs §3.9)
                humTubes.push_back({k, part, m.get("rhk", kHumRhkPentode) / par, m.get("chk", 10e-12) * par, m.get("hkv", kHumHkv)});
        } else if (p.type == "triode") {
            const auto m = model();
            const int pl = nd[0], g = nd[1], k = nd[2];
            NlDevice d;
            d.kind = NlKind::Triode;
            d.part = part;
            d.nports = 2;
            d.port[0] = {g, k, g, k, 0};
            d.port[1] = {pl, k, pl, k, 0};
            const char* keys[] = {"mu", "ex", "kg1", "kp", "kvb", "gscale", "gknee", "vgo"};
            for (int i = 0; i < 8; ++i) d.p[i] = m.get(keys[i], 0); // p[7] = vgo, the plate law's contact-potential offset
            const double par = p.parallel;
            d.p[2] /= par;
            d.p[5] *= par;
            if (m.get("lut", 1.0) > 0.5 && !std::getenv("CE_NOTRIODELUT")) d.lut = korenTables(d.p); // compile time (control thread)
            devices_.push_back(d);
            const bool beCaps = kEnvBeCaps; // experiment
            if (m.get("cgk", 0) > 0) { addPrim(Prim::Cap, g, k, constSlot(par * m.get("cgk", 0), part), part); prims_.back().be = beCaps; }
            if (m.get("cgp", 0) > 0) { addPrim(Prim::Cap, g, pl, constSlot(par * m.get("cgp", 0), part), part); prims_.back().be = beCaps; }
            if (m.get("cpk", 0) > 0) { addPrim(Prim::Cap, pl, k, constSlot(par * m.get("cpk", 0), part), part); prims_.back().be = beCaps; }
            if (opt.hum && k > 0) // small triodes: typical rhk 1 GΩ, chk 5 pF (measured / published; docs §3.9)
                humTubes.push_back({k, part, m.get("rhk", kHumRhkTriode) / par, m.get("chk", 5e-12) * par, m.get("hkv", kHumHkv)});
        } else {
            error = "part " + p.ref + ": type " + p.type + " not supported by this engine version";
            return false;
        }
    }
    // Light nodes: 1 F with the leak 1/τ_light (the first emitter's), so λ settles at the emitted Φ.
    for (const auto& [ln, tau] : lightTau) {
        addPrim(Prim::Cap, ln, 0, constSlot(1.0, -1), -1);
        addPrim(Prim::G, ln, 0, constSlot(tau, -1), -1);
    }
    for (const auto& [ln, ref] : lightReaders)
        if (!lightTau.count(ln)) {
            error = "part " + ref + ": no emitter (lamp, led, neon, elpanel) writes light node " + nodeNames_[static_cast<std::size_t>(ln)];
            return false;
        }
    // Heater-cathode hum (CompileOptions::hum): the heaters' effective AC (hkv rms, a sine at the mains frequency and
    // phase: the netlist's 45-65 Hz source with the largest amplitude) drives each cathode through rhk ∥ chk. The
    // cathode current is V·(1/rhk + jω·chk) − Vk·(1/rhk + jω·chk); the cathode sits at ≤ a few kΩ against
    // |1/rhk + jω·chk|⁻¹ ≥ 100 MΩ, so stamping rhk alone from a source V·(1 + jω·rhk·chk) gives the same current within
    // Zk·|1/rhk + jω·chk| (< 0.01 %) without a capacitor state per tube. All heaters share a winding: tubes with the same
    // hkv and rhk·chk (the defaults: 5 ms for triodes, 1 ms for pentodes) share one source node, so a group gains at most
    // one input per class.
    if (opt.hum && !humTubes.empty()) {
        const AcSource* mains = nullptr;
        for (const auto& a : acSrc_)
            if (a.w > 2 * M_PI * 45 && a.w < 2 * M_PI * 65 && (!mains || std::abs(a.amp) > std::abs(mains->amp))) mains = &a;
        if (mains) {
            const AcSource ref = *mains;
            std::vector<std::pair<std::pair<double, double>, int>> heaters; // (hkv, τ) -> node
            for (const auto& t : humTubes) {
                if (!(t.rhk > 0)) continue;
                const double tau = t.rhk * std::max(t.chk, 0.0);
                int h = -1;
                for (const auto& [key, nd] : heaters)
                    if (std::abs(key.first - t.hkv) <= 1e-9 * std::abs(t.hkv) && std::abs(key.second - tau) <= 1e-9 * tau) h = nd;
                if (h < 0) {
                    h = nodeId("heater#" + std::to_string(heaters.size()));
                    heaters.push_back({{t.hkv, tau}, h});
                    known[h] = 0.0;
                    const double wt = ref.w * tau;
                    acSrc_.push_back(AcSource{h, 0.0, std::sqrt(2.0) * t.hkv * std::sqrt(1.0 + wt * wt), ref.w, ref.phase + std::atan(wt)});
                }
                addPrim(Prim::G, h, t.k, constSlot(t.rhk, t.part), t.part);
            }
        }
    }
    const int N = static_cast<int>(nodeNames_.size());
    nodeKind_.assign(static_cast<std::size_t>(N), kFree);
    knownValue_.assign(static_cast<std::size_t>(N), 0.0);
    nodeKind_[0] = kGround;
    for (const auto& [nd, v] : known) {
        nodeKind_[static_cast<std::size_t>(nd)] = kKnown;
        knownValue_[static_cast<std::size_t>(nd)] = v;
    }
    for (const auto& a : acSrc_) knownValue_[static_cast<std::size_t>(a.node)] = a.dc + a.amp * std::sin(a.phase); // t = 0 (default phase 90° = crest: supplies start charged); control scale applied by refreshScaledSources
    for (std::size_t pi = 0; pi < prims_.size(); ++pi) {
        const auto& p = prims_[pi];
        if (p.kind != Prim::Vcvs && p.kind != Prim::Nullor) continue;
        if (p.a == 0 || nodeKind_[static_cast<std::size_t>(p.a)] == kKnown) {
            error = "an ideal output (" + (p.part >= 0 ? n.parts[static_cast<std::size_t>(p.part)].ref : std::string("?")) + ") drives a node that is already a source";
            return false;
        }
        if (driven.count(p.a)) {
            error = "node " + nodeNames_[static_cast<std::size_t>(p.a)] + " is driven by two ideal outputs";
            return false;
        }
        driven[p.a] = static_cast<int>(pi);
        nodeKind_[static_cast<std::size_t>(p.a)] = kDriven;
    }
    outputNode_ = node(n.io.output);
    if (outputNode_ <= 0) {
        error = "output node " + n.io.output + " does not exist";
        return false;
    }
    outputNodeR_ = n.io.outputR.empty() ? -1 : node(n.io.outputR);
    if (!n.io.outputR.empty() && outputNodeR_ <= 0) {
        error = "outputR node " + n.io.outputR + " does not exist";
        return false;
    }
    // ── open switch contacts (CompileOptions::openSwitches): no edge ──
    // Every open candidate contact leaves the circuit (its prim's terminals go to ground: no stamp, no partition edge),
    // except one whose ends would then lose their linear path to ground, a source or an ideal output, in the transient
    // matrix or at DC (a selector's unused throw, a capacitor-coupled stub): that node would float, so the contact keeps
    // its roff. The anchors are found with every candidate out, so each removal holds whatever else is removed.
    if (std::any_of(sw_.begin(), sw_.end(), [](const SwitchState& s) { return !s.closed; })) {
        std::vector<std::uint8_t> cand(prims_.size(), 0);
        for (const auto& s : sw_)
            if (!s.closed) cand[static_cast<std::size_t>(s.prim)] = 1;
        UnionFind uf(N), ufDc(N);
        for (std::size_t i = 0; i < prims_.size(); ++i) {
            const auto& p = prims_[i];
            if (cand[i]) continue;
            if (p.kind == Prim::G || p.kind == Prim::Cap || p.kind == Prim::Ind || p.kind == Prim::Winding) uf.unite(p.a, p.b);
            if (p.kind == Prim::G || p.kind == Prim::Ind || p.kind == Prim::Winding) ufDc.unite(p.a, p.b);
        }
        std::vector<std::uint8_t> anchored(static_cast<std::size_t>(N), 0), anchoredDc(static_cast<std::size_t>(N), 0);
        for (int nd = 0; nd < N; ++nd)
            if (nodeKind_[static_cast<std::size_t>(nd)] != kFree) {
                anchored[static_cast<std::size_t>(uf.find(nd))] = 1;
                anchoredDc[static_cast<std::size_t>(ufDc.find(nd))] = 1;
            }
        auto holds = [&](int nd) {
            return nodeKind_[static_cast<std::size_t>(nd)] != kFree ||
                   (anchored[static_cast<std::size_t>(uf.find(nd))] && anchoredDc[static_cast<std::size_t>(ufDc.find(nd))]);
        };
        // ...and only where the contact's leakage is below rounding: the roff current, (va − vb)/roff, moves either end
        // by at most R/roff of the voltage across the contact, R the end's resistance to ground through the resistors
        // alone (capacitors, inductors, windings and devices open: an RC network's |Z(jω)| never exceeds that, and the
        // other branches only add paths). Required: R ≤ kOpenLeak·roff at both ends (1 kΩ for 1 GΩ: ≤ −120 dB). A
        // contact into a grid or a jack's 1 MΩ keeps its roff (the 5E3's input jacks leak −63 dB through it).
        std::map<int, int> loc; // free nodes reached from the candidates' ends through resistors
        {
            std::vector<int> todo;
            for (const auto& s : sw_) {
                if (s.closed) continue;
                const auto& p = prims_[static_cast<std::size_t>(s.prim)];
                for (int t : {p.a, p.b})
                    if (nodeKind_[static_cast<std::size_t>(t)] == kFree && !loc.count(t)) loc.emplace(t, static_cast<int>(loc.size())), todo.push_back(t);
            }
            std::vector<std::vector<int>> adj(static_cast<std::size_t>(N));
            for (std::size_t i = 0; i < prims_.size(); ++i)
                if (prims_[i].kind == Prim::G && !cand[i] && prims_[i].a != prims_[i].b) {
                    adj[static_cast<std::size_t>(prims_[i].a)].push_back(prims_[i].b);
                    adj[static_cast<std::size_t>(prims_[i].b)].push_back(prims_[i].a);
                }
            while (!todo.empty()) {
                const int x = todo.back();
                todo.pop_back();
                for (int y : adj[static_cast<std::size_t>(x)])
                    if (nodeKind_[static_cast<std::size_t>(y)] == kFree && !loc.count(y)) loc.emplace(y, static_cast<int>(loc.size())), todo.push_back(y);
            }
        }
        // each resistor at its largest value over the controls that move without a recompile (pots, variable parts, a
        // pot's switch); the contacts on switches and selectors at their compiled state
        auto rMax = [&](const Prim& p) {
            if (p.slot < 0) return p.fixed;
            const auto& sl = slots_[static_cast<std::size_t>(p.slot)];
            if (sl.kind == ValueSlot::Const || sl.part < 0) return sl.base * sl.scale;
            const Part& pt = n.parts[static_cast<std::size_t>(sl.part)];
            switch (sl.kind) {
                case ValueSlot::Const: return sl.base * sl.scale;
                case ValueSlot::Law: return sl.scale * (pt.value.variable() ? std::max(pt.value.min, pt.value.max) : pt.value.nominal);
                case ValueSlot::PotLower:
                case ValueSlot::PotUpper: return sl.scale * (pt.value.variable() ? std::max(pt.value.min, pt.value.max) : pt.value.nominal);
                case ValueSlot::Switch: {
                    const ControlDef* cd = n.control(pt.control);
                    if (cd && cd->kind != "continuous") return switchClosed(n, pt, compileCtl) ? pt.ron : pt.roff;
                    return std::max(pt.ron, pt.roff);
                }
            }
            return p.fixed;
        };
        const int m = static_cast<int>(loc.size());
        std::vector<double> Gm(static_cast<std::size_t>(m) * static_cast<std::size_t>(m), 0.0);
        for (int i = 0; i < m; ++i) Gm[static_cast<std::size_t>(i * m + i)] = 1e-15; // a node with no resistive path down: R = 1e15
        for (std::size_t i = 0; i < prims_.size(); ++i) {
            const auto& p = prims_[i];
            if (p.kind != Prim::G || cand[i] || p.a == p.b) continue;
            const auto ia = loc.find(p.a), ib = loc.find(p.b);
            if (ia == loc.end() && ib == loc.end()) continue;
            const double r = rMax(p);
            if (!(r > 0)) continue;
            const double g = 1.0 / r;
            const int a = ia == loc.end() ? -1 : ia->second, b = ib == loc.end() ? -1 : ib->second;
            if (a >= 0) Gm[static_cast<std::size_t>(a * m + a)] += g;
            if (b >= 0) Gm[static_cast<std::size_t>(b * m + b)] += g;
            if (a >= 0 && b >= 0) Gm[static_cast<std::size_t>(a * m + b)] -= g, Gm[static_cast<std::size_t>(b * m + a)] -= g;
        }
        std::vector<int> piv(static_cast<std::size_t>(m));
        const bool fact = m > 0 && la::luFactor(Gm.data(), m, piv.data());
        auto rToGround = [&](int nd) {
            if (nodeKind_[static_cast<std::size_t>(nd)] != kFree) return 0.0;
            if (!fact) return 1e300;
            std::vector<double> e(static_cast<std::size_t>(m), 0.0);
            const int i = loc.at(nd);
            e[static_cast<std::size_t>(i)] = 1.0;
            la::luSolve(Gm.data(), piv.data(), m, e.data());
            return e[static_cast<std::size_t>(i)];
        };
        const bool swDiag = std::getenv("CE_SWDIAG") != nullptr;
        for (auto& s : sw_) {
            auto& p = prims_[static_cast<std::size_t>(s.prim)];
            if (s.closed) continue;
            const double roff = n.parts[static_cast<std::size_t>(s.part)].roff;
            const double ra = rToGround(p.a), rb = rToGround(p.b);
            const bool leak = ra <= kOpenLeak * roff && rb <= kOpenLeak * roff;
            if (swDiag)
                std::fprintf(stderr, "open contact %-12s %s–%s: R to ground %.4g / %.4g ohm (limit %.4g)%s%s\n", n.parts[static_cast<std::size_t>(s.part)].ref.c_str(),
                             nodeNames_[static_cast<std::size_t>(p.a)].c_str(), nodeNames_[static_cast<std::size_t>(p.b)].c_str(), ra, rb, kOpenLeak * roff,
                             !holds(p.a) || !holds(p.b) ? "  kept: an end would float" : "", leak ? "" : "  kept: leakage above rounding");
            if (!holds(p.a) || !holds(p.b) || !leak) continue;
            s.removed = true;
            p.a = p.b = 0;
        }
    }
    // ── linearly floating islands: exact linear split ──
    // Free nodes whose linear elements (R, C, L, windings) connect them only among themselves, with no linear path to
    // ground, a source or an ideal output (the middle of a series diode string, with the diodes' series resistances),
    // leave the linear matrix singular. Stamp a conductance g0 across each nonlinear two-terminal port that touches
    // such an island and subtract g0·v from that port's law (NlDevice::gShift): the total current, and so the solution,
    // is unchanged (no bleeder), and the matrix is regular. g0 sits between gmin and any real circuit conductance.
    {
        constexpr double g0 = 1e-9;
        // Islands with capacitors (singular in every solve) and, for the DC solve, without them: a node reached only by
        // capacitors and nonlinear ports (an OTA's output with its integrating capacitor, an OTA follower) has only
        // gmin at DC, so its DC Newton is ill-conditioned (K ~ 1/gmin). Both get the exact split.
        UnionFind uf(N), ufDc(N);
        for (const auto& p : prims_) {
            if (p.kind == Prim::G || p.kind == Prim::Cap || p.kind == Prim::Ind || p.kind == Prim::Winding) uf.unite(p.a, p.b);
            if (p.kind == Prim::G || p.kind == Prim::Ind || p.kind == Prim::Winding) ufDc.unite(p.a, p.b);
        }
        std::vector<std::uint8_t> anchored(static_cast<std::size_t>(N), 0), anchoredDc(static_cast<std::size_t>(N), 0);
        for (int nd = 0; nd < N; ++nd)
            if (nodeKind_[static_cast<std::size_t>(nd)] != kFree) {
                anchored[static_cast<std::size_t>(uf.find(nd))] = 1;
                anchoredDc[static_cast<std::size_t>(ufDc.find(nd))] = 1;
            }
        auto floating = [&](int nd) {
            if (nd <= 0 || nodeKind_[static_cast<std::size_t>(nd)] != kFree) return false;
            return !anchored[static_cast<std::size_t>(uf.find(nd))] || !anchoredDc[static_cast<std::size_t>(ufDc.find(nd))];
        };
        for (auto& d : devices_)
            for (int k = 0; k < d.nports; ++k) {
                auto& pt = d.port[k];
                if (pt.cp != pt.from || pt.cn != pt.to || !(floating(pt.from) || floating(pt.to))) continue;
                addPrim(Prim::G, pt.from, pt.to, -1, d.part, 1.0 / g0);
                d.gShift[k] += g0;
                d.hasShift = true;
                auto floatsInTransient = [&](int nd) { return nd > 0 && nodeKind_[static_cast<std::size_t>(nd)] == kFree && !anchored[static_cast<std::size_t>(uf.find(nd))]; };
                if (floatsInTransient(pt.from) || floatsInTransient(pt.to)) d.floatingPorts |= static_cast<std::uint8_t>(1u << k);
                warnings_.push_back((d.part >= 0 ? n.parts[static_cast<std::size_t>(d.part)].ref : std::string("?")) + " port " + std::to_string(k) +
                                    ": linear split at a linearly floating node (" + nodeNames_[static_cast<std::size_t>(floating(pt.from) ? pt.from : pt.to)] + ")");
            }
    }
    // ── approximate cuts: supply-like elements stay with the rail, consumers see it as a known voltage ──
    cut_.assign(static_cast<std::size_t>(N), 0);
    for (const auto& cn : n.cuts) {
        const int id = node(cn);
        if (id <= 0 || nodeKind_[static_cast<std::size_t>(id)] != kFree) {
            error = "cut node " + cn + " must be an ordinary circuit node";
            return false;
        }
        cut_[static_cast<std::size_t>(id)] = 1;
    }
    // Signal cuts name their loads explicitly (by part); supply cuts use the "supply-like" rule.
    std::vector<std::set<int>> loadsAt(static_cast<std::size_t>(N));
    std::vector<std::uint8_t> explicitCut(static_cast<std::size_t>(N), 0);
    for (const auto& [cn, refs] : n.cutLoads) {
        const int id = node(cn);
        if (id <= 0) continue;
        explicitCut[static_cast<std::size_t>(id)] = 1;
        for (const auto& r : refs) {
            bool found = false;
            for (std::size_t pi = 0; pi < n.parts.size(); ++pi)
                if (n.parts[pi].ref == r) {
                    loadsAt[static_cast<std::size_t>(id)].insert(static_cast<int>(pi));
                    found = true;
                }
            if (!found) {
                error = "cut " + cn + ": unknown load part " + r;
                return false;
            }
        }
    }
    supplyCut_.assign(static_cast<std::size_t>(N), 0);
    for (int i = 0; i < N; ++i) supplyCut_[static_cast<std::size_t>(i)] = cut_[static_cast<std::size_t>(i)] && !explicitCut[static_cast<std::size_t>(i)];
    auto railish = [&](int nd) { return nd == 0 || nodeKind_[static_cast<std::size_t>(nd)] == kKnown || (cut_[static_cast<std::size_t>(nd)] && !explicitCut[static_cast<std::size_t>(nd)]); };
    // Supply-like = every external pin of the element's part is ground, a source or a supply cut (internal
    // expansion nodes such as a diode's series-resistance node don't count).
    auto partSupply = [&](int part) {
        if (part < 0) return false;
        for (const auto& nm : n.parts[static_cast<std::size_t>(part)].nodes)
            if (!railish(node(nm))) return false;
        return true;
    };
    auto primSupply = [&](const Prim& p) {
        if (p.part >= 0) return partSupply(p.part);
        for (int t : {p.a, p.b, p.c, p.d})
            if (!railish(t)) return false;
        return true;
    };
    auto devSupply = [&](const NlDevice& d) { return partSupply(d.part); };
    // ── supply-cut stability: a supply cut is an explicit one-sample lag. The rail's change in response to its loads'
    // current, Z_owner·Y_loads per sample, is a loop gain that must stay well below 1. A rail held by a reservoir cap has
    // Z_owner ≈ T/2C (milliohms), but a cap-less rail behind a dropping resistor does not: the DR103's screen rail (470 Ω
    // to four 100 Ω screen stoppers) has Z·Y ≈ 19 and limit-cycles at ~35 kHz as soon as the screens draw (docs §2.6).
    // Such nodes are demoted to ordinary nodes here (solved implicitly with their loads, exact); the rail's other nodes
    // keep their cuts. Z_owner is the instantaneous driving-point impedance of the supply network (trapezoidal companions
    // at this rate; rectifier diodes open), Y_loads the bound Σ 1/R of the load resistors on the node. Load capacitors
    // are left out (a decoupling cap into a floating consumer node is no conductance; counted as 2C/T it flags every
    // decoupled rail), and so are device ports wired straight to a rail (no bound before the operating point).
    // No reservoir (2026-11-10): a loaded supply cut whose supply caps do not at least halve Z_owner (Z with the caps
    // open) lags against its loads' full admittance, whatever the bound says: the 1176's +30 V rail (an ideal source
    // behind 2 Ω, transistor collectors on it, bound 0.02) grew an idle mode with tau ≈ 32 s through the rail and the
    // pre_ac signal cut, reaching Newton failures after ~17 min (stable uncut, even behind 30 Ω). Such a cut keeps its
    // partition but its owner carries the loads' estimate like a signal cut (G of the load resistors ∥ C_hf, below), so
    // only the residual lags. Demoting it instead cost the 1176 2.7x CPU and left the Small Stone's merged 60-port group
    // without a DC solution. Device ports and load caps count as loads here.
    std::vector<std::uint8_t> estimateCut(static_cast<std::size_t>(N), 0); // no-reservoir supply cuts (see above)
    if (!n.cuts.empty()) {
        ctlCur_.assign(n.controls.size(), 0.5); // slot values at the control defaults (reassigned below)
        for (std::size_t c = 0; c < n.controls.size(); ++c) ctlCur_[c] = std::clamp(n.controls[c].def, 0.0, 1.0);
        const double Tc = (opt.multirate ? 2.0 : 1.0) / fs_; // a supply group runs at the host rate in multirate circuits
        const char* resEnv = std::getenv("CE_CUT_RESERVOIR"); // developer A/B: "0" = the gain bound only (pre 2026-11-10)
        const bool reservoirRule = !(resEnv && std::string_view(resEnv) == "0");
        auto companionG = [&](const Prim& p) {
            const double val = p.slot >= 0 ? slotValue(slots_[static_cast<std::size_t>(p.slot)]) : p.fixed;
            if (!(val > 0)) return 0.0;
            if (p.kind == Prim::G) return 1.0 / val;
            if (p.kind == Prim::Cap) return (p.be ? 1.0 : 2.0) * val / Tc;
            if (p.kind == Prim::Ind) return Tc / ((p.be ? 1.0 : 2.0) * val);
            return 0.0;
        };
        // The bound below counts each load resistor at the cut node as if its far end were grounded. A load that is only
        // the first element of a series path (a rectifier's series Rs, then its junction, then the winding resistance:
        // the SVT's bias rail) is far stiffer than the path. When the bound fails, the loads' admittance is measured
        // through their own network instead: every load element beyond the cut node (resistors, inductor and capacitor
        // companions, ideal windings as shorts (their incremental impedance is the reflected source, >= 0) and device
        // ports as shorts (a junction or tube at its largest conductance)), every other rail, source and ideal output at
        // AC ground, the cut node at 1 V. At the cut node itself only the resistors count, as in the bound (a device port
        // wired to the rail has no bound before the operating point; a load capacitor there is no conductance). The
        // network is passive, so each far end sits between 0 and 1 V: the result never exceeds the bound, and it equals
        // it where every load resistor ends on ground. CE_CUTLOADNET=0: the bound alone (before 2026-11-11).
        const char* lnEnv = std::getenv("CE_CUTLOADNET");
        const bool loadNet = !(lnEnv && std::string_view(lnEnv) == "0");
        auto loadNetworkY = [&](int x) {
            UnionFind cls(N);
            for (const auto& d : devices_) {
                if (devSupply(d)) continue;
                for (int k = 0; k < d.nports; ++k) {
                    const auto& pt = d.port[k];
                    if (pt.from == x || pt.to == x || pt.cp == x || pt.cn == x) continue;
                    cls.unite(pt.from, pt.to);
                }
            }
            for (const auto& p : prims_)
                if (p.kind == Prim::Winding && !primSupply(p) && p.a != x && p.b != x) cls.unite(p.a, p.b);
            std::vector<std::uint8_t> gnd(static_cast<std::size_t>(N), 0);
            for (int nd = 0; nd < N; ++nd)
                if (nd != x && (nd == 0 || nodeKind_[static_cast<std::size_t>(nd)] != kFree || railish(nd))) gnd[static_cast<std::size_t>(cls.find(nd))] = 1;
            const int cx = cls.find(x);
            if (gnd[static_cast<std::size_t>(cx)]) return 0.0; // (not reached: nothing at x is shorted)
            std::vector<const Prim*> elems; // load elements, each between two classes
            for (const auto& p : prims_) {
                if (!(p.kind == Prim::G || p.kind == Prim::Cap || p.kind == Prim::Ind) || primSupply(p)) continue;
                if ((p.a == x || p.b == x) && p.kind != Prim::G) continue;
                if (cls.find(p.a) == cls.find(p.b)) continue;
                elems.push_back(&p);
            }
            std::map<int, int> loc; // unknown class -> local index, reached from x through load elements
            std::vector<int> todo{cx};
            std::set<int> seen{cx};
            while (!todo.empty()) {
                const int c = todo.back();
                todo.pop_back();
                for (const Prim* p : elems) {
                    const int ca = cls.find(p->a), cb = cls.find(p->b);
                    const int o = ca == c ? cb : (cb == c ? ca : -1);
                    if (o < 0 || seen.count(o)) continue;
                    seen.insert(o);
                    if (gnd[static_cast<std::size_t>(o)] || o == cx) continue;
                    loc.emplace(o, static_cast<int>(loc.size()));
                    todo.push_back(o);
                }
            }
            const int m = static_cast<int>(loc.size());
            std::vector<double> A(static_cast<std::size_t>(m) * static_cast<std::size_t>(m), 0.0), rhs(static_cast<std::size_t>(m), 0.0);
            for (int i = 0; i < m; ++i) A[static_cast<std::size_t>(i * m + i)] = 1e-12; // a cluster with no path to ground
            std::vector<std::pair<int, double>> atX; // (far class, G) of the elements at x
            for (const Prim* p : elems) {
                const int ca = cls.find(p->a), cb = cls.find(p->b);
                if (ca != cx && cb != cx && !loc.count(ca) && !loc.count(cb)) continue;
                const double G = companionG(*p);
                if (!(G > 0)) continue;
                const auto ia = loc.find(ca), ib = loc.find(cb);
                const int a = ia == loc.end() ? -1 : ia->second, b = ib == loc.end() ? -1 : ib->second;
                if (ca == cx || cb == cx) {
                    const int far = ca == cx ? b : a;
                    atX.push_back({far, G});
                    if (far >= 0) {
                        A[static_cast<std::size_t>(far * m + far)] += G;
                        rhs[static_cast<std::size_t>(far)] += G;
                    }
                    continue;
                }
                if (a >= 0) A[static_cast<std::size_t>(a * m + a)] += G;
                if (b >= 0) A[static_cast<std::size_t>(b * m + b)] += G;
                if (a >= 0 && b >= 0) {
                    A[static_cast<std::size_t>(a * m + b)] -= G;
                    A[static_cast<std::size_t>(b * m + a)] -= G;
                }
            }
            if (m > 0) {
                std::vector<int> piv(static_cast<std::size_t>(m));
                if (!la::luFactor(A.data(), m, piv.data())) return -1.0; // (no measurement: keep the bound)
                la::luSolve(A.data(), piv.data(), m, rhs.data());
            }
            double y = 0;
            for (const auto& [far, G] : atX) y += G * (1.0 - (far >= 0 ? std::clamp(rhs[static_cast<std::size_t>(far)], 0.0, 1.0) : 0.0));
            return y;
        };
        for (int pass = 0; pass < 8; ++pass) {
            std::map<int, int> loc; // supply-side free node -> local index
            for (const auto& p : prims_)
                if ((p.kind == Prim::G || p.kind == Prim::Cap || p.kind == Prim::Ind) && primSupply(p))
                    for (int t : {p.a, p.b})
                        if (t > 0 && nodeKind_[static_cast<std::size_t>(t)] == kFree && !loc.count(t)) loc.emplace(t, static_cast<int>(loc.size()));
            const int m = static_cast<int>(loc.size());
            if (m == 0) break;
            std::vector<double> A(static_cast<std::size_t>(m * m), 0.0);
            for (int i = 0; i < m; ++i) A[static_cast<std::size_t>(i * m + i)] = 1e-9; // keeps diode-only nodes regular
            std::vector<double> Ar = A; // the same network with its capacitors open (no reservoir)
            for (const auto& p : prims_) {
                if (!(p.kind == Prim::G || p.kind == Prim::Cap || p.kind == Prim::Ind) || !primSupply(p)) continue;
                const double G = companionG(p);
                const auto ia = loc.find(p.a), ib = loc.find(p.b);
                const int a = ia == loc.end() ? -1 : ia->second, b = ib == loc.end() ? -1 : ib->second;
                for (auto* M : {&A, &Ar}) {
                    if (M == &Ar && p.kind == Prim::Cap) continue;
                    auto& X = *M;
                    if (a >= 0) X[static_cast<std::size_t>(a * m + a)] += G;
                    if (b >= 0) X[static_cast<std::size_t>(b * m + b)] += G;
                    if (a >= 0 && b >= 0) {
                        X[static_cast<std::size_t>(a * m + b)] -= G;
                        X[static_cast<std::size_t>(b * m + a)] -= G;
                    }
                }
            }
            std::vector<int> piv(static_cast<std::size_t>(m)), pivR(static_cast<std::size_t>(m));
            if (!la::luFactor(A.data(), m, piv.data())) break;
            const bool okR = la::luFactor(Ar.data(), m, pivR.data());
            std::vector<int> demote;
            for (const auto& [x, i] : loc) {
                if (!supplyCut_[static_cast<std::size_t>(x)]) continue;
                double Y = 0;
                bool loaded = false;
                for (const auto& p : prims_) {
                    if (p.a != x && p.b != x && p.c != x && p.d != x) continue;
                    if (primSupply(p)) continue;
                    loaded = true;
                    if (p.kind == Prim::G && (p.a == x || p.b == x)) Y += companionG(p);
                }
                for (const auto& d : devices_) {
                    if (loaded || devSupply(d)) continue;
                    for (int k = 0; k < d.nports; ++k)
                        loaded |= d.port[k].from == x || d.port[k].to == x || d.port[k].cp == x || d.port[k].cn == x;
                }
                if (!loaded) continue;
                std::vector<double> e(static_cast<std::size_t>(m), 0.0);
                e[static_cast<std::size_t>(i)] = 1.0;
                la::luSolve(A.data(), piv.data(), m, e.data());
                const double z = e[static_cast<std::size_t>(i)];
                double gain = z * Y;
                if (gain > kCutLoopGainMax && loadNet) { // the bound fails: measure the loads through their network
                    const double yn = loadNetworkY(x);
                    if (std::getenv("CE_CUTDIAG"))
                        std::fprintf(stderr, "cut %-8s bound Y %.4g S fails (loop gain %.3g); through the load network Y %.4g S, loop gain %.3g\n",
                                     nodeNames_[static_cast<std::size_t>(x)].c_str(), Y, gain, yn, z * yn);
                    if (yn >= 0) gain = z * std::min(Y, yn);
                }
                double zr = 0;
                if (okR) {
                    std::vector<double> er(static_cast<std::size_t>(m), 0.0);
                    er[static_cast<std::size_t>(i)] = 1.0;
                    la::luSolve(Ar.data(), pivR.data(), m, er.data());
                    zr = er[static_cast<std::size_t>(i)];
                }
                const bool noReservoir = reservoirRule && okR && zr > 0 && z > kCutReservoirMax * zr;
                if (std::getenv("CE_CUTDIAG"))
                    std::fprintf(stderr, "cut %-8s Z_owner %10.4g ohm (caps open %10.4g)  Y_loads %10.4g S  loop gain %.3g%s\n",
                                 nodeNames_[static_cast<std::size_t>(x)].c_str(), z, zr, Y, gain, noReservoir ? "  NO RESERVOIR" : "");
                if (gain > kCutLoopGainMax) demote.push_back(x);
                else if (noReservoir && !estimateCut[static_cast<std::size_t>(x)]) {
                    estimateCut[static_cast<std::size_t>(x)] = 1;
                    warnings_.push_back("supply cut " + nodeNames_[static_cast<std::size_t>(x)] +
                                        " has no reservoir capacitance: its owner carries an estimate of the loads and only the residual lags "
                                        "(docs/CIRCUIT_ENGINE.md section 2.6)");
                }
            }
            if (demote.empty()) break;
            for (int x : demote) {
                cut_[static_cast<std::size_t>(x)] = 0;
                supplyCut_[static_cast<std::size_t>(x)] = 0;
                estimateCut[static_cast<std::size_t>(x)] = 0;
                warnings_.push_back("supply cut " + nodeNames_[static_cast<std::size_t>(x)] +
                                    " was solved implicitly instead: it has no reservoir capacitance, so the one-sample lag would be unstable "
                                    "against its loads (docs/CIRCUIT_ENGINE.md section 2.6)");
            }
        }
    }
    // ── exact partition: union-find over free nodes along conductive elements ──
    auto isFreeNode = [&](int nd) { return nodeKind_[static_cast<std::size_t>(nd)] == kFree; };
    const Prim* curPrim = nullptr;
    const NlDevice* curDev = nullptr;
    // Is this element a load of cut node nd (sees it as a known voltage)?
    auto primLoadAt = [&](const Prim& p, int nd) {
        if (explicitCut[static_cast<std::size_t>(nd)]) return loadsAt[static_cast<std::size_t>(nd)].count(p.part) > 0;
        return !primSupply(p);
    };
    auto devLoadAt = [&](const NlDevice& d, int nd) {
        if (explicitCut[static_cast<std::size_t>(nd)]) return loadsAt[static_cast<std::size_t>(nd)].count(d.part) > 0;
        return !devSupply(d);
    };
    // A cut node is free only for its owning side; for its loads it behaves like a known node.
    auto isFree = [&](int nd) {
        if (!isFreeNode(nd)) return false;
        if (!cut_[static_cast<std::size_t>(nd)]) return true;
        if (curPrim) return !primLoadAt(*curPrim, nd);
        if (curDev) return !devLoadAt(*curDev, nd);
        return true;
    };
    UnionFind uf(N);
    for (const auto& p : prims_) {
        curPrim = &p;
        if (p.kind == Prim::G || p.kind == Prim::Cap || p.kind == Prim::Ind || p.kind == Prim::Vccs)
            if (isFree(p.a) && isFree(p.b)) uf.unite(p.a, p.b);
        if (p.kind == Prim::Winding) {
            int first = -1;
            for (int t : {p.a, p.b, p.c})
                if (isFree(t)) {
                    if (first < 0) first = t;
                    else uf.unite(first, t);
                }
        }
    }
    curPrim = nullptr;
    for (const auto& d : devices_) {
        curDev = &d;
        int first = -1;
        for (int k = 0; k < d.nports; ++k)
            for (int t : {d.port[k].from, d.port[k].to})
                if (isFree(t)) {
                    if (first < 0) first = t;
                    else uf.unite(first, t);
                }
    }
    curDev = nullptr;
    std::vector<int> blockOf(static_cast<std::size_t>(N), -1);
    int nBlocks = 0;
    {
        std::map<int, int> rootBlock;
        for (int i = 1; i < N; ++i) {
            if (isFree(i)) {
                const int r = uf.find(i);
                auto it = rootBlock.find(r);
                if (it == rootBlock.end()) it = rootBlock.emplace(r, nBlocks++).first;
                blockOf[static_cast<std::size_t>(i)] = it->second;
            } else if (nodeKind_[static_cast<std::size_t>(i)] == kDriven) {
                blockOf[static_cast<std::size_t>(i)] = nBlocks++;
            }
        }
    }
    // Element ownership.
    std::vector<int> primBlock(prims_.size(), -1), devBlock(devices_.size(), -1);
    for (std::size_t i = 0; i < prims_.size(); ++i) {
        const auto& p = prims_[i];
        curPrim = &p;
        if (p.kind == Prim::Vcvs || p.kind == Prim::Nullor) primBlock[i] = blockOf[static_cast<std::size_t>(p.a)];
        else if (p.kind == Prim::Winding && isFree(p.c)) primBlock[i] = blockOf[static_cast<std::size_t>(p.c)];
        else if (isFree(p.a)) primBlock[i] = blockOf[static_cast<std::size_t>(p.a)];
        else if (isFree(p.b)) primBlock[i] = blockOf[static_cast<std::size_t>(p.b)];
        else if (p.part >= 0 && (p.a != 0 || p.b != 0)) // (both ends grounded: an open contact left out, openSwitches)
            warnings_.push_back(n.parts[static_cast<std::size_t>(p.part)].ref + " only carries current between sources (no voltage effect)");
    }
    curPrim = nullptr;
    for (std::size_t i = 0; i < devices_.size(); ++i) {
        const auto& d = devices_[i];
        curDev = &d;
        for (int k = 0; k < d.nports && devBlock[i] < 0; ++k)
            for (int t : {d.port[k].from, d.port[k].to})
                if (isFree(t)) devBlock[i] = blockOf[static_cast<std::size_t>(t)];
        if (devBlock[i] < 0) {
            // A device whose only circuit node is a cut node would be silently lost (its current never reaches the
            // rail): the netlist must keep that node uncut (or name the device's owner side).
            for (int k = 0; k < d.nports; ++k)
                for (int t : {d.port[k].from, d.port[k].to})
                    if (t > 0 && cut_[static_cast<std::size_t>(t)]) {
                        error = "part " + n.parts[static_cast<std::size_t>(d.part)].ref + ": a nonlinear port whose only circuit node is the cut node " +
                                nodeNames_[static_cast<std::size_t>(t)] + " would carry no current; keep that node out of \"cuts\"";
                        return false;
                    }
            warnings_.push_back(n.parts[static_cast<std::size_t>(d.part)].ref + " is connected only to sources (ignored)");
        }
    }
    curDev = nullptr;
    // Dependencies.
    std::vector<std::set<int>> deps(static_cast<std::size_t>(nBlocks));
    auto dep = [&](int from, int nd) {
        if (from < 0 || nd <= 0) return;
        const int b = blockOf[static_cast<std::size_t>(nd)];
        if (b >= 0 && b != from) deps[static_cast<std::size_t>(from)].insert(b);
    };
    // Delay cuts: their loads read the node's value from before the owner's solve this sample (the previous sample
    // when the cut closes a feedback loop), so they add no dependency and break the cycle.
    std::vector<std::uint8_t> delayCut(static_cast<std::size_t>(N), 0);
    for (const auto& dn : n.delayCuts)
        if (const int id = node(dn); id > 0) delayCut[static_cast<std::size_t>(id)] = 1, hasDelayCuts_ = true;
    for (std::size_t i = 0; i < prims_.size(); ++i) {
        const auto& p = prims_[i];
        for (int t : {p.a, p.b, p.c, p.d})
            if (!(t > 0 && delayCut[static_cast<std::size_t>(t)] && primLoadAt(p, t))) dep(primBlock[i], t);
    }
    for (std::size_t i = 0; i < devices_.size(); ++i)
        for (int k = 0; k < devices_[i].nports; ++k) {
            const auto& pt = devices_[i].port[k];
            for (int t : {pt.cp, pt.cn, pt.from, pt.to})
                if (!(t > 0 && delayCut[static_cast<std::size_t>(t)] && devLoadAt(devices_[i], t))) dep(devBlock[i], t);
        }
    // Tarjan SCC (emits dependencies first = evaluation order).
    std::vector<int> order, low(static_cast<std::size_t>(nBlocks), 0), num(static_cast<std::size_t>(nBlocks), -1), comp(static_cast<std::size_t>(nBlocks), -1);
    std::vector<int> stack;
    std::vector<char> onStack(static_cast<std::size_t>(nBlocks), 0);
    int counter = 0, nComp = 0;
    std::function<void(int)> strong = [&](int v) {
        num[static_cast<std::size_t>(v)] = low[static_cast<std::size_t>(v)] = counter++;
        stack.push_back(v);
        onStack[static_cast<std::size_t>(v)] = 1;
        for (int w : deps[static_cast<std::size_t>(v)]) {
            if (num[static_cast<std::size_t>(w)] < 0) {
                strong(w);
                low[static_cast<std::size_t>(v)] = std::min(low[static_cast<std::size_t>(v)], low[static_cast<std::size_t>(w)]);
            } else if (onStack[static_cast<std::size_t>(w)]) {
                low[static_cast<std::size_t>(v)] = std::min(low[static_cast<std::size_t>(v)], num[static_cast<std::size_t>(w)]);
            }
        }
        if (low[static_cast<std::size_t>(v)] == num[static_cast<std::size_t>(v)]) {
            for (;;) {
                const int w = stack.back();
                stack.pop_back();
                onStack[static_cast<std::size_t>(w)] = 0;
                comp[static_cast<std::size_t>(w)] = nComp;
                if (w == v) break;
            }
            ++nComp;
        }
    };
    for (int b = 0; b < nBlocks; ++b)
        if (num[static_cast<std::size_t>(b)] < 0) strong(b);
    groups_.assign(static_cast<std::size_t>(nComp), Group{});
    std::vector<int> groupOfNode(static_cast<std::size_t>(N), -1);
    for (int i = 1; i < N; ++i) {
        const int b = blockOf[static_cast<std::size_t>(i)];
        if (b < 0) continue;
        const int g = comp[static_cast<std::size_t>(b)];
        groups_[static_cast<std::size_t>(g)].unknowns.push_back(i);
        groupOfNode[static_cast<std::size_t>(i)] = g;
    }
    for (std::size_t i = 0; i < prims_.size(); ++i)
        if (primBlock[i] >= 0) {
            auto& g = groups_[static_cast<std::size_t>(comp[static_cast<std::size_t>(primBlock[i])])];
            g.prims.push_back(static_cast<int>(i));
            if (prims_[i].kind == Prim::Cap || prims_[i].kind == Prim::Ind) g.states.push_back(static_cast<int>(i));
            if (prims_[i].kind == Prim::Winding) g.branches.push_back(static_cast<int>(i));
        }
    for (std::size_t i = 0; i < devices_.size(); ++i)
        if (devBlock[i] >= 0) {
            auto& g = groups_[static_cast<std::size_t>(comp[static_cast<std::size_t>(devBlock[i])])];
            g.devices.push_back(static_cast<int>(i));
            if (devices_[i].kind == NlKind::Magnetizing && devices_[i].p[2] > 0) g.hyst.push_back({static_cast<int>(i), g.nn});
            if (devices_[i].kind == NlKind::Speaker)
                for (std::size_t t = 0; t < spkTh_.size(); ++t)
                    if (spkTh_[t].dev == static_cast<int>(i)) g.spk.push_back({static_cast<int>(t), g.nn});
            g.nn += devices_[i].nports;
        }
    for (std::size_t gi = 0; gi < groups_.size(); ++gi)
        if (groups_[gi].nn > kMaxNlPorts) {
            std::set<int> refs;
            for (int di : groups_[gi].devices) refs.insert(devices_[static_cast<std::size_t>(di)].part);
            std::string list;
            for (int r : refs)
                if (r >= 0) list += (list.empty() ? "" : " ") + n.parts[static_cast<std::size_t>(r)].ref;
            error = "a solve group has " + std::to_string(groups_[gi].nn) + " coupled nonlinear ports (limit " + std::to_string(kMaxNlPorts) + "): add a buffer or split the circuit (devices: " + list + ")";
            return false;
        }
    // Inputs and which nodes other groups read.
    std::vector<char> readOutside(static_cast<std::size_t>(N), 0);
    for (std::size_t gi = 0; gi < groups_.size(); ++gi) {
        auto& g = groups_[gi];
        std::set<int> in;
        auto ref = [&](int nd) {
            if (nd <= 0) return;
            if (groupOfNode[static_cast<std::size_t>(nd)] == static_cast<int>(gi)) return;
            in.insert(nd);
            readOutside[static_cast<std::size_t>(nd)] = 1;
        };
        for (int pi : g.prims) {
            const auto& p = prims_[static_cast<std::size_t>(pi)];
            for (int t : {p.a, p.b, p.c, p.d}) ref(t);
        }
        for (int di : g.devices)
            for (int k = 0; k < devices_[static_cast<std::size_t>(di)].nports; ++k) {
                const auto& pt = devices_[static_cast<std::size_t>(di)].port[k];
                for (int t : {pt.cp, pt.cn, pt.from, pt.to}) ref(t);
            }
        g.inputs.assign(in.begin(), in.end());
        std::set<int> parts;
        for (int pi : g.prims)
            if (prims_[static_cast<std::size_t>(pi)].part >= 0) parts.insert(prims_[static_cast<std::size_t>(pi)].part);
        for (int di : g.devices) parts.insert(devices_[static_cast<std::size_t>(di)].part);
        g.parts.assign(parts.begin(), parts.end());
    }
    // Consumer current terms at cut nodes, and the supply groups that receive them.
    std::set<int> forceExport;
    for (const auto& b : blocks_) {
        forceExport.insert(b.senseA);
        forceExport.insert(b.senseB);
    }
    for (const auto& b : bbds_) {
        forceExport.insert(b.in);
        if (b.ctl > 0) forceExport.insert(b.ctl);
    }
    for (const auto& t : tapes_) {
        forceExport.insert(t.recA);
        forceExport.insert(t.recX);
    }
    for (const auto& c : logic_) { // CMOS cells read their pins and supply (DC levels, thresholds, delays)
        for (int k = 0; k < c.nin; ++k) forceExport.insert(c.in[k]);
        forceExport.insert(c.vdd);
        forceExport.insert(c.vss);
    }
    {
        std::set<int> withConsumers;
        std::vector<std::uint8_t> probeOut(static_cast<std::size_t>(N), 0); // rail-node op-amp outputs: load current measured
        for (const auto& ra : railAmps_) {
            probeOut[static_cast<std::size_t>(ra.out)] = 1;
            for (int rn : {ra.vp, ra.vn})
                if (rn > 0 && !cut_[static_cast<std::size_t>(rn)]) {
                    error = "op-amp supply pin " + nodeNames_[static_cast<std::size_t>(rn)] +
                            " is a circuit node: list it in \"cuts\" (the op-amp draws its current from it one sample late)";
                    return false;
                }
        }
        for (auto& g : groups_) {
            int stateIdx = 0;
            (void)stateIdx;
            // A load that ended up in the cut node's own group (the cut did not split the solve: another conductive path
            // joins them) is already stamped two-sided there, so a consumer term would count its current twice (and
            // the companion conductance a third time). Only loads in other groups draw through the cut.
            const int gHere = static_cast<int>(&g - groups_.data());
            auto sameGroup = [&](int x) { return x > 0 && groupOfNode[static_cast<std::size_t>(x)] == gHere; };
            for (int pi : g.prims) {
                const auto& p = prims_[static_cast<std::size_t>(pi)];
                for (int side = 0; side < 2; ++side) {
                    const int x = side == 0 ? p.a : p.b, y = side == 0 ? p.b : p.a;
                    const bool probe = x > 0 && probeOut[static_cast<std::size_t>(x)] && p.kind != Prim::Vcvs && p.kind != Prim::Nullor;
                    if (!probe && (x <= 0 || !cut_[static_cast<std::size_t>(x)] || !primLoadAt(p, x) || sameGroup(x))) continue;
                    Group::CutTerm t{};
                    t.node = x;
                    t.other = y;
                    t.sign = side == 0 ? 1.0 : -1.0;
                    t.index = pi;
                    if (p.kind == Prim::G) t.kind = 0;
                    else if (p.kind == Prim::Cap || p.kind == Prim::Ind) {
                        t.kind = p.kind == Prim::Cap ? 1 : 2;
                        t.state = static_cast<int>(std::find(g.states.begin(), g.states.end(), pi) - g.states.begin());
                    } else if (p.kind == Prim::Vccs) {
                        t.kind = 4;
                        for (int cn : {p.c, p.d})
                            if (cn > 0 && nodeKind_[static_cast<std::size_t>(cn)] != kKnown) forceExport.insert(cn);
                    } else {
                        error = "only resistors, capacitors, inductors and devices may draw from cut node " + nodeNames_[static_cast<std::size_t>(x)] +
                                " (add a winding resistance to transformer windings there)";
                        return false;
                    }
                    if (y > 0 && nodeKind_[static_cast<std::size_t>(y)] != kKnown) forceExport.insert(y);
                    if (probe) forceExport.insert(x);
                    g.cutTerms.push_back(t);
                    if (!probe) withConsumers.insert(x);
                }
            }
            int o = 0;
            for (int di : g.devices) {
                const auto& d = devices_[static_cast<std::size_t>(di)];
                for (int k = 0; k < d.nports; ++k)
                        for (int side = 0; side < 2; ++side) {
                            const int x = side == 0 ? d.port[k].from : d.port[k].to;
                            const bool probe = x > 0 && probeOut[static_cast<std::size_t>(x)];
                            if (!probe && (x <= 0 || !cut_[static_cast<std::size_t>(x)] || !devLoadAt(d, x) || sameGroup(x))) continue;
                            g.cutTerms.push_back({3, o + k, 0, x, 0, side == 0 ? 1.0 : -1.0});
                            if (!probe) withConsumers.insert(x);
                        }
                o += d.nports;
            }
        }
        for (const auto& ra : railAmps_) // rail-node op-amps draw from their rails (railDraws) even with no other load there
            for (int rn : {ra.vp, ra.vn})
                if (rn > 0) withConsumers.insert(rn);
        for (int x : withConsumers)
            groups_[static_cast<std::size_t>(groupOfNode[static_cast<std::size_t>(x)])].cutIn.push_back(x);
    }
    // DC interface conductance of each cut node's resistive loads (Newton-style DC coupling, see reset()).
    // A resistor whose far end reaches nothing but junctions (a diode's series resistance, an emitter or base lead)
    // is in series with a junction, not a load of 1/R: counting it as one (a 0.65 Ω diode Rs on a bias rail) turns the
    // DC interface iteration into a crawl. Such resistors are left out of the estimate.
    dcLoadG_.assign(static_cast<std::size_t>(N), 0.0);
    {
        std::vector<int> dcLinks(static_cast<std::size_t>(N), 0);
        for (const auto& p : prims_)
            if (p.kind == Prim::G || p.kind == Prim::Ind || p.kind == Prim::Winding) {
                ++dcLinks[static_cast<std::size_t>(p.a)];
                ++dcLinks[static_cast<std::size_t>(p.b)];
            }
        std::vector<std::uint8_t> nonJunction(static_cast<std::size_t>(N), 0);
        for (const auto& d : devices_) {
            const bool junction = d.kind == NlKind::Diode || d.kind == NlKind::DiodePair || d.kind == NlKind::Bjt || d.kind == NlKind::Jfet || d.kind == NlKind::Zener ||
                                  d.kind == NlKind::ZenerPair;
            if (junction) continue;
            for (int k = 0; k < d.nports; ++k) {
                nonJunction[static_cast<std::size_t>(d.port[k].from)] = 1;
                nonJunction[static_cast<std::size_t>(d.port[k].to)] = 1;
            }
        }
        for (const auto& g : groups_)
            for (const auto& t : g.cutTerms)
                if (t.kind == 0) {
                    const auto& p = prims_[static_cast<std::size_t>(t.index)];
                    const auto far = static_cast<std::size_t>(t.other);
                    if (t.other > 0 && nodeKind_[far] == kFree && dcLinks[far] == 1 && !nonJunction[far] && !cut_[far]) continue;
                    const double val = p.slot >= 0 ? slotValue(slots_[static_cast<std::size_t>(p.slot)]) : p.fixed;
                    if (val > 0) dcLoadG_[static_cast<std::size_t>(t.node)] += 1.0 / val;
                }
    }
    // Floating-node check: every free unknown needs a conductive element in its group.
    for (auto& g : groups_) {
        for (int u : g.unknowns) {
            if (nodeKind_[static_cast<std::size_t>(u)] != kFree) continue;
            bool touched = false;
            for (int pi : g.prims) {
                const auto& p = prims_[static_cast<std::size_t>(pi)];
                if ((p.kind == Prim::G || p.kind == Prim::Cap || p.kind == Prim::Ind || p.kind == Prim::Vccs) && (p.a == u || p.b == u)) touched = true;
                if (p.kind == Prim::Winding && (p.a == u || p.b == u || p.c == u)) touched = true;
            }
            for (int di : g.devices)
                for (int k = 0; k < devices_[static_cast<std::size_t>(di)].nports; ++k)
                    if (devices_[static_cast<std::size_t>(di)].port[k].from == u || devices_[static_cast<std::size_t>(di)].port[k].to == u) touched = true;
            if (!touched) {
                error = "node " + nodeNames_[static_cast<std::size_t>(u)] + " is only sensed: nothing conducts to it";
                return false;
            }
        }
    }
    retired_.reserve(2 * (blocks_.size() + bbds_.size() + tapes_.size()) + 2); // see adoptStateFrom
    // Cut diagnostics (ENGINE_QUEUE item 2): a signal cut only splits a solve when the cut node is the sole path
    // between its owner and its loads. Say so when a cut changed nothing, so an author doesn't believe a group was
    // split (or a port count reduced) when other conductive paths keep it whole.
    {
        std::vector<int> groupOfPart(n.parts.size(), -1);
        for (std::size_t gi = 0; gi < groups_.size(); ++gi) {
            for (int pi : groups_[gi].prims) {
                const int part = prims_[static_cast<std::size_t>(pi)].part;
                if (part >= 0 && groupOfPart[static_cast<std::size_t>(part)] < 0) groupOfPart[static_cast<std::size_t>(part)] = static_cast<int>(gi);
            }
            for (int di : groups_[gi].devices) {
                const int part = devices_[static_cast<std::size_t>(di)].part;
                if (part >= 0 && groupOfPart[static_cast<std::size_t>(part)] < 0) groupOfPart[static_cast<std::size_t>(part)] = static_cast<int>(gi);
            }
        }
        for (const auto& [cn, refs] : n.cutLoads) {
            const int id = node(cn);
            if (id <= 0) continue;
            int owner = -1;
            for (std::size_t gi = 0; gi < groups_.size() && owner < 0; ++gi)
                for (int u : groups_[gi].unknowns)
                    if (u == id) owner = static_cast<int>(gi);
            bool split = false;
            for (const auto& r : refs)
                for (std::size_t pi = 0; pi < n.parts.size(); ++pi)
                    if (n.parts[pi].ref == r && groupOfPart[pi] >= 0 && groupOfPart[pi] != owner) split = true;
            if (!split)
                warnings_.push_back("cut " + cn + " did not split its solve: its loads stay in the same group as the node "
                                    "(another conductive path joins them), so it removes no unknowns or nonlinear ports");
        }
    }
    // Exports.
    exported_.assign(static_cast<std::size_t>(N), 0);
    std::set<int> probeNodes;
    std::vector<std::string> noProbes;
    for (const auto* list : {opt.exportNetlistProbes ? &n.probes : &noProbes, &opt.probes})
        for (const auto& pr : *list) {
            const int id = node(pr);
            if (id > 0) probeNodes.insert(id);
            else warnings_.push_back("probe " + pr + " is not a node");
        }
    if (opt.settleMs > 0 && mainsCore()) { // settlePeriodic reads the rectifier outputs' voltages
        Netlist averaged;
        std::vector<std::string> rect;
        if (mainsAveragedNetlist(n, averaged, rect))
            for (const auto& r : rect)
                if (node(r) > 0) probeNodes.insert(node(r));
    } else if (opt.settleMs > 0 && kEnvSlowSettle && !acSrc_.empty()) { // ...and so does settleSlowRails (docs §10.49)
        Netlist averaged;
        std::vector<std::string> rect;
        if (mainsAveragedNetlist(n, averaged, rect, true))
            for (const auto& r : rect)
                if (node(r) > 0) probeNodes.insert(node(r));
    }
    for (auto& g : groups_) {
        for (int u : g.unknowns)
            if (opt.exportAllNodes || readOutside[static_cast<std::size_t>(u)] || u == outputNode_ || u == outputNodeR_ || probeNodes.count(u) || forceExport.count(u)) {
                g.exports.push_back(u);
                exported_[static_cast<std::size_t>(u)] = 1;
            }
    }
    if (nodeKind_[static_cast<std::size_t>(outputNode_)] == kKnown) exported_[static_cast<std::size_t>(outputNode_)] = 1;
    if (!exported_[static_cast<std::size_t>(outputNode_)]) {
        error = "output node " + n.io.output + " is not solved by any group";
        return false;
    }
    if (outputNodeR_ > 0) {
        if (nodeKind_[static_cast<std::size_t>(outputNodeR_)] == kKnown) exported_[static_cast<std::size_t>(outputNodeR_)] = 1;
        if (!exported_[static_cast<std::size_t>(outputNodeR_)]) {
            error = "outputR node " + n.io.outputR + " is not solved by any group";
            return false;
        }
    }
    // Signal cuts: the owner carries an estimate of the loads' admittance implicitly and only the residual
    // current i_load(n-1) - i_est(n-1) lags a sample. The plain lag is a loop of gain Y_load·Z_owner that must stay
    // below 1 up to Nyquist; with trapezoidal companions a capacitive load path to a fixed node (a coupling cap
    // into a tube grid with its Cgk to a bypassed cathode, say) has Y → ∞ there, and the loop rings at fs/2.
    // The estimate is G (the loads' direct resistors) ∥ C_hf (the loads' high-frequency capacitance, the Schur
    // complement of their capacitor network seen from the cut node), so the residual |Y - Y_est| / |Y_owner + Y_est|
    // stays a contraction at both ends of the band. The C_hf part is a synthetic owner capacitor whose own
    // current is subtracted through the cut-term machinery; G enters the owner matrix directly (tranLoadG_).
    tranLoadG_.assign(static_cast<std::size_t>(N), 0.0);
    {
        std::map<int, double> cHf;
        for (const auto& g : groups_) {
            std::set<int> here;
            for (const auto& t : g.cutTerms) {
                if (!explicitCut[static_cast<std::size_t>(t.node)] && !estimateCut[static_cast<std::size_t>(t.node)]) continue;
                here.insert(t.node);
                if (t.kind != 0) continue;
                const auto& p = prims_[static_cast<std::size_t>(t.index)];
                const double val = p.slot >= 0 ? slotValue(slots_[static_cast<std::size_t>(p.slot)]) : p.fixed;
                if (val > 0) tranLoadG_[static_cast<std::size_t>(t.node)] += 1.0 / val;
            }
            if (here.empty()) continue;
            const int nu = static_cast<int>(g.unknowns.size());
            std::map<int, int> loc;
            for (int i = 0; i < nu; ++i) loc[g.unknowns[static_cast<std::size_t>(i)]] = i;
            std::vector<double> Cm(static_cast<std::size_t>(nu * nu), 0.0);
            for (int pi : g.states) {
                const auto& p = prims_[static_cast<std::size_t>(pi)];
                if (p.kind != Prim::Cap) continue;
                const double c = p.slot >= 0 ? slotValue(slots_[static_cast<std::size_t>(p.slot)]) : p.fixed;
                const auto ia = loc.find(p.a), ib = loc.find(p.b);
                const int a = ia == loc.end() ? -1 : ia->second, b = ib == loc.end() ? -1 : ib->second;
                if (a >= 0) Cm[static_cast<std::size_t>(a * nu + a)] += c;
                if (b >= 0) Cm[static_cast<std::size_t>(b * nu + b)] += c;
                if (a >= 0 && b >= 0) {
                    Cm[static_cast<std::size_t>(a * nu + b)] -= c;
                    Cm[static_cast<std::size_t>(b * nu + a)] -= c;
                }
            }
            // Nodes without capacitance of their own are held by a small capacitor to ground on every diagonal: 1e-9 × the
            // group's largest capacitance, at most kChfStray (2026-11-13, docs §10.40). Without the cap, a group holding an
            // output transformer (the core's 1 F flux node) or a speaker (Le / Mms / excursion states) put a ~1 nF hold
            // behind every coupling cap: C_hf came out 50 pF … 1.8 nF where the load network's own is ~0 (a coupling cap
            // into a resistive node follows x). Zero is no good either: with G alone the lag runs away at Nyquist when the
            // owner's impedance rises (the 5F6A's and AC15's dimed sines: hundreds of PTC rescues). kChfStray is a
            // wiring-scale stray capacitance; every group without such states keeps its hold (≤ 0.5 pF), bit-identical.
            // A cut marked "chfHold": "group" keeps the uncapped hold (its own measurements were worse with the cap).
            // CE_CUT_CHF=0 restores the uncapped hold on every cut, CE_CUT_CHF=1 ignores the per-cut marks (developer A/B).
            constexpr double kChfStray = 10e-12;
            const char* chfEnv = std::getenv("CE_CUT_CHF");
            const bool chfAllGroup = chfEnv && std::string_view(chfEnv) == "0", chfNoMarks = chfEnv && std::string_view(chfEnv) == "1";
            double cmax = 0;
            for (int i = 0; i < nu; ++i) cmax = std::max(cmax, Cm[static_cast<std::size_t>(i * nu + i)]);
            const std::vector<double> Cm0 = Cm;
            std::vector<int> piv(static_cast<std::size_t>(std::max(nu, 1)));
            double heldBy = -1; // the hold Cm is factored with
            bool okLu = false;
            for (int x : here) {
                const bool legacy = chfAllGroup || (!chfNoMarks && std::find(n.chfGroupHold.begin(), n.chfGroupHold.end(),
                                                                               nodeNames_[static_cast<std::size_t>(x)]) != n.chfGroupHold.end());
                const double hold = legacy ? 1e-9 * cmax : std::min(1e-9 * cmax, kChfStray);
                if (hold != heldBy) {
                    Cm = Cm0;
                    for (int i = 0; i < nu; ++i) Cm[static_cast<std::size_t>(i * nu + i)] += hold + 1e-30; // cap-free nodes: held
                    okLu = nu == 0 || la::luFactor(Cm.data(), nu, piv.data());
                    heldBy = hold;
                }
                // Drive x with 1 V (other groups' nodes and ground at 0) and sum the charge the loads draw from x.
                std::vector<double> rhs(static_cast<std::size_t>(nu), 0.0);
                std::vector<std::pair<int, double>> direct; // (other node, C) for caps touching x
                for (int pi : g.states) {
                    const auto& p = prims_[static_cast<std::size_t>(pi)];
                    if (p.kind != Prim::Cap || (p.a != x && p.b != x)) continue;
                    const double c = p.slot >= 0 ? slotValue(slots_[static_cast<std::size_t>(p.slot)]) : p.fixed;
                    const int o = p.a == x ? p.b : p.a;
                    direct.emplace_back(o, c);
                    const auto io = loc.find(o);
                    if (io != loc.end()) rhs[static_cast<std::size_t>(io->second)] += c;
                }
                if (direct.empty()) continue;
                if (okLu && nu > 0) la::luSolve(Cm.data(), piv.data(), nu, rhs.data());
                double q = 0;
                for (const auto& [o, c] : direct) {
                    const auto io = loc.find(o);
                    q += c * (1.0 - (io != loc.end() && okLu ? rhs[static_cast<std::size_t>(io->second)] : 0.0));
                }
                if (std::getenv("CE_CUTDIAG"))
                    std::fprintf(stderr, "cut %-8s C_hf %.4g F (hold %.4g F%s)\n", nodeNames_[static_cast<std::size_t>(x)].c_str(), q, hold,
                                 legacy ? ", uncapped" : "");
                if (q > 0) cHf[x] += q;
            }
        }
        for (const auto& [x, c] : cHf) {
            auto& og = groups_[static_cast<std::size_t>(groupOfNode[static_cast<std::size_t>(x)])];
            Prim p;
            p.kind = Prim::Cap;
            p.a = x;
            p.b = 0;
            p.slot = constSlot(c, -1);
            const int pi = static_cast<int>(prims_.size());
            prims_.push_back(p);
            og.prims.push_back(pi);
            og.states.push_back(pi);
            Group::CutTerm t{};
            t.kind = 1;
            t.index = pi;
            t.state = static_cast<int>(og.states.size()) - 1;
            t.node = x;
            t.other = 0;
            t.sign = -1.0; // the estimate's own current comes back out of the lagged injection
            og.cutTerms.push_back(t);
        }
    }
    // Value slots → groups / controls.
    slotGroups_.assign(slots_.size(), {});
    controlSlots_.assign(n.controls.size(), {});
    for (std::size_t gi = 0; gi < groups_.size(); ++gi) {
        std::set<int> sl;
        for (int pi : groups_[gi].prims)
            if (prims_[static_cast<std::size_t>(pi)].slot >= 0) sl.insert(prims_[static_cast<std::size_t>(pi)].slot);
        groups_[gi].slots.assign(sl.begin(), sl.end());
        for (int s : sl) slotGroups_[static_cast<std::size_t>(s)].push_back(static_cast<int>(gi));
    }
    for (std::size_t s = 0; s < slots_.size(); ++s)
        for (int c : slots_[s].controls)
            if (c >= 0) controlSlots_[static_cast<std::size_t>(c)].push_back(static_cast<int>(s));
    {
        auto findSlotByRef = [&](const std::string& ref) -> int {
            for (std::size_t s = 0; s < slots_.size(); ++s) {
                const int pi = slots_[s].part;
                if (pi >= 0 && n.parts[static_cast<std::size_t>(pi)].ref == ref) return static_cast<int>(s);
            }
            return -1;
        };
        for (std::size_t s = 0; s < slots_.size(); ++s) {
            const int pi = slots_[s].part;
            if (pi < 0) continue;
            const std::string& ref = n.parts[static_cast<std::size_t>(pi)].ref;
            std::string pfx;
            if (ref == "LCES") pfx = "";
            else if (ref.size() > 5 && ref.substr(ref.size() - 5) == ".LCES") pfx = ref.substr(0, ref.size() - 4);
            else continue;
            const int lces = static_cast<int>(s);
            const int cmes = findSlotByRef(pfx + "CMES");
            if (cmes < 0) continue;
            const double l0 = slots_[static_cast<std::size_t>(lces)].base, c0 = slots_[static_cast<std::size_t>(cmes)].base;
            if (!(l0 > 0.0 && c0 > 0.0)) continue;
            SpeakerSlots sp;
            sp.fs0 = 1.0 / (2.0 * M_PI * std::sqrt(l0 * c0));
            sp.lces = lces;
            sp.cmes = cmes;
            sp.res = findSlotByRef(pfx + "RES");
            sp.le = findSlotByRef(pfx + "LE");
            sp.le2 = findSlotByRef(pfx + "LE2");
            speakers_.push_back(sp);
        }
    }
    // Solver selection.
    const std::string want = opt.solver.empty() ? n.solver : opt.solver;
    for (std::size_t gi = 0; gi < groups_.size(); ++gi) {
        auto& g = groups_[gi];
        if (g.nn == 0) g.solver = SolverKind::Linear;
        else g.solver = SolverKind::Dk;
        // A consumer whose cut current runs through a capacitor or inductor (a choke-input filter's choke on a cut rail,
        // say) needs the DK companion state and branch voltage, which a WDF group does not keep: it stays DK. With the
        // WDF form the 6G15's rail read stale companions and drifted ~95 V (ENGINE_QUEUE item 18).
        bool reactiveCut = false;
        for (const auto& t : g.cutTerms) reactiveCut |= t.kind == 1 || t.kind == 2;
        if (want != "dk" && !reactiveCut && WdfGroup::eligible(*this, g)) g.solver = SolverKind::Wdf;
        if (want == "wdf" && g.nn > 0 && g.solver != SolverKind::Wdf)
            warnings_.push_back("group " + std::to_string(gi) + " is not WDF-eligible; using " + solverName(g.solver));
    }
    // Multirate partitions: only groups holding an "oversampled" part run at the full rate.
    multirate_ = opt.multirate && !n.oversampledParts.empty();
    for (auto& g : groups_) {
        g.T = T_;
        g.lowRate = false;
        if (!multirate_) continue;
        bool over = false;
        for (int pi : g.parts)
            for (const auto& ref : n.oversampledParts) over |= n.parts[static_cast<std::size_t>(pi)].ref == ref;
        g.lowRate = !over;
        if (g.lowRate) g.T = 2.0 * T_;
    }
    // Rate ladder (CompileOptions::partLevels; docs §3.7).
    ladder_ = false;
    maxLevel_ = 0;
    for (auto& g : groups_) g.level = g.evos = 0;
    if (!multirate_ && !opt.partLevels.empty() && logic_.empty()) {
        for (auto& g : groups_)
            for (int pi : g.parts)
                for (const auto& [ref, lv] : opt.partLevels)
                    if (n.parts[static_cast<std::size_t>(pi)].ref == ref) g.level = std::max(g.level, std::clamp(lv, 0, 5));
        // A signal cut whose loads include a device port (a grid) never crosses a rate boundary: the converters' delay
        // would sit inside its lagged loop (the Bassman's g3a cut failed that way in the 2x mode). Owner and loads then
        // share the higher level. Linear loads (a tone stack behind a cathode follower) may cross, as in the 2x mode.
        for (bool changed = true; changed;) {
            changed = false;
            for (std::size_t gi = 0; gi < groups_.size(); ++gi)
                for (const auto& t : groups_[gi].cutTerms) {
                    if (!explicitCut[static_cast<std::size_t>(t.node)] || t.kind != 3) continue;
                    const int og = groupOfNode[static_cast<std::size_t>(t.node)];
                    if (og < 0) continue;
                    auto& a = groups_[gi];
                    auto& b = groups_[static_cast<std::size_t>(og)];
                    if (a.level != b.level) a.level = b.level = std::max(a.level, b.level), changed = true;
                }
        }
        // event-local oversampling (docs §10.48): the raised groups stay at the base rate (evosPlan); M = 2^evos substeps
        // per event, up to 4, or 8 where the netlist asks for events itself (docs §10.62: the 5150's knees at 768 kHz)
        if (kEnvEvos > 0 || (kEnvEvos < 0 && n.rateEvents))
            for (auto& g : groups_)
                if (g.level > 0) g.evos = std::min(g.level, n.rateEvents && kEnvEvosMax8 ? 3 : 2), g.level = 0;
        for (auto& g : groups_) {
            maxLevel_ = std::max(maxLevel_, g.level);
            g.T = T_ / static_cast<double>(1 << g.level);
        }
        ladder_ = maxLevel_ > 0;
    }
    // Slow partitions (multirate.slow): every div-th sample of their loop, staggered.
    cutHold_.assign(groups_.size(), {});
    for (auto& g : groups_) {
        g.div = 1;
        g.phase = 0;
    }
    // A slow partition must never hold the audio path: a group that solves the input or output node, or reads the
    // input directly, is solved at the full rate whatever the netlist asks. (One slow node in an amp's main group once
    // dropped the whole amp to 12 kHz: everything above 6 kHz aliased.) Side-chain and LFO loops stay slow.
    std::vector<std::uint8_t> onPath(groups_.size(), 0);
    for (std::size_t gi = 0; gi < groups_.size(); ++gi) {
        const auto& g = groups_[gi];
        for (int u : g.unknowns) onPath[gi] |= isInputNode(u) || u == outputNode_ || u == outputNodeR_;
        for (int x : g.inputs) onPath[gi] |= isInputNode(x);
    }
    for (std::size_t sk = 0; sk < n.slowNodes.size(); ++sk) {
        const auto& nm = n.slowNodes[sk];
        const int id = node(nm);
        if (id <= 0 || groupOfNode[static_cast<std::size_t>(id)] < 0) {
            warnings_.push_back("multirate.slow: " + nm + " is not a solved node");
            continue;
        }
        const auto gi = static_cast<std::size_t>(groupOfNode[static_cast<std::size_t>(id)]);
        auto& g = groups_[gi];
        if (g.div > 1) continue;
        if (onPath[gi]) {
            warnings_.push_back("multirate.slow: " + nm + " is in group " + std::to_string(gi) +
                                ", which carries the audio path; left at full rate");
            continue;
        }
        if ((multirate_ && !g.lowRate) || g.level > 0) {
            warnings_.push_back("multirate.slow: " + nm + " is in an oversampled group; left at full rate");
            continue;
        }
        const double base = multirate_ ? 0.5 * fs_ : fs_;
        g.div = std::max(1, static_cast<int>(std::lround(base / n.slowRate)));
        // staggered by the first slow node's place in the netlist's list, not the group's index: the partition may
        // order its groups differently (an open contact left out, CompileOptions::openSwitches), and a slow group's
        // update samples must not move with it (the Bi-Phase nulled at -77 dB against its roff contacts by group index)
        g.phase = static_cast<int>((kEnvOldStagger ? gi : sk) % static_cast<std::size_t>(g.div)); // CE_OLDSTAGGER: by group index
        g.T *= g.div;
        cutHold_[gi].reserve(g.cutTerms.size() + 2);
    }
    // Behavioural blocks run once per host sample (spring tanks are band-limited to a few kHz: their drive is read
    // at the host rate even when the driver stage is oversampled).
    if (multirate_) {
        for (auto& b : bbds_) b.line->rateChanged(0.5 * fs_);
        for (std::size_t t = 0; t < tapes_.size(); ++t) tapes_[t].loop->prepare(0.5 * fs_, tapeParams_[t]);
    }
    if (multirate_ && !blocks_.empty())
        for (auto& b : blocks_) {
            SpringTank::Params sp;
            const auto& q = b.params;
            sp.delay1 = q[0], sp.delay2 = q[1], sp.t60 = q[2], sp.disp = q[3];
            sp.stages = static_cast<int>(q[4]), sp.lp = q[5], sp.hp = q[6], sp.gain = q[7], sp.delay3 = q[8];
            b.tank->prepare(0.5 * fs_, sp);
        }
    // Structure hash.
    std::uint64_t h = 1469598103934665603ull;
    h = fnv(h, n.id);
    for (const auto& s : nodeNames_) h = fnv(h, s);
    for (const auto& p : prims_) h = fnv(h, std::to_string(p.kind) + "/" + std::to_string(p.a) + "/" + std::to_string(p.b) + "/" + std::to_string(p.c));
    for (const auto& d : devices_) h = fnv(h, std::to_string(static_cast<int>(d.kind)) + "/" + std::to_string(d.part));
    for (const auto& g : groups_) h = fnv(h, std::to_string(g.unknowns.size()) + ":" + std::to_string(static_cast<int>(g.solver)) + (g.lowRate ? "L" : "H") + std::to_string(g.level));
    h = fnv(h, std::to_string(fs_));
    hash_ = h;
    // ...and of everything but the partition and the open contacts (adoptAcrossTopology: the same netlist, rate and
    // options compiled at other switch positions has the same nodes, prims and devices, index for index).
    {
        std::uint64_t q = 1469598103934665603ull;
        q = fnv(q, n.id);
        for (const auto& s : nodeNames_) q = fnv(q, s);
        for (const auto& p : prims_) q = fnv(q, std::to_string(p.kind) + "/" + std::to_string(p.part) + "/" + std::to_string(p.slot));
        for (const auto& d : devices_) q = fnv(q, std::to_string(static_cast<int>(d.kind)) + "/" + std::to_string(d.part) + "/" + std::to_string(d.nports));
        q = fnv(q, std::to_string(fs_) + (multirate_ ? "M" : "") + (ladder_ ? "L" : "") + std::to_string(slots_.size()) + "/" + std::to_string(blocks_.size()) + "/" +
                       std::to_string(bbds_.size()) + "/" + std::to_string(tapes_.size()) + "/" + std::to_string(logic_.size()));
        netHash_ = q;
    }

    nodalPlan(); // docs §10.51 (exactmisc): capacitor loops per group
    V_.assign(static_cast<std::size_t>(N), 0.0);
    if (!n.cuts.empty()) {
        cutI_.assign(static_cast<std::size_t>(N), 0.0);
        cutAcc_.assign(static_cast<std::size_t>(N), 0.0);
    } else {
        cutI_.clear();
        cutAcc_.clear();
    }
    for (int i = 0; i < N; ++i)
        if (nodeKind_[static_cast<std::size_t>(i)] == kKnown) V_[static_cast<std::size_t>(i)] = knownValue_[static_cast<std::size_t>(i)];
    ctlCur_ = compileCtl; // the defaults, or CompileOptions::controls
    ctlTarget_ = ctlCur_;
    refreshScaledSources(); // control-scaled sources at these positions (setControlImmediate does the same)
    // Control glides refresh the matrices about every 0.33 ms whatever the internal rate (32 samples at 96 kHz): at the
    // 4x rates a fixed 32-sample interval doubled the refresh work per second and made knob edits miss deadlines.
    refreshInterval_ = std::max(kRefreshInterval, static_cast<int>(std::lround(kRefreshInterval * fs_ / 96000.0)));
    glideAlpha_ = 1.0 - std::exp(-static_cast<double>(refreshInterval_) / (std::max(0.1, opt.glideMs) * 1e-3 * fs_));
    for (auto& s : slots_) s.value = slotValue(s);
    groupDirty_.assign(groups_.size(), 1);
    if (!buildGroupsRuntime(error)) return false;
    // Where each reactive state and device port lives in this partition (adoptAcrossTopology).
    primStateG_.assign(prims_.size(), -1);
    primStateK_.assign(prims_.size(), -1);
    devG_.assign(devices_.size(), -1);
    devPort_.assign(devices_.size(), -1);
    for (std::size_t gi = 0; gi < groups_.size(); ++gi) {
        const auto& g = groups_[gi];
        for (std::size_t k = 0; k < g.states.size(); ++k) {
            primStateG_[static_cast<std::size_t>(g.states[k])] = static_cast<int>(gi);
            primStateK_[static_cast<std::size_t>(g.states[k])] = static_cast<int>(k);
        }
        int off = 0;
        for (int di : g.devices) {
            devG_[static_cast<std::size_t>(di)] = static_cast<int>(gi);
            devPort_[static_cast<std::size_t>(di)] = off;
            off += devices_[static_cast<std::size_t>(di)].nports;
        }
    }
    // Multirate converter plan: a node gets an up-converter if it is produced (or enters) at the host rate and an
    // oversampled group reads it, a down-converter if an oversampled group produces it and a host-rate group (or
    // the output) reads it.
    nodeLow_.assign(static_cast<std::size_t>(N), 1);
    upOf_.assign(static_cast<std::size_t>(N), -1);
    downOf_.assign(static_cast<std::size_t>(N), -1);
    conv_.clear();
    lo_.assign(static_cast<std::size_t>(N), 0.0);
    hi0_.assign(static_cast<std::size_t>(N), 0.0);
    hi1_.assign(static_cast<std::size_t>(N), 0.0);
    if (multirate_) {
        for (const auto& g : groups_)
            for (int e : g.exports) nodeLow_[static_cast<std::size_t>(e)] = g.lowRate ? 1 : 0;
        int nConv = 0;
        for (const auto& g : groups_)
            for (int in : g.inputs) {
                if (nodeKind_[static_cast<std::size_t>(in)] == kKnown && !isInputNode(in)) continue; // DC rails
                if (!g.lowRate && nodeLow_[static_cast<std::size_t>(in)] && upOf_[static_cast<std::size_t>(in)] < 0) upOf_[static_cast<std::size_t>(in)] = nConv++;
                if (g.lowRate && !nodeLow_[static_cast<std::size_t>(in)] && downOf_[static_cast<std::size_t>(in)] < 0) downOf_[static_cast<std::size_t>(in)] = nConv++;
            }
        if (!nodeLow_[static_cast<std::size_t>(outputNode_)] && downOf_[static_cast<std::size_t>(outputNode_)] < 0) downOf_[static_cast<std::size_t>(outputNode_)] = nConv++;
        if (outputNodeR_ > 0 && !nodeLow_[static_cast<std::size_t>(outputNodeR_)] && downOf_[static_cast<std::size_t>(outputNodeR_)] < 0)
            downOf_[static_cast<std::size_t>(outputNodeR_)] = nConv++;
        conv_.resize(static_cast<std::size_t>(nConv));
    }
    // Rate ladder plan: producer level per node, a converter per (node, consumer level) pair that crosses a rate.
    blk_.clear();
    lconv_.clear();
    ladderIn_.clear();
    levelOfNode_.clear();
    outConv_ = outConvR_ = -1;
    if (ladder_) {
        blk_.assign(static_cast<std::size_t>(N) * 32, 0.0);
        levelOfNode_.assign(static_cast<std::size_t>(N), -1);
        for (const auto& g : groups_)
            for (int e : g.exports) levelOfNode_[static_cast<std::size_t>(e)] = g.level;
        levelOfNode_[static_cast<std::size_t>(inputNode_)] = 0;
        if (inputNodeR_ > 0) levelOfNode_[static_cast<std::size_t>(inputNodeR_)] = 0;
        auto convFor = [&](int node, int to) {
            for (std::size_t c = 0; c < lconv_.size(); ++c)
                if (lconv_[c].node == node && lconv_[c].to == to) return static_cast<int>(c);
            LadderConv c;
            c.node = node;
            c.from = levelOfNode_[static_cast<std::size_t>(node)];
            c.to = to;
            // Stage designs as the unit's CircuitOversampler: absolute stage k converts host·2^(k−1) ↔ host·2^k.
            const int lo = std::min(c.from, to), hi = std::max(c.from, to);
            for (int lv = lo + 1; lv <= hi; ++lv) {
                c.st.emplace_back();
                c.st.back().design(opt.ladderBaseStage + lv);
            }
            if (to < c.from) std::reverse(c.st.begin(), c.st.end()); // down: the highest stage first
            lconv_.push_back(std::move(c));
            return static_cast<int>(lconv_.size()) - 1;
        };
        ladderIn_.assign(groups_.size(), {});
        ladderAc_.assign(groups_.size(), 0);
        ladderAcIdx_.assign(groups_.size(), {});
        for (std::size_t gi = 0; gi < groups_.size(); ++gi)
            for (std::size_t ai = 0; ai < acSrc_.size(); ++ai)
                if (std::find(groups_[gi].inputs.begin(), groups_[gi].inputs.end(), acSrc_[ai].node) != groups_[gi].inputs.end()) {
                    ladderAc_[gi] = 1;
                    ladderAcIdx_[gi].push_back(static_cast<int>(ai));
                }
        for (std::size_t gi = 0; gi < groups_.size(); ++gi)
            for (int x : groups_[gi].inputs) {
                if (nodeKind_[static_cast<std::size_t>(x)] == kKnown && !isInputNode(x)) continue; // rails, mains: held
                const int from = levelOfNode_[static_cast<std::size_t>(x)];
                if (from < 0) continue;
                ladderIn_[gi].push_back({x, from == groups_[gi].level ? -1 : convFor(x, groups_[gi].level)});
            }
        if (levelOfNode_[static_cast<std::size_t>(outputNode_)] > 0) outConv_ = convFor(outputNode_, 0);
        if (outputNodeR_ > 0 && levelOfNode_[static_cast<std::size_t>(outputNodeR_)] > 0) outConvR_ = convFor(outputNodeR_, 0);
    }
    // Ladder cut currents (see LadderCut): per (consumer group, cut node), a converter when the owner runs at another
    // level. Rail-node op-amps and slow partitions keep the base-rate mean.
    lcut_.clear();
    lcutOf_.clear();
    lcutNodes_.clear();
    lcutConv_.clear();
    ladderCuts_ = false;
    if (ladder_ && railAmps_.empty() && std::all_of(groups_.begin(), groups_.end(), [](const Group& g) { return g.div <= 1; })) {
        std::vector<int> owner(static_cast<std::size_t>(N), -1);
        for (std::size_t gi = 0; gi < groups_.size(); ++gi)
            for (int x : groups_[gi].cutIn) owner[static_cast<std::size_t>(x)] = static_cast<int>(gi);
        lcutOf_.assign(groups_.size(), {});
        for (std::size_t gi = 0; gi < groups_.size(); ++gi)
            for (const auto& t : groups_[gi].cutTerms) {
                const int o = owner[static_cast<std::size_t>(t.node)];
                if (o < 0 || o == static_cast<int>(gi)) continue;
                bool seen = false;
                for (int e : lcutOf_[gi]) seen |= lcut_[static_cast<std::size_t>(e)].node == t.node;
                if (seen) continue;
                LadderCut c;
                c.group = static_cast<int>(gi);
                c.node = t.node;
                c.owner = o;
                const int from = groups_[gi].level, to = groups_[static_cast<std::size_t>(o)].level;
                if (from != to) {
                    LadderConv cv;
                    cv.node = t.node;
                    cv.from = from;
                    cv.to = to;
                    for (int lv = std::min(from, to) + 1; lv <= std::max(from, to); ++lv) {
                        cv.st.emplace_back();
                        cv.st.back().design(opt.ladderBaseStage + lv);
                    }
                    if (to < from) std::reverse(cv.st.begin(), cv.st.end());
                    c.conv = static_cast<int>(lcutConv_.size());
                    lcutConv_.push_back(std::move(cv));
                }
                lcutOf_[gi].push_back(static_cast<int>(lcut_.size()));
                lcut_.push_back(c);
                if (std::find(lcutNodes_.begin(), lcutNodes_.end(), t.node) == lcutNodes_.end()) lcutNodes_.push_back(t.node);
            }
        ladderCuts_ = !lcut_.empty();
        cutNext_.assign(ladderCuts_ ? static_cast<std::size_t>(N) * 32 : 0, 0.0);
        cutPlay_.assign(cutNext_.size(), 0.0);
        lcutSave_.assign(lcut_.size(), 0.0);
        cutKeep_.assign(cutAcc_.size(), 0.0);
    }
    evosPlan(opt); // event-local oversampling (docs §10.48)
    // CMOS logic cells: logic nets (an input wired to another cell's output pin takes its events), then the schedule.
    // A cell runs just before the first group that reads its drive λ when every node it senses is solved earlier in the
    // sample (exact edge timing); otherwise after the sample, with a one-sample delay (a loop through the cell, a
    // multirate circuit or a slow partition).
    if (!logic_.empty()) {
        blep_ = sharedMinBlep();
        const auto G = groups_.size();
        logicAt_.assign(G + 1, {});
        std::vector<std::size_t> first(logic_.size(), G);
        for (std::size_t i = 0; i < logic_.size(); ++i) {
            auto& c = logic_[i];
            c.table = blep_.get();
            c.fs = multirate_ ? 0.5 * fs_ : fs_;
            for (int j = 0; j < c.nout; ++j)
                for (std::size_t gi = 0; gi < G; ++gi)
                    for (int x : groups_[gi].inputs)
                        if (x == c.out[j].node) first[i] = std::min(first[i], gi);
        }
        std::vector<int> srcOf(logic_.size() * LogicCell::kMaxIn, -1);
        for (std::size_t i = 0; i < logic_.size(); ++i)
            for (int k = 0; k < logic_[i].nin; ++k)
                for (std::size_t s = 0; s < logic_.size(); ++s)
                    for (int j = 0; j < logic_[s].nout; ++j)
                        if (logic_[i].in[k] == logic_[s].out[j].pin && logic_[s].nsub[j] < LogicCell::kMaxSubs) { // incl. a cell's own output (D = Q̄)
                            logic_[i].fromNet[k] = true;
                            logic_[s].subs[j][logic_[s].nsub[j]++] = {&logic_[i], k};
                            srcOf[i * LogicCell::kMaxIn + static_cast<std::size_t>(k)] = static_cast<int>(s);
                        }
        // topological order over the nets (sources first); cells on a logic loop keep netlist order
        std::vector<int> order, state(logic_.size(), 0);
        std::function<void(int)> visit = [&](int i) {
            if (state[static_cast<std::size_t>(i)]) return;
            state[static_cast<std::size_t>(i)] = 1;
            for (int k = 0; k < LogicCell::kMaxIn; ++k)
                if (srcOf[static_cast<std::size_t>(i) * LogicCell::kMaxIn + static_cast<std::size_t>(k)] >= 0 && srcOf[static_cast<std::size_t>(i) * LogicCell::kMaxIn + static_cast<std::size_t>(k)] != i)
                    visit(srcOf[static_cast<std::size_t>(i) * LogicCell::kMaxIn + static_cast<std::size_t>(k)]);
            order.push_back(i);
        };
        for (std::size_t i = 0; i < logic_.size(); ++i) visit(static_cast<int>(i));
        std::vector<std::size_t> slot(logic_.size(), G);
        for (int i : order) {
            auto& c = logic_[static_cast<std::size_t>(i)];
            bool exact = !multirate_;
            std::size_t need = 0; // groups that must be solved before the cell runs
            auto solvedBy = [&](int nd) -> long {
                if (nd <= 0 || nodeKind_[static_cast<std::size_t>(nd)] == kKnown) return -1;
                return groupOfNode[static_cast<std::size_t>(nd)];
            };
            for (int k = 0; k < c.nin; ++k) {
                if (c.fromNet[k]) {
                    const auto s = static_cast<std::size_t>(srcOf[static_cast<std::size_t>(i) * LogicCell::kMaxIn + static_cast<std::size_t>(k)]);
                    if (s != static_cast<std::size_t>(i) && (logic_[s].late || slot[s] > first[static_cast<std::size_t>(i)])) exact = false;
                    continue;
                }
                const long g = solvedBy(c.in[k]);
                if (g >= 0) need = std::max(need, static_cast<std::size_t>(g) + 1);
            }
            for (int nd : {c.vdd, c.vss}) {
                const long g = solvedBy(nd);
                if (g >= 0) need = std::max(need, static_cast<std::size_t>(g) + 1);
            }
            const std::size_t at = first[static_cast<std::size_t>(i)];
            if (need > at) exact = false;
            for (std::size_t gi = 0; gi < G && exact; ++gi)
                if (groups_[gi].div > 1) exact = false;
            c.late = !exact;
            slot[static_cast<std::size_t>(i)] = exact ? at : G;
            logicAt_[slot[static_cast<std::size_t>(i)]].push_back(i);
            if (!exact) warnings_.push_back(n.parts[static_cast<std::size_t>(c.part)].ref + ": logic evaluated after the sample (one-sample delay: loop or multirate)");
            if (std::getenv("CE_LOGIC"))
                std::fprintf(stderr, "logic %s: slot %zu of %zu (first reader %zu, need %zu)\n", n.parts[static_cast<std::size_t>(c.part)].ref.c_str(),
                             slot[static_cast<std::size_t>(i)], G, at, need);
        }
    }
    if (opt.settleFrom && opt.settleFrom != this && opt.settleFrom->hash_ == hash_ && !opt.settleFrom->pss_.empty()) {
        std::vector<std::pair<double*, std::size_t>> arr;
        stateArrays(arr);
        std::size_t dim = 0;
        for (const auto& a : arr) dim += a.second;
        if (dim == opt.settleFrom->pss_.size()) { // settlePeriodic's warm start, at the control positions it was settled at
            pss_ = opt.settleFrom->pss_;
            for (std::size_t c = 0; c < ctlCur_.size() && c < opt.settleFrom->ctlCur_.size(); ++c) setControlImmediate(static_cast<int>(c), opt.settleFrom->ctlCur_[c]);
        }
    }
    opt_.settleFrom = nullptr; // read here only
    buildLanes(); // intra-unit lanes (docs §10.33)
    if (!reset()) {
        if (debugFail_) std::fprintf(stderr, "%s", describeGroups().c_str());
        error = "the DC operating point did not converge";
        return false;
    }
    error.clear();
    return true;
}

// The structure a specialised Newton kernel bakes in (docs §10.45; see cg::NewtonPattern and nk::StaticSpec) and its
// signature. False: a structure the specialisation does not cover (no ports, or a device without 1-3 ports).
static bool newtonPattern(const std::vector<const NlDevice*>& dev, const std::uint8_t* lim, int n, cg::NewtonPattern& q) {
    q = cg::NewtonPattern{};
    q.n = n;
    if (n < 1 || n > kMaxNlPorts) return false;
    int o = 0;
    for (const NlDevice* d : dev) {
        if (d->nports < 1 || d->nports > 3) return false;
        q.kind.push_back(static_cast<int>(d->kind));
        q.np.push_back(d->nports);
        q.off.push_back(o);
        q.shift.push_back(d->hasShift ? 1 : 0);
        for (int a = 0; a < d->nports; ++a) {
            q.dev.push_back(static_cast<int>(q.kind.size()) - 1);
            q.ccA.push_back(d->kind == NlKind::Zener && (d->floatingPorts >> a & 1u) ? 1 : 0);
            q.ccB.push_back(d->kind == NlKind::Diode || d->kind == NlKind::DiodePair || d->kind == NlKind::Zener ? 1 : 0);
        }
        o += d->nports;
    }
    if (o != n) return false;
    for (int k = 0; k < n; ++k) q.lim.push_back(lim[k]);
    std::uint64_t h1 = 1469598103934665603ull, h2 = 0x510e527fade682d1ull;
    auto put = [&](std::int64_t v) {
        const auto u = static_cast<std::uint64_t>(v);
        for (int b = 0; b < 8; ++b) h1 = (h1 ^ ((u >> (8 * b)) & 0xffu)) * 1099511628211ull;
        std::uint64_t z = h2 + u + 0x9E3779B97F4A7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        h2 = z ^ (z >> 31);
    };
    put(0x4e4b0001); // "NK", layout version 1
    put(n);
    put(static_cast<std::int64_t>(dev.size()));
    for (std::size_t q2 = 0; q2 < dev.size(); ++q2) put(q.kind[q2]), put(q.np[q2]), put(q.shift[q2]);
    for (int k = 0; k < n; ++k) put(q.lim[static_cast<std::size_t>(k)]), put(q.ccA[static_cast<std::size_t>(k)]), put(q.ccB[static_cast<std::size_t>(k)]);
    q.h1 = h1;
    q.h2 = h2;
    return true;
}

std::unique_ptr<Circuit::GroupRt> Circuit::makeGroupRt(std::size_t gi, std::string& error) {
    const int N = static_cast<int>(nodeNames_.size());
    const auto& g = groups_[gi];
    auto r = std::make_unique<GroupRt>();
    r->n = static_cast<int>(g.unknowns.size() + g.branches.size());
    r->nu = static_cast<int>(g.inputs.size());
    r->ns = static_cast<int>(g.states.size());
    r->nn = g.nn;
    r->ny = static_cast<int>(g.exports.size());
    r->nuc = static_cast<int>(g.cutIn.size());
    groups_[gi].unread = !kEnvNoUnread && r->ny == 0 && g.cutTerms.empty() && g.cutIn.empty(); // docs §10.60
    if (g.unread && kEnvUnreadInfo) std::fprintf(stderr, "unread group %zu (%zu unknowns, %d ports)\n", gi, g.unknowns.size(), g.nn);
    r->W = r->ns + r->nu + r->nn + r->nuc; // [states; inputs; NL currents; cut currents]
    r->unkOf.assign(static_cast<std::size_t>(N), -1);
    r->inOf.assign(static_cast<std::size_t>(N), -1);
    for (int i = 0; i < static_cast<int>(g.unknowns.size()); ++i) r->unkOf[static_cast<std::size_t>(g.unknowns[static_cast<std::size_t>(i)])] = i;
    for (int j = 0; j < r->nu; ++j) r->inOf[static_cast<std::size_t>(g.inputs[static_cast<std::size_t>(j)])] = j;
    r->kcl.assign(static_cast<std::size_t>(r->n), 0);
    for (int i = 0; i < static_cast<int>(g.unknowns.size()); ++i) r->kcl[static_cast<std::size_t>(i)] = nodeKind_[static_cast<std::size_t>(g.unknowns[static_cast<std::size_t>(i)])] == kFree;
    const auto n = static_cast<std::size_t>(r->n), W = static_cast<std::size_t>(r->W), nn = static_cast<std::size_t>(r->nn);
    r->M.assign(n * n, 0.0);
    r->R.assign(n * W, 0.0);
    r->Z.assign(n * W, 0.0);
    r->piv.assign(n, 0);
    r->Dp.assign(nn * static_cast<std::size_t>(r->ns + r->nu), 0.0);
    r->Dc.assign(nn * static_cast<std::size_t>(r->nuc), 0.0);
    r->K.assign(nn * nn, 0.0);
    r->kcol.assign(nn, 0.0);
    r->kac = std::make_unique<KaCache>();
    r->kacR = std::make_unique<KaCache>();
    r->kac->KA.assign(std::max<std::size_t>(nn * nn, 1), 0.0);
    r->kacR->KA.assign(std::max<std::size_t>(nn * nn, 1), 0.0); // the clamp-free set is a subset of the ports
    r->H.assign(static_cast<std::size_t>(r->ny + r->ns) * W, 0.0);
    r->w.assign(W, 0.0);
    r->gc.assign(static_cast<std::size_t>(r->ns), 0.0);
    r->G2.assign(static_cast<std::size_t>(r->ns), 0.0);
    r->sgn.assign(static_cast<std::size_t>(r->ns), 0.0);
    r->vcLast.assign(static_cast<std::size_t>(r->ns), 0.0);
    for (std::size_t k = 0; k < static_cast<std::size_t>(r->ns); ++k)
        r->sgn[k] = prims_[static_cast<std::size_t>(g.states[k])].kind == Prim::Cap ? -1.0 : 1.0;
    for (auto* v : {&r->p, &r->pPrev, &r->vp, &r->vpPrev, &r->vpPrev2, &r->F, &r->delta, &r->pPrev2, &r->dpPred, &r->pPrev3, &r->vpPrev3}) v->assign(nn, 0.0);
    const NewtonHints nh = newtonHints(net_); // docs §10.50
    r->quadPorts = nh.quadPorts, r->subPred = nh.subPred, r->subPredPorts = nh.subPredPorts;
    for (KernelState* k : {&r->ks, &r->ksR}) { // (the clamp-free set is a subset of the ports)
        k->chordGate = nh.chordGate;
        k->LU.assign(std::max<std::size_t>(nn * nn, 1), 0.0);
        k->rinv.assign(std::max<std::size_t>(nn, 1), 0.0);
        k->Jd.assign(std::max<std::size_t>(nn * 6, 1), 0.0);
        k->piv.assign(std::max<std::size_t>(nn, 1), 0);
        k->live.assign(std::max<std::size_t>(nn, 1), 0);
    }
    if (r->nn >= kEnvSpMinPorts && g.solver == SolverKind::Dk) {
        r->sp = std::make_unique<SparseLu>(); // built by buildSparse() at reset()
        r->Mraw.assign(n * n, 0.0);
    }
    if (n >= 24 && g.solver != SolverKind::Wdf) { // sparse linear update candidates (buildSparseLin decides)
        r->sl = std::make_unique<SparseLin>();
        if (r->Mraw.empty()) r->Mraw.assign(n * n, 0.0);
    }
    r->J.assign(nn * nn, 0.0);
    r->KJd.assign(std::max<std::size_t>(nn * nn, 1), 0.0);
    r->Jf.assign(nn * nn, 0.0);
    r->vloc.assign(4, 0.0);
    r->out.assign(static_cast<std::size_t>(r->ny + r->ns), 0.0);
    if (g.solver != SolverKind::Wdf && r->nn > 0 && r->ns > 0 && !kEnvNoPortStates) { // port-defined states (GroupRt::pdRow)
        std::vector<std::array<int, 2>> ps; // per port: sense nodes (cp, cn)
        for (int di : g.devices)
            for (int k = 0; k < devices_[static_cast<std::size_t>(di)].nports; ++k)
                ps.push_back({devices_[static_cast<std::size_t>(di)].port[k].cp, devices_[static_cast<std::size_t>(di)].port[k].cn});
        const int np = static_cast<int>(ps.size());
        // node x − (the port's other end): +vp if x is the port's cp, −vp if its cn
        auto side = [&](int j, int x, int& other) {
            const auto& q = ps[static_cast<std::size_t>(j)];
            if (q[0] == q[1]) return 0;
            if (q[0] == x) {
                other = q[1];
                return 1;
            }
            if (q[1] == x) {
                other = q[0];
                return -1;
            }
            return 0;
        };
        // out row = (node a) − (node b): one port across (a, b), or two ports from a and b to one shared node c
        auto match = [&](int a, int b, int row) {
            if (a == b) return;
            int A = -1, B = -1;
            for (int j = 0; j < np && A < 0 && B < 0; ++j) {
                int c = -2;
                const int s = side(j, a, c);
                if (s != 0 && c == b) (s > 0 ? A : B) = j;
            }
            for (int j1 = 0; j1 < np && A < 0 && B < 0; ++j1) {
                int c1 = -2;
                const int s1 = side(j1, a, c1);
                if (s1 == 0) continue;
                for (int j2 = 0; j2 < np; ++j2) {
                    int c2 = -2;
                    const int s2 = side(j2, b, c2);
                    if (j2 == j1 || s2 != s1 || c2 != c1) continue; // a − b = (a − c) − (b − c): same signs only
                    A = s1 > 0 ? j1 : j2;
                    B = s1 > 0 ? j2 : j1;
                    break;
                }
            }
            if (A < 0 && B < 0) return;
            r->pdRow.push_back(row);
            r->pdA.push_back(A);
            r->pdB.push_back(B);
        };
        for (int e = 0; e < r->ny; ++e) match(g.exports[static_cast<std::size_t>(e)], 0, e);
        for (int k = 0; k < r->ns; ++k) {
            const auto& pr = prims_[static_cast<std::size_t>(g.states[static_cast<std::size_t>(k)])];
            match(pr.a, pr.b, r->ny + k);
        }
        // only where the rows saved outweigh the scatter (small pedal groups: a 4-row gemv beats 3 rows plus a gather)
        if (r->pdRow.size() * W < 128) r->pdRow.clear(), r->pdA.clear(), r->pdB.clear();
        if (!r->pdRow.empty()) {
            std::vector<std::uint8_t> isPd(static_cast<std::size_t>(r->ny + r->ns), 0);
            for (int q : r->pdRow) isPd[static_cast<std::size_t>(q)] = 1;
            for (int e = 0; e < r->ny + r->ns; ++e)
                if (!isPd[static_cast<std::size_t>(e)]) r->hcOut.push_back(e);
            r->hcY0 = static_cast<int>(std::count_if(r->hcOut.begin(), r->hcOut.end(), [&](int e) { return e < r->ny; }));
            r->Hc.assign(r->hcOut.size() * W, 0.0);
            r->hcY.assign(r->hcOut.size(), 0.0);
        }
        if (std::getenv("CE_SPINFO"))
            std::fprintf(stderr, "group %zu: %zu of %d exports + %d states port-defined (%d ports)\n", gi, r->pdRow.size(), r->ny, r->ns, r->nn);
    }
    r->pivN.assign(std::max<std::size_t>(nn, 1), 0);
    r->lim.assign(nn, kLimNone);
    r->limA.assign(nn, 0.0);
    r->limB.assign(nn, 0.0);
    r->limC.assign(nn, 0.0);
    r->portDev.assign(nn, -1);
    int o = 0;
    for (int di : g.devices) {
        const auto& d = devices_[static_cast<std::size_t>(di)];
        for (int k = 0; k < d.nports; ++k) {
            const auto pk = static_cast<std::size_t>(o + k);
            r->portDev[pk] = di;
            if (d.kind == NlKind::Diode || d.kind == NlKind::DiodePair) {
                r->lim[pk] = d.kind == NlKind::Diode && (d.floatingPorts >> k & 1u) ? kLimPnjF : kLimPnj;
                r->limA[pk] = d.p[1]; // nVt
                r->limB[pk] = d.p[1] * std::log(d.p[1] / (std::sqrt(2.0) * d.p[0]));
                r->limC[pk] = d.kind == NlKind::DiodePair || r->lim[pk] == kLimPnjF ? 1.0 : 0.0; // symmetric; kLimPnjF: current-space step on
            } else if (d.kind == NlKind::Bjt) {
                r->lim[pk] = kLimPnj;
                r->limA[pk] = kVt;
                r->limB[pk] = kVt * std::log(kVt / (std::sqrt(2.0) * d.p[0]));
                r->limC[pk] = d.pnp ? -1.0 : 0.0; // junction sign
            } else if (d.kind == NlKind::Jfet && k == 0) { // gate junction (the channel port is smooth)
                const double nvt = 1.0 / d.p[4];
                r->lim[pk] = kLimPnj;
                r->limA[pk] = nvt;
                r->limB[pk] = nvt * std::log(nvt / (std::sqrt(2.0) * std::max(d.p[3], 1e-30)));
                r->limC[pk] = d.pnp ? -1.0 : 0.0;
            } else if (d.kind == NlKind::Ota) {
                r->lim[pk] = kLimSat;
                r->limA[pk] = 3.0 / d.p[1];
            } else if (d.kind == NlKind::OtaHalf && k == 0) { // vd: tanh-saturating input pair
                r->lim[pk] = kLimSat;
                r->limA[pk] = 4.0 * kVt;
            } else if (d.kind == NlKind::OtaHalf && k == 1) { // the abc junction voltage
                r->lim[pk] = kLimPnj;
                r->limA[pk] = 1.0 / d.p[1];
                r->limB[pk] = r->limA[pk] * std::log(r->limA[pk] / (std::sqrt(2.0) * std::max(d.p[0], 1e-30)));
            } else if (d.kind == NlKind::LampTh && k == 1) { // θ: bounded steps (the law is steep in θ)
                r->lim[pk] = kLimStep;
                r->limA[pk] = 0.2;
                r->limB[pk] = -1e9;
                r->limC[pk] = 1.0;
            } else if (d.kind == NlKind::Ad536 && k == 1) { // the averaged current across R1: bounded steps
                r->lim[pk] = kLimStep;
                r->limA[pk] = 0.25;
                r->limB[pk] = -1e9;
                r->limC[pk] = 1.0;
            } else if (d.kind == NlKind::Zener) { // SPICE DIOlimit: pnjlim forward, and on the shifted coordinate in breakdown
                r->lim[pk] = (d.floatingPorts >> k & 1u) && !kEnvNoFltConv ? kLimZenerF : kLimZener; // CE_NOFLTCONV: as before
                r->limA[pk] = d.p[1];
                r->limB[pk] = d.p[1] * std::log(d.p[1] / (std::sqrt(2.0) * d.p[0]));
                r->limC[pk] = d.p[5];
            } else if (d.kind == NlKind::ZenerPair) { // pnjlim on the distance past either breakdown knee (kLimPair)
                r->lim[pk] = kLimPair;
                r->limA[pk] = std::max(1.0 / d.p[4] + d.p[7], 1.0 / d.p[10] + d.p[1]); // the series pair's exponential scale
                r->limB[pk] = d.p[3];  // + side: the first junction breaks down
                r->limC[pk] = d.p[9];  // − side: the second
            } else if (d.kind == NlKind::OutLim) { // tanh-saturating output current
                r->lim[pk] = kLimSat;
                r->limA[pk] = 3.0 / d.p[1];
            } else if (d.kind == NlKind::CmosOut && k > 0) { // the drive saturates: bounded steps on the pin
                r->lim[pk] = kLimStep;
                r->limA[pk] = 1.0;
                r->limB[pk] = -1e9;
                r->limC[pk] = 1.0;
            } else if (d.kind == NlKind::RmsLog && k == 1) { // RMS CAP pin (y = gnd + vofs − cap): a junction law whose
                r->lim[pk] = kLimPnj;                          // scale moves with the level; vcrit at a 7.5 µA timing current
                r->limA[pk] = 1.0 / d.p[2];
                r->limB[pk] = r->limA[pk] * std::log(r->limA[pk] / (std::sqrt(2.0) * 7.5e-6));
                r->limC[pk] = 0.0;
            } else if (d.kind == NlKind::DiodeLight) {
                r->lim[pk] = kLimPnj;
                r->limA[pk] = d.p[1];
                r->limB[pk] = d.p[1] * std::log(d.p[1] / (std::sqrt(2.0) * d.p[0]));
            } else if (d.kind == NlKind::Magnetizing) {
                r->lim[pk] = kLimPnj; // symmetric, log-compressed steps beyond 2·λsat (sinh grows like exp)
                r->limA[pk] = d.p[1];
                r->limB[pk] = 2.0 * d.p[1];
                r->limC[pk] = 1.0;
            } else if (d.kind == NlKind::Triode || d.kind == NlKind::Pentode) {
                // damped steps where the tube law bends (see kLimStep): grids 5 V near cut-in, plates 60 V in the knee
                r->lim[pk] = kLimStep;
                r->limA[pk] = (k == 0) ? 5.0 : 60.0; // swept 2-10 V / 30-120 V on the dimed Twin: these are best
                r->limB[pk] = (k == 0) ? -1.0 : -80.0; // grid: limited above -1 V; plate/screen: limited below 80 V
                r->limC[pk] = (k == 0) ? 1.0 : -1.0;
            } else if (d.kind == NlKind::RailClamp) {
                r->lim[pk] = kLimRail;
                r->limA[pk] = d.p[1];
                r->limB[pk] = d.p[2];
                r->limC[pk] = d.p[3];
            }
        }
        o += d.nports;
    }
    {
        // reduced (clamp-free) port set
        int o = 0;
        for (int di : g.devices) {
            const auto& d = devices_[static_cast<std::size_t>(di)];
            if (d.kind == NlKind::RailClamp) {
                r->clampPort.push_back(o);
                r->clampLo.push_back(d.p[2] + 36.0 * d.p[3]);
                r->clampHi.push_back(d.p[1] - 36.0 * d.p[3]);
            } else {
                r->devR.push_back(&d);
                for (int k = 0; k < d.nports; ++k) {
                    r->red.push_back(o + k);
                    r->bsR.push_back(static_cast<int>(r->red.size()) - 1 - k);
                    r->bnR.push_back(d.nports);
                }
            }
            o += d.nports;
        }
        r->lazyClamps = !r->clampPort.empty() && r->clampPort.size() <= 12 && !r->red.empty() && r->red.size() <= 12 && opt_.lazyClamps;
        const std::size_t nr = r->red.size();
        r->Kr.assign(nr * nr, 0.0);
        r->kcolR.assign(nr, 0.0);
        for (std::size_t j = 0; j < nr; ++j) {
            const auto k = static_cast<std::size_t>(r->red[j]);
            r->limR.push_back(r->lim[k]);
            r->limAr.push_back(r->limA[k]);
            r->limBr.push_back(r->limB[k]);
            r->limCr.push_back(r->limC[k]);
        }
    }
    { // per-port Newton tolerance: tube ports (triode, pentode, vacuum diode) take tubeNewtonTol, every other port newtonTol
        // (or the group's own, "newton": {"groups": {...}}, docs §10.55)
        double jtol = opt_.newtonTol;
        if (!(opt_.newtonTolForced)) {
            std::vector<std::pair<std::string, double>> gt = net_.newtonGroupTols;
            if (const char* e = std::getenv("CE_TOLGROUPS"); e && *e) { // developer A/B: "ref=x,ref=x" ("*=x": every group)
                gt.clear();
                for (std::string_view sv(e); !sv.empty();) {
                    const auto c = sv.find(',');
                    const std::string item(sv.substr(0, c));
                    sv = c == std::string_view::npos ? std::string_view() : sv.substr(c + 1);
                    const auto q = item.find('=');
                    if (q != std::string::npos) gt.emplace_back(item.substr(0, q), std::atof(item.c_str() + q + 1));
                }
            }
            for (const auto& [ref, t] : gt) {
                bool hit = ref == "*";
                for (int pi : g.parts) hit = hit || net_.parts[static_cast<std::size_t>(pi)].ref == ref;
                if (hit && t > 0) jtol = t;
            }
        }
        std::vector<double> pt(static_cast<std::size_t>(nn), jtol);
        int oo = 0;
        for (int di : g.devices) {
            const auto& d = devices_[static_cast<std::size_t>(di)];
            const bool tube = d.kind == NlKind::Triode || d.kind == NlKind::Pentode || d.kind == NlKind::VacDiode;
            for (int k = 0; k < d.nports; ++k) pt[static_cast<std::size_t>(oo + k)] = tube ? opt_.tubeNewtonTol : jtol;
            oo += d.nports;
        }
        r->tol = nn > 0 ? *std::min_element(pt.begin(), pt.end()) : opt_.newtonTol;
        r->ptol.clear();
        if (nn > 0 && *std::max_element(pt.begin(), pt.end()) != r->tol) r->ptol = std::move(pt);
    }
    r->sm = opt_.shermanMorrison && r->n >= 6;
    if (r->sm) {
        r->X.assign(n * n, 0.0);
        r->gStamp.assign(g.prims.size(), 0.0);
        r->colTmp.assign(n, 0.0);
        r->rowTmp.assign(W, 0.0);
        r->rowTmp2.assign(n, 0.0);
    }
    r->bs.assign(std::max<std::size_t>(nn, 1), 0);
    r->bn.assign(std::max<std::size_t>(nn, 1), 1);
    {
        int oo = 0;
        for (int di : g.devices) {
            const auto& d = devices_[static_cast<std::size_t>(di)];
            r->dev.push_back(&d);
            for (int k = 0; k < d.nports; ++k) {
                r->bs[static_cast<std::size_t>(oo + k)] = oo;
                r->bn[static_cast<std::size_t>(oo + k)] = d.nports;
            }
            oo += d.nports;
        }
    }
    if (g.solver == SolverKind::Wdf) {
        r->wdf = std::make_unique<WdfGroup>();
        if (!r->wdf->build(*this, g, error)) return nullptr;
    }
    r->uPrev.assign(static_cast<std::size_t>(r->nu + r->nuc), 0.0);
    {
        const auto nnz = static_cast<std::size_t>(r->nn);
        r->devOfPort.assign(nnz, 0);
        r->baseOfPort.assign(nnz, 0);
        r->dropKind.assign(nnz, 0);
        r->dropBelow.assign(nnz, 0.0);
        r->curPrev1.assign(nnz, 0.0);
        r->curPrev2.assign(nnz, 0.0);
        bool ok = r->nn >= 2 && r->nn <= 24 && !r->lazyClamps;
        int o = 0, q = 0, drops = 0;
        for (int di : g.devices) {
            const auto& d = devices_[static_cast<std::size_t>(di)];
            // current-space Newton suits tubes, cores and lamps; exponential junctions stay in voltage space
            if (d.kind == NlKind::Diode || d.kind == NlKind::DiodePair || d.kind == NlKind::Bjt || d.kind == NlKind::Ota || d.kind == NlKind::RailClamp ||
                d.kind == NlKind::VacDiode || d.kind == NlKind::Neon || d.kind == NlKind::OtaHalf || d.kind == NlKind::DiodeLight ||
                d.kind == NlKind::LampTh || d.kind == NlKind::RmsSq || d.kind == NlKind::RmsLog ||
                d.kind == NlKind::CmosOut || d.kind == NlKind::MosHalf || d.kind == NlKind::Zener || d.kind == NlKind::ZenerPair || d.kind == NlKind::OutLim ||
                d.kind == NlKind::Ad536)
                ok = false;
            for (int k = 0; k < d.nports; ++k) {
                const auto pk = static_cast<std::size_t>(o + k);
                r->devOfPort[pk] = q;
                r->baseOfPort[pk] = o;
                if (d.port[k].from == 0 && d.port[k].to == 0) r->dropKind[pk] = 1, ++drops;
                else if ((d.kind == NlKind::Triode || d.kind == NlKind::Pentode) && k == 0) {
                    r->dropKind[pk] = 2, ++drops;
                    r->dropBelow[pk] = -10.0 * d.p[d.kind == NlKind::Triode ? 6 : 7]; // 10 grid knees below cut-in
                }
            }
            o += d.nports;
            ++q;
        }
        r->activeSet = ok && drops > 0 && !std::getenv("CE_NOACTIVE"); // CE_NOACTIVE: developer A/B
    }
    if (!kEnvNoCodegen && opt_.codegen && nn > 0 && g.solver != SolverKind::Wdf) { // specialised Newton kernels (docs §10.45)
        cg::NewtonPattern q;
        if (newtonPattern(r->dev, r->lim.data(), static_cast<int>(nn), q))
            if (const auto* k = cg::findNewton(q.h1, q.h2)) r->nkFull = k->fn;
        if (r->lazyClamps && r->red.size() <= 12 && newtonPattern(r->devR, r->limR.data(), static_cast<int>(r->red.size()), q))
            if (const auto* k = cg::findNewton(q.h1, q.h2)) r->nkRed = k->fn;
        if (kEnvCgDiag)
            std::fprintf(stderr, "codegen newton: group %zu (nn %d%s) %s%s\n", gi, static_cast<int>(nn), r->lazyClamps ? ", lazy clamps" : "", r->nkFull ? "kernel" : "interpreter",
                         r->lazyClamps ? (r->nkRed ? " / reduced kernel" : " / reduced interpreter") : "");
    }
    return r;
}

bool Circuit::buildGroupsRuntime(std::string& error) {
    rt_.clear();
    rtHalf_.clear();
    rtBe_.clear();
    rtTr_.clear();
    // Op-amp input current noise (docs §3.9): a source at each input pin, owned by whichever group solves that node (a
    // sense-only input is often in the driving network's group, not the op-amp's).
    struct PinNoise {
        int node;
        double in, fci;
    };
    std::vector<PinNoise> pinNoise;
    if (opt_.noise)
        for (const auto& pp : net_.parts) {
            if (pp.type != "opamp" || pp.nodes.size() < 2) continue;
            const auto m = resolveModel(net_, pp.model);
            if (m && m->get("level", 1) < 0.5) continue; // the nullor has no input stage
            const OpampNoise dn = opampNoiseDefaults(pp.model);
            const double in = m ? m->get("in", dn.in) : dn.in, fci = m ? m->get("fci", dn.fci) : dn.fci;
            if (in > 0)
                for (int pin = 0; pin < 2; ++pin) pinNoise.push_back({node(pp.nodes[static_cast<std::size_t>(pin)]), in, fci});
        }
    for (std::size_t gi = 0; gi < groups_.size(); ++gi) {
        auto r = makeGroupRt(gi, error);
        if (!r) return false;
        rt_.push_back(std::move(r));
        std::unique_ptr<GroupRt> h;
        if (groups_[gi].nn > 0 && groups_[gi].solver == SolverKind::Dk) h = makeGroupRt(gi, error);
        rtHalf_.push_back(std::move(h));
        std::array<std::unique_ptr<GroupRt>, 3> be, tr;
        auto& r0 = *rt_.back();
        r0.lteOn = opt_.lteTol > 0 && rtHalf_.back() && r0.ns > 0 && r0.nn <= opt_.lteMaxPorts && groups_[gi].level == 0;
        if (r0.lteOn) {
            // BDF2 start: not with parts flagged backward Euler on their own (Prim::be), whose companions assume the step
            r0.bdf2 = opt_.lteStart == 1;
            for (int st : groups_[gi].states) r0.bdf2 = r0.bdf2 && !prims_[static_cast<std::size_t>(st)].be;
            for (std::size_t lv = 0; lv < 2; ++lv) { // T/2 and T/4 (kLteMaxSubsteps)
                be[lv] = makeGroupRt(gi, error);
                if (!be[lv]) return false;
                be[lv]->beAll = !r0.bdf2; // BDF2: a trapezoidal runtime at step 2βT/m (subRuntime)
                be[lv]->iLast.assign(static_cast<std::size_t>(r0.ns), 0.0);
                if (lv > 0 && !(tr[lv] = makeGroupRt(gi, error))) return false;
            }
            // TR-BDF2 substeps (docs §10.47): trapezoidal runtimes at γT and γT/2 in the unused [2] slots
            r0.tb = kEnvTrBdf2 > 0 || (kEnvTrBdf2 < 0 && opt_.lteStart == 2);
            for (int st : groups_[gi].states) r0.tb = r0.tb && !prims_[static_cast<std::size_t>(st)].be;
            if (r0.tb) {
                if (!(tr[2] = makeGroupRt(gi, error)) || !(be[2] = makeGroupRt(gi, error))) return false;
                for (auto* t : {tr[2].get(), be[2].get()}) t->xh.assign(static_cast<std::size_t>(3 * r0.ns), 0.0); // stage scratch
            } else if (groups_[gi].evos >= 3) { // 8-substep events (docs §10.62): T/8 runtimes in the [2] slots
                if (!(tr[2] = makeGroupRt(gi, error)) || !(be[2] = makeGroupRt(gi, error))) return false;
                be[2]->beAll = !r0.bdf2;
                be[2]->iLast.assign(static_cast<std::size_t>(r0.ns), 0.0);
            }
            r0.xh.assign(static_cast<std::size_t>(5 * r0.ns), 0.0);
            r0.xInd.assign(static_cast<std::size_t>(r0.ns), 0);
            r0.lteAbs.assign(static_cast<std::size_t>(r0.ns), 0.0);
            if (kEnvLteWho) r0.lteWho.assign(static_cast<std::size_t>(r0.ns), 0);
            for (int k = 0; k < r0.ns; ++k) {
                const bool ind = prims_[static_cast<std::size_t>(groups_[gi].states[static_cast<std::size_t>(k)])].kind == Prim::Ind;
                r0.xInd[static_cast<std::size_t>(k)] = ind ? 1 : 0;
                r0.lteAbs[static_cast<std::size_t>(k)] = opt_.lteTol * (ind ? kLteAbsA : kLteAbsV);
            }
        }
        // event substeps' Newton tolerance (docs §10.64): the substep runtimes' tube ports only; the full step and every
        // other port keep their own (a junction's law bends on n·Vt, §10.42: at 10x the 5150's clamp network misses its
        // SPICE gate)
        const double subTol = kEnvSubTol > 0 ? kEnvSubTol : (net_.newtonSubsteps > 0 ? net_.newtonSubsteps : 1.0);
        if (subTol != 1.0 && groups_[gi].nn > 0) {
            const auto& gs = groups_[gi];
            std::vector<std::uint8_t> tube(static_cast<std::size_t>(gs.nn), 0);
            int o = 0;
            for (int di : gs.devices) {
                const auto& d = devices_[static_cast<std::size_t>(di)];
                const bool t = d.kind == NlKind::Triode || d.kind == NlKind::Pentode || d.kind == NlKind::VacDiode;
                for (int k = 0; k < d.nports; ++k) tube[static_cast<std::size_t>(o + k)] = t ? 1 : 0;
                o += d.nports;
            }
            const bool allTube = std::all_of(tube.begin(), tube.end(), [](std::uint8_t t) { return t != 0; });
            auto scale = [&](GroupRt* x) {
                if (!x) return;
                if (x->ptol.empty()) { // one tolerance for every port: scaled only if they are all a tube's
                    if (allTube) x->tol *= subTol;
                    return;
                }
                for (std::size_t k = 0; k < x->ptol.size(); ++k)
                    if (tube[k]) x->ptol[k] *= subTol;
            };
            scale(rtHalf_.back().get());
            for (auto& x : be) scale(x.get());
            for (auto& x : tr) scale(x.get());
        }
        rtBe_.push_back(std::move(be));
        rtTr_.push_back(std::move(tr));
        if (opt_.noise && !r0.wdf && r0.n > 0) { // noise columns: the unknown nodes (KCL rows) its resistors touch
            const auto& g0n = groups_[gi];
            std::map<int, int> colOf;
            auto col = [&](int node) {
                if (node <= 0) return -1;
                const int u = r0.unkOf[static_cast<std::size_t>(node)];
                if (u < 0 || !r0.kcl[static_cast<std::size_t>(u)]) return -1;
                auto it = colOf.find(u);
                if (it != colOf.end()) return it->second;
                const int c = static_cast<int>(colOf.size());
                colOf.emplace(u, c);
                r0.noiseRow.push_back(u);
                return c;
            };
            for (int pi : g0n.prims) {
                const auto& p = prims_[static_cast<std::size_t>(pi)];
                if (p.kind != Prim::G || p.part < 0) continue; // netlist resistors and pot segments (not internal stamps)
                const auto& part = net_.parts[static_cast<std::size_t>(p.part)];
                if (part.type != "R" && part.type != "pot" && part.type != "rheo") continue;
                const int ca = col(p.a), cb = col(p.b);
                if (ca < 0 && cb < 0) continue;
                r0.noiseR.push_back({pi, ca, cb, 0.0, -1, -1});
            }
            std::vector<double> flickFc; // corner per bank, designed below once the draw rate is known
            auto newFlick = [&](double fc) {
                if (!(fc > 0)) return -1;
                flickFc.push_back(fc);
                return static_cast<int>(flickFc.size()) - 1;
            };
            int o = 0;
            for (int di : g0n.devices) { // junctions with shot noise; op-amp input noise
                const auto& d = devices_[static_cast<std::size_t>(di)];
                const auto* pp = d.part >= 0 ? &net_.parts[static_cast<std::size_t>(d.part)] : nullptr;
                switch (d.kind) {
                    case NlKind::Diode: // a series string of m junctions: m independent sources across 1/m of its rd each
                        r0.shotDev.push_back({o, 0, -1, 1.0 - d.p[3]});
                        break;
                    case NlKind::DiodePair:
                    case NlKind::Zener:
                    case NlKind::ZenerPair: // (the pair's current through both junctions in series)
                    case NlKind::Jfet: r0.shotDev.push_back({o, 0}); break; // JFET: gate leakage
                    case NlKind::Bjt: r0.shotDev.push_back({o, 1}); break;
                    case NlKind::Triode:
                    case NlKind::Pentode: { // plate flicker: model "fcf" (corner, Hz; 0 = none, docs §3.9)
                        const auto m = pp ? resolveModel(net_, pp->model) : std::nullopt;
                        r0.shotDev.push_back({o, 2, newFlick(m ? m->get("fcf", 0.0) : 0.0)});
                        break;
                    }
                    case NlKind::Ota: // op-amp macro input stage: en in series with the sensing port, in at each input pin
                        if (pp && pp->type == "opamp") {
                            const auto m = resolveModel(net_, pp->model);
                            const OpampNoise dn = opampNoiseDefaults(pp->model);
                            const double en = m ? m->get("en", dn.en) : dn.en;
                            if (en > 0) r0.portV.push_back({o, en, 0.0, newFlick(m ? m->get("fce", dn.fce) : dn.fce)});
                        }
                        break;
                    default: break;
                }
                o += d.nports;
            }
            for (const auto& pn : pinNoise) { // op-amp input current noise at the pins this group solves
                const int c = col(pn.node);
                if (c < 0) continue;
                GroupRt::NoiseR q{-1, c, -1, 0.0, -1, -1};
                q.dens = pn.in;
                q.fl = newFlick(pn.fci);
                r0.noiseR.push_back(q);
            }
            r0.nNoise = static_cast<int>(colOf.size());
            r0.noiseOn = r0.nNoise > 0 || r0.nn > 0;
            const auto nc = static_cast<std::size_t>(r0.nNoise);
            r0.Pn.assign(static_cast<std::size_t>(r0.nn) * nc, 0.0);
            r0.Hn.assign(static_cast<std::size_t>(r0.ny + r0.ns) * nc, 0.0);
            r0.zcol.assign(static_cast<std::size_t>(r0.n), 0.0);
            r0.dP.assign(static_cast<std::size_t>(r0.nn), 0.0);
            r0.dOut.assign(static_cast<std::size_t>(r0.ny + r0.ns), 0.0);
            r0.dI.assign(static_cast<std::size_t>(r0.nn), 0.0);
            r0.noiseI0.assign(static_cast<std::size_t>(r0.nn), 0.0);
            r0.noiseGm0.assign(static_cast<std::size_t>(r0.nn), 0.0);
            r0.actCol.assign(nc, 0);
            r0.colKeep.assign(nc, 0);
            r0.colVar.assign(nc, 0.0);
            r0.rAct.assign(r0.noiseR.size(), 0);
            const std::size_t ncP = (nc + 3) & ~std::size_t{3}, nnP = (static_cast<std::size_t>(r0.nn) + 3) & ~std::size_t{3};
            { // the unknowns the exports, states and ports read, and each one's gather index
                std::map<int, int> zOf;
                for (int e : g0n.exports) { // the exports first, one row each: dOut's export part is z's head
                    const int u = e > 0 ? r0.unkOf[static_cast<std::size_t>(e)] : -1;
                    if (u >= 0) zOf.emplace(u, static_cast<int>(r0.zNeed.size()));
                    r0.zNeed.push_back(u);
                }
                auto zi = [&](int node) {
                    if (node <= 0) return -1;
                    const int u = r0.unkOf[static_cast<std::size_t>(node)];
                    if (u < 0) return -1;
                    auto it = zOf.find(u);
                    if (it != zOf.end()) return it->second;
                    const int k = static_cast<int>(r0.zNeed.size());
                    zOf.emplace(u, k);
                    r0.zNeed.push_back(u);
                    return k;
                };
                for (int pi : g0n.states) {
                    const auto& p = prims_[static_cast<std::size_t>(pi)];
                    r0.gSt.push_back(zi(p.a));
                    r0.gSt.push_back(zi(p.b));
                }
                for (int di : g0n.devices) {
                    const auto& d = devices_[static_cast<std::size_t>(di)];
                    for (int k = 0; k < d.nports; ++k) {
                        r0.gPort.push_back(zi(d.port[k].cp));
                        r0.gPort.push_back(zi(d.port[k].cn));
                    }
                }
                r0.nz = static_cast<int>(r0.zNeed.size());
                for (auto* v : {&r0.gSt, &r0.gPort})
                    for (int& x : *v)
                        if (x < 0) x = r0.nz;
            }
            r0.Zd.assign(static_cast<std::size_t>(r0.nz) * nc, 0.0);
            r0.zv.assign(static_cast<std::size_t>(r0.nz) + 1, 0.0);
            const std::size_t srcP = std::max(ncP, (r0.noiseR.size() + 3) & ~std::size_t{3}); // folded: a column per source
            r0.Zf.assign(static_cast<std::size_t>(r0.nz) * srcP, 0.0f);
            r0.Kf.assign(static_cast<std::size_t>(r0.nn) * nnP, 0.0f);
            r0.xf.assign(srcP + 2 * nnP, 0.0f); // [columns (folded: sources) | port currents | port voltages]
            r0.xr.assign(static_cast<std::size_t>(kNoiseTaps) * (srcP + 2 * nnP), 0.0f);
            const std::size_t zBlocks = static_cast<std::size_t>(r0.nz / 4 + r0.nz % 4);
            r0.zcOff.assign(zBlocks + 1, 0);
            r0.zcCol.assign(zBlocks * (srcP / 4), 0);
            r0.zRowMin.assign(static_cast<std::size_t>(r0.nz), 0.0);
            r0.gz.assign(((r0.noiseR.size() + 3 * static_cast<std::size_t>(r0.nn) + kFlickMax * flickFc.size()) + 3) & ~std::size_t{3}, 0.0f);
            r0.nDraw = 2 * r0.nn; // + the kept resistors and the 1/f sections (deriveFromZ)
            // Draws at ~44.1-48 kHz, each held for noiseDiv samples. A hold of d samples shapes the noise by
            // D = |sin(dθ)/(d·sin θ)|² (θ = πfT); the symmetric 7-tap compensation C(φ) = c0 + 2Σ ck·cos(kφ) on the
            // draw sequence (φ = 2πf·dT, c0 = 1 − 2Σ ck so the DC level 4kT/R is exact) is the least-squares fit of
            // 1/√D over 0-20 kHz: C²·D is flat within 0.19 dB at 20 kHz (d = 4 at 176.4 kHz; 0.15 dB at d = 2,
            // 88.2 kHz), within 0.02 dB below 10 kHz.
            r0.noiseDiv = std::max(1, static_cast<int>(std::floor(1.0 / (g0n.T * 44100.0) + 1e-6)));
            if (r0.noiseDiv > 1) {
                const double dd = r0.noiseDiv, fu = 1.0 / (g0n.T * dd);
                double A[3][3] = {}, bv[3] = {};
                for (int i = 1; i <= 400; ++i) {
                    const double f = 20000.0 * i / 400.0, th = M_PI * f * g0n.T, ph = 2.0 * M_PI * f / fu;
                    const double D = std::pow(std::sin(dd * th) / (dd * std::sin(th)), 2.0), t = 1.0 / std::sqrt(D) - 1.0;
                    const double a[3] = {2.0 * (std::cos(ph) - 1.0), 2.0 * (std::cos(2.0 * ph) - 1.0), 2.0 * (std::cos(3.0 * ph) - 1.0)};
                    for (int u = 0; u < 3; ++u) {
                        bv[u] += a[u] * t;
                        for (int v = 0; v < 3; ++v) A[u][v] += a[u] * a[v];
                    }
                }
                for (int k = 0; k < 3; ++k) // Gauss-Jordan on the 3×3 normal equations (well conditioned)
                    for (int u = 0; u < 3; ++u) {
                        if (u == k) continue;
                        const double m = A[u][k] / A[k][k];
                        for (int v = 0; v < 3; ++v) A[u][v] -= m * A[k][v];
                        bv[u] -= m * bv[k];
                    }
                double sum = 0;
                for (int k = 0; k < 3; ++k) {
                    r0.comp[k + 1] = static_cast<float>(bv[k] / A[k][k]);
                    sum += bv[k] / A[k][k];
                }
                r0.comp[0] = static_cast<float>(1.0 - 2.0 * sum);
            }
            // 1/f banks: sections 0.75 decade apart, down from min(10·fc, fu/4) to 2 Hz (3-6 for corners 45 Hz-2 kHz),
            // unit AR(1) each, weights c_k ≥ 0 fitted (least squares, active set) so 1 + Σ c_k·H_k(f) matches 1 + fc/f
            // relative to the total over 5 Hz-20 kHz; H_k = (1 − a²)/|1 − a·e^{−jω}|² at the draw rate. Within 0.1 dB
            // over 20 Hz-20 kHz; zero-weight sections are dropped.
            {
                const double fu = 1.0 / (g0n.T * r0.noiseDiv);
                for (double fc : flickFc) {
                    GroupRt::Flick fl;
                    double fk[kFlickMax];
                    const double top = std::min(10.0 * fc, fu / 4);
                    for (int k = 0; k < kFlickMax; ++k) {
                        const double f = top * std::pow(10.0, -0.75 * k);
                        if (f < 2.0) break;
                        fk[fl.n++] = f;
                    }
                    const int K = fl.n;
                    std::vector<double> A, b; // rows: grid points; relative to the total 1 + fc/f
                    for (int i = 0; i <= 120; ++i) {
                        const double f = 5.0 * std::pow(4000.0, i / 120.0), w = 2.0 * M_PI * f / fu, tgt = 1.0 + fc / f;
                        for (int k = 0; k < K; ++k) {
                            const double a = std::exp(-2.0 * M_PI * fk[k] / fu);
                            A.push_back((1.0 - a * a) / (1.0 + a * a - 2.0 * a * std::cos(w)) / tgt);
                        }
                        b.push_back((fc / f) / tgt);
                    }
                    std::vector<int> act(static_cast<std::size_t>(K), 1);
                    double cw[kFlickMax] = {};
                    for (int pass = 0; pass < K; ++pass) { // normal equations over the active sections; drop the most negative
                        double M[kFlickMax][kFlickMax + 1] = {};
                        for (std::size_t i = 0; i < b.size(); ++i)
                            for (int u = 0; u < K; ++u) {
                                if (!act[static_cast<std::size_t>(u)]) continue;
                                const double au = A[i * static_cast<std::size_t>(K) + static_cast<std::size_t>(u)];
                                M[u][K] += au * b[i];
                                for (int v = 0; v < K; ++v)
                                    if (act[static_cast<std::size_t>(v)]) M[u][v] += au * A[i * static_cast<std::size_t>(K) + static_cast<std::size_t>(v)];
                            }
                        for (int u = 0; u < K; ++u)
                            if (!act[static_cast<std::size_t>(u)]) M[u][u] = 1.0, M[u][K] = 0.0;
                        for (int k = 0; k < K; ++k) // Gauss-Jordan with partial pivoting
                        {
                            int pv = k;
                            for (int u = k + 1; u < K; ++u)
                                if (std::abs(M[u][k]) > std::abs(M[pv][k])) pv = u;
                            for (int v = 0; v <= K; ++v) std::swap(M[k][v], M[pv][v]);
                            for (int u = 0; u < K; ++u) {
                                if (u == k || M[k][k] == 0.0) continue;
                                const double f = M[u][k] / M[k][k];
                                for (int v = 0; v <= K; ++v) M[u][v] -= f * M[k][v];
                            }
                        }
                        int worst = -1;
                        for (int u = 0; u < K; ++u) {
                            cw[u] = M[u][u] != 0.0 ? M[u][K] / M[u][u] : 0.0;
                            if (act[static_cast<std::size_t>(u)] && cw[u] < 0 && (worst < 0 || cw[u] < cw[worst])) worst = u;
                        }
                        if (worst < 0) break;
                        act[static_cast<std::size_t>(worst)] = 0;
                    }
                    fl.n = 0; // the sections with a weight
                    for (int k = 0; k < K; ++k) {
                        if (!(act[static_cast<std::size_t>(k)] && cw[k] > 0)) continue;
                        const double a = std::exp(-2.0 * M_PI * fk[k] / fu);
                        fl.a[fl.n] = static_cast<float>(a);
                        fl.c[fl.n] = static_cast<float>(std::sqrt(1.0 - a * a));
                        fl.w[fl.n++] = static_cast<float>(std::sqrt(cw[k]));
                    }
                    fl.fc = fc;
                    r0.flick.push_back(fl);
                }
            }
            std::uint64_t h = opt_.noiseSeed * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull + gi;
            for (const auto& pr : g0n.parts) h = fnv(h, net_.parts[static_cast<std::size_t>(pr)].ref);
            for (auto& s : r0.rng) { // splitmix64 (two lanes' states)
                h += 0x9E3779B97F4A7C15ull;
                std::uint64_t z = h;
                z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
                z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
                s = z ^ (z >> 31);
            }
        }
    }
    halfStale_.assign(groups_.size(), 1);
    beStale_.assign(groups_.size(), {1, 1, 1});
    trStale_.assign(groups_.size(), {1, 1, 1});
    groupNs_.assign(groups_.size(), 0.0);
    newtonNs_.assign(groups_.size(), 0.0);
    buildSmallSignal(); // GR_LESSONS P2 (Small-signal path section)
    return true;
}

// ───────────────────────────── matrices ─────────────────────────────
// Solve LU·X = P·B for all W right-hand-side columns of the row-major n×W block B at once (la::luFactor's factors).
// Row operations over contiguous rows (vectorised) replace one strided column at a time: the per-element operations
// and their order are those of the column-wise substitution, and zero multipliers (most of a sparse MNA factor) are
// skipped (subtracting 0·x leaves a finite value unchanged). About 6x faster on a 51×60 group, so a control refresh
// (every 64th glide step, every switch) no longer lands a 100-250 µs spike in an audio block.
static void luSolveRows(const double* __restrict LU, const int* piv, int n, double* __restrict B, int W) noexcept {
    for (int k = 0; k < n; ++k)
        if (piv[k] != k) std::swap_ranges(B + k * W, B + (k + 1) * W, B + piv[k] * W);
    for (int i = 1; i < n; ++i) {
        double* __restrict bi = B + i * W;
        for (int k = 0; k < i; ++k) {
            const double f = LU[i * n + k];
            if (f == 0.0) continue;
            const double* __restrict bk = B + k * W;
            for (int c = 0; c < W; ++c) bi[c] -= f * bk[c];
        }
    }
    for (int i = n - 1; i >= 0; --i) {
        double* __restrict bi = B + i * W;
        for (int k = i + 1; k < n; ++k) {
            const double f = LU[i * n + k];
            if (f == 0.0) continue;
            const double* __restrict bk = B + k * W;
            for (int c = 0; c < W; ++c) bi[c] -= f * bk[c];
        }
        const double d = LU[i * n + i];
        for (int c = 0; c < W; ++c) bi[c] = bi[c] / d;
    }
}

void Circuit::refreshGroup(std::size_t gi, bool dc) noexcept {
    const auto& g = groups_[gi];
    auto& r = *rt_[gi];
    ++dg(gi).refreshes;
    if (gi < halfStale_.size()) halfStale_[gi] = 1;
    if (gi < beStale_.size()) beStale_[gi] = trStale_[gi] = {1, 1, 1};
    const int n = r.n, W = r.W, ns = r.ns, nu = r.nu;
    std::fill(r.M.begin(), r.M.end(), 0.0);
    std::fill(r.R.begin(), r.R.end(), 0.0);
    auto row = [&](int node) { return node > 0 && r.unkOf[static_cast<std::size_t>(node)] >= 0 && r.kcl[static_cast<std::size_t>(r.unkOf[static_cast<std::size_t>(node)])] ? r.unkOf[static_cast<std::size_t>(node)] : -1; };
    // Add coef·V[node] to equation row `rw` (moves to the RHS for inputs).
    auto addV = [&](int rw, int node, double coef) {
        if (rw < 0 || node <= 0) return;
        const int u = r.unkOf[static_cast<std::size_t>(node)];
        if (u >= 0) r.M[static_cast<std::size_t>(rw * n + u)] += coef;
        else {
            const int j = r.inOf[static_cast<std::size_t>(node)];
            if (j >= 0) r.R[static_cast<std::size_t>(rw * W + ns + j)] -= coef;
        }
    };
    auto conductance = [&](int a, int b, double G) {
        const int ra = row(a), rb = row(b);
        addV(ra, a, G);
        addV(ra, b, -G);
        addV(rb, b, G);
        addV(rb, a, -G);
    };
    int stateIdx = 0;
    for (int pi : g.prims) {
        const auto& p = prims_[static_cast<std::size_t>(pi)];
        const double val = p.slot >= 0 ? slots_[static_cast<std::size_t>(p.slot)].value : p.fixed;
        switch (p.kind) {
            case Prim::G: conductance(p.a, p.b, 1.0 / val); break;
            case Prim::Cap:
            case Prim::Ind: {
                // find this prim's state index
                int k = stateIdx;
                while (g.states[static_cast<std::size_t>(k)] != pi) ++k;
                // Trapezoidal: G = 2C/T (T/2L), s' = 2G·v ∓ s.  Backward Euler: G = C/T (T/L), s' = G·v (+ s for L).
                const double Tg = g.T;
                const bool be = p.be || r.beAll; // beAll: the LTE control's backward-Euler substep runtimes
                const double G = p.kind == Prim::Cap ? (be ? val / Tg : 2.0 * val / Tg) : (be ? Tg / val : Tg / (2.0 * val));
                if (dc) {
                    if (p.kind == Prim::Ind) conductance(p.a, p.b, kDcInductorG);
                } else {
                    conductance(p.a, p.b, G);
                    const double s = p.kind == Prim::Cap ? 1.0 : -1.0; // cap: i = Gv - s ; ind: i = Gv + s
                    const int ra = row(p.a), rb = row(p.b);
                    if (ra >= 0) r.R[static_cast<std::size_t>(ra * W + k)] += s;
                    if (rb >= 0) r.R[static_cast<std::size_t>(rb * W + k)] -= s;
                    auto& gcK = r.gc[static_cast<std::size_t>(k)];
                    if (gcK != 0.0 && gcK != G) r.w[static_cast<std::size_t>(k)] += (G - gcK) * r.vcLast[static_cast<std::size_t>(k)];
                    gcK = G;
                    r.G2[static_cast<std::size_t>(k)] = be ? G : 2.0 * G;
                    r.sgn[static_cast<std::size_t>(k)] = p.kind == Prim::Cap ? (be ? 0.0 : -1.0) : 1.0;
                }
                break;
            }
            case Prim::Vcvs: {
                const int rw = r.unkOf[static_cast<std::size_t>(p.a)];
                addV(rw, p.a, 1.0);
                addV(rw, p.c, -p.fixed);
                addV(rw, p.d, p.fixed);
                break;
            }
            case Prim::Nullor: {
                const int rw = r.unkOf[static_cast<std::size_t>(p.a)];
                addV(rw, p.c, 1.0);
                addV(rw, p.d, -1.0);
                break;
            }
            case Prim::Vccs: {
                const int ra = row(p.a), rb = row(p.b);
                addV(ra, p.c, p.fixed);
                addV(ra, p.d, -p.fixed);
                addV(rb, p.c, -p.fixed);
                addV(rb, p.d, p.fixed);
                break;
            }
            case Prim::Winding: {
                // Branch current i (a → b through the winding) is an extra unknown. KCL: +i at a, −i at b, and N·i
                // into the core node's MMF balance. Its own row: v_a − v_b − N·v_core = 0.
                int k = 0;
                while (g.branches[static_cast<std::size_t>(k)] != pi) ++k;
                const int col = static_cast<int>(g.unknowns.size()) + k;
                const int ra = row(p.a), rb = row(p.b), rc = row(p.c);
                if (ra >= 0) r.M[static_cast<std::size_t>(ra * n + col)] += 1.0;
                if (rb >= 0) r.M[static_cast<std::size_t>(rb * n + col)] -= 1.0;
                if (rc >= 0) r.M[static_cast<std::size_t>(rc * n + col)] += p.fixed;
                addV(col, p.a, 1.0);
                addV(col, p.b, -1.0);
                addV(col, p.c, -p.fixed);
                break;
            }
        }
    }
    if (!dc)
        for (int x : g.cutIn) { // signal cuts: the loads' companion conductance (see compile)
            const int rx = row(x);
            if (rx >= 0) r.M[static_cast<std::size_t>(rx * n + rx)] += tranLoadG_[static_cast<std::size_t>(x)];
        }
    if (dc) {
        for (int i = 0; i < n; ++i)
            if (r.kcl[static_cast<std::size_t>(i)]) r.M[static_cast<std::size_t>(i * n + i)] += dcGmin_;
        for (int x : g.cutIn) { // the loads' DC conductance (their current is linearised around the last iterate)
            const int rx = row(x);
            if (rx >= 0) r.M[static_cast<std::size_t>(rx * n + rx)] += dcLoadG_[static_cast<std::size_t>(x)];
        }
        // Transformer cores at DC: the flux row is the constraint v_core = 0 (no gmin), and the magnetizing law's
        // linear part λ/Lm enters the core row so the matrix stays regular; the device supplies the rest.
        for (const auto& mg : mags_) {
            const int uf = r.unkOf[static_cast<std::size_t>(mg.flux)], uc = r.unkOf[static_cast<std::size_t>(mg.core)];
            if (uf < 0 || uc < 0) continue;
            r.M[static_cast<std::size_t>(uf * n + uf)] -= dcGmin_;
            r.M[static_cast<std::size_t>(uc * n + uf)] -= 1.0 / mg.lm;
        }
    }
    // NL current injections.
    {
        int o = 0;
        for (int di : g.devices) {
            const auto& d = devices_[static_cast<std::size_t>(di)];
            for (int k = 0; k < d.nports; ++k) {
                const int col = ns + nu + o + k;
                const int rf = row(d.port[k].from), rt = row(d.port[k].to);
                if (rf >= 0) r.R[static_cast<std::size_t>(rf * W + col)] -= 1.0;
                if (rt >= 0) r.R[static_cast<std::size_t>(rt * W + col)] += 1.0;
            }
            o += d.nports;
        }
    }
    // Cut-current inputs: the consumers' current enters the KCL row of its cut node.
    for (int j = 0; j < r.nuc; ++j) {
        const int rx = row(g.cutIn[static_cast<std::size_t>(j)]);
        if (rx >= 0) r.R[static_cast<std::size_t>(rx * W + ns + nu + r.nn + j)] += 1.0;
    }
    // Z = M⁻¹ R
    std::copy(r.R.begin(), r.R.end(), r.Z.begin());
    if ((r.sp || r.sl) && n > 0) { // the sparse paths factorise from M's values (M − B·Jd·C; M itself)
        std::copy(r.M.begin(), r.M.end(), r.Mraw.begin());
        if (r.sp) r.sp->stale = true; // its static part (spSnapshot) is rebuilt at the next factorisation
        if (r.sp && r.sp->ok && !dc) // a stamp outside the pattern fixed at reset (cannot happen with today's stamps): dense from now on
            for (std::size_t q = 0; q < r.Mraw.size(); ++q)
                if (r.Mraw[q] != 0.0 && !r.sp->mStruct[q]) {
                    r.sp->ok = false;
                    break;
                }
        if (r.sl && r.sl->on) {
            if (!dc) {
                for (std::size_t q = 0; q < r.Mraw.size() && r.sl->on; ++q) r.sl->on = !(r.Mraw[q] != 0.0 && !r.sl->lu.mStruct[q]);
                for (std::size_t q = 0; q < r.R.size() && r.sl->on; ++q) r.sl->on = !(r.R[q] != 0.0 && !r.sl->rStruct[q]);
            }
            r.sl->valid = r.sl->on && !dc && spFactor(r.sl->lu, nullptr, nullptr, 0, 0, nullptr) && r.sl->pack(r.R.data());
        }
    }
    if (n > 0) {
        if (!la::luFactor(r.M.data(), n, r.piv.data())) {
            ++dg(gi).failures;
            return;
        }
        luSolveRows(r.M.data(), r.piv.data(), n, r.Z.data(), W);
    }
    // Sherman–Morrison bookkeeping: explicit inverse and the conductances just stamped.
    if (!dc && r.sm && n > 0) {
        std::fill(r.X.begin(), r.X.end(), 0.0);
        for (int c = 0; c < n; ++c) r.X[static_cast<std::size_t>(c * n + c)] = 1.0;
        luSolveRows(r.M.data(), r.piv.data(), n, r.X.data(), n); // X = M⁻¹, all columns at once
        for (std::size_t q = 0; q < g.prims.size(); ++q) r.gStamp[q] = primConductance(g, g.prims[q]);
        r.smCount = 0;
    }
    r.dcMode = dc;
    deriveFromZ(gi, dc);
}

double Circuit::primConductance(const Group& g, int pi) const noexcept {
    const auto& p = prims_[static_cast<std::size_t>(pi)];
    if (p.kind != Prim::G && p.kind != Prim::Cap && p.kind != Prim::Ind) return 0.0;
    const double val = p.slot >= 0 ? slots_[static_cast<std::size_t>(p.slot)].value : p.fixed;
    if (p.kind == Prim::G) return 1.0 / val;
    const double Tg = g.T;
    return p.kind == Prim::Cap ? (p.be ? val / Tg : 2.0 * val / Tg) : (p.be ? Tg / val : Tg / (2.0 * val));
}

// Rank-1 Sherman–Morrison updates for changed conductances (pots, value laws, glided caps): O(n² + n·W) each
// instead of a fresh LU and solve. Switches (huge steps) and every 64th update take the full path.
bool Circuit::smUpdate(std::size_t gi) noexcept {
    const auto& g = groups_[gi];
    auto& r = *rt_[gi];
    if (gi < halfStale_.size()) halfStale_[gi] = 1;
    if (gi < beStale_.size()) beStale_[gi] = trStale_[gi] = {1, 1, 1};
    if (!r.sm || r.dcMode || r.smCount >= 64) return false;
    const int n = r.n, W = r.W, ns = r.ns;
    auto row = [&](int node) { return node > 0 && r.unkOf[static_cast<std::size_t>(node)] >= 0 && r.kcl[static_cast<std::size_t>(r.unkOf[static_cast<std::size_t>(node)])] ? r.unkOf[static_cast<std::size_t>(node)] : -1; };
    auto unk = [&](int node) { return node > 0 ? r.unkOf[static_cast<std::size_t>(node)] : -1; };
    auto inp = [&](int node) { return node > 0 ? r.inOf[static_cast<std::size_t>(node)] : -1; };
    for (std::size_t q = 0; q < g.prims.size(); ++q) {
        const int pi = g.prims[q];
        const auto& p = prims_[static_cast<std::size_t>(pi)];
        if (p.slot < 0 || (p.kind != Prim::G && p.kind != Prim::Cap && p.kind != Prim::Ind)) continue;
        if (slots_[static_cast<std::size_t>(p.slot)].kind == ValueSlot::Switch) {
            if (primConductance(g, pi) != r.gStamp[q]) return false;
            continue;
        }
        const double Gn = primConductance(g, pi), dG = Gn - r.gStamp[q];
        if (dG == 0.0) continue;
        const int ra = row(p.a), rb = row(p.b), ua = unk(p.a), ub = unk(p.b), ja = inp(p.a), jb = inp(p.b);
        if (r.sp || r.sl) { // keep M's raw values current for the sparse paths (the same stamps as refreshGroup)
            if (r.sp) r.sp->stale = true;
            auto at = [&](int rw, int u, double v) {
                if (rw >= 0 && u >= 0) r.Mraw[static_cast<std::size_t>(rw * n + u)] += v;
            };
            at(ra, ua, dG);
            at(ra, ub, -dG);
            at(rb, ub, dG);
            at(rb, ua, -dG);
        }
        if (r.sl) { // ...and R's input columns (refreshGroup's addV moves an input node's coefficient to the right side)
            auto atR = [&](int rw, int j, double v) {
                if (rw >= 0 && j >= 0) r.R[static_cast<std::size_t>(rw * W + ns + j)] += v;
            };
            atR(ra, ja, -dG);
            atR(rb, ja, dG);
            atR(ra, jb, dG);
            atR(rb, jb, -dG);
        }
        if (ra >= 0 || rb >= 0) {
            double* xr = r.colTmp.data(); // X r
            for (int k = 0; k < n; ++k)
                xr[k] = (ra >= 0 ? r.X[static_cast<std::size_t>(k * n + ra)] : 0.0) - (rb >= 0 ? r.X[static_cast<std::size_t>(k * n + rb)] : 0.0);
            const double beta = (ua >= 0 ? xr[ua] : 0.0) - (ub >= 0 ? xr[ub] : 0.0);
            const double den = 1.0 + dG * beta;
            if (!(std::abs(den) > 1e-9)) return false;
            const double gam = dG / den;
            // wᵀ = γ (qᵀ − cᵀZ)
            double* w = r.rowTmp.data();
            for (int c = 0; c < W; ++c)
                w[c] = -gam * ((ua >= 0 ? r.Z[static_cast<std::size_t>(ua * W + c)] : 0.0) - (ub >= 0 ? r.Z[static_cast<std::size_t>(ub * W + c)] : 0.0));
            if (jb >= 0) w[ns + jb] += gam;
            if (ja >= 0) w[ns + ja] -= gam;
            for (int k = 0; k < n; ++k) {
                const double a = xr[k];
                if (a == 0.0) continue;
                double* z = &r.Z[static_cast<std::size_t>(k * W)];
                for (int c = 0; c < W; ++c) z[c] += a * w[c];
            }
            // X' = X − γ (X r)(cᵀ X)
            double* cx = r.rowTmp2.data();
            for (int c = 0; c < n; ++c)
                cx[c] = (ua >= 0 ? r.X[static_cast<std::size_t>(ua * n + c)] : 0.0) - (ub >= 0 ? r.X[static_cast<std::size_t>(ub * n + c)] : 0.0);
            for (int k = 0; k < n; ++k) {
                const double a = gam * xr[k];
                if (a == 0.0) continue;
                double* x = &r.X[static_cast<std::size_t>(k * n)];
                for (int c = 0; c < n; ++c) x[c] -= a * cx[c];
            }
        }
        if (p.kind != Prim::G) { // companion state continuity, as in the full refresh
            int k = 0;
            while (g.states[static_cast<std::size_t>(k)] != pi) ++k;
            auto& gcK = r.gc[static_cast<std::size_t>(k)];
            if (gcK != 0.0 && gcK != Gn) r.w[static_cast<std::size_t>(k)] += (Gn - gcK) * r.vcLast[static_cast<std::size_t>(k)];
            gcK = Gn;
            r.G2[static_cast<std::size_t>(k)] = p.be ? Gn : 2.0 * Gn;
        }
        r.gStamp[q] = Gn;
        ++r.smCount;
    }
    ++dg(gi).refreshes;
    if (r.sl && r.sl->on) r.sl->valid = spFactor(r.sl->lu, nullptr, nullptr, 0, 0, nullptr) && r.sl->pack(r.R.data());
    deriveFromZ(gi, false);
    return true;
}

// Port, export and state rows (Dp, K, H) from Z = M⁻¹·[Bs | Bu | Bi].
void Circuit::deriveFromZ(std::size_t gi, bool dc) noexcept {
    const auto& g = groups_[gi];
    auto& r = *rt_[gi];
    const int W = r.W, ns = r.ns, nu = r.nu;
    // Rows of "voltage between two nodes" in terms of w = [s; u; i].
    auto diffRow = [&](int a, int b, double* dst) {
        std::fill(dst, dst + W, 0.0);
        for (int t = 0; t < 2; ++t) {
            const int nd = t == 0 ? a : b;
            const double sg = t == 0 ? 1.0 : -1.0;
            if (nd <= 0) continue;
            const int u = r.unkOf[static_cast<std::size_t>(nd)];
            if (u >= 0) {
                const double* z = &r.Z[static_cast<std::size_t>(u * W)];
                for (int c = 0; c < W; ++c) dst[c] += sg * z[c];
            } else {
                const int j = r.inOf[static_cast<std::size_t>(nd)];
                if (j >= 0) dst[ns + j] += sg;
            }
        }
    };
    // Ports → Dp, K.
    {
        int o = 0;
        double tmp[256];
        std::vector<double> big; // only for very wide groups (never on the audio path for library circuits)
        double* rowBuf = W <= 256 ? tmp : nullptr;
        if (!rowBuf) {
            big.assign(static_cast<std::size_t>(W), 0.0);
            rowBuf = big.data();
        }
        for (int di : g.devices) {
            const auto& d = devices_[static_cast<std::size_t>(di)];
            for (int k = 0; k < d.nports; ++k) {
                diffRow(d.port[k].cp, d.port[k].cn, rowBuf);
                const int pk = o + k;
                for (int c = 0; c < ns + nu; ++c) r.Dp[static_cast<std::size_t>(pk * (ns + nu) + c)] = rowBuf[c];
                for (int c = 0; c < r.nn; ++c) r.K[static_cast<std::size_t>(pk * r.nn + c)] = rowBuf[ns + nu + c];
                for (int c = 0; c < r.nuc; ++c) r.Dc[static_cast<std::size_t>(pk * r.nuc + c)] = rowBuf[ns + nu + r.nn + c];
            }
            o += d.nports;
        }
    }
    if (r.lazyClamps) {
        const int nr = static_cast<int>(r.red.size());
        for (int a = 0; a < nr; ++a)
            for (int b = 0; b < nr; ++b)
                r.Kr[static_cast<std::size_t>(a * nr + b)] = r.K[static_cast<std::size_t>(r.red[static_cast<std::size_t>(a)] * r.nn + r.red[static_cast<std::size_t>(b)])];
    }
    if (r.kac) r.kac->valid = false;
    r.ks.valid = r.ksR.valid = false; // K changed: the kept factorisations no longer match
    if (r.ss) r.ss->on = false;       // ...nor does the small-signal path's frozen solve (GR_LESSONS P2)
    if (r.kacR) r.kacR->valid = false;
    {
        const int nn = r.nn;
        r.kcol.assign(static_cast<std::size_t>(nn), 0.0);
        for (int a = 0; a < nn; ++a)
            for (int c = 0; c < nn; ++c) r.kcol[static_cast<std::size_t>(c)] = std::max(r.kcol[static_cast<std::size_t>(c)], std::abs(r.K[static_cast<std::size_t>(a * nn + c)]));
        if (r.lazyClamps) {
            const int nr = static_cast<int>(r.red.size());
            r.kcolR.assign(static_cast<std::size_t>(nr), 0.0);
            for (int a = 0; a < nr; ++a)
                for (int c = 0; c < nr; ++c) r.kcolR[static_cast<std::size_t>(c)] = std::max(r.kcolR[static_cast<std::size_t>(c)], std::abs(r.Kr[static_cast<std::size_t>(a * nr + c)]));
        }
    }
    // Exports and state voltages → H.
    for (int e = 0; e < r.ny; ++e) diffRow(g.exports[static_cast<std::size_t>(e)], 0, &r.H[static_cast<std::size_t>(e * W)]);
    for (int k = 0; k < ns; ++k) {
        const auto& p = prims_[static_cast<std::size_t>(g.states[static_cast<std::size_t>(k)])];
        diffRow(p.a, p.b, &r.H[static_cast<std::size_t>((r.ny + k) * W)]);
    }
    for (std::size_t c = 0; c < r.hcOut.size(); ++c) // the rows linOut computes beside the port-defined states
        std::copy_n(&r.H[static_cast<std::size_t>(r.hcOut[c]) * static_cast<std::size_t>(W)], W, &r.Hc[c * static_cast<std::size_t>(W)]);
    if (r.wdf && !dc) r.wdf->refresh(*this, g);
    if (r.noiseOn && !dc) { // noise columns: M⁻¹·e_row from the explicit inverse (Sherman–Morrison) or the LU
        const double band = 0.5 / (g.T * r.noiseDiv), fourKT = 4.0 * 1.380649e-23 * opt_.noiseTempK;
        for (auto& q : r.noiseR) { // the resistors' current-noise scale at their present values (pots move with controls)
            if (q.prim < 0) { // a fixed density (op-amp input current noise)
                q.scale = q.dens * std::sqrt(band);
                continue;
            }
            const auto& p = prims_[static_cast<std::size_t>(q.prim)];
            const double R = p.slot >= 0 ? slots_[static_cast<std::size_t>(p.slot)].value : p.fixed;
            q.scale = R > 0 ? std::sqrt(fourKT / R * band) : 0.0;
        }
        for (auto& v : r.portV) v.scale = v.dens * std::sqrt(band);
        const int n = r.n, nc = r.nNoise, nn = r.nn;
        for (int c = 0; c < nc; ++c) {
            double* z = r.zcol.data();
            const int row = r.noiseRow[static_cast<std::size_t>(c)];
            if (r.sm)
                for (int k = 0; k < n; ++k) z[k] = r.X[static_cast<std::size_t>(k * n + row)];
            else {
                for (int k = 0; k < n; ++k) z[k] = k == row ? 1.0 : 0.0;
                la::luSolve(r.M.data(), r.piv.data(), n, z);
            }
            auto vOf = [&](int node) {
                if (node <= 0) return 0.0;
                const int u = r.unkOf[static_cast<std::size_t>(node)];
                return u >= 0 ? z[u] : 0.0;
            };
            int o = 0;
            for (int di : g.devices) {
                const auto& d = devices_[static_cast<std::size_t>(di)];
                for (int k = 0; k < d.nports; ++k) r.Pn[static_cast<std::size_t>((o + k) * nc + c)] = vOf(d.port[k].cp) - vOf(d.port[k].cn);
                o += d.nports;
            }
            (void)nn;
            for (int e = 0; e < r.ny; ++e) r.Hn[static_cast<std::size_t>(e * nc + c)] = vOf(g.exports[static_cast<std::size_t>(e)]);
            for (int k = 0; k < ns; ++k) {
                const auto& p = prims_[static_cast<std::size_t>(g.states[static_cast<std::size_t>(k)])];
                r.Hn[static_cast<std::size_t>((r.ny + k) * nc + c)] = vOf(p.a) - vOf(p.b);
            }
            for (int k = 0; k < r.nz; ++k) {
                const int u = r.zNeed[static_cast<std::size_t>(k)];
                r.Zd[static_cast<std::size_t>(k * nc + c)] = u >= 0 ? z[u] : 0.0;
            }
        }
        // Columns that add less than kNoisePrune of the noise power to every port, export and state are dropped from the
        // per-draw products (a low-impedance power-stage node next to a preamp grid); the kept ones are compacted.
        double* var = r.colVar.data();
        std::fill(var, var + nc, 0.0);
        for (const auto& q : r.noiseR) {
            if (q.ca >= 0) var[q.ca] += q.scale * q.scale;
            if (q.cb >= 0) var[q.cb] += q.scale * q.scale;
        }
        std::fill(r.colKeep.begin(), r.colKeep.end(), 0);
        auto mark = [&](const double* row) {
            double tot = 0;
            for (int c = 0; c < nc; ++c) tot += row[c] * row[c] * var[c];
            if (!(tot > 0)) return;
            for (int c = 0; c < nc; ++c)
                if (row[c] * row[c] * var[c] >= kNoisePrune * tot) r.colKeep[static_cast<std::size_t>(c)] = 1;
        };
        for (int e = 0; e < r.ny + ns; ++e) mark(&r.Hn[static_cast<std::size_t>(e * nc)]);
        for (int k = 0; k < r.nn; ++k) mark(&r.Pn[static_cast<std::size_t>(k * nc)]);
        r.nAct = 0;
        for (int c = 0; c < nc; ++c)
            if (r.colKeep[static_cast<std::size_t>(c)]) r.actCol[static_cast<std::size_t>(r.nAct++)] = c;
        const int na = r.nAct, naP = (na + 3) & ~3, nnP = (nn + 3) & ~3;
        r.naP = naP;
        r.nxP = naP + 2 * nnP;
        std::fill(r.Zf.begin(), r.Zf.end(), 0.0f);
        std::fill(r.Kf.begin(), r.Kf.end(), 0.0f);
        std::fill(r.xf.begin(), r.xf.end(), 0.0f); // the layout may have moved: no stale draws in the shot slots
        std::fill(r.xr.begin(), r.xr.end(), 0.0f);
        std::fill(r.dOut.begin(), r.dOut.end(), 0.0);
        std::fill(r.dP.begin(), r.dP.end(), 0.0);
        std::fill(r.dI.begin(), r.dI.end(), 0.0);
        for (int k = 0; k < r.nz; ++k)
            for (int a = 0; a < na; ++a)
                r.Zf[static_cast<std::size_t>(k * naP + a)] = static_cast<float>(r.Zd[static_cast<std::size_t>(k * nc + r.actCol[static_cast<std::size_t>(a)])]);
        for (int k = 0; k < nn; ++k)
            for (int j = 0; j < nn; ++j) r.Kf[static_cast<std::size_t>(k * nnP + j)] = static_cast<float>(r.K[static_cast<std::size_t>(k * nn + j)]);
        const std::vector<int>& pos = r.actCol; // kept columns, ascending
        r.nRAct = 0;
        for (std::size_t i = 0; i < r.noiseR.size(); ++i) {
            auto& q = r.noiseR[i];
            auto keptPos = [&](int c) {
                if (c < 0 || !r.colKeep[static_cast<std::size_t>(c)]) return -1;
                return static_cast<int>(std::lower_bound(pos.begin(), pos.begin() + na, c) - pos.begin());
            };
            q.ka = keptPos(q.ca);
            q.kb = keptPos(q.cb);
            if (!((q.ka >= 0 || q.kb >= 0) && q.scale > 0)) continue;
            if (q.prim < 0 && q.ca >= 0) { // an op-amp pin's current noise under 1 % of the node's other noise (with its 1/f
                                           // rise at 20 Hz) is dropped with its bank: ≤ 0.04 dB (a JFET input on 1 MΩ)
                const double own = q.scale * q.scale * (1.0 + (q.fl >= 0 ? r.flick[static_cast<std::size_t>(q.fl)].fc / 20.0 : 0.0));
                if (own < 0.01 * (var[q.ca] - q.scale * q.scale)) continue;
            }
            r.rAct[static_cast<std::size_t>(r.nRAct++)] = static_cast<int>(i);
        }
        r.nDraw = r.nRAct + 2 * nn + static_cast<int>(r.portV.size()); // the draws updateNoise takes
        auto bank = [&](int fl) { return fl >= 0 ? r.flick[static_cast<std::size_t>(fl)].n : 0; };
        for (int a = 0; a < r.nRAct; ++a) r.nDraw += bank(r.noiseR[static_cast<std::size_t>(r.rAct[static_cast<std::size_t>(a)])].fl);
        for (const auto& v : r.portV) r.nDraw += bank(v.fl);
        for (const auto& sd : r.shotDev) r.nDraw += bank(sd.fl);
        if (kNoiseFold) { // a column per kept source: Zf[:, a] = scale·(Zd[:, b] − Zd[:, a]) over its kept ends (a current a→b)
            const int nt = r.nRAct, ntP = (nt + 3) & ~3, nz = r.nz;
            r.naP = ntP;
            r.nxP = ntP + 2 * nnP;
            r.thFl = false;
            std::fill(r.Zf.begin(), r.Zf.end(), 0.0f);
            for (int a = 0; a < nt; ++a) {
                const auto& q = r.noiseR[static_cast<std::size_t>(r.rAct[static_cast<std::size_t>(a)])];
                r.thFl = r.thFl || q.fl >= 0;
                const int ca = q.ka >= 0 ? q.ca : -1, cb = q.kb >= 0 ? q.cb : -1;
                for (int k = 0; k < nz; ++k) {
                    const double* zr = &r.Zd[static_cast<std::size_t>(k * nc)];
                    r.Zf[static_cast<std::size_t>(k * ntP + a)] = static_cast<float>(q.scale * ((cb >= 0 ? zr[cb] : 0.0) - (ca >= 0 ? zr[ca] : 0.0)));
                }
            }
            // Each z row's least noise power over the outputs reading it (exports z[e]; states and ports a difference)
            double* rmin = r.zRowMin.data();
            std::fill(rmin, rmin + nz, std::numeric_limits<double>::infinity());
            auto out = [&](int ia, int ib) {
                double v = 0;
                for (int a = 0; a < nt; ++a) {
                    const double d = static_cast<double>(ia < nz ? r.Zf[static_cast<std::size_t>(ia * ntP + a)] : 0.0f) -
                                     static_cast<double>(ib < nz ? r.Zf[static_cast<std::size_t>(ib * ntP + a)] : 0.0f);
                    v += d * d;
                }
                if (!(v > 0)) return;
                if (ia < nz) rmin[ia] = std::min(rmin[ia], v);
                if (ib < nz) rmin[ib] = std::min(rmin[ib], v);
            };
            for (int e = 0; e < r.ny; ++e) out(e, nz);
            for (int k = 0; k < ns; ++k) out(r.gSt[static_cast<std::size_t>(2 * k)], r.gSt[static_cast<std::size_t>(2 * k + 1)]);
            for (int j = 0; j < nn; ++j) out(r.gPort[static_cast<std::size_t>(2 * j)], r.gPort[static_cast<std::size_t>(2 * j + 1)]);
            // A 4-column chunk of a row block is kept if any entry adds at least kNoiseChunkEps of its row's least power.
            // A dropped chunk moves an output by at most 2·nt·eps of its power (< 1e-8 at nt ≤ 50; < 4e-8 at 200).
            int nb = 0, ne = 0;
            auto block = [&](int r0, int rows) {
                r.zcOff[static_cast<std::size_t>(nb)] = ne;
                for (int c = 0; c < ntP; c += 4) {
                    bool keep = !(kNoiseChunkEps > 0);
                    for (int i = r0; i < r0 + rows && !keep; ++i) {
                        const double lim = kNoiseChunkEps * (std::isfinite(rmin[i]) ? rmin[i] : 0.0);
                        for (int a = c; a < c + 4 && !keep; ++a) {
                            const double zv = r.Zf[static_cast<std::size_t>(i * ntP + a)];
                            keep = zv * zv > lim;
                        }
                    }
                    if (keep) r.zcCol[static_cast<std::size_t>(ne++)] = c;
                }
                ++nb;
            };
            int rr = 0;
            for (; rr + 4 <= nz; rr += 4) block(rr, 4);
            for (; rr < nz; ++rr) block(rr, 1);
            r.zcOff[static_cast<std::size_t>(nb)] = ne;
        }
    }
}

// ───────────────────────────── Newton ─────────────────────────────
namespace nk {
// The static part of a SparseLu factorisation (see the struct), from M alone: snap holds every static entry's final
// value and every live entry's value at the point it goes live; static pivots' inverses go to inv. The operations are
// the full elimination's, in its order, restricted to the static ones. snapOk false: a static pivot test fails, so
// every factorisation fails until M changes (as the full elimination would at that pivot).
void spSnapshot(SparseLu& s) noexcept {
    const int n = s.n;
    double* __restrict w = s.snap.data();
    std::fill(s.snap.begin(), s.snap.end(), 0.0);
    for (std::size_t e = 0; e < s.mPos.size(); ++e) w[s.mPos[e]] = s.Mraw[s.mSrc[e]];
    s.stale = false;
    s.snapOk = false;
    const int* lOff = s.lOff.data();
    const int* uOff = s.uOff.data();
    const int* op = s.opDst.data();
    const std::uint8_t* lv = s.opLive.data();
    double* const Lw = w;
    const double* const Uw = w + s.lIdx.size();
    const double* const Dw = Uw + s.uIdx.size();
    for (int k = 0; k < n; ++k) {
        const std::uint8_t fl = s.kFl[static_cast<std::size_t>(k)];
        const int nu = uOff[k + 1] - uOff[k];
        if (fl & 1) { // live pivot: its whole column is scaled live, and every update in it has a live operand
            s.cmaxS[static_cast<std::size_t>(k)] = 0.0;
            const auto skip = static_cast<std::ptrdiff_t>(lOff[k + 1] - lOff[k]) * nu;
            op += skip;
            lv += skip;
            continue;
        }
        const double pv = Dw[k];
        double cs = 0.0;
        for (int e = lOff[k]; e < lOff[k + 1]; ++e)
            if (!s.lLive[static_cast<std::size_t>(e)]) cs = std::max(cs, std::abs(Lw[e]));
        s.cmaxS[static_cast<std::size_t>(k)] = cs;
        if (!(fl & 2)) { // static test: the full elimination's, on the same values
            double cmax = std::abs(pv);
            for (int e = lOff[k]; e < lOff[k + 1]; ++e) cmax = std::max(cmax, std::abs(Lw[e]));
            if (!(std::abs(pv) >= 1e-6 * cmax) || !(std::abs(pv) > 1e-300)) return;
        }
        const double iv = 1.0 / pv;
        s.inv[static_cast<std::size_t>(k)] = iv;
        const double* uk = Uw + uOff[k];
        for (int e = lOff[k]; e < lOff[k + 1]; ++e, op += nu, lv += nu) {
            if (s.lLive[static_cast<std::size_t>(e)]) continue; // scaled live; all its updates are live
            const double f = (Lw[e] *= iv);
            if (f == 0.0) continue;
            for (int u = 0; u < nu; ++u)
                if (!lv[u]) w[op[u]] -= f * uk[u];
        }
    }
    s.snapOk = true;
}
// Sparse nodal factorisation of M − B·Jd·C over the live ports (all ports when live is null). False if a pivot is small
// against its row (the caller then uses the dense path).
bool spFactor(SparseLu& s, const double (*Jd)[3], const int* live, int na, int nports, const int* bn) noexcept {
    if (s.stale || !Jd) spSnapshot(s); // without stamps (M alone: the linear update's LU) it is all static
    if (!s.snapOk) return false;
    if (Jd && s.cg && !kEnvNoCodegen)
        return s.cg->factor(s.snap.data(), s.cmaxS.data(), Jd, live, na, s.v.data(), s.inv.data());
    const int n = s.n;
    double* __restrict v = s.v.data();
    std::copy(s.snap.begin(), s.snap.end(), s.v.begin());
    auto stamp = [&](int a) {
        const int np = bn[a];
        for (int m = 0; m < np; ++m) {
            const double g = Jd[a][m];
            if (g == 0.0) continue;
            const int k = a * 3 + m;
            for (int e = s.stOff[static_cast<std::size_t>(k)]; e < s.stOff[static_cast<std::size_t>(k) + 1]; ++e)
                v[s.stPos[static_cast<std::size_t>(e)]] += s.stSign[static_cast<std::size_t>(e)] * g;
        }
    };
    if (Jd) {
        if (live)
            for (int i = 0; i < na; ++i) stamp(live[i]);
        else
            for (int a = 0; a < nports; ++a) stamp(a);
    }
    const double* const Dv = v + s.lIdx.size() + s.uIdx.size(); // diagonal
    const std::uint8_t* kFl = s.kFl.data();
    const int* dOff = s.dOff.data();
    const int* dIdx = s.dIdx.data();
    const int* rOff = s.rOff.data();
    const int* rE = s.rE.data();
    const int* oOff = s.oOff.data();
    const int* oQ = s.oQ.data();
    const int* oU = s.oU.data();
    double* __restrict inv = s.inv.data();
    for (int k = 0; k < n; ++k) {
        const std::uint8_t fl = kFl[k];
        if (fl & 2) {
            // the fixed pivot must keep a share of its column (chosen at threshold 0.1 at reset; accepted down to 1e-6,
            // beyond which the factorisation falls back to dense: on the SVT that keeps 98 % of factorisations sparse,
            // null -200 dB). The static entries' part of the column maximum comes from the snapshot (a maximum does
            // not depend on its order; NaN entries drop out of std::max either way, a NaN pivot stays NaN).
            const double pv = Dv[k];
            double cmax = std::max(std::abs(pv), s.cmaxS[static_cast<std::size_t>(k)]);
            for (int e = dOff[k]; e < dOff[k + 1]; ++e) cmax = std::max(cmax, std::abs(v[dIdx[e]]));
            if (!(std::abs(pv) >= 1e-6 * cmax) || !(std::abs(pv) > 1e-300)) return false;
        }
        const double iv = (fl & 1) ? (inv[k] = 1.0 / Dv[k]) : inv[k];
        // scalings write column k, updates write rows and columns beyond k: independent, so scaling first is exact
        for (int e = dOff[k]; e < dOff[k + 1]; ++e) v[dIdx[e]] *= iv;
        for (int r = rOff[k]; r < rOff[k + 1]; ++r) {
            const double f = v[rE[r]];
            if (f == 0.0) continue;
            for (int o = oOff[r]; o < oOff[r + 1]; ++o) v[oQ[o]] -= f * v[oU[o]]; // (i, j) -= l(i, k)·u(k, j)
        }
    }
    return true;
}
// d = F + C·x, (M − B·Jd·C)·x = B·Jd·F with the factorisation above (Jd the one it was built from).
void spStep(SparseLu& s, const double (*Jd)[3], const int* live, int na, int nports, const int* bs, const int* bn, const double* F,
                   double* d) noexcept {
    if (s.cg && !kEnvNoCodegen) {
        s.cg->step(s.v.data(), s.inv.data(), Jd, live, na, F, d);
        return;
    }
    const int n = s.n;
    double* __restrict rhs = s.rhs.data();
    for (int i = 0; i < n; ++i) rhs[i] = 0.0;
    auto inject = [&](int a) {
        const int b0 = bs[a], np = bn[a];
        double v = 0;
        for (int m = 0; m < np; ++m) v += Jd[a][m] * F[b0 + m];
        if (s.pFrom[static_cast<std::size_t>(a)] >= 0) rhs[s.pFrom[static_cast<std::size_t>(a)]] -= v;
        if (s.pTo[static_cast<std::size_t>(a)] >= 0) rhs[s.pTo[static_cast<std::size_t>(a)]] += v;
    };
    if (live)
        for (int i = 0; i < na; ++i) inject(live[i]);
    else
        for (int a = 0; a < nports; ++a) inject(a);
    const double* __restrict Lv = s.v.data();
    const double* __restrict Uv = Lv + s.lIdx.size();
    const int* __restrict lOff = s.lOff.data();
    const int* __restrict lIdx = s.lIdx.data();
    const int* __restrict uOff = s.uOff.data();
    const int* __restrict uIdx = s.uIdx.data();
    for (int k = 0; k < n; ++k) {
        const double bk = rhs[k];
        if (bk == 0.0) continue;
        for (int e = lOff[k]; e < lOff[k + 1]; ++e) rhs[lIdx[e]] -= Lv[e] * bk;
    }
    // back substitution over the rows the step reads only (the others' x is never used)
    double* __restrict x = s.x.data();
    for (const int k : s.bRows) {
        double v = rhs[k];
        for (int e = uOff[k]; e < uOff[k + 1]; ++e) v -= Uv[e] * x[uIdx[e]];
        x[k] = v * s.inv[static_cast<std::size_t>(k)];
    }
    for (int a = 0; a < nports; ++a) {
        double v = F[a];
        if (s.xP[static_cast<std::size_t>(a)] >= 0) v += x[s.xP[static_cast<std::size_t>(a)]];
        if (s.xN[static_cast<std::size_t>(a)] >= 0) v -= x[s.xN[static_cast<std::size_t>(a)]];
        d[a] = v;
    }
}

// x = A⁻¹·b for a factorised SparseLu with packed factors (SparseLin::pack), permuted coordinates (b is consumed). The
// back substitution runs over rows (descending, spNeeded): the x entries the caller reads and those they depend on.
void spSolvePacked(const SparseLu& s, const double* __restrict lVal, const double* __restrict uVal, double* __restrict b,
                          double* __restrict x, const std::vector<int>& rows) noexcept {
    const int n = s.n;
    const int* __restrict lOff = s.lOff.data();
    const int* __restrict lIdx = s.lIdx.data();
    const int* __restrict uOff = s.uOff.data();
    const int* __restrict uIdx = s.uIdx.data();
    const double* __restrict inv = s.inv.data();
    for (int k = 0; k < n; ++k) {
        const double bk = b[k];
        if (bk == 0.0) continue;
        for (int e = lOff[k]; e < lOff[k + 1]; ++e) b[lIdx[e]] -= lVal[e] * bk;
    }
    for (const int k : rows) {
        double v = b[k];
        for (int e = uOff[k]; e < uOff[k + 1]; ++e) v -= uVal[e] * x[uIdx[e]];
        x[k] = v * inv[k];
    }
}

} // namespace nk
// Active-set Newton in current space. Ports that carry no current in the present regime (a grid below cut-in,
// control-only ports such as a lamp's light input) drop out: with i = 0 there, every port voltage is affine in the
// NA active currents, v = p + K[:,A]·i_A, and Newton runs on G(i_A) = i_A − f_A(v) with the NA×NA Jacobian
// I − Jd_A·K[:,A]. The caller verifies the dropped ports stayed inactive.
template <int NA>
static inline bool activeKernel(const NlDevice* const* devs, int ndev, int nn, const double* __restrict K, const int* act,
                                const int* devOfPort, const int* baseOfPort, const double* __restrict p, double* __restrict vpOut,
                                double* __restrict curOut, const double* iPred, const std::uint8_t* lim, const double* limA, int maxIt,
                                double tol, int& itersOut, const double* ptol = nullptr) noexcept {
    double M[24 * NA], iA[NA], v[24], cur[24], Jd[24][3], G[NA], Jc[NA * NA], d[NA], dv[24];
    for (int k = 0; k < nn; ++k)
        for (int b = 0; b < NA; ++b) M[k * NA + b] = K[k * nn + act[b]];
    for (int b = 0; b < NA; ++b) iA[b] = iPred[b];
    for (int it = 0; it < maxIt; ++it) {
        for (int k = 0; k < nn; ++k) {
            double s = p[k];
            for (int b = 0; b < NA; ++b) s += M[k * NA + b] * iA[b];
            v[k] = s;
        }
        int o = 0;
        for (int q = 0; q < ndev; ++q) {
            const NlDevice& dvc = *devs[q];
            double Jl[9];
            dvc.eval(v + o, cur + o, Jl);
            const int np = dvc.nports;
            for (int a = 0; a < np; ++a)
                for (int b = 0; b < np; ++b) Jd[o + a][b] = Jl[a * np + b];
            o += np;
        }
        for (int a = 0; a < NA; ++a) {
            const int pa = act[a], base = baseOfPort[pa], np = devs[devOfPort[pa]]->nports;
            G[a] = iA[a] - cur[pa];
            for (int b = 0; b < NA; ++b) {
                double s = a == b ? 1.0 : 0.0;
                for (int m = 0; m < np; ++m) s -= Jd[pa][m] * M[(base + m) * NA + b];
                Jc[a * NA + b] = s;
            }
        }
        for (int a = 0; a < NA; ++a) d[a] = -G[a];
        if (!solveFixed<NA>(Jc, d)) return false;
        double scale = 1.0;
        for (int k = 0; k < nn; ++k) {
            double s = 0;
            for (int b = 0; b < NA; ++b) s += M[k * NA + b] * d[b];
            dv[k] = s;
            if (lim[k] == 4 && std::abs(s) > limA[k]) scale = std::min(scale, limA[k] / std::abs(s)); // damped (kLimStep)
        }
        bool small = scale == 1.0;
        for (int k = 0; k < nn && small; ++k) small = std::abs(dv[k]) <= (ptol ? ptol[k] : tol) * (1.0 + std::abs(v[k]));
        for (int b = 0; b < NA; ++b) iA[b] += scale * d[b];
        if (small) {
            for (int k = 0; k < nn; ++k) {
                vpOut[k] = v[k] + dv[k];
                curOut[k] = 0.0;
            }
            for (int b = 0; b < NA; ++b) curOut[act[b]] = iA[b];
            itersOut = it + 1;
            return true;
        }
    }
    return false;
}

// The extrapolated start of a sample, limited on junction ports against the last solution v1 the way SPICE limits its
// predicted junction voltages (pnjlim against the last time point, MODEINITPRED): pnjlim only limits Newton steps, so an
// unlimited start could sit far up the exponential. A floating winding's diode that turned on during the last sample
// extrapolated to +290 V forward, and Newton then needed 15-40 iterations to come back down.
inline void limitPredicted(int nn, const std::uint8_t* lim, const double* A, const double* B, const double* C, const double* v1, double* vp) noexcept {
    for (int k = 0; k < nn; ++k)
        if (lim[k] == kLimPnj || lim[k] == kLimPnjF || lim[k] == kLimZener || lim[k] == kLimZenerF || lim[k] == kLimPair)
            vp[k] = lim[k] == kLimPnjF ? limitPort(kLimPnj, A[k], B[k], 0.0, v1[k], vp[k])
                    : limitPort(lim[k] == kLimZenerF ? kLimZener : lim[k], A[k], B[k], C[k], v1[k], vp[k]); // upward only
}

template <typename... A>
static bool runKernel(int n, A&&... a) noexcept {
    switch (n) {
        case 1: return newtonKernel<1>(a...);
        case 2: return newtonKernel<2>(a...);
        case 3: return newtonKernel<3>(a...);
        case 4: return newtonKernel<4>(a...);
        case 5: return newtonKernel<5>(a...);
        case 6: return newtonKernel<6>(a...);
        case 7: return newtonKernel<7>(a...);
        case 8: return newtonKernel<8>(a...);
        case 9: return newtonKernel<9>(a...);
        case 10: return newtonKernel<10>(a...);
        case 11: return newtonKernel<11>(a...);
        case 12: return newtonKernel<12>(a...);
        default: return false;
    }
}

bool Circuit::newton(GroupRt& r, const Group& g, const double* p, int maxIt) noexcept {
    // At DC the RMS log detector can start far above its timing current. The Woodbury
    // step then subtracts nearly equal terms and can round to zero before CAP balances.
    // Factor the full port Jacobian for that operating point.
    const bool kcolOn = kEnvKcolOn && !(r.dcMode && std::any_of(r.dev.begin(), r.dev.end(),
        [](const NlDevice* d) { return d->kind == NlKind::RmsLog; }));
    const bool chordOn = kEnvChordOn;   // developer A/B
    const int nn = r.nn;
    (void)g;
    // the group's smallest port tolerance; r.ptol: each port's, where they differ (not in a DC solve: the operating point
    // is solved at the tight tolerance throughout, as the SPICE .op gate compares it node by node)
    const double tol = r.dcMode ? std::min(r.tol, opt_.newtonTol) : r.tol;
    const double* const ptol = r.ptol.empty() || r.dcMode ? nullptr : r.ptol.data();
    double* vp = r.vp.data();
    double* cur = &r.w[static_cast<std::size_t>(r.ns + r.nu)];
    int its = 0;
    double res = 0;
    bool ok = false;
    // The clamp-free solve is taken only while every clamp was off at the last solution too. A clamp that conducted
    // there may be holding a regenerative stage (a Schmitt comparator on its rail): its clamp-free system then also
    // has an in-band solution, the unstable middle branch of the positive-feedback loop, which is a solution of the
    // full equations as well but not the continuation of the clamped state. Accepting it flipped the comparator early
    // (DMM LFO: swing 1.60 V instead of 1.85 V, 5.1 Hz instead of 4.4 Hz in Vibrato); the full solve follows the
    // clamp off continuously. (Transient solves only: a DC solve has no previous state to continue from.)
    bool clampsWereOff = r.lazyClamps;
    for (std::size_t q = 0; clampsWereOff && !r.dcMode && q < r.clampPort.size() && q < 12; ++q) {
        const double v = r.vpPrev[static_cast<std::size_t>(r.clampPort[q])];
        clampsWereOff = v > r.clampLo[q] && v < r.clampHi[q];
    }
    if (clampsWereOff) {
        const int nr = static_cast<int>(r.red.size());
        double pr[12], vr[12], cr[12];
        for (int j = 0; j < nr; ++j) {
            pr[j] = p[r.red[static_cast<std::size_t>(j)]];
            vr[j] = vp[r.red[static_cast<std::size_t>(j)]];
        }
        double dqr[12]; // the predictor correction (processGroup) on the clamp-free ports
        if (r.predDp)
            for (int j = 0; j < nr; ++j) dqr[j] = r.predDp[r.red[static_cast<std::size_t>(j)]];
        const bool okR = r.nkRed ? r.nkRed(r.devR.data(), static_cast<int>(r.devR.size()), r.bsR.data(), r.bnR.data(), r.Kr.data(), static_cast<const double*>(pr),
                                           static_cast<double*>(vr), static_cast<double*>(cr), r.limR.data(), r.limAr.data(), r.limBr.data(), r.limCr.data(), maxIt, tol, its,
                                           res, kcolOn ? r.kcolR.data() : nullptr, chordOn && !r.dcMode, r.kacR.get(), nr, r.dcMode || kEnvNoClampKs ? nullptr : &r.ksR,
                                           static_cast<const double*>(r.predDp ? dqr : nullptr), nullptr, nullptr, nullptr)
                                 : runKernel(nr, r.devR.data(), static_cast<int>(r.devR.size()), r.bsR.data(), r.bnR.data(), r.Kr.data(), static_cast<const double*>(pr),
                      static_cast<double*>(vr), static_cast<double*>(cr), r.limR.data(), r.limAr.data(), r.limBr.data(), r.limCr.data(), maxIt, tol, its, res, kcolOn ? r.kcolR.data() : nullptr, chordOn && !r.dcMode, r.kacR.get(),
                      nr, r.dcMode || kEnvNoClampKs ? nullptr : &r.ksR, static_cast<const double*>(r.predDp ? dqr : nullptr));
        if (okR) {
            bool inBand = true;
            double vc[12];
            for (std::size_t q = 0; q < r.clampPort.size() && q < 12; ++q) {
                const int k = r.clampPort[q];
                double v = p[k];
                for (int j = 0; j < nr; ++j) v += r.K[static_cast<std::size_t>(k * nn + r.red[static_cast<std::size_t>(j)])] * cr[j];
                vc[q] = v;
                inBand &= v > r.clampLo[q] && v < r.clampHi[q];
            }
            for (int j = 0; j < nr; ++j) vp[r.red[static_cast<std::size_t>(j)]] = vr[j]; // warm start either way
            if (inBand) {
                for (int j = 0; j < nr; ++j) cur[r.red[static_cast<std::size_t>(j)]] = cr[j];
                for (std::size_t q = 0; q < r.clampPort.size() && q < 12; ++q) {
                    vp[r.clampPort[q]] = vc[q];
                    cur[r.clampPort[q]] = 0.0; // exactly the clamp law inside the swing
                }
                r.count(its);
                dg(g).newtonIterations += static_cast<std::uint64_t>(its);
                dg(g).maxIterations = std::max(dg(g).maxIterations, its);
                return true;
            }
        }
        ++r.fullSolves;
    }
    // Lazy-clamp groups keep a factorisation for each solve (ksR above, ks here), so both get the predictor correction.
    // After a clamp-free attempt the full solve starts from its result, which already answers this sample's p.
    KernelState* const ks = r.dcMode || (r.lazyClamps && kEnvNoClampKs) ? nullptr : &r.ks;
    if (ks) ks->xchord = kEnvXChord == 2 || (kEnvXChord == 1 && g.level > 0) || (kEnvXChord == 3 && (g.level > 0 || nn > 24));
    const double* const predFull = r.lazyClamps && clampsWereOff ? nullptr : r.predDp;
    auto run = [&](auto tag) {
        constexpr int NN = decltype(tag)::value;
        return newtonKernel<NN>(r.dev.data(), static_cast<int>(r.dev.size()), r.bs.data(), r.bn.data(), r.K.data(), p, vp, cur, r.lim.data(), r.limA.data(),
                                r.limB.data(), r.limC.data(), maxIt, tol, its, res, kcolOn ? r.kcol.data() : nullptr, chordOn && !r.dcMode, r.kac.get(), NN,
                                ks, predFull, nullptr, r.dcMode ? nullptr : r.sp.get(), ptol);
    };
    if (r.nkFull && !(nn > 24 && kEnvGenericNewton)) // the kernel specialised to this group's structure (docs §10.45): the same arguments and arithmetic
        ok = r.nkFull(r.dev.data(), static_cast<int>(r.dev.size()), r.bs.data(), r.bn.data(), r.K.data(), p, vp, cur, r.lim.data(), r.limA.data(), r.limB.data(),
                      r.limC.data(), maxIt, tol, its, res, kcolOn ? r.kcol.data() : nullptr, chordOn && !r.dcMode, r.kac.get(), nn, ks, predFull,
                      nn > 24 ? r.J.data() : nullptr, r.dcMode ? nullptr : r.sp.get(), ptol);
    else switch (nn) {
        case 1: ok = run(std::integral_constant<int, 1>{}); break;
        case 2: ok = run(std::integral_constant<int, 2>{}); break;
        case 3: ok = run(std::integral_constant<int, 3>{}); break;
        case 4: ok = run(std::integral_constant<int, 4>{}); break;
        case 5: ok = run(std::integral_constant<int, 5>{}); break;
        case 6: ok = run(std::integral_constant<int, 6>{}); break;
        case 7: ok = run(std::integral_constant<int, 7>{}); break;
        case 8: ok = run(std::integral_constant<int, 8>{}); break;
        // Whole tube power stages inside their feedback loop (PI + paralleled pentodes + core) land at 9-16 ports.
        case 9: ok = run(std::integral_constant<int, 9>{}); break;
        case 10: ok = run(std::integral_constant<int, 10>{}); break;
        case 11: ok = run(std::integral_constant<int, 11>{}); break;
        case 12: ok = run(std::integral_constant<int, 12>{}); break;
        case 13: ok = run(std::integral_constant<int, 13>{}); break;
        case 14: ok = run(std::integral_constant<int, 14>{}); break;
        case 15: ok = run(std::integral_constant<int, 15>{}); break;
        case 16: ok = run(std::integral_constant<int, 16>{}); break;
        case 17: ok = run(std::integral_constant<int, 17>{}); break;
        case 18: ok = run(std::integral_constant<int, 18>{}); break;
        case 19: ok = run(std::integral_constant<int, 19>{}); break;
        case 20: ok = run(std::integral_constant<int, 20>{}); break;
        case 21: ok = run(std::integral_constant<int, 21>{}); break;
        case 22: ok = run(std::integral_constant<int, 22>{}); break;
        case 23: ok = run(std::integral_constant<int, 23>{}); break;
        case 24: ok = run(std::integral_constant<int, 24>{}); break;
        default:
            // Above 24 ports: the same kernel at a run-time size (ENGINE_QUEUE item 13 follow-up); CE_GENERIC keeps the old
            // general path for A/B.
            if (nn <= kMaxNlPorts && !kEnvGenericNewton)
                ok = newtonKernel<0>(r.dev.data(), static_cast<int>(r.dev.size()), r.bs.data(), r.bn.data(), r.K.data(), p, vp, cur, r.lim.data(), r.limA.data(),
                                     r.limB.data(), r.limC.data(), maxIt, tol, its, res, kcolOn ? r.kcol.data() : nullptr, chordOn && !r.dcMode, r.kac.get(), nn,
                                     ks, predFull, r.J.data(), r.dcMode ? nullptr : r.sp.get(), ptol); // r.J: nn² scratch when ks is null
            else
                ok = newtonGeneric(r, g, p, maxIt, its, res);
            break;
    }
    if (ok && debugFail_ && !r.dcMode && diag_.samples > 400000 && its >= 5 && std::getenv("CE_TRACE_IT")) {
        static int traced = 0;
        if (traced++ < 2) {
            std::fprintf(stderr, "slow convergence (%d iterations), nn %d, retrace from the predictor:\n", its, nn);
            std::vector<double> keep(vp, vp + nn), start(r.vpPrev.begin(), r.vpPrev.end());
            for (int k = 0; k < nn; ++k) vp[k] = 2.0 * r.vpPrev[static_cast<std::size_t>(k)] - r.vpPrev2[static_cast<std::size_t>(k)];
            int i2 = 0;
            double r2 = 0;
            traceNewton_ = true;
            newtonGeneric(r, g, p, maxIt, i2, r2);
            traceNewton_ = false;
            std::copy(keep.begin(), keep.end(), vp);
        }
    }
    // Fallback before the homotopy: the active-set (current-space) Newton converges in hard-clipping samples where
    // the voltage-space iteration cycles (fewer rescues and failures on a fully driven power stage).
    if (!ok && r.activeSet && !r.dcMode && nn <= 24) { // fixed-size kernel (activeKernel) path
        int act[24], na = 0;
        for (int k = 0; k < nn; ++k) {
            const auto kk = static_cast<std::size_t>(k);
            const bool drop = r.dropKind[kk] == 1 || (r.dropKind[kk] == 2 && r.vpPrev[kk] < r.dropBelow[kk]);
            if (!drop) act[na++] = k;
        }
        if (na < nn && na >= 1) {
            double iPred[24];
            for (int b = 0; b < na; ++b) {
                const auto k = static_cast<std::size_t>(act[b]);
                iPred[b] = 2.0 * r.curPrev1[k] - r.curPrev2[k];
            }
            double vNew[24], cNew[24];
            auto runA = [&](auto tag) {
                constexpr int NA = decltype(tag)::value;
                return activeKernel<NA>(r.dev.data(), static_cast<int>(r.dev.size()), nn, r.K.data(), act, r.devOfPort.data(), r.baseOfPort.data(), p, vNew, cNew,
                                        iPred, r.lim.data(), r.limA.data(), maxIt, tol, its, ptol);
            };
            bool okA = false;
            switch (na) {
                case 1: okA = runA(std::integral_constant<int, 1>{}); break;
                case 2: okA = runA(std::integral_constant<int, 2>{}); break;
                case 3: okA = runA(std::integral_constant<int, 3>{}); break;
                case 4: okA = runA(std::integral_constant<int, 4>{}); break;
                case 5: okA = runA(std::integral_constant<int, 5>{}); break;
                case 6: okA = runA(std::integral_constant<int, 6>{}); break;
                case 7: okA = runA(std::integral_constant<int, 7>{}); break;
                case 8: okA = runA(std::integral_constant<int, 8>{}); break;
                case 9: okA = runA(std::integral_constant<int, 9>{}); break;
                case 10: okA = runA(std::integral_constant<int, 10>{}); break;
                default: break;
            }
            // the dropped grids must still be below cut-in at the solution
            for (int k = 0; k < nn && okA; ++k)
                if (r.dropKind[static_cast<std::size_t>(k)] == 2 && vNew[k] >= r.dropBelow[static_cast<std::size_t>(k)]) {
                    bool dropped = true;
                    for (int b = 0; b < na; ++b) dropped &= act[b] != k;
                    if (dropped) okA = false;
                }
            if (okA) ++dg(g).activeSolves;
            else ++dg(g).activeMisses;
            if (okA) {
                for (int k = 0; k < nn; ++k) {
                    vp[k] = vNew[k];
                    cur[k] = cNew[k];
                }
                r.count(its);
                dg(g).newtonIterations += static_cast<std::uint64_t>(its);
                dg(g).maxIterations = std::max(dg(g).maxIterations, its);
                return true;
            }
        }
    }
    if (!ok && !r.dcMode && !r.pssNoPtc && nn >= 1 && nn <= kMaxNlPorts) { // (pssNoPtc: §10.53)
        std::copy(r.vpPrev.begin(), r.vpPrev.end(), r.vp.begin());
        if (newtonPtc(r, g, p, 240)) {
            ++dg(g).ptcRescues;
            return true;
        }
    }
    if (ok) {
        r.count(its);
        dg(g).newtonIterations += static_cast<std::uint64_t>(its);
        dg(g).maxIterations = std::max(dg(g).maxIterations, its);
        dg(g).worstResidual = std::max(dg(g).worstResidual, res);
    }
    return ok;
}

// General-size Newton (groups with more than 8 coupled ports) on preallocated storage.
bool Circuit::newtonGeneric(GroupRt& r, const Group& g, const double* p, int maxIt, int& itersOut, double& resOut) noexcept {
    const int nn = r.nn;
    const double tol = r.tol; // the group's smallest port tolerance (conservative where r.ptol differs)
    double* vp = r.vp.data();
    double* cur = &r.w[static_cast<std::size_t>(r.ns + r.nu)];
    double lastStep = 1e30;
    bool prevLimited = false;
    bool chordLu = false; // r.J holds a full nn×nn LU from this call (chord step allowed)
    // Woodbury chord: the live set, the live rows of Jf (≤ 3 entries each) and the |A|×|A| LU in r.J from the last
    // reduced factorisation, so a confirming step reuses them exactly like the full chord.
    bool woodLu = false;
    int wLive[kMaxNlPorts], wNa = 0, wBlk0[kMaxNlPorts], wBlkN[kMaxNlPorts];
    double wRows[kMaxNlPorts][3];
    for (int it = 0;; ++it) {
        std::fill(r.Jf.begin(), r.Jf.end(), 0.0);
        int o = 0;
        for (int di : g.devices) {
            const auto& d = devices_[static_cast<std::size_t>(di)];
            double Jl[9];
            d.eval(vp + o, cur + o, Jl);
            for (int a = 0; a < d.nports; ++a)
                for (int b = 0; b < d.nports; ++b) r.Jf[static_cast<std::size_t>((o + a) * nn + o + b)] = Jl[a * d.nports + b];
            o += d.nports;
        }
        double maxF = 0;
        for (int k = 0; k < nn; ++k) {
            double s = p[k] - vp[k];
            const double* Kr = &r.K[static_cast<std::size_t>(k * nn)];
            for (int j = 0; j < nn; ++j) s += Kr[j] * cur[j];
            r.F[static_cast<std::size_t>(k)] = s;
            maxF = std::max(maxF, std::abs(s));
        }
        if (traceNewton_) {
            std::fprintf(stderr, "   it %2d vp", it);
            for (int k = 0; k < nn; ++k) std::fprintf(stderr, " %+.6f", vp[k]);
            std::fprintf(stderr, "  i");
            for (int k = 0; k < nn; ++k) std::fprintf(stderr, " %+.3e", cur[k]);
            std::fprintf(stderr, "  F");
            for (int k = 0; k < nn; ++k) std::fprintf(stderr, " %+.3e", r.F[static_cast<std::size_t>(k)]);
            std::fprintf(stderr, "\n");
        }
        if (!std::isfinite(maxF)) return false;
        if (it > 0 && !prevLimited && lastStep <= tol) {
            itersOut = it;
            resOut = maxF;
            return true;
        }
        if (it >= maxIt) return false;
        // Chord confirmation (as in newtonKernel): from the second iteration on, first try the step with the previous
        // factorisation (r.J still holds its LU). If it already meets the tolerance unlimited, take it: it differs
        // from the Newton step by O(|Δprev|·|Δ|), far below the tolerance, and the next pass re-evaluates the devices
        // and the exact residual before accepting. Otherwise factorise at this iterate as plain Newton.
        if (it > 0 && woodLu && kEnvChordOn && !r.dcMode) {
            double y[kMaxNlPorts];
            for (int i = 0; i < wNa; ++i) {
                double rhs = 0;
                for (int b = 0; b < wBlkN[i]; ++b) rhs += wRows[i][b] * r.F[static_cast<std::size_t>(wBlk0[i] + b)];
                y[i] = rhs;
            }
            la::luSolve(r.J.data(), r.pivN.data(), wNa, y);
            double step = 0;
            bool lim = false;
            for (int k = 0; k < nn; ++k) {
                double d = r.F[static_cast<std::size_t>(k)];
                const double* Kr = &r.K[static_cast<std::size_t>(k * nn)];
                for (int j = 0; j < wNa; ++j) d += Kr[wLive[j]] * y[j];
                r.delta[static_cast<std::size_t>(k)] = d;
                const double vfull = vp[k] + d;
                const auto ks = static_cast<std::size_t>(k);
                lim |= limitPort(r.lim[ks], r.limA[ks], r.limB[ks], r.limC[ks], vp[k], vfull) != vfull;
                step = std::max(step, std::abs(d) / (1.0 + std::abs(vfull)));
            }
            if (!lim && std::isfinite(step) && step <= tol) {
                for (int k = 0; k < nn; ++k) vp[k] += r.delta[static_cast<std::size_t>(k)];
                lastStep = step;
                prevLimited = false;
                continue;
            }
        }
        if (it > 0 && chordLu && kEnvChordOn && !r.dcMode) {
            for (int k = 0; k < nn; ++k) r.delta[static_cast<std::size_t>(k)] = -r.F[static_cast<std::size_t>(k)];
            la::luSolve(r.J.data(), r.pivN.data(), nn, r.delta.data());
            double step = 0;
            bool lim = false;
            for (int k = 0; k < nn; ++k) {
                const auto ks = static_cast<std::size_t>(k);
                const double vfull = vp[k] + r.delta[ks];
                lim |= limitPort(r.lim[ks], r.limA[ks], r.limB[ks], r.limC[ks], vp[k], vfull) != vfull;
                step = std::max(step, std::abs(r.delta[ks]) / (1.0 + std::abs(vfull)));
            }
            if (!lim && std::isfinite(step) && step <= tol) {
                for (int k = 0; k < nn; ++k) vp[k] += r.delta[static_cast<std::size_t>(k)];
                lastStep = step;
                prevLimited = false;
                continue;
            }
        }
        // Low-rank step (exact, as in newtonKernel): rows of Jf that are negligible (a grid below conduction, a device
        // in cut-off) contribute nothing to K·Jf, so with A the live rows the Newton step is the Woodbury solve
        // (I − Jf[A,:]·K[:,A])·y = Jf[A,:]·F, δ = F + K[:,A]·y — an |A|×|A| system instead of nn×nn. A row is dropped
        // only when its largest entry times K's largest column entry is below 1e-9 (the Jacobian moves < 1e-9).
        if (kEnvKcolOn && nn >= 8 && r.kcol.size() == static_cast<std::size_t>(nn)) {
            int live[kMaxNlPorts], na = 0, blk0[kMaxNlPorts], blkN[kMaxNlPorts];
            {
                int o = 0;
                for (int di : g.devices) {
                    const int np = devices_[static_cast<std::size_t>(di)].nports;
                    for (int k = o; k < o + np; ++k) {
                        blk0[k] = o;
                        blkN[k] = np;
                        double m = 0;
                        for (int b = o; b < o + np; ++b) m = std::max(m, std::abs(r.Jf[static_cast<std::size_t>(k * nn + b)]));
                        if (m * r.kcol[static_cast<std::size_t>(k)] > 1e-9) live[na++] = k;
                    }
                    o += np;
                }
            }
            if (na < (3 * nn) / 4) {
                double* M = r.J.data(); // |A|×|A|, row-major
                double y[kMaxNlPorts];
                for (int i = 0; i < na; ++i) {
                    const int ki = live[i], o = blk0[ki], np = blkN[ki];
                    const double* Jrow = &r.Jf[static_cast<std::size_t>(ki * nn)];
                    double rhs = 0;
                    for (int b = o; b < o + np; ++b) rhs += Jrow[b] * r.F[static_cast<std::size_t>(b)];
                    y[i] = rhs;
                    for (int j = 0; j < na; ++j) {
                        const int kj = live[j];
                        double s = i == j ? 1.0 : 0.0;
                        for (int b = o; b < o + np; ++b) s -= Jrow[b] * r.K[static_cast<std::size_t>(b * nn + kj)];
                        M[i * na + j] = s;
                    }
                }
                if (na > 0 && !la::solveInPlace(M, y, na, r.pivN.data())) return false;
                woodLu = na > 0;
                wNa = na;
                for (int i = 0; i < na; ++i) {
                    const int ki = live[i];
                    wLive[i] = ki;
                    wBlk0[i] = blk0[ki];
                    wBlkN[i] = blkN[ki];
                    for (int b = 0; b < blkN[ki]; ++b) wRows[i][b] = r.Jf[static_cast<std::size_t>(ki * nn + blk0[ki] + b)];
                }
                for (int k = 0; k < nn; ++k) {
                    double d = r.F[static_cast<std::size_t>(k)];
                    const double* Kr = &r.K[static_cast<std::size_t>(k * nn)];
                    for (int j = 0; j < na; ++j) d += Kr[live[j]] * y[j];
                    r.delta[static_cast<std::size_t>(k)] = d;
                }
                // r.J now holds a reduced factorisation: the chord step must not reuse it
                chordLu = false;
                goto applyStep;
            }
        }
        // J = K·Jf − I. Jf is block-diagonal (each device couples only its own ports, at most 3), so each column b
        // needs only K's columns inside b's block: nn·nn·(block) instead of nn³ multiply-adds.
        {
            int o = 0;
            for (int di : g.devices) {
                const int np = devices_[static_cast<std::size_t>(di)].nports;
                for (int b = o; b < o + np; ++b)
                    for (int a = 0; a < nn; ++a) {
                        double s = a == b ? -1.0 : 0.0;
                        const double* Ka = &r.K[static_cast<std::size_t>(a * nn)];
                        for (int k = o; k < o + np; ++k) s += Ka[k] * r.Jf[static_cast<std::size_t>(k * nn + b)];
                        r.J[static_cast<std::size_t>(a * nn + b)] = s;
                    }
                o += np;
            }
        }
        for (int k = 0; k < nn; ++k) r.delta[static_cast<std::size_t>(k)] = -r.F[static_cast<std::size_t>(k)];
        if (!la::solveInPlace(r.J.data(), r.delta.data(), nn, r.pivN.data())) return false;
        chordLu = true;
        woodLu = false;
    applyStep:
        lastStep = 0;
        prevLimited = false;
        for (int k = 0; k < nn; ++k) {
            const auto ks = static_cast<std::size_t>(k);
            const double vold = vp[k], vfull = vold + r.delta[ks];
            const double vnew = limitPort(r.lim[ks], r.limA[ks], r.limB[ks], r.limC[ks], vold, vfull);
            prevLimited |= vnew != vfull;
            vp[k] = vnew;
            lastStep = std::max(lastStep, std::abs(vnew - vold) / (1.0 + std::abs(vnew)));
        }
    }
}

bool Circuit::newtonPtc(GroupRt& r, const Group& g, const double* p, int maxIt) noexcept {
    // Pseudo-transient continuation (Kelley & Keyes 1998) for regenerative / Schmitt-trigger folds where the
    // previous sample's branch disappears and K·Jd has a positive-feedback eigenvalue > 1 across the transition.
    // Integrates dvp/dtau = F(vp) via (K·Jd - (1 + mu)I) d = -F with mu adapted from the Rayleigh quotient of K·Jd,
    // then decays mu -> 0 and accepts only at mu == 0 (exact unmodified circuit equations).
    const int nn = r.nn;
    if (nn <= 0 || nn > kMaxNlPorts) return false;
    double* vp = r.vp.data();
    double* cur = &r.w[static_cast<std::size_t>(r.ns + r.nu)];
    double* KJd = r.KJd.data(); // nn², preallocated (kMaxNlPorts² would not belong on the audio thread's stack)
    double mu = 2.0, prevMaxF = 1e30, lastStep = 1e30, lastRho = 1.0;
    bool prevLimited = false;
    int blk0[kMaxNlPorts], blkN[kMaxNlPorts]; // each port's device block: Jf is block diagonal
    for (int it = 0; it < maxIt; ++it) {
        std::fill(r.Jf.begin(), r.Jf.end(), 0.0);
        int o = 0;
        for (int di : g.devices) {
            const auto& d = devices_[static_cast<std::size_t>(di)];
            double Jl[9];
            d.eval(vp + o, cur + o, Jl);
            for (int a = 0; a < d.nports; ++a) {
                blk0[o + a] = o;
                blkN[o + a] = d.nports;
                for (int b = 0; b < d.nports; ++b) r.Jf[static_cast<std::size_t>((o + a) * nn + o + b)] = Jl[a * d.nports + b];
            }
            o += d.nports;
        }
        double maxF = 0;
        for (int k = 0; k < nn; ++k) {
            double s = p[k] - vp[k];
            const double* Kr = &r.K[static_cast<std::size_t>(k * nn)];
            for (int j = 0; j < nn; ++j) s += Kr[j] * cur[j];
            r.F[static_cast<std::size_t>(k)] = s;
            maxF = std::max(maxF, std::abs(s));
        }
        if (!std::isfinite(maxF)) return false;
        if (it > 0 && mu == 0.0 && !prevLimited && lastStep <= r.tol) {
            r.count(it);
            dg(g).newtonIterations += static_cast<std::uint64_t>(it);
            dg(g).maxIterations = std::max(dg(g).maxIterations, it);
            dg(g).worstResidual = std::max(dg(g).worstResidual, maxF);
            return true;
        }
        const bool growing = maxF >= 0.99 * prevMaxF;
        if (it > 0) {
            if (maxF > 2.0 * prevMaxF && maxF > 1e-2)
                mu = std::min(std::max(mu * 4.0, 2.0), 4096.0);
            else if (mu > 0.0 && !prevLimited) {
                if (lastRho < 0.5 && maxF < prevMaxF)
                    mu = (mu <= 0.125 && maxF < 0.5) ? 0.0 : std::max(0.125, mu * 0.25);
                else if (lastRho >= 0.5 && (1.0 + mu) > 3.5 * std::max(lastRho, 1.0))
                    mu = std::max(2.0 * lastRho, 0.25 * mu);
            }
        }
        prevMaxF = maxF;
        // K·Jf over Jf's nonzero rows only: column b is non-zero in its device's block (the terms skipped are exact zeros,
        // the rest are summed in the same order, so the product is the full one bit for bit)
        for (int a = 0; a < nn; ++a)
            for (int b = 0; b < nn; ++b) {
                double s = 0.0;
                for (int k = blk0[b], ke = blk0[b] + blkN[b]; k < ke; ++k) s += r.K[static_cast<std::size_t>(a * nn + k)] * r.Jf[static_cast<std::size_t>(k * nn + b)];
                KJd[a * nn + b] = s;
            }
        bool solved = false;
        for (int tries = 0; tries < 6; ++tries) {
            for (int a = 0; a < nn; ++a)
                for (int b = 0; b < nn; ++b)
                    r.J[static_cast<std::size_t>(a * nn + b)] = KJd[a * nn + b] - (a == b ? (1.0 + mu) : 0.0);
            if (!la::luFactor(r.J.data(), nn, r.pivN.data())) {
                mu = std::min(std::max(mu * 4.0, 1.0), 4096.0);
                continue;
            }
            for (int k = 0; k < nn; ++k) r.delta[static_cast<std::size_t>(k)] = -r.F[static_cast<std::size_t>(k)];
            la::luSolve(r.J.data(), r.pivN.data(), nn, r.delta.data());
            auto rayleigh = [&](const double* v) {
                double vv = 0.0, vKv = 0.0;
                for (int a = 0; a < nn; ++a) {
                    vv += v[a] * v[a];
                    double sum = 0.0;
                    for (int b = 0; b < nn; ++b) sum += KJd[a * nn + b] * v[b];
                    vKv += v[a] * sum;
                }
                return vv > 1e-30 ? vKv / vv : 0.0;
            };
            double rho = rayleigh(r.delta.data());
            const double sigma = 1.0 + mu;
            if (rho > 0.5 && sigma < 2.0 * rho && rho < 2.0 * sigma) {
                // Non-normal BJT stages can give d^T KJd d / d^T d ~ sigma even when all eigenvalues of KJd are < 0;
                // inverse iteration (-J)^{-1} = (sigma I - KJd)^{-1} amplifies every eigenvalue in (0, 2*sigma) over
                // every passive eigenvalue <= 0 while collapsing Jordan transients.
                double v[kMaxNlPorts];
                for (int k = 0; k < nn; ++k) v[k] = r.delta[static_cast<std::size_t>(k)];
                for (int pIt = 0; pIt < 4; ++pIt) {
                    for (int k = 0; k < nn; ++k) v[k] = -v[k];
                    la::luSolve(r.J.data(), r.pivN.data(), nn, v);
                    double nrm = 0.0;
                    for (int k = 0; k < nn; ++k) nrm = std::max(nrm, std::abs(v[k]));
                    if (nrm > 1e-30)
                        for (int k = 0; k < nn; ++k) v[k] /= nrm;
                }
                rho = rayleigh(v);
            }
            lastRho = rho;
            if (traceNewton_) std::fprintf(stderr, "   ptc it %3d try %d mu=%.4g rho=%+.4g maxF=%.3e lim=%d vp5=%+.5f vp6=%+.5f\n", it, tries, mu, rho, maxF, prevLimited ? 1 : 0, vp[5 < nn ? 5 : 0], vp[6 < nn ? 6 : 0]);
            if (rho > 0.5 && (1.0 + mu) < 2.0 * rho && mu < 4096.0) {
                mu = std::min(std::max(2.5 * rho, mu * 2.0 + 1.0), 4096.0);
                continue;
            }
            solved = true;
            break;
        }
        if (!solved) return false;
        {
            double maxDelta = 0.0;
            for (int k = 0; k < nn; ++k) maxDelta = std::max(maxDelta, std::abs(r.delta[static_cast<std::size_t>(k)]));
            if (it >= 3 && lastRho > 1.02 && growing && !prevLimited && maxDelta > 0.0 && maxDelta < 0.015) {
                const double boost = std::min(0.015 / maxDelta, 64.0);
                for (int k = 0; k < nn; ++k) r.delta[static_cast<std::size_t>(k)] *= boost;
            } else if (mu > 0.0 && maxDelta > 5.0) {
                const double scale = 5.0 / maxDelta;
                for (int k = 0; k < nn; ++k) r.delta[static_cast<std::size_t>(k)] *= scale;
            }
        }
        lastStep = 0;
        prevLimited = false;
        for (int k = 0; k < nn; ++k) {
            const auto ks = static_cast<std::size_t>(k);
            const double vold = vp[k], vfull = vold + r.delta[ks];
            const double vnew = limitPort(r.lim[ks], r.limA[ks], r.limB[ks], r.limC[ks], vold, vfull);
            prevLimited |= vnew != vfull;
            vp[k] = vnew;
            lastStep = std::max(lastStep, std::abs(vnew - vold) / (1.0 + std::abs(vnew)));
        }
        if (mu > 0.0 && lastRho < 0.5 && !prevLimited && lastStep * (1.0 + mu) <= 0.1 * r.tol && maxF <= r.tol) mu = 0.0;
    }
    return false;
}

std::vector<double> Circuit::benchNewton(std::size_t gi, int reps) {
    auto& r = *rt_[gi];
    const auto& g = groups_[gi];
    if (r.nn == 0 || r.wdf) return {0, 0, 0};
    std::vector<double> keep(r.vp), keepW(r.w), p(r.pPrev);
    const auto diag = diag_;
    std::uint64_t it0 = r.iters;
    auto t0 = std::chrono::steady_clock::now();
    for (int k = 0; k < reps; ++k) {
        for (int j = 0; j < r.nn; ++j) r.vp[static_cast<std::size_t>(j)] = 2.0 * r.vpPrev2[static_cast<std::size_t>(j)] - r.vpPrev2[static_cast<std::size_t>(j)] + (r.vpPrev[static_cast<std::size_t>(j)] - r.vpPrev2[static_cast<std::size_t>(j)]);
        newton(r, g, p.data(), opt_.maxIterations);
    }
    const double tSolve = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / reps;
    const double its = static_cast<double>(r.iters - it0) / reps;
    double sink = 0, cur[kMaxNlPorts], Jl[9];
    t0 = std::chrono::steady_clock::now();
    for (int k = 0; k < reps; ++k) {
        int o = 0;
        for (int di : g.devices) {
            const auto& d = devices_[static_cast<std::size_t>(di)];
            d.eval(r.vp.data() + o, cur + o, Jl);
            sink += Jl[0];
            o += d.nports;
        }
        r.vp[0] += 1e-12 * sink; // keep the loop alive
    }
    const double tEval = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / reps;
    r.vp = keep;
    r.w = keepW;
    diag_ = diag;
    auto nz = [](const std::vector<double>& m) {
        double mx = 0;
        for (double x : m) mx = std::max(mx, std::abs(x));
        double c = 0;
        for (double x : m) c += std::abs(x) > 1e-14 * mx ? 1 : 0;
        return c;
    };
    return {tSolve, its, tEval, static_cast<double>(r.H.size()), nz(r.H), static_cast<double>(r.Dp.size()), nz(r.Dp), static_cast<double>(r.ny), static_cast<double>(r.ns),
            static_cast<double>(r.W), static_cast<double>(r.nuc)};
}

bool Circuit::solveGroupDc(std::size_t gi) noexcept {
    const auto& g = groups_[gi];
    auto& r = *rt_[gi];
    for (const auto& mg : mags_) devices_[static_cast<std::size_t>(mg.device)].dcLinear = 1.0 / mg.lm;
    struct Restore {
        Circuit* c;
        ~Restore() {
            for (const auto& mg : c->mags_) c->devices_[static_cast<std::size_t>(mg.device)].dcLinear = 0.0;
        }
    } restore{this};
    refreshGroup(gi, true);
    for (int j = 0; j < r.nu; ++j) r.w[static_cast<std::size_t>(r.ns + j)] = V_[static_cast<std::size_t>(g.inputs[static_cast<std::size_t>(j)])];
    for (int j = 0; j < r.nuc; ++j) {
        const auto x = static_cast<std::size_t>(g.cutIn[static_cast<std::size_t>(j)]);
        r.w[static_cast<std::size_t>(r.ns + r.nu + r.nn + j)] = -(cutI_[x] - dcLoadG_[x] * V_[x]);
    }
    for (int k = 0; k < r.ns; ++k) r.w[static_cast<std::size_t>(k)] = 0.0;
    bool ok = true;
    if (r.nn > 0) {
        // p (DC: no states)
        std::vector<double>& p = r.p;
        for (int k = 0; k < r.nn; ++k) {
            double s = 0;
            for (int c = 0; c < r.ns + r.nu; ++c) s += r.Dp[static_cast<std::size_t>(k * (r.ns + r.nu) + c)] * r.w[static_cast<std::size_t>(c)];
            for (int c = 0; c < r.nuc; ++c) s += r.Dc[static_cast<std::size_t>(k * r.nuc + c)] * r.w[static_cast<std::size_t>(r.ns + r.nu + r.nn + c)];
            p[static_cast<std::size_t>(k)] = s;
        }
        std::fill(r.vp.begin(), r.vp.end(), 0.0);
        ok = newton(r, g, p.data(), 200);
        if (!ok) {
            // Source stepping: the DC equation depends on the sources only through p, so step p from 0.
            std::fill(r.vp.begin(), r.vp.end(), 0.0);
            std::vector<double> pl(p.size());
            double lam = 0, step = 1.0 / 16;
            ok = true;
            int guard = 0;
            while (lam < 1.0 && guard++ < 4000) {
                const double next = std::min(1.0, lam + step);
                for (std::size_t k = 0; k < p.size(); ++k) pl[k] = next * p[k];
                const auto save = r.vp;
                if (newton(r, g, pl.data(), 100)) {
                    lam = next;
                    step = std::min(step * 2, 0.25);
                } else {
                    r.vp = save;
                    step *= 0.5;
                    if (step < 1e-6) {
                        ok = false;
                        break;
                    }
                }
            }
            if (lam < 1.0) ok = false;
        }
        if (!ok) {
            // Gmin stepping (as ngspice): a large shunt conductance on every node makes an open-loop integrator inside a
            // servo (whose DC gain is otherwise set only by junctions) a well-conditioned stage; relax it toward the
            // normal 1e-12 S a step at a time from each solution, halving the step when Newton fails. The last solve uses
            // the normal DC matrices, so the answer satisfies exactly the equations of the direct path.
            auto pOf = [&](std::vector<double>& out) {
                for (int k = 0; k < r.nn; ++k) {
                    double s = 0;
                    for (int c = 0; c < r.ns + r.nu; ++c) s += r.Dp[static_cast<std::size_t>(k * (r.ns + r.nu) + c)] * r.w[static_cast<std::size_t>(c)];
                    for (int c = 0; c < r.nuc; ++c) s += r.Dc[static_cast<std::size_t>(k * r.nuc + c)] * r.w[static_cast<std::size_t>(r.ns + r.nu + r.nn + c)];
                    out[static_cast<std::size_t>(k)] = s;
                }
            };
            struct GminRestore {
                Circuit* c;
                ~GminRestore() { c->dcGmin_ = kDcGmin; }
            } gminRestore{this};
            std::fill(r.vp.begin(), r.vp.end(), 0.0);
            double lg = -2.0, lstep = 1.0; // log10(gmin), from 1e-2 S
            bool have = false;
            int guard = 0;
            while (guard++ < 400) {
                dcGmin_ = std::pow(10.0, lg);
                refreshGroup(gi, true);
                pOf(p);
                const auto save = r.vp;
                if (newton(r, g, p.data(), 200)) {
                    have = true;
                    if (lg <= -12.0) break;
                    lg = std::max(-12.0, lg - lstep);
                    lstep = std::min(lstep * 1.5, 2.0);
                } else {
                    r.vp = save;
                    if (!have) { // not even the strongest shunt: start stronger
                        lg += 1.0;
                        if (lg > 2.0) break;
                        continue;
                    }
                    lg += lstep; // back to the last solved shunt, smaller step
                    lstep *= 0.5;
                    if (lstep < 1e-3) break;
                    lg -= lstep;
                }
            }
            ok = have && lg <= -12.0 && dcGmin_ <= kDcGmin * 1.000001;
            dcGmin_ = kDcGmin;
            refreshGroup(gi, true);
            pOf(p);
            if (ok) ok = newton(r, g, p.data(), 200); // polish on the exact DC matrices
            if (debugFail_) std::fprintf(stderr, "DC group %zu: gmin stepping %s\n", gi, ok ? "converged" : "failed");
        }
    }
    // Exports + state voltages.
    const int rows = r.ny + r.ns;
    for (int e = 0; e < rows; ++e) {
        double s = 0;
        const double* h = &r.H[static_cast<std::size_t>(e * r.W)];
        for (int c = 0; c < r.W; ++c) s += h[c] * r.w[static_cast<std::size_t>(c)];
        r.out[static_cast<std::size_t>(e)] = s;
    }
    for (int e = 0; e < r.ny; ++e) V_[static_cast<std::size_t>(g.exports[static_cast<std::size_t>(e)])] = r.out[static_cast<std::size_t>(e)];
    for (int k = 0; k < r.ns; ++k) r.vcLast[static_cast<std::size_t>(k)] = r.out[static_cast<std::size_t>(r.ny + k)];
    return ok;
}

bool Circuit::dcCutIterations(const std::vector<std::uint8_t>* skip) {
    // Supply rails and their consumers alternate until the rail voltages settle (the cut is exact at DC).
    // skip: groups held at their present state (their exported nodes act as fixed sources).
    std::fill(cutAcc_.begin(), cutAcc_.end(), 0.0);
    double change = 0, dv = 0;
    std::vector<double> vLast(V_.size(), 0.0);
    // The owner sees its loads as a Norton equivalent with conductance dcLoadG_ (the loads' resistors). Where the loads
    // are held by feedback (an OTA stage keeps its input at the bias rail through 1 kΩ) that overstates the real
    // incremental load by orders of magnitude and the iteration crawls (4 % per sweep). From the third sweep the
    // conductance follows the measured secant ΔI/ΔV of the consumer current (clamped to [0, the resistor estimate]);
    // the converged point does not depend on it.
    const std::vector<double> gStatic = dcLoadG_;
    std::vector<double> vPrev(V_.size(), 0.0), iPrev(V_.size(), 0.0);
    struct RestoreG {
        std::vector<double>& g;
        const std::vector<double>& s;
        ~RestoreG() { g = s; }
    } restoreG{dcLoadG_, gStatic};
    for (int it = 0; it < 400; ++it) {
        for (std::size_t nd = 0; nd < cut_.size(); ++nd) vLast[nd] = V_[nd];
        for (std::size_t gi = 0; gi < groups_.size(); ++gi) {
            if (!skip || !(*skip)[gi]) solveGroupDc(gi);
            if (!groups_[gi].cutTerms.empty()) accumulateCutTerms(gi, true);
        }
        if (!railAmps_.empty()) railDraws(); // rail-node op-amps' supply currents belong to this iteration's balance
        change = 0;
        dv = 0;
        for (std::size_t nd = 0; nd < cutI_.size(); ++nd) change = std::max(change, std::abs(cutAcc_[nd] - cutI_[nd]));
        std::size_t worst = 0;
        for (std::size_t nd = 0; nd < cut_.size(); ++nd)
            if (cut_[nd] && std::abs(V_[nd] - vLast[nd]) > dv) dv = std::abs(V_[nd] - vLast[nd]), worst = nd;
        if (debugFail_ && it % 50 == 49) std::fprintf(stderr, "  DC cut iteration %d: dv %.3g V at %s\n", it, dv, nodeNames_[worst].c_str());
        if (debugFail_ && it < 12) {
            std::fprintf(stderr, "DC cut iteration %d: change %.3g A;", it, change);
            for (std::size_t nd = 0; nd < cut_.size(); ++nd)
                if (cut_[nd]) std::fprintf(stderr, " %s=%.2fV/%.2fmA", nodeNames_[nd].c_str(), V_[nd], 1e3 * cutAcc_[nd]);
            std::fprintf(stderr, "\n");
        }
        if (it >= 2)
            for (std::size_t nd = 0; nd < cut_.size(); ++nd) {
                if (!cut_[nd] || gStatic[nd] <= 0) continue;
                const double dV = V_[nd] - vPrev[nd];
                if (std::abs(dV) > 1e-7) {
                    const double g = (cutAcc_[nd] - iPrev[nd]) / dV;
                    if (std::isfinite(g) && g > 0.0) {
                        if (g >= dcLoadG_[nd] || std::abs(dV) < 10.0)
                            dcLoadG_[nd] = std::clamp(g, 1e-3 * gStatic[nd], std::max(gStatic[nd], 100.0));
                    }
                }
            }
        for (std::size_t nd = 0; nd < cut_.size(); ++nd) vPrev[nd] = V_[nd], iPrev[nd] = cutAcc_[nd];
        finishCutSample();
        // Currents settle first; a cut node held only weakly (a mixing node behind coupling caps) needs its
        // voltage settled too, or a 1 nA residual sits there as volts.
        if (change < 1e-9 && dv < 1e-6) {
            if (debugFail_) std::fprintf(stderr, "DC cut iterations: converged after %d\n", it + 1);
            break;
        }
        if (debugFail_ && it == 399) std::fprintf(stderr, "DC cut iterations: not converged (change %.3g A, dv %.3g V)\n", change, dv);
    }
    return change < 1e-6;
}

void Circuit::initGroupStates(std::size_t gi) noexcept {
    auto& r = *rt_[gi];
    // Initialise reactive states at the operating point (zero capacitor current, DC inductor current).
    std::fill(r.gc.begin(), r.gc.end(), 0.0);
    refreshGroup(gi, false);
    const auto& g = groups_[gi];
    for (int k = 0; k < r.ns; ++k) {
        const auto& p = prims_[static_cast<std::size_t>(g.states[static_cast<std::size_t>(k)])];
        const double vc = r.vcLast[static_cast<std::size_t>(k)];
        const double G = r.gc[static_cast<std::size_t>(k)];
        r.w[static_cast<std::size_t>(k)] = p.kind == Prim::Cap ? G * vc : kDcInductorG * vc - G * vc;
    }
    for (int j = 0; j < r.nu; ++j) r.w[static_cast<std::size_t>(r.ns + j)] = V_[static_cast<std::size_t>(g.inputs[static_cast<std::size_t>(j)])];
    for (int j = 0; j < r.nuc; ++j) {
        const auto x = static_cast<std::size_t>(g.cutIn[static_cast<std::size_t>(j)]);
        r.w[static_cast<std::size_t>(r.ns + r.nu + r.nn + j)] = -(cutI_[x] - tranLoadG_[x] * V_[x]);
    }
    // Newton warm start + homotopy anchor at the operating point under the transient matrices.
    for (int k = 0; k < r.nn; ++k) {
        double s = 0;
        for (int c = 0; c < r.ns + r.nu; ++c) s += r.Dp[static_cast<std::size_t>(k * (r.ns + r.nu) + c)] * r.w[static_cast<std::size_t>(c)];
        for (int c = 0; c < r.nuc; ++c) s += r.Dc[static_cast<std::size_t>(k * r.nuc + c)] * r.w[static_cast<std::size_t>(r.ns + r.nu + r.nn + c)];
        r.pPrev[static_cast<std::size_t>(k)] = s;
    }
    if (rtHalf_[gi]) {
        for (int j = 0; j < r.nu; ++j) r.uPrev[static_cast<std::size_t>(j)] = r.w[static_cast<std::size_t>(r.ns + j)];
        for (int j = 0; j < r.nuc; ++j) r.uPrev[static_cast<std::size_t>(r.nu + j)] = r.w[static_cast<std::size_t>(r.ns + r.nu + r.nn + j)];
    }
    r.vpPrev = r.vp;
    if (r.noiseOn && r.nn > 0) { // tube plate current and gm at the operating point (shot-noise scaling, updateNoise)
        int o = 0;
        for (int di : g.devices) {
            const auto& d = devices_[static_cast<std::size_t>(di)];
            if (d.kind == NlKind::Triode || d.kind == NlKind::Pentode) {
                double cur[3], J[9];
                d.eval(r.vp.data() + o, cur, J);
                r.noiseI0[static_cast<std::size_t>(o + 1)] = cur[1];
                r.noiseGm0[static_cast<std::size_t>(o + 1)] = std::max(J[1 * d.nports + 0], 0.0);
            }
            o += d.nports;
        }
    }
    r.subPrev = 1;
    if (r.lteOn) { // a flat history at the operating point
        r.lteWarm = 0;
        r.subNext = 1;
        for (int i = 0; i < 5; ++i) pushLteHistory(r);
    }
    r.vpPrev2 = r.vp;
    r.predHist = 0;
    for (int k = 0; k < r.nn; ++k) {
        const double c = r.w[static_cast<std::size_t>(r.ns + r.nu + k)];
        if (r.nn <= kMaxNlPorts) r.curPrev1[static_cast<std::size_t>(k)] = r.curPrev2[static_cast<std::size_t>(k)] = c;
    }
    for (const auto& [di, o] : g.hyst) // hysteresis plays start centred on the operating point
        for (double& c : devices_[static_cast<std::size_t>(di)].hs) c = r.vp[static_cast<std::size_t>(o)];
    for (const auto& [ti, o] : g.spk) { // voice coils start at ambient
        auto& t = spkTh_[static_cast<std::size_t>(ti)];
        for (double& c : devices_[static_cast<std::size_t>(t.dev)].hs) c = 0.0;
        t.pPrev = 0.0;
    }
    if (r.wdf) r.wdf->resetFrom(*this, g, r.w.data(), r.vcLast.data());
}

// DC with CMOS cells: levels from the operating point, re-solved until no output changes (a DC-stable loop settles;
// an oscillator keeps its last state and starts from there).
bool Circuit::settleLogicDc() {
    bool ok = true;
    for (auto& c : logic_) {
        c.reset(V_.data());
        for (int j = 0; j < c.nout; ++j) V_[static_cast<std::size_t>(c.out[j].node)] = c.out[j].base;
    }
    for (int pass = 0; pass < 12; ++pass) {
        for (std::size_t gi = 0; gi < groups_.size(); ++gi) {
            if (!solveGroupDc(gi)) {
                ok = false;
                if (debugFail_) std::fprintf(stderr, "logic DC pass %d: group %zu failed\n", pass, gi);
            }
            initGroupStates(gi);
        }
        bool changed = false;
        for (auto& c : logic_) changed |= c.settleDc(V_.data());
        if (!changed) break;
    }
    return ok;
}

// Threshold-0.1 Markowitz pivot order on a representative n×n matrix W with pattern S (fill tracked in S), then the
// SparseLu's fill pattern and L/U index lists in the permuted coordinates. rowPos / colPos: original -> permuted.
// Control thread. False if no pivot order exists (structurally or numerically singular here).
// Depth-aware ties (docs §10.66): on groups of at least kSpDepthMinPorts nonlinear ports, equal-fill candidates go to the
// lower elimination level first. CE_SPDEPTH=0: never (the order before §10.66), 1: every group.
static const int kEnvSpDepth = std::getenv("CE_SPDEPTH") ? std::atoi(std::getenv("CE_SPDEPTH")) : -1;
constexpr int kSpDepthMinPorts = 20;
static int spDepthMode(int nn) noexcept { return kEnvSpDepth >= 0 ? kEnvSpDepth : (nn >= kSpDepthMinPorts ? 1 : 0); }
static bool spOrder(SparseLu& s, int n, std::vector<std::uint8_t>& S, std::vector<double>& W, std::vector<int>& rowPos, std::vector<int>& colPos,
                    double& flops, int depthMode) {
    const auto N = static_cast<std::size_t>(n);
    // threshold Markowitz on the representative matrix (fill tracked in S)
    std::vector<int> rc(N, 0), cc(N, 0), rowOrd(N), colOrd(N);
    std::vector<std::uint8_t> rDone(N, 0), cDone(N, 0);
    for (std::size_t i = 0; i < N; ++i)
        for (std::size_t j = 0; j < N; ++j)
            if (S[i * N + j]) ++rc[i], ++cc[j];
    // depthMode (docs §10.66): the elimination level of each row / column (1 + the deepest pivot whose update reached
    // it), a pivot's level the larger of its row's and column's. 1: equal-fill ties go to the lower level (then the tiers
    // below), so the elimination tree is shallower and the solves' dependency chains shorter (5150 step 549 -> 451
    // cycles of latency) at equal or less fill; 2 (experiment): the lowest level first, fill second (more fill, no
    // shorter chain on the 5150). Structural only, so the order still depends on the switch topology alone.
    std::vector<int> lvR(N, 0), lvC(N, 0);
    for (int k = 0; k < n; ++k) {
        long best = -1;
        int bestTier = -1, bestLv = 0;
        int bi = -1, bj = -1;
        for (int j = 0; j < n; ++j) {
            if (cDone[static_cast<std::size_t>(j)]) continue;
            double cmax = 0;
            for (int i = 0; i < n; ++i)
                if (!rDone[static_cast<std::size_t>(i)] && S[static_cast<std::size_t>(i) * N + static_cast<std::size_t>(j)]) cmax = std::max(cmax, std::abs(W[static_cast<std::size_t>(i) * N + static_cast<std::size_t>(j)]));
            if (!(cmax > 0)) continue;
            for (int i = 0; i < n; ++i) {
                const auto q = static_cast<std::size_t>(i) * N + static_cast<std::size_t>(j);
                if (rDone[static_cast<std::size_t>(i)] || !S[q] || std::abs(W[q]) < 0.1 * cmax) continue;
                const long cost = static_cast<long>(rc[static_cast<std::size_t>(i)] - 1) * (cc[static_cast<std::size_t>(j)] - 1);
                // Break equal Markowitz fill ties canonically (diagonal first, then >= 0.5*cmax column-relative strength,
                // then index order) rather than raw |W| across unscaled columns, so continuous knob sweeps and T/2, T/4
                // substeps keep the same permutation unless threshold admissibility itself moves.
                const int tier = ((i == j) ? 2 : 0) + (std::abs(W[q]) >= 0.5 * cmax ? 1 : 0);
                const int lv = std::max(lvR[static_cast<std::size_t>(i)], lvC[static_cast<std::size_t>(j)]);
                bool take;
                if (depthMode == 1) take = best < 0 || cost < best || (cost == best && (lv < bestLv || (lv == bestLv && tier > bestTier)));
                else if (depthMode == 2) take = best < 0 || lv < bestLv || (lv == bestLv && (cost < best || (cost == best && tier > bestTier)));
                else take = best < 0 || cost < best || (cost == best && tier > bestTier);
                if (take) {
                    best = cost;
                    bestTier = tier;
                    bestLv = lv;
                    bi = i;
                    bj = j;
                }
            }
        }
        if (bi < 0) return false; // structurally or numerically singular here: dense paths only
        rowOrd[static_cast<std::size_t>(k)] = bi;
        colOrd[static_cast<std::size_t>(k)] = bj;
        rDone[static_cast<std::size_t>(bi)] = cDone[static_cast<std::size_t>(bj)] = 1;
        const auto BI = static_cast<std::size_t>(bi), BJ = static_cast<std::size_t>(bj);
        const double pv = W[BI * N + BJ];
        if (depthMode) {
            const int nl = std::max(lvR[BI], lvC[BJ]) + 1;
            for (std::size_t i = 0; i < N; ++i)
                if (!rDone[i] && S[i * N + BJ]) lvR[i] = std::max(lvR[i], nl);
            for (std::size_t j = 0; j < N; ++j)
                if (!cDone[j] && S[BI * N + j]) lvC[j] = std::max(lvC[j], nl);
        }
        for (std::size_t i = 0; i < N; ++i) {
            if (rDone[i] || !S[i * N + BJ]) continue;
            const double l = W[i * N + BJ] / pv;
            for (std::size_t j = 0; j < N; ++j) {
                if (cDone[j] || !S[BI * N + j]) continue;
                if (!S[i * N + j]) {
                    S[i * N + j] = 1;
                    ++rc[i];
                    ++cc[j];
                }
                W[i * N + j] -= l * W[BI * N + j];
            }
        }
        for (std::size_t j = 0; j < N; ++j)
            if (!cDone[j] && S[BI * N + j]) --cc[j];
        for (std::size_t i = 0; i < N; ++i)
            if (!rDone[i] && S[i * N + BJ]) --rc[i];
    }
    rowPos.assign(N, 0);
    colPos.assign(N, 0);
    for (int k = 0; k < n; ++k) {
        rowPos[static_cast<std::size_t>(rowOrd[static_cast<std::size_t>(k)])] = k;
        colPos[static_cast<std::size_t>(colOrd[static_cast<std::size_t>(k)])] = k;
    }
    std::vector<std::vector<int>> Lc(N), Ur(N);
    s.pat.clear();
    for (std::size_t i = 0; i < N; ++i)
        for (std::size_t j = 0; j < N; ++j) {
            if (!S[i * N + j]) continue;
            const int pi = rowPos[i], pj = colPos[j];
            s.pat.push_back(pi * n + pj);
            if (pi > pj) Lc[static_cast<std::size_t>(pj)].push_back(pi);
            if (pj > pi) Ur[static_cast<std::size_t>(pi)].push_back(pj);
        }
    std::sort(s.pat.begin(), s.pat.end());
    s.lOff.assign(1, 0);
    s.uOff.assign(1, 0);
    s.lIdx.clear();
    s.uIdx.clear();
    flops = 0;
    for (std::size_t k = 0; k < N; ++k) {
        std::sort(Lc[k].begin(), Lc[k].end());
        std::sort(Ur[k].begin(), Ur[k].end());
        s.lIdx.insert(s.lIdx.end(), Lc[k].begin(), Lc[k].end());
        s.uIdx.insert(s.uIdx.end(), Ur[k].begin(), Ur[k].end());
        s.lOff.push_back(static_cast<int>(s.lIdx.size()));
        s.uOff.push_back(static_cast<int>(s.uIdx.size()));
        flops += static_cast<double>(Lc[k].size()) * static_cast<double>(Ur[k].size() + 1);
    }
    return true;
}

// Packed storage for a SparseLu after spOrder and its mPos / stPos (still permuted n×n positions, s.n set): those become
// packed positions, the elimination's target list is compiled and the value array sized. Control thread. False if a
// position falls outside the fill pattern (cannot happen: the pattern is closed under elimination).
static bool spPack(SparseLu& s) {
    const int n = s.n;
    const auto N = static_cast<std::size_t>(n), nL = s.lIdx.size(), nU = s.uIdx.size();
    std::vector<int> at(N * N, -1);
    for (int k = 0; k < n; ++k) {
        for (int e = s.lOff[static_cast<std::size_t>(k)]; e < s.lOff[static_cast<std::size_t>(k) + 1]; ++e)
            at[static_cast<std::size_t>(s.lIdx[static_cast<std::size_t>(e)] * n + k)] = e;
        for (int e = s.uOff[static_cast<std::size_t>(k)]; e < s.uOff[static_cast<std::size_t>(k) + 1]; ++e)
            at[static_cast<std::size_t>(k * n + s.uIdx[static_cast<std::size_t>(e)])] = static_cast<int>(nL) + e;
        at[static_cast<std::size_t>(k * n + k)] = static_cast<int>(nL + nU) + k;
    }
    for (auto* idx : {&s.mPos, &s.stPos})
        for (int& q : *idx) {
            q = at[static_cast<std::size_t>(q)];
            if (q < 0) return false;
        }
    s.opDst.clear();
    for (int k = 0; k < n; ++k)
        for (int e = s.lOff[static_cast<std::size_t>(k)]; e < s.lOff[static_cast<std::size_t>(k) + 1]; ++e)
            for (int u = s.uOff[static_cast<std::size_t>(k)]; u < s.uOff[static_cast<std::size_t>(k) + 1]; ++u) {
                const int q = at[static_cast<std::size_t>(s.lIdx[static_cast<std::size_t>(e)] * n + s.uIdx[static_cast<std::size_t>(u)])];
                if (q < 0) return false;
                s.opDst.push_back(q);
            }
    s.v.assign(nL + nU + N, 0.0);
    // static / live split (see the struct): walk the elimination once, marking entries live from the stamp positions on
    std::vector<std::uint8_t> dyn(nL + nU + N, 0);
    for (int q : s.stPos) dyn[static_cast<std::size_t>(q)] = 1;
    s.opLive.assign(s.opDst.size(), 0);
    s.lLive.assign(nL, 0);
    s.kFl.assign(N, 0);
    s.dOff.assign(1, 0);
    s.rOff.assign(1, 0);
    s.oOff.assign(1, 0);
    s.dIdx.clear(), s.rE.clear(), s.oQ.clear(), s.oU.clear();
    std::size_t o = 0;
    for (int k = 0; k < n; ++k) {
        const auto K = static_cast<std::size_t>(k);
        const bool pivLive = dyn[nL + nU + K] != 0;
        std::uint8_t fl = pivLive ? 3 : 0;
        for (int e = s.lOff[K]; e < s.lOff[K + 1]; ++e) {
            const auto E = static_cast<std::size_t>(e);
            if (pivLive || dyn[E]) {
                s.lLive[E] = 1;
                dyn[E] = 1;
                fl |= 2;
                s.dIdx.push_back(e);
            }
            bool any = false;
            for (int u = s.uOff[K]; u < s.uOff[K + 1]; ++u, ++o) {
                const auto q = static_cast<std::size_t>(s.opDst[o]);
                if (dyn[E] || dyn[nL + static_cast<std::size_t>(u)]) dyn[q] = 1;
                if (!dyn[q]) continue;
                s.opLive[o] = 1;
                s.oQ.push_back(static_cast<int>(q));
                s.oU.push_back(static_cast<int>(nL) + u);
                any = true;
            }
            if (any) {
                s.rE.push_back(e);
                s.oOff.push_back(static_cast<int>(s.oQ.size()));
            }
        }
        s.kFl[K] = fl;
        s.dOff.push_back(static_cast<int>(s.dIdx.size()));
        s.rOff.push_back(static_cast<int>(s.rE.size()));
    }
    s.snap.assign(nL + nU + N, 0.0);
    s.cmaxS.assign(N, 0.0);
    s.stale = true;
    s.snapOk = false;
    return true;
}
// Back substitution rows for a solve whose caller reads x at cols (permuted; −1 = none): those columns and every
// column their U rows reach, descending. The other rows' x is never read, so skipping them changes nothing.
static std::vector<int> spNeeded(const SparseLu& s, const std::vector<int>& colsA, const std::vector<int>& colsB) {
    std::vector<std::uint8_t> need(static_cast<std::size_t>(s.n), 0);
    for (const auto* c : {&colsA, &colsB})
        for (int j : *c)
            if (j >= 0) need[static_cast<std::size_t>(j)] = 1;
    for (int k = 0; k < s.n; ++k) // U's rows reach only later columns: one ascending sweep closes the set
        if (need[static_cast<std::size_t>(k)])
            for (int e = s.uOff[static_cast<std::size_t>(k)]; e < s.uOff[static_cast<std::size_t>(k) + 1]; ++e) need[static_cast<std::size_t>(s.uIdx[static_cast<std::size_t>(e)])] = 1;
    std::vector<int> rows;
    for (int k = s.n - 1; k >= 0; --k)
        if (need[static_cast<std::size_t>(k)]) rows.push_back(k);
    return rows;
}

// Sparse nodal Newton step (SparseLu): pivot order and fill pattern for a group above 24 ports, from M's pattern plus
// every device stamp position, with values at the current operating point (threshold-0.1 Markowitz). Control thread.
static void luSignature(const SparseLu& s, const int* bs, const int* bn, int nn, std::uint64_t& h1, std::uint64_t& h2) {
    h1 = 1469598103934665603ull;
    h2 = 0x6a09e667f3bcc909ull;
    auto put = [&](std::int64_t v) {
        const auto u = static_cast<std::uint64_t>(v);
        for (int b = 0; b < 8; ++b) h1 = (h1 ^ ((u >> (8 * b)) & 0xffu)) * 1099511628211ull;
        std::uint64_t z = h2 + u + 0x9E3779B97F4A7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        h2 = z ^ (z >> 31);
    };
    auto putv = [&](const std::vector<int>& v) {
        put(-1 - static_cast<std::int64_t>(v.size()));
        for (int x : v) put(x);
    };
    put(0x4c55);
    put(s.n);
    put(nn);
    put(-1 - nn);
    for (int i = 0; i < nn; ++i) put(bs[i]);
    put(-1 - nn);
    for (int i = 0; i < nn; ++i) put(bn[i]);
    for (const auto* v : {&s.lOff, &s.lIdx, &s.uOff, &s.uIdx, &s.stOff, &s.stPos, &s.dOff, &s.dIdx, &s.rOff, &s.rE, &s.oOff, &s.oQ, &s.oU, &s.bRows, &s.pFrom,
                          &s.pTo, &s.xP, &s.xN})
        putv(*v);
    put(-1 - static_cast<std::int64_t>(s.stSign.size()));
    for (double sg : s.stSign) put(sg > 0 ? 1 : -1);
    put(-1 - static_cast<std::int64_t>(s.kFl.size()));
    for (std::uint8_t fl : s.kFl) put(fl);
}

void Circuit::buildSparse(std::size_t gi) {
    auto& r = *rt_[gi];
    if (!r.sp || r.wdf || r.dcMode || kEnvNoSparse) return;
    const auto& g = groups_[gi];
    SparseLu& s = *r.sp;
    s.ok = false;
    s.cg = nullptr;
    const int n = r.n, nn = r.nn;
    if (n <= 0 || nn <= 0) return;
    const auto N = static_cast<std::size_t>(n);
    std::vector<double> W(r.Mraw);
    std::vector<std::uint8_t> S(N * N);
    for (std::size_t q = 0; q < N * N; ++q) S[q] = W[q] != 0.0;
    s.mStruct = S;
    auto row = [&](int node) { return node > 0 && r.unkOf[static_cast<std::size_t>(node)] >= 0 && r.kcl[static_cast<std::size_t>(r.unkOf[static_cast<std::size_t>(node)])] ? r.unkOf[static_cast<std::size_t>(node)] : -1; };
    auto unk = [&](int node) { return node > 0 ? r.unkOf[static_cast<std::size_t>(node)] : -1; };
    std::vector<int> rF(static_cast<std::size_t>(nn)), rT(rF), cP(rF), cN(rF);
    std::vector<double> Jd(static_cast<std::size_t>(nn) * 3, 0.0);
    {
        int o = 0;
        double cur[3], Jl[9];
        for (int di : g.devices) {
            const auto& d = devices_[static_cast<std::size_t>(di)];
            d.eval(r.vp.data() + o, cur, Jl);
            for (int a = 0; a < d.nports; ++a) {
                const auto k = static_cast<std::size_t>(o + a);
                rF[k] = row(d.port[a].from);
                rT[k] = row(d.port[a].to);
                cP[k] = unk(d.port[a].cp);
                cN[k] = unk(d.port[a].cn);
                for (int b = 0; b < d.nports; ++b) Jd[k * 3 + static_cast<std::size_t>(b)] = std::isfinite(Jl[a * d.nports + b]) ? Jl[a * d.nports + b] : 0.0;
            }
            o += d.nports;
        }
    }
    // stamps of −B·Jd·C: (from, cp) +g, (from, cn) −g, (to, cp) −g, (to, cn) +g for each port a and block member b.
    // Order on Mraw alone (with S tracking all nonlinear stamp positions) so the permutation does not drift with the DC
    // operating point r.vp or pivot on pure-device entries that vanish at cutoff; fall back to Mraw + Jd(r.vp) only if
    // a nonlinear-only node has no linear pivot or fails to factor at DC.
    struct St { int k, i, j; double sg; };
    std::vector<St> st;
    std::vector<std::uint8_t> Sbase(S);
    for (int a = 0; a < nn; ++a) {
        const int b0 = r.bs[static_cast<std::size_t>(a)], np = r.bn[static_cast<std::size_t>(a)];
        for (int m = 0; m < np; ++m) {
            const auto b = static_cast<std::size_t>(b0 + m), A = static_cast<std::size_t>(a);
            const int rows[2] = {rF[A], rT[A]}, cols[2] = {cP[b], cN[b]};
            const double sgR[2] = {1.0, -1.0}, sgC[2] = {1.0, -1.0};
            for (int u = 0; u < 2; ++u)
                for (int v = 0; v < 2; ++v) {
                    if (rows[u] < 0 || cols[v] < 0) continue;
                    st.push_back({a * 3 + m, rows[u], cols[v], sgR[u] * sgC[v]});
                    Sbase[static_cast<std::size_t>(rows[u]) * N + static_cast<std::size_t>(cols[v])] = 1;
                }
        }
    }
    const auto* JdP = reinterpret_cast<const double (*)[3]>(Jd.data());
    auto tryBuild = [&](bool withJd) -> bool {
        std::vector<std::uint8_t> Sw(Sbase);
        std::vector<double> Ww(r.Mraw);
        if (withJd)
            for (const auto& e : st) Ww[static_cast<std::size_t>(e.i) * N + static_cast<std::size_t>(e.j)] += e.sg * Jd[static_cast<std::size_t>(e.k)];
        std::vector<int> rowPos, colPos;
        double flops = 0;
        if (!spOrder(s, n, Sw, Ww, rowPos, colPos, flops, spDepthMode(nn))) return false;
        s.mPos.clear();
        s.mSrc.clear();
        for (std::size_t q = 0; q < N * N; ++q)
            if (s.mStruct[q]) {
                s.mPos.push_back(rowPos[q / N] * n + colPos[q % N]);
                s.mSrc.push_back(static_cast<int>(q));
            }
        s.stOff.assign(static_cast<std::size_t>(nn) * 3 + 1, 0);
        s.stPos.clear();
        s.stSign.clear();
        for (int k = 0; k < nn * 3; ++k) {
            for (const auto& e : st)
                if (e.k == k) {
                    s.stPos.push_back(rowPos[static_cast<std::size_t>(e.i)] * n + colPos[static_cast<std::size_t>(e.j)]);
                    s.stSign.push_back(e.sg);
                }
            s.stOff[static_cast<std::size_t>(k) + 1] = static_cast<int>(s.stPos.size());
        }
        s.pFrom.assign(static_cast<std::size_t>(nn), -1);
        s.pTo = s.xP = s.xN = s.pFrom;
        for (std::size_t a = 0; a < static_cast<std::size_t>(nn); ++a) {
            if (rF[a] >= 0) s.pFrom[a] = rowPos[static_cast<std::size_t>(rF[a])];
            if (rT[a] >= 0) s.pTo[a] = rowPos[static_cast<std::size_t>(rT[a])];
            if (cP[a] >= 0) s.xP[a] = colPos[static_cast<std::size_t>(cP[a])];
            if (cN[a] >= 0) s.xN[a] = colPos[static_cast<std::size_t>(cN[a])];
        }
        s.n = n;
        if (!spPack(s)) return false;
        s.bRows = spNeeded(s, s.xP, s.xN);
        s.inv.assign(N, 0.0);
        s.rhs.assign(N, 0.0);
        s.x.assign(N, 0.0);
        s.Mraw = r.Mraw.data();
        return spFactor(s, JdP, nullptr, nn, nn, r.bn.data());
    };
    if (!tryBuild(false) && !tryBuild(true)) return;
    std::size_t bRowsU = 0;
    for (int k : s.bRows) bRowsU += static_cast<std::size_t>(s.uOff[static_cast<std::size_t>(k) + 1] - s.uOff[static_cast<std::size_t>(k)]);
    // Live operations per factorisation + two steps (matching sparseWanted's dense estimate: 1 factor + 2 steps).
    s.cost = static_cast<double>(s.oQ.size() + 2 * s.dIdx.size() + s.stPos.size()) + 0.25 * static_cast<double>(s.snap.size()) +
             2.0 * static_cast<double>(s.lIdx.size() + bRowsU + s.bRows.size() + 2 * static_cast<std::size_t>(nn));
    const double nd = nn, denseFull = nd * nd * nd / 3.0 + 4.0 * nd * nd;
    if (!(kEnvSpFullRatio * s.cost < denseFull)) return;
    bool anyWanted = false;
    for (int a = 1; a <= nn && !anyWanted; ++a) {
        const double ad = a;
        const double dense = a < nn ? ad * ad * ad / 3.0 + 4.0 * ad * ad + 2.0 * nd * ad : denseFull;
        anyWanted = kSpWeight * s.cost < dense;
    }
    if (!anyWanted) return;
    if (!kEnvNoCodegen && opt_.codegen) {
        std::uint64_t h1 = 0, h2 = 0;
        luSignature(s, r.bs.data(), r.bn.data(), nn, h1, h2);
        s.cg = cg::findLu(h1, h2);
        if (kEnvCgDiag) std::fprintf(stderr, "codegen lu: group %zu (n %d, nn %d, T %.4g us) signature %016llx%016llx: %s\n", gi, n, nn, 1e6 * groups_[gi].T,
                                     static_cast<unsigned long long>(h1), static_cast<unsigned long long>(h2), s.cg ? "kernel" : "interpreter");
    }
    // verify on the representative matrix (exercises s.cg->factor when bound)
    s.ok = spFactor(s, JdP, nullptr, nn, nn, r.bn.data());
    if (std::getenv("CE_SPINFO"))
        std::fprintf(stderr, "sparse group %zu: %d unknowns, %d ports, ok %d, pattern %zu (M %zu, stamps %zu), L %zu U %zu, cost %.0f (dense full %.0f)\n", gi, n, nn, s.ok ? 1 : 0, s.pat.size(),
                     s.mPos.size(), s.stPos.size(), s.lIdx.size(), s.uIdx.size(), s.cost, double(nn) * nn * nn / 3.0 + 4.0 * nn * nn);
}

// The structure a SparseLin's per-sample solves depend on (sizes, R's column pattern, L / U fill, back-substitution rows,
// the port and output difference maps), as two independent 64-bit hashes: the key of a generated kernel (Codegen.hpp).
static void linSignature(const SparseLin& L, const int (&sz)[6], std::uint64_t& h1, std::uint64_t& h2) { // sz: ns nu nn nuc ny W
    h1 = 1469598103934665603ull; // FNV-1a
    h2 = 0x6a09e667f3bcc909ull;  // splitmix-style
    auto put = [&](std::int64_t v) {
        const auto u = static_cast<std::uint64_t>(v);
        for (int b = 0; b < 8; ++b) h1 = (h1 ^ ((u >> (8 * b)) & 0xffu)) * 1099511628211ull;
        std::uint64_t z = h2 + u + 0x9E3779B97F4A7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        h2 = z ^ (z >> 31);
    };
    auto putv = [&](const std::vector<int>& v) {
        put(-1 - static_cast<std::int64_t>(v.size()));
        for (int x : v) put(x);
    };
    put(L.lu.n);
    for (int x : sz) put(x);
    for (const auto* v : {&L.lu.lOff, &L.lu.lIdx, &L.lu.uOff, &L.lu.uIdx, &L.cOff, &L.cRow, &L.bPorts, &L.bOut, &L.pA, &L.pB, &L.pJA, &L.pJB, &L.oA,
                          &L.oB, &L.oJA, &L.oJB})
        putv(*v);
}

// Sparse linear update (SparseLin) for group gi: pivot order of M (threshold Markowitz on its current transient values),
// R by column, the output / port difference maps; on when kEnvLinWeight * its operations beat the dense products. Control thread.
void Circuit::buildSparseLin(std::size_t gi) {
    auto& r = *rt_[gi];
    if (!r.sl || r.wdf || r.dcMode || kEnvDenseLin) return;
    SparseLin& L = *r.sl;
    L.on = L.valid = false;
    L.cg = nullptr;
    L.cgOk = false;
    const auto& g = groups_[gi];
    const int n = r.n, W = r.W, ns = r.ns, nu = r.nu, nn = r.nn;
    const auto N = static_cast<std::size_t>(n);
    std::vector<double> Wm(r.Mraw);
    std::vector<std::uint8_t> S(N * N);
    for (std::size_t q = 0; q < N * N; ++q) S[q] = Wm[q] != 0.0;
    SparseLu& s = L.lu;
    s.mStruct = S;
    std::vector<int> rowPos, colPos;
    double flops = 0;
    if (!spOrder(s, n, S, Wm, rowPos, colPos, flops, spDepthMode(nn))) return;
    s.mPos.clear();
    s.mSrc.clear();
    for (std::size_t q = 0; q < N * N; ++q)
        if (s.mStruct[q]) {
            s.mPos.push_back(rowPos[q / N] * n + colPos[q % N]);
            s.mSrc.push_back(static_cast<int>(q));
        }
    s.stOff.assign(1, 0);
    s.stPos.clear();
    s.stSign.clear();
    s.n = n;
    if (!spPack(s)) return;
    s.inv.assign(N, 0.0);
    s.rhs.assign(N, 0.0);
    s.x.assign(N, 0.0);
    s.Mraw = r.Mraw.data();
    // R by column (its pattern now; a refresh that stamps outside it turns the group dense)
    const auto WW = static_cast<std::size_t>(W);
    L.rStruct.assign(N * WW, 0);
    L.cOff.assign(1, 0);
    L.cRow.clear();
    L.cIdx.clear();
    for (int c = 0; c < W; ++c) {
        for (int i = 0; i < n; ++i) {
            const auto q = static_cast<std::size_t>(i) * WW + static_cast<std::size_t>(c);
            if (r.R[q] == 0.0) continue;
            L.rStruct[q] = 1;
            L.cRow.push_back(rowPos[static_cast<std::size_t>(i)]);
            L.cIdx.push_back(static_cast<int>(q));
        }
        L.cOff.push_back(static_cast<int>(L.cRow.size()));
    }
    // a node's voltage: a permuted column of x (unknown) or an input of w
    auto col = [&](int node) { return node > 0 && r.unkOf[static_cast<std::size_t>(node)] >= 0 ? colPos[static_cast<std::size_t>(r.unkOf[static_cast<std::size_t>(node)])] : -1; };
    auto inp = [&](int node) { return node > 0 && r.unkOf[static_cast<std::size_t>(node)] < 0 ? r.inOf[static_cast<std::size_t>(node)] : -1; };
    L.oA.clear(), L.oB.clear(), L.oJA.clear(), L.oJB.clear();
    auto addOut = [&](int a, int b) {
        L.oA.push_back(col(a));
        L.oJA.push_back(inp(a));
        L.oB.push_back(col(b));
        L.oJB.push_back(inp(b));
    };
    for (int e = 0; e < r.ny; ++e) addOut(g.exports[static_cast<std::size_t>(e)], 0);
    for (int k = 0; k < ns; ++k) {
        const auto& p = prims_[static_cast<std::size_t>(g.states[static_cast<std::size_t>(k)])];
        addOut(p.a, p.b);
    }
    L.pA.clear(), L.pB.clear(), L.pJA.clear(), L.pJB.clear();
    for (int di : g.devices) {
        const auto& d = devices_[static_cast<std::size_t>(di)];
        for (int k = 0; k < d.nports; ++k) {
            L.pA.push_back(col(d.port[k].cp));
            L.pJA.push_back(inp(d.port[k].cp));
            L.pB.push_back(col(d.port[k].cn));
            L.pJB.push_back(inp(d.port[k].cn));
        }
    }
    L.bPorts = spNeeded(s, L.pA, L.pB);
    L.bOut = spNeeded(s, L.oA, L.oB);
    auto uOps = [&](const std::vector<int>& rows) {
        std::size_t u = rows.size();
        for (int k : rows) u += static_cast<std::size_t>(s.uOff[static_cast<std::size_t>(k) + 1] - s.uOff[static_cast<std::size_t>(k)]);
        return static_cast<double>(u);
    };
    const int sw = ns + nu;
    const double rPorts = static_cast<double>(L.cOff[static_cast<std::size_t>(sw)] + (L.cOff[static_cast<std::size_t>(W)] - L.cOff[static_cast<std::size_t>(sw + nn)]));
    const double rOut = static_cast<double>(kEnvNoBigExact ? L.cOff[static_cast<std::size_t>(W)] : L.cOff[static_cast<std::size_t>(W)] - L.cOff[static_cast<std::size_t>(sw)]);
    const double lOps = static_cast<double>(s.lIdx.size());
    const double sparseOps = nn > 0 ? (rPorts + lOps + uOps(L.bPorts) + 2.0 * nn) + (rOut + lOps + uOps(L.bOut) + 2.0 * (r.ny + ns))
                                    : static_cast<double>(L.cOff[static_cast<std::size_t>(W)]) + lOps + uOps(L.bOut) + 2.0 * (r.ny + ns);
    const std::size_t outRows = !kEnvNoPortStates && !r.pdRow.empty() ? r.hcOut.size() : static_cast<std::size_t>(r.ny + ns);
    const double denseOps = static_cast<double>(outRows) * W + static_cast<double>(nn) * (ns + nu + r.nuc);
    if (!(kEnvLinWeight * sparseOps < denseOps)) return;
    L.on = true;
    L.lVal.assign(s.lIdx.size(), 0.0);
    L.uVal.assign(s.uIdx.size(), 0.0);
    L.rVal.assign(L.cIdx.size(), 0.0);
    L.bPart.assign(N, 0.0); // sized here, not on the audio thread
    if (!kEnvNoCodegen && opt_.codegen) { // a kernel generated for exactly this structure (Codegen.hpp, docs §10.44)
        std::uint64_t h1 = 0, h2 = 0;
        const int sz[6] = {r.ns, r.nu, r.nn, r.nuc, r.ny, r.W};
        linSignature(L, sz, h1, h2);
        L.cg = cg::findLin(h1, h2);
        if (kEnvCgDiag) std::fprintf(stderr, "codegen: group %zu (n %d, T %.4g us) signature %016llx%016llx: %s\n", gi, n, 1e6 * groups_[gi].T,
                                     static_cast<unsigned long long>(h1), static_cast<unsigned long long>(h2), L.cg ? "kernel" : "interpreter");
    }
    L.valid = spFactor(s, nullptr, nullptr, 0, 0, nullptr) && L.pack(r.R.data());
    if (std::getenv("CE_SPINFO"))
        std::fprintf(stderr, "sparse linear group %zu: n %d, W %d, L %zu U %zu, R %zu: %.0f vs dense %.0f ops, valid %d\n", gi, n, W, s.lIdx.size(), s.uIdx.size(), L.cRow.size(),
                     sparseOps, denseOps, L.valid ? 1 : 0);
}

// The substep runtimes (T/2 and T/4, trapezoidal and backward Euler: step halving and LTE control) get their own sparse
// Newton step and sparse linear update, on the same terms as the full step. Each is brought up to date first; its pivot
// order is chosen on its own matrix, with the devices at the full step's operating point. Control thread.
void Circuit::buildSubSparse(std::size_t gi) {
    if (!rt_[gi] || (!rt_[gi]->sp && !rt_[gi]->sl) || rt_[gi]->wdf || rt_[gi]->dcMode) return;
    const int nLv = !rt_[gi]->tb && groups_[gi].evos >= 3 ? 3 : 2; // T/8 for 8-substep events (docs §10.62)
    for (int lv = 0; lv < nLv; ++lv)
        for (int be = 0; be < 2; ++be) {
            const int m = 2 << lv;
            GroupRt* h = subRuntime(gi, m, be != 0); // refreshed here if stale
            if (!h) continue;
            auto& slot = be ? rtBe_[gi][static_cast<std::size_t>(lv)] : (lv == 0 ? rtHalf_[gi] : rtTr_[gi][static_cast<std::size_t>(lv)]);
            std::copy(rt_[gi]->vp.begin(), rt_[gi]->vp.end(), h->vp.begin()); // every substep sets its own start
            std::swap(rt_[gi], slot);
            buildSparse(gi);
            buildSparseLin(gi);
            std::swap(rt_[gi], slot);
        }
    for (int M = 1; M <= 2 && rt_[gi]->tb; ++M) { // TR-BDF2 runtimes (docs §10.47)
        GroupRt* h = tbRuntime(gi, M);
        if (!h) continue;
        auto& slot = M == 1 ? rtTr_[gi][2] : rtBe_[gi][2];
        std::copy(rt_[gi]->vp.begin(), rt_[gi]->vp.end(), h->vp.begin());
        std::swap(rt_[gi], slot);
        buildSparse(gi);
        buildSparseLin(gi);
        std::swap(rt_[gi], slot);
    }
}

std::vector<cg::LinPattern> Circuit::codegenLinPatterns() const {
    std::vector<cg::LinPattern> out;
    auto add = [&](const GroupRt* rp, std::size_t gi, const char* what) {
        if (!rp || !rp->sl || !rp->sl->on || !rp->sl->valid) return;
        const GroupRt& r = *rp;
        const SparseLin& L = *r.sl;
        cg::LinPattern q;
        const int sz[6] = {r.ns, r.nu, r.nn, r.nuc, r.ny, r.W};
        linSignature(L, sz, q.h1, q.h2);
        q.n = L.lu.n, q.ns = r.ns, q.nu = r.nu, q.nn = r.nn, q.nuc = r.nuc, q.ny = r.ny, q.W = r.W;
        q.lOff = L.lu.lOff, q.lIdx = L.lu.lIdx, q.uOff = L.lu.uOff, q.uIdx = L.lu.uIdx, q.cOff = L.cOff, q.cRow = L.cRow;
        q.bPorts = L.bPorts, q.bOut = L.bOut;
        q.pA = L.pA, q.pB = L.pB, q.pJA = L.pJA, q.pJB = L.pJB, q.oA = L.oA, q.oB = L.oB, q.oJA = L.oJA, q.oJB = L.oJB;
        q.rVal = L.rVal;
        q.where = "group " + std::to_string(gi) + " " + what + ", T " + std::to_string(1e6 * groups_[gi].T) + " us";
        out.push_back(std::move(q));
    };
    for (std::size_t gi = 0; gi < rt_.size(); ++gi) {
        add(rt_[gi].get(), gi, "full step");
        if (gi < rtHalf_.size()) add(rtHalf_[gi].get(), gi, "T/2 trapezoidal");
        if (gi < rtTr_.size()) add(rtTr_[gi][1].get(), gi, "T/4 trapezoidal");
        if (gi < rtBe_.size()) {
            add(rtBe_[gi][0].get(), gi, "T/2 start");
            add(rtBe_[gi][1].get(), gi, "T/4 start");
            add(rtTr_[gi][2].get(), gi, "TR-BDF2 gT"); // docs §10.47 (null unless TR-BDF2 is on)
            add(rtBe_[gi][2].get(), gi, "TR-BDF2 gT/2");
        }
    }
    return out;
}
std::vector<cg::LuPattern> Circuit::codegenLuPatterns() const {
    std::vector<cg::LuPattern> out;
    auto add = [&](const GroupRt* rp, std::size_t gi, const char* what) {
        if (!rp || !rp->sp || !rp->sp->ok) return;
        const GroupRt& r = *rp;
        const SparseLu& s = *r.sp;
        cg::LuPattern q;
        luSignature(s, r.bs.data(), r.bn.data(), r.nn, q.h1, q.h2);
        q.n = s.n, q.nn = r.nn;
        q.bs.assign(r.bs.begin(), r.bs.begin() + r.nn);
        q.bn.assign(r.bn.begin(), r.bn.begin() + r.nn);
        q.lOff = s.lOff, q.lIdx = s.lIdx, q.uOff = s.uOff, q.uIdx = s.uIdx;
        q.stOff = s.stOff, q.stPos = s.stPos;
        q.stSign.resize(s.stSign.size());
        for (std::size_t i = 0; i < s.stSign.size(); ++i) q.stSign[i] = s.stSign[i] > 0 ? 1 : -1;
        q.kFl = s.kFl;
        q.dOff = s.dOff, q.dIdx = s.dIdx, q.rOff = s.rOff, q.rE = s.rE, q.oOff = s.oOff, q.oQ = s.oQ, q.oU = s.oU;
        q.bRows = s.bRows, q.pFrom = s.pFrom, q.pTo = s.pTo, q.xP = s.xP, q.xN = s.xN;
        q.where = "group " + std::to_string(gi) + " " + what + ", T " + std::to_string(1e6 * groups_[gi].T) + " us";
        out.push_back(std::move(q));
    };
    for (std::size_t gi = 0; gi < rt_.size(); ++gi) {
        add(rt_[gi].get(), gi, "full step");
        if (gi < rtHalf_.size()) add(rtHalf_[gi].get(), gi, "T/2 trapezoidal");
        if (gi < rtTr_.size()) add(rtTr_[gi][1].get(), gi, "T/4 trapezoidal");
        if (gi < rtBe_.size()) {
            add(rtBe_[gi][0].get(), gi, "T/2 start");
            add(rtBe_[gi][1].get(), gi, "T/4 start");
            add(rtTr_[gi][2].get(), gi, "TR-BDF2 gT"); // docs §10.47 (null unless TR-BDF2 is on)
            add(rtBe_[gi][2].get(), gi, "TR-BDF2 gT/2");
        }
    }
    return out;
}
std::vector<cg::NewtonPattern> Circuit::codegenNewtonPatterns() const {
    std::vector<cg::NewtonPattern> out;
    auto add = [&](const GroupRt* rp, std::size_t gi, const char* what) {
        if (!rp || rp->wdf || rp->nn <= 0 || groups_[gi].solver == SolverKind::Wdf) return;
        const GroupRt& r = *rp;
        cg::NewtonPattern q;
        const std::string tail = std::string(" ") + what + ", T " + std::to_string(1e6 * groups_[gi].T) + " us";
        if (newtonPattern(r.dev, r.lim.data(), r.nn, q)) {
            q.where = "group " + std::to_string(gi) + tail;
            out.push_back(q);
        }
        if (r.lazyClamps && r.red.size() <= 12 && newtonPattern(r.devR, r.limR.data(), static_cast<int>(r.red.size()), q)) {
            q.where = "group " + std::to_string(gi) + " clamp-free" + tail;
            out.push_back(q);
        }
    };
    for (std::size_t gi = 0; gi < rt_.size(); ++gi) {
        add(rt_[gi].get(), gi, "full step");
        if (gi < rtHalf_.size()) add(rtHalf_[gi].get(), gi, "T/2 trapezoidal");
        if (gi < rtTr_.size()) add(rtTr_[gi][1].get(), gi, "T/4 trapezoidal");
        if (gi < rtBe_.size()) {
            add(rtBe_[gi][0].get(), gi, "T/2 start");
            add(rtBe_[gi][1].get(), gi, "T/4 start");
            add(rtTr_[gi][2].get(), gi, "TR-BDF2 gT"); // docs §10.47 (null unless TR-BDF2 is on)
            add(rtBe_[gi][2].get(), gi, "TR-BDF2 gT/2");
        }
    }
    return out;
}
void Circuit::codegenStats(int& bound, int& total) const noexcept {
    bound = total = 0;
    auto add = [&](const GroupRt* rp) {
        if (!rp) return;
        bool nkOk = !rp->wdf && rp->nn > 0 && rp->nn <= kMaxNlPorts;
        for (const NlDevice* d : rp->dev) nkOk = nkOk && d->nports >= 1 && d->nports <= 3;
        if (nkOk) { // the specialised Newton kernels (docs §10.45)
            ++total;
            bound += rp->nkFull != nullptr;
            if (rp->lazyClamps && rp->red.size() <= 12) {
                ++total;
                bound += rp->nkRed != nullptr;
            }
        }
        if (rp->sl && rp->sl->on) {
            ++total;
            bound += rp->sl->cg != nullptr;
        }
        if (rp->sp && rp->sp->ok) {
            ++total;
            bound += rp->sp->cg != nullptr;
        }
    };
    for (std::size_t gi = 0; gi < rt_.size(); ++gi) {
        add(rt_[gi].get());
        if (gi < rtHalf_.size()) add(rtHalf_[gi].get());
        if (gi < rtTr_.size()) add(rtTr_[gi][1].get());
        if (gi < rtBe_.size()) add(rtBe_[gi][0].get()), add(rtBe_[gi][1].get()), add(rtTr_[gi][2].get()), add(rtBe_[gi][2].get());
    }
}

void Circuit::linPorts(GroupRt& r, const double* w, double* p) noexcept {
    const int ns = r.ns, nu = r.nu, sw = ns + nu;
    if (r.sl && r.sl->valid) {
        SparseLin& L = *r.sl;
        if (L.cgOk) { // the generated kernel for this structure: the same operations below, straight-line (docs §10.44)
            double* bp = kEnvNoBigExact ? nullptr : L.bPart.data();
            L.cg->ports(L.lVal.data(), L.uVal.data(), L.rVal.data(), L.lu.inv.data(), w, p, r.noiseOn ? r.dP.data() : nullptr, bp);
            L.bPartW = bp ? w : nullptr;
            return;
        }
        double* __restrict b = L.lu.rhs.data();
        double* __restrict x = L.lu.x.data();
        const double* __restrict R = L.rVal.data();
        const int* __restrict cOff = L.cOff.data();
        const int* __restrict cRow = L.cRow.data();
        for (int i = 0; i < L.lu.n; ++i) b[i] = 0.0;
        auto addCol = [&](int c) {
            const double wc = w[c];
            if (wc == 0.0) return;
            for (int e = cOff[c]; e < cOff[c + 1]; ++e) b[cRow[e]] += R[e] * wc;
        };
        for (int c = 0; c < sw; ++c) addCol(c);
        if (!kEnvNoBigExact) {
            L.bPart.resize(static_cast<std::size_t>(L.lu.n));
            std::copy(b, b + L.lu.n, L.bPart.data());
            L.bPartW = w;
        }
        for (int c = 0; c < r.nuc; ++c) addCol(sw + r.nn + c);
        spSolvePacked(L.lu, L.lVal.data(), L.uVal.data(), b, x, L.bPorts);
        for (int k = 0; k < r.nn; ++k) {
            const auto K = static_cast<std::size_t>(k);
            double v = 0.0;
            if (L.pA[K] >= 0) v += x[L.pA[K]];
            if (L.pJA[K] >= 0) v += w[ns + L.pJA[K]];
            if (L.pB[K] >= 0) v -= x[L.pB[K]];
            if (L.pJB[K] >= 0) v -= w[ns + L.pJB[K]];
            p[k] = r.noiseOn ? v + r.dP[K] : v;
        }
        return;
    }
    if (r.noiseOn) gemvB(r.Dp.data(), r.nn, sw, w, p, r.dP.data()); // + the held noise (thermal + K·shot)
    else gemv(r.Dp.data(), r.nn, sw, w, p);
    if (r.nuc > 0)
        for (int k = 0; k < r.nn; ++k) {
            double s = 0;
            for (int c = 0; c < r.nuc; ++c) s += r.Dc[static_cast<std::size_t>(k * r.nuc + c)] * w[sw + r.nn + c];
            p[k] += s;
        }
}

void Circuit::linOut(GroupRt& r, const double* w, double* out) noexcept {
    const int ns = r.ns, W = r.W;
    if (r.sl && r.sl->valid) {
        SparseLin& L = *r.sl;
        if (L.cgOk) { // generated kernel (see linPorts)
            L.cg->out(L.lVal.data(), L.uVal.data(), L.rVal.data(), L.lu.inv.data(), w, out, r.noiseOn ? r.dOut.data() : nullptr,
                      L.bPartW == w ? L.bPart.data() : nullptr);
            L.bPartW = nullptr;
            return;
        }
        double* __restrict b = L.lu.rhs.data();
        double* __restrict x = L.lu.x.data();
        const double* __restrict R = L.rVal.data();
        const int* __restrict cOff = L.cOff.data();
        const int* __restrict cRow = L.cRow.data();
        int c0 = 0;
        if (L.bPartW == w) { // linPorts summed the same state and input columns, in the same order, this sample
            std::copy(L.bPart.begin(), L.bPart.begin() + L.lu.n, b);
            c0 = ns + r.nu;
        } else {
            for (int i = 0; i < L.lu.n; ++i) b[i] = 0.0;
        }
        L.bPartW = nullptr;
        for (int c = c0; c < W; ++c) {
            const double wc = w[c];
            if (wc == 0.0) continue;
            for (int e = cOff[c]; e < cOff[c + 1]; ++e) b[cRow[e]] += R[e] * wc;
        }
        spSolvePacked(L.lu, L.lVal.data(), L.uVal.data(), b, x, L.bOut);
        const int rows = r.ny + ns;
        for (int e = 0; e < rows; ++e) {
            const auto E = static_cast<std::size_t>(e);
            double v = 0.0;
            if (L.oA[E] >= 0) v += x[L.oA[E]];
            if (L.oJA[E] >= 0) v += w[ns + L.oJA[E]];
            if (L.oB[E] >= 0) v -= x[L.oB[E]];
            if (L.oJB[E] >= 0) v -= w[ns + L.oJB[E]];
            out[e] = r.noiseOn ? v + r.dOut[E] : v;
        }
        return;
    }
    if (r.pdOk && !r.pdRow.empty()) { // port-defined rows from the accepted solution, the others from Hc
        const int rows = static_cast<int>(r.hcOut.size());
        double* __restrict y = r.hcY.data();
        const int* __restrict at = r.hcOut.data();
        gemv(r.Hc.data(), rows, W, w, y);
        if (r.noiseOn)
            for (int c = 0; c < rows; ++c) out[at[c]] = y[c] + r.dOut[static_cast<std::size_t>(at[c])];
        else
            for (int c = 0; c < rows; ++c) out[at[c]] = y[c];
        const double* __restrict vp = r.vp.data();
        for (std::size_t q = 0; q < r.pdRow.size(); ++q) {
            const int a = r.pdA[q], b = r.pdB[q];
            out[r.pdRow[q]] = (a >= 0 ? vp[a] : 0.0) - (b >= 0 ? vp[b] : 0.0);
        }
        return;
    }
    if (r.noiseOn) gemvB(r.H.data(), r.ny + ns, W, w, out, r.dOut.data());
    else gemv(r.H.data(), r.ny + ns, W, w, out);
}

bool Circuit::reset() {
    // CompileOptions::openSwitches: switches moved since the compile need another partition (and a contact compiled
    // open could not close at all): compile again at the current positions (which ends in this reset).
    if (!recompiling_ && topologyStale()) return recompileTopology();
    bool ok = true;
    phase_ = 0;
    tick_ = 0;
    for (auto& h : cutHold_) h.clear();
    for (auto& b : blocks_) {
        b.tank->reset();
        b.y = 0;
    }
    for (auto& t : tapes_) t.loop->reset(t.ctl >= 0 ? ctlCur_[static_cast<std::size_t>(t.ctl)] : 1.0);
    for (std::size_t i = 0; i < V_.size(); ++i)
        V_[i] = nodeKind_[i] == kKnown ? knownValue_[i] : 0.0;
    V_[static_cast<std::size_t>(inputNode_)] = 0.0;
    if (inputNodeR_ > 0) V_[static_cast<std::size_t>(inputNodeR_)] = 0.0;
    for (auto& s : slots_) s.value = slotValue(s);
    if (!cutI_.empty()) {
        std::fill(cutI_.begin(), cutI_.end(), 0.0);
        dcCutIterations(nullptr);
    }
    for (std::size_t gi = 0; gi < groups_.size(); ++gi) {
        if (!solveGroupDc(gi)) {
            ok = false;
            if (debugFail_) std::fprintf(stderr, "DC failed in group %zu (%d NL ports)\n", gi, rt_[gi]->nn);
        }
        initGroupStates(gi);
    }
    if (hasDelayCuts_) // a delay cut's loads were solved with the owner's old value: sweep until the DC agrees
        for (int sweep = 0; sweep < 12; ++sweep) {
            const std::vector<double> before = V_;
            if (!cutI_.empty()) dcCutIterations(nullptr);
            for (std::size_t gi = 0; gi < groups_.size(); ++gi) {
                if (!solveGroupDc(gi)) ok = false;
                initGroupStates(gi);
            }
            double worst = 0;
            for (std::size_t i = 0; i < V_.size(); ++i) worst = std::max(worst, std::abs(V_[i] - before[i]));
            if (worst < 1e-9) break;
        }
    if (!bbds_.empty() && !settleBbdDc()) ok = false;
    if (!logic_.empty() && !settleLogicDc()) ok = false;
    for (std::size_t gi = 0; gi < groups_.size() && !dcOnly_; ++gi) { // (dcOnly_: §10.53)
        buildSparse(gi);
        buildSparseLin(gi);
        buildSubSparse(gi);
    }
    groupDirty_.assign(groups_.size(), 0);
    subCounter_ = 0;
    primeConverters();
    // Transformer-fed mains supply or a requested cold start (CompileOptions::settleMs / coldMs, docs §3.10): the state
    // is built from a cold circuit instead of from the DC point above.
    const bool running = coldMs_ >= 0 || (settleMs_ > 0 && mainsCore());
    if (running) {
        coldStart();
        if (coldMs_ >= 0) {
            const double step = multirate_ ? 2.0 * T_ : T_;
            runMains(std::lround(coldMs_ * 1e-3 / step));
        } else if (!pss_.empty()) {
            // a later reset (a control set after compile): the last periodic state is the better start
            std::vector<std::pair<double*, std::size_t>> arr;
            stateArrays(arr);
            std::size_t at = 0;
            for (auto& [p, n] : arr) {
                if (at + n > pss_.size()) break;
                std::copy(pss_.begin() + static_cast<long>(at), pss_.begin() + static_cast<long>(at + n), p);
                at += n;
            }
            primeConverters();
            settlePeriodic();
        } else
            settlePeriodic();
        diag_ = Diagnostics{};
    } else if (settleMs_ > 0 && !acSrc_.empty()) {
        // Mains-fed supply (see CompileOptions::settleMs). 1) Run the mains into the circuit until the rails
        // reach their running level, averaging each supply-cut node over the last mains period. 2) With the
        // supply groups held where they are and their rails pinned at those averages, re-solve every other
        // group's operating point (coupling caps, cathodes, bias) so nothing starts from the crest-B+ charge.
        // 3) A short run reconciles the two.
        const double step = multirate_ ? 2.0 * T_ : T_;
        double fMains = 0;
        for (const auto& a : acSrc_) {
            const double f = a.w / (2.0 * M_PI);
            if (f >= 40.0) fMains = std::max(fMains, f);
        }
        const long nAvg = fMains > 0 ? std::max(1L, static_cast<long>(std::lround(1.0 / (fMains * step)))) : 1L;
        const long nMin = std::max(nAvg, static_cast<long>(settleMs_ * 1e-3 / step));
        const long nMax = std::max(nMin, static_cast<long>(2.0 / step));
        std::vector<double> avg(V_.size(), 0.0), prevAvg(V_.size(), -1e30), prevAvg2(V_.size(), -1e30);
        // Slow rails (docs §10.49). The per-period test below passes while a rail behind a long RC (a decoupling chain,
        // a bias or FX-loop rail: seconds) still moves less than its tolerance per period but volts in all, and a rail
        // inside a group is not among the ones it watches. Where the netlist's supply has an averaged circuit
        // (mainsAveragedNetlist, capacitors as its boundary), the run ends at nMin and settleSlowRails solves the rest of
        // the circuit at DC around the rectifier outputs' means; where that does not converge, the run goes on as before.
        Netlist slowNet;
        std::vector<std::string> slowRect, mainsSide;
        const bool slow = kEnvSlowSettle && fMains > 0 && mainsAveragedNetlist(net_, slowNet, slowRect, true, &mainsSide);
        std::vector<std::uint8_t> watch(supplyCut_.begin(), supplyCut_.end()); // the rails the run's tests read: the supply
        if (slow)                                                               // cuts, and with §10.49 the rectifier outputs
            for (const auto& r : slowRect)
                if (const int id = node(r); id > 0 && isExported(id)) watch[static_cast<std::size_t>(id)] = 1;
        long nRun = 0;        // samples run so far
        double lastDrift = 0; // the per-period test at the run's last window (> 1: not passed)
        double far = 0;       // the largest remaining distance of a watched rail (its means' geometric tail) over its tolerance
        auto runPeriods = [&](long from, long to, bool early) {
            std::fill(avg.begin(), avg.end(), 0.0);
            long inWin = 0;
            for (long i = from; i < to; ++i) {
                nRun = i + 1;
                multirate_ ? processHost(0.0) : process(0.0);
                for (std::size_t nd = 0; nd < V_.size(); ++nd)
                    if (watch[nd]) avg[nd] += V_[nd];
                if (++inWin == nAvg) {
                    double drift = 0.0;
                    far = 0.0;
                    for (std::size_t nd = 0; nd < V_.size(); ++nd)
                        if (watch[nd]) {
                            const double cur = avg[nd] / static_cast<double>(nAvg);
                            const double tol = std::max(0.01, 1e-3 * std::abs(cur));
                            drift = std::max(drift, std::abs(cur - prevAvg[nd]) / tol);
                            if (early) { // the tail of a geometric sequence through the last three means (or far: not yet one)
                                const double d1 = prevAvg[nd] - prevAvg2[nd], d2 = cur - prevAvg[nd];
                                double tail = prevAvg2[nd] < -1e29 ? 1e30 : std::abs(d2) * 1e3;
                                if (prevAvg2[nd] >= -1e29 && std::abs(d2) < std::abs(d1)) tail = std::abs(d2) * std::abs(d2 / d1) / (1.0 - d2 / d1);
                                far = std::max(far, tail / tol);
                            }
                            prevAvg2[nd] = prevAvg[nd];
                            prevAvg[nd] = cur;
                        }
                    lastDrift = drift;
                    if (i + 1 >= nMin && (drift <= 1.0 || early || i + 1 + nAvg > nMax)) break;
                    std::fill(avg.begin(), avg.end(), 0.0);
                    inWin = 0;
                }
            }
        };
        runPeriods(0, nMax, slow);
        // Where the rails the run watches are still far from their running level, or where it watches none (a supply inside
        // one group: the Ecstasy), the averaged circuit follows; where they have arrived (the JC-120), nothing more.
        bool anyCut = false;
        for (std::size_t nd = 0; nd < V_.size(); ++nd) anyCut = anyCut || supplyCut_[nd];
        if (slow && (far > 1.0 || !anyCut)) {
            if (settleSlowRails(slowNet, slowRect, nAvg)) { // the supply means of the period that follows
                std::fill(avg.begin(), avg.end(), 0.0);
                for (long i = 0; i < nAvg; ++i) {
                    multirate_ ? processHost(0.0) : process(0.0);
                    for (std::size_t nd = 0; nd < V_.size(); ++nd)
                        if (supplyCut_[nd]) avg[nd] += V_[nd];
                }
            } else if (lastDrift > 1.0 && nRun < nMax) { // the run as before: on until the per-period test passes
                std::copy(supplyCut_.begin(), supplyCut_.end(), watch.begin()); // (on the supply cuts alone, as before)
                runPeriods(nRun, nMax, false);
            }
        }
        std::vector<std::uint8_t> hold(groups_.size(), 0);
        bool anyHeld = false;
        for (std::size_t gi = 0; gi < groups_.size(); ++gi)
            for (int u : groups_[gi].unknowns)
                if (supplyCut_[static_cast<std::size_t>(u)]) hold[gi] = 1, anyHeld = true;
        // (§10.49) A group on the mains side without a supply cut of its own (a bias rectifier on its own winding) keeps
        // its running state too: its DC point holds the winding at this instant's value (the crest, at the period boundary
        // of a 270° source) and charges its reservoir there, seconds from its running level.
        if (slow && anyHeld)
            for (std::size_t gi = 0; gi < groups_.size(); ++gi)
                for (int pi : groups_[gi].parts)
                    for (const auto& nd : net_.parts[static_cast<std::size_t>(pi)].nodes)
                        if (std::find(mainsSide.begin(), mainsSide.end(), nd) != mainsSide.end()) hold[gi] = 1;
        if (anyHeld) {
            const std::vector<double> held = V_;
            for (std::size_t nd = 0; nd < V_.size(); ++nd)
                if (supplyCut_[nd]) V_[nd] = avg[nd] / static_cast<double>(nAvg);
            for (const auto& a : acSrc_) V_[static_cast<std::size_t>(a.node)] = held[static_cast<std::size_t>(a.node)];
            for (std::size_t gi = 0; gi < groups_.size(); ++gi)
                for (int u : groups_[gi].unknowns)
                    if (!supplyCut_[static_cast<std::size_t>(u)]) V_[static_cast<std::size_t>(u)] = hold[gi] ? held[static_cast<std::size_t>(u)] : 0.0;
            // Signal-cut currents restart from zero like the first operating point: a mixing node held only by
            // gmin behind coupling caps converges from 0 at once but would creep for hundreds of iterations
            // from the transient's value.
            for (std::size_t nd = 0; nd < cutI_.size(); ++nd)
                if (!supplyCut_[nd]) cutI_[nd] = 0.0;
            for (auto& b : blocks_) {
                b.tank->reset();
                b.y = 0;
                V_[static_cast<std::size_t>(b.src)] = 0.0;
            }
            for (auto& t : tapes_) {
                t.loop->reset(t.ctl >= 0 ? ctlCur_[static_cast<std::size_t>(t.ctl)] : 1.0);
                for (int h = 0; h < 3; ++h)
                    if (t.src[h] >= 0) V_[static_cast<std::size_t>(t.src[h])] = 0.0;
            }
            dcCutIterations(&hold); // consumer currents into the rails become their DC values at the averages
            for (std::size_t gi = 0; gi < groups_.size(); ++gi)
                if (!hold[gi]) initGroupStates(gi);
            if (!bbds_.empty()) settleBbdDc(&hold);
            for (std::size_t nd = 0; nd < V_.size(); ++nd)
                if (supplyCut_[nd]) V_[nd] = held[nd];
            for (std::size_t gi = 0; gi < groups_.size(); ++gi)
                if (hold[gi])
                    for (int u : groups_[gi].unknowns) V_[static_cast<std::size_t>(u)] = held[static_cast<std::size_t>(u)];
            primeConverters();
            const long nTail = static_cast<long>(0.06 / step);
            for (long i = 0; i < nTail; ++i) multirate_ ? processHost(0.0) : process(0.0);
        }
        diag_ = Diagnostics{};
    }
    if (ok && !running) escapeUnstableBranches();
    nodalInfo(); // CE_NODALINFO (docs §10.51)
    return ok;
}

std::vector<std::pair<std::string, double>> Circuit::inductorCurrents() const {
    std::vector<std::pair<std::string, double>> out;
    for (std::size_t gi = 0; gi < groups_.size(); ++gi) {
        const auto& r = *rt_[gi];
        for (std::size_t k = 0; k < groups_[gi].states.size(); ++k) {
            const auto& p = prims_[static_cast<std::size_t>(groups_[gi].states[k])];
            if (p.kind != Prim::Ind || p.part < 0) continue;
            const auto& part = net_.parts[static_cast<std::size_t>(p.part)];
            std::string key;
            if (part.type == "L") key = part.ref;
            else if (part.type == "transformer") key = nodeNames_[static_cast<std::size_t>(p.b)]; // REF#l<k>
            else continue;
            // post-update states: trapezoidal s = G·v + i, backward Euler s = i (see substepGroup)
            out.push_back({key, p.be ? r.w[k] : r.w[k] - r.gc[k] * r.vcLast[k]});
        }
    }
    return out;
}

// ---- Transformer-fed mains supplies (docs §3.10) ----
bool Circuit::mainsCore() const noexcept {
    for (const auto& a : acSrc_)
        if (a.amp != 0.0 && a.w >= 2.0 * M_PI * 40.0)
            for (int e : windingEnds_)
                if (e == a.node) return true;
    return false;
}

// Everything the next sample depends on, array by array, in a fixed order: node voltages, cut currents, per group the
// reactive states and inputs (w), Newton warm starts, predictor and LTE histories, and the devices' memories.
void Circuit::stateArrays(std::vector<std::pair<double*, std::size_t>>& out) {
    out.clear();
    // The LTE history is a ring (pushLteHistory): put it in its canonical order (newest first) so the arrays read and
    // written here mean the same history whatever the ring position (control thread).
    for (auto& rp : rt_) {
        auto& r = *rp;
        if (r.xhHead == 0 || r.xh.empty()) continue;
        const auto ns = r.xh.size() / 5;
        std::rotate(r.xh.begin(), r.xh.begin() + static_cast<long>(static_cast<std::size_t>(r.xhHead) * ns), r.xh.end());
        r.xhHead = 0;
    }
    auto add = [&](std::vector<double>& v) {
        if (!v.empty()) out.push_back({v.data(), v.size()});
    };
    add(V_);
    add(cutI_);
    add(cutAcc_);
    for (auto& rp : rt_) {
        auto& r = *rp;
        for (auto* v : {&r.w, &r.vp, &r.vpPrev, &r.vpPrev2, &r.pPrev, &r.vcLast, &r.uPrev, &r.xh, &r.curPrev1, &r.curPrev2}) add(*v);
    }
    for (auto& d : devices_) out.push_back({d.hs, std::size(d.hs)});
}

std::uint64_t Circuit::stateDigest() {
    std::uint64_t h = 0xcbf29ce484222325ull;
    auto mix = [&](std::uint64_t x) {
        for (int b = 0; b < 8; ++b) h = (h ^ ((x >> (8 * b)) & 0xffu)) * 0x100000001b3ull;
    };
    std::vector<std::pair<double*, std::size_t>> arr;
    stateArrays(arr);
    for (const auto& [p, n] : arr) {
        mix(n);
        for (std::size_t i = 0; i < n; ++i) {
            std::uint64_t u;
            std::memcpy(&u, p + i, sizeof u);
            mix(u);
        }
    }
    for (const auto& rp : rt_)
        for (std::uint64_t s : rp->rng) mix(s);
    std::uint64_t ph;
    std::memcpy(&ph, &phase_, sizeof ph);
    mix(ph);
    mix(tick_);
    return h;
}

// The circuit at the instant it is switched on (ngspice `.tran … uic`): every free node, capacitor voltage, inductor
// current and core flux at zero, the sources at their t = 0 values, Newton started from the zero-current port voltages.
void Circuit::coldStart() noexcept {
    phase_ = 0;
    tick_ = 0;
    for (auto& h : cutHold_) h.clear();
    std::fill(cutI_.begin(), cutI_.end(), 0.0);
    std::fill(cutAcc_.begin(), cutAcc_.end(), 0.0);
    for (std::size_t i = 0; i < V_.size(); ++i) V_[i] = nodeKind_[i] == kKnown ? knownValue_[i] : 0.0;
    V_[static_cast<std::size_t>(inputNode_)] = 0.0;
    if (inputNodeR_ > 0) V_[static_cast<std::size_t>(inputNodeR_)] = 0.0;
    if (!acSrc_.empty()) advanceAc(0.0); // the AC sources at t = 0 (control-scaled ones included)
    for (std::size_t gi = 0; gi < groups_.size(); ++gi) {
        auto& r = *rt_[gi];
        std::fill(r.vcLast.begin(), r.vcLast.end(), 0.0);
        std::fill(r.vp.begin(), r.vp.end(), 0.0);
        for (int k = 0; k < r.nn; ++k) r.w[static_cast<std::size_t>(r.ns + r.nu + k)] = 0.0;
        initGroupStates(gi);
        r.vp = r.pPrev;
        // junctions start where SPICE starts them on a cold (uic) start: no further up the exponential than pnjlim
        // allows from 0 V (p alone, every current zero, put the SVT's bridge diodes 412 V forward at the mains crest,
        // and the first sample's Newton failed from there, and with it every later one)
        for (int k = 0; k < r.nn; ++k) {
            const double zero = 0.0;
            limitPredicted(1, &r.lim[static_cast<std::size_t>(k)], &r.limA[static_cast<std::size_t>(k)], &r.limB[static_cast<std::size_t>(k)],
                           &r.limC[static_cast<std::size_t>(k)], &zero, &r.vp[static_cast<std::size_t>(k)]);
        }
        r.vpPrev = r.vp;
        r.vpPrev2 = r.vp;
    }
    primeConverters();
}

void Circuit::runMains(long n) noexcept {
    for (long i = 0; i < n; ++i) multirate_ ? processHost(0.0) : process(0.0);
}

// Mains-fed settle, slow rails (docs §10.49; reset(), between its run and step 2): settlePeriodic's averaged operating
// point, for a supply fed straight from mains sources. hn is the netlist without its mains side (mainsAveragedNetlist with
// capacitors as its boundary), each rectifier output (rect) driven by a DC source. Each step measures every state's mean
// over one mains period, solves hn at DC with the rectifier outputs at their pins, and moves every state hn shares by
// (its DC value − its mean): the ripple stays, and the decoupling chains, filtered and bias rails and the consumers on
// them land at their running levels at once, whatever their time constants. Then 4 periods of the circuit, and the next
// pin of each rectifier output is the geometric tail of its last three period means (a reservoir straight on a bridge is
// there within a period; a bias rectifier behind 27 kΩ moves by seconds), and of its own last three pins once those
// decay monotonically. At most 12 steps, until no state moves by more than 1e-3 of the largest capacitor voltage and every
// rectifier output's mean is within max(10 mV, 1e-3) of its pin. WDF groups keep their state in the tree: it is read
// out before and written back after every use of the arrays. False when nothing usable came of it (no averaged circuit,
// no DC point, or steps that do not shrink: then the state before is restored).
bool Circuit::settleSlowRails(const Netlist& hn, const std::vector<std::string>& rect, long N) {
    const bool dbg = std::getenv("CE_PSS_DEBUG") != nullptr;
    const auto tStart = std::chrono::steady_clock::now();
    // (§10.52) The outcome depends on the circuit, not on where its continuous controls are (the stacked supplies of the
    // OR120 and SLO-100 MK II fail at every setting): a fallback seen before for this netlist (its values included),
    // partition, rate and options skips the averaged circuit and goes to the same exit as the failure, with nothing run.
    std::uint64_t memoKey = 0;
    int known = 0;
    if (kEnvSlowMemo) {
        memoKey = fnv(fnv(1469598103934665603ull, toJson(net_).dump()),
                      std::to_string(hash_) + "/" + std::to_string(N) + "/" + std::to_string(opt_.lteTol) + "/" + std::to_string(opt_.lteMaxPorts) + "/" +
                          std::to_string(opt_.lteStart) + (multirate_ ? "M" : "") + (ladder_ ? "L" : "") + (opt_.noise ? "n" : "") + (opt_.hum ? "h" : "") +
                          (opt_.smallSignal ? "s" : "") + (opt_.codegen ? "c" : "") + (opt_.openSwitches ? "o" : ""));
        std::lock_guard<std::mutex> lock(gSlowMemoMutex);
        if (const auto f = gSlowMemo.find(memoKey); f != gSlowMemo.end()) known = f->second;
    }
    auto remember = [&](int how) {
        if (!kEnvSlowMemo) return;
        std::lock_guard<std::mutex> lock(gSlowMemoMutex);
        gSlowMemo[memoKey] = how;
    };
    if (known == 1) {
        if (dbg) std::fprintf(stderr, "settleSlowRails: averaged circuit does not compile (remembered)\n");
        return false;
    }
    Circuit helper;
    helper.dcOnly_ = kEnvPssFast; // (§10.53) its DC points only
    CompileOptions ho = opt_;
    ho.settleMs = 0;
    ho.coldMs = -1;
    ho.settleFrom = nullptr;
    ho.noise = ho.hum = ho.bbdNoise = false;
    ho.probes.clear();
    ho.exportAllNodes = true; // adoptStateByName reads its node voltages
    ho.multirate = false;
    ho.partLevels.clear();
    std::string err;
    if (known == 0 && !helper.compile(hn, ho, err)) {
        if (dbg) std::fprintf(stderr, "settleSlowRails: averaged circuit: %s\n", err.c_str());
        remember(1);
        return false;
    }
    std::vector<std::pair<std::size_t, std::size_t>> pin; // helper source node, our node
    if (known == 0)
        for (const auto& r : rect) pin.push_back({static_cast<std::size_t>(helper.node(r)), static_cast<std::size_t>(node(r))});
    const std::size_t R = pin.size();
    std::vector<std::uint8_t> noiseWas(rt_.size());
    for (std::size_t gi = 0; gi < rt_.size(); ++gi) noiseWas[gi] = rt_[gi]->noiseOn, rt_[gi]->noiseOn = false;
    std::vector<std::pair<double*, std::size_t>> arr;
    stateArrays(arr);
    std::size_t dim = 0;
    for (const auto& a : arr) dim += a.second;
    auto syncWdf = [&] {
        for (std::size_t gi = 0; gi < rt_.size(); ++gi)
            if (rt_[gi]->wdf) rt_[gi]->wdf->stateTo(*this, groups_[gi], rt_[gi]->w.data(), rt_[gi]->vcLast.data());
    };
    auto gather = [&](std::vector<double>& x) {
        syncWdf();
        x.resize(dim);
        std::size_t at = 0;
        for (const auto& [p, n] : arr) std::copy(p, p + n, x.begin() + static_cast<long>(at)), at += n;
    };
    auto scatter = [&](const std::vector<double>& x) { // the moved state starts the predictor and the LTE history afresh
        std::size_t at = 0;                              // (a BDF2 start through a pre-move sample reads the move as a current)
        for (auto& [p, n] : arr) std::copy(x.begin() + static_cast<long>(at), x.begin() + static_cast<long>(at + n), p), at += n;
        for (std::size_t gi = 0; gi < rt_.size(); ++gi) {
            auto& r = *rt_[gi];
            if (r.wdf) r.wdf->resetFrom(*this, groups_[gi], r.w.data(), r.vcLast.data());
            r.predHist = 0;
            if (r.lteOn) {
                r.lteWarm = 0;
                r.subNext = r.subPrev = 1;
                for (int i = 0; i < 5; ++i) pushLteHistory(r);
            }
        }
        primeConverters();
    };
    // The measure of a step: the capacitor and inductor state voltages, but not a transformer's (an output transformer's
    // core rides along: its DC point and its running mean differ by a constant it keeps returning to).
    // Where step 2 follows (some group owns a supply cut), only those groups: step 2 re-solves the rest at DC anyway.
    std::vector<std::uint8_t> owner(groups_.size(), 0);
    bool anyOwner = false;
    for (std::size_t gi = 0; gi < groups_.size(); ++gi)
        for (int u : groups_[gi].unknowns)
            if (supplyCut_[static_cast<std::size_t>(u)]) owner[gi] = 1, anyOwner = true;
    std::vector<std::size_t> fit;
    {
        std::size_t at = 0;
        for (const auto& [p, n] : arr) {
            for (std::size_t gi = 0; gi < rt_.size(); ++gi)
                if (p == rt_[gi]->vcLast.data() && (!anyOwner || owner[gi]))
                    for (std::size_t k = 0; k < n; ++k) {
                        const int part = prims_[static_cast<std::size_t>(groups_[gi].states[k])].part;
                        if (part < 0 || net_.parts[static_cast<std::size_t>(part)].type != "transformer") fit.push_back(at + k);
                    }
            at += n;
        }
    }
    // The group holding the unit's LFO (Netlist::lfoNode) keeps its running state: its DC point is the oscillator's
    // unstable equilibrium, where a slow tremolo would take seconds to start again (the Supro's speed 0).
    std::vector<std::uint8_t> frozen(dim, 0);
    if (const int lfo = net_.lfoNode.empty() ? -1 : node(net_.lfoNode); lfo > 0)
        for (std::size_t gi = 0; gi < groups_.size(); ++gi) {
            const auto& un = groups_[gi].unknowns;
            if (std::find(un.begin(), un.end(), lfo) == un.end()) continue;
            for (int u : un) frozen[static_cast<std::size_t>(u)] = 1; // V_ leads the state vector
            const auto& r = *rt_[gi];
            std::size_t at = 0;
            for (const auto& [p, n] : arr) {
                for (const auto* v : {&r.w, &r.vp, &r.vpPrev, &r.vpPrev2, &r.pPrev, &r.vcLast, &r.uPrev, &r.xh, &r.curPrev1, &r.curPrev2})
                    if (p == v->data()) std::fill(frozen.begin() + static_cast<long>(at), frozen.begin() + static_cast<long>(at + n), 1);
                at += n;
            }
        }
    auto adoptKeepingBlocks = [&](const Circuit& o) { // (as in settlePeriodic: this circuit keeps its own blocks)
        std::vector<std::shared_ptr<SpringTank>> tanks;
        std::vector<std::shared_ptr<BbdLine>> lines;
        std::vector<std::shared_ptr<TapeLoop>> loops;
        for (const auto& b : blocks_) tanks.push_back(b.tank);
        for (const auto& b : bbds_) lines.push_back(b.line);
        for (const auto& t : tapes_) loops.push_back(t.loop);
        adoptStateByName(o);
        for (std::size_t b = 0; b < blocks_.size(); ++b) blocks_[b].tank = tanks[b];
        for (std::size_t b = 0; b < bbds_.size(); ++b) bbds_[b].line = lines[b];
        for (std::size_t t = 0; t < tapes_.size(); ++t) tapes_[t].loop = loops[t];
    };
    std::vector<double> x0, xm(dim), xb, xa;
    gather(x0);
    const double phase0 = phase_;
    const std::uint64_t tick0 = tick_;
    double vmax = 1.0;
    for (std::size_t i : fit) vmax = std::max(vmax, std::abs(x0[i]));
    std::vector<double> pinNow(R, 0.0);
    std::vector<std::array<double, 3>> perMean(R), pinHist(R);
    bool converged = false;
    double shiftMax = 0, shift = 0, lastShift = 0;
    int it = 0;
    for (; known == 0 && it < 12 && !converged; ++it) {
        std::fill(xm.begin(), xm.end(), 0.0);
        for (long i = 0; i < N; ++i) {
            multirate_ ? processHost(0.0) : process(0.0);
            syncWdf();
            std::size_t at = 0;
            for (const auto& [p, n] : arr)
                for (std::size_t e = 0; e < n; ++e) xm[at++] += p[e];
        }
        for (double& v : xm) v /= static_cast<double>(N);
        gather(xb);
        double pinErr = 0;
        for (std::size_t k = 0; k < R; ++k) {
            const double m = xm[pin[k].second]; // V_ leads the state vector
            double next = m;
            if (it >= 1) {
                pinErr = std::max(pinErr, std::abs(m - pinNow[k]) / std::max(0.01, 1e-3 * std::abs(m)));
                const double d1 = perMean[k][2] - perMean[k][1], d2 = m - perMean[k][2]; // the output's own geometric tail
                if (d1 != 0.0 && d1 * d2 > 0.0 && std::abs(d2) < 0.97 * std::abs(d1)) next = m + d2 * (d2 / d1) / (1.0 - d2 / d1);
            }
            auto& h = pinHist[k]; // ...and that of the pins' sequence once it decays monotonically
            h[0] = h[1], h[1] = h[2], h[2] = next;
            if (it >= 3) {
                const double d1 = h[1] - h[0], d2 = h[2] - h[1];
                if (d1 != 0.0 && d1 * d2 > 0.0 && std::abs(d2) < 0.95 * std::abs(d1)) {
                    next = h[2] + d2 * (d2 / d1) / (1.0 - d2 / d1);
                    h[0] = h[1] = h[2] = next;
                }
            }
            pinNow[k] = next;
        }
        for (std::size_t c = 0; c < ctlCur_.size(); ++c) helper.setControlImmediate(static_cast<int>(c), ctlCur_[c]);
        for (std::size_t k = 0; k < R; ++k) helper.knownValue_[pin[k].first] = helper.V_[pin[k].first] = pinNow[k];
        if (!helper.reset()) {
            if (dbg) std::fprintf(stderr, "settleSlowRails: averaged circuit: no DC point\n");
            break;
        }
        syncWdf(); // (adoptStateByName re-initialises every WDF group from its arrays)
        adoptKeepingBlocks(helper);
        gather(xa);
        for (std::size_t i = 0; i < dim; ++i)
            if (frozen[i]) xa[i] = xb[i];
            else if (xa[i] != xb[i]) xa[i] = xb[i] + (xa[i] - xm[i]);
        shift = 0;
        std::size_t worst = 0;
        for (std::size_t i : fit)
            if (std::abs(xa[i] - xb[i]) > shift) shift = std::abs(xa[i] - xb[i]), worst = i;
        if (it >= 1) shiftMax = std::max(shiftMax, shift);
        if (dbg) { // the part whose state moved most
            std::size_t at = 0;
            for (const auto& [p, n] : arr) {
                if (worst >= at && worst < at + n)
                    for (std::size_t gi = 0; gi < rt_.size(); ++gi)
                        if (p == rt_[gi]->vcLast.data()) {
                            const int part = prims_[static_cast<std::size_t>(groups_[gi].states[worst - at])].part;
                            std::fprintf(stderr, "settleSlowRails: largest move at %s (group %zu)\n", part >= 0 ? net_.parts[static_cast<std::size_t>(part)].ref.c_str() : "?", gi);
                        }
                at += n;
            }
        }
        scatter(xa);
        if (dbg) {
            std::fprintf(stderr, "settleSlowRails: step %d (%.3f s): largest move %.3e V, rectifier means %.2f tolerances off their pins;", it,
                         std::chrono::duration<double>(std::chrono::steady_clock::now() - tStart).count(), shift, pinErr);
            for (std::size_t k = 0; k < R; ++k) std::fprintf(stderr, " %s %.3f", rect[k].c_str(), pinNow[k]);
            std::fprintf(stderr, "\n");
        }
        // done when the pins hold and the moves are under 1e-3 of the largest state, or no longer shrink (a state the
        // averaged circuit does not describe, moved back by the circuit each time)
        const bool stalled = it >= 3 && shift > 0.8 * lastShift;
        converged = it >= 1 && pinErr <= 1.0 && (shift <= 1e-3 * vmax || (stalled && shift <= 0.1 * shiftMax));
        const double lastMove = lastShift;
        lastShift = shift;
        if (it >= 3 && shift > 0.9 * shiftMax && lastMove > 0.9 * shiftMax) { // moves that never shrank: a supply this averaged circuit does not describe
            ++it;
            break;
        }
        if (!converged) // the rectifier outputs follow their new load (their period means kept for the tails above)
            for (int q = 0; q < 3; ++q) {
                double acc[64] = {};
                for (long i = 0; i < N; ++i) {
                    multirate_ ? processHost(0.0) : process(0.0);
                    for (std::size_t k = 0; k < R && k < 64; ++k) acc[k] += V_[pin[k].second];
                }
                for (std::size_t k = 0; k < R && k < 64; ++k) perMean[k][static_cast<std::size_t>(q)] = acc[k] / static_cast<double>(N);
            }
    }
    const bool ok = converged || (it > 1 && shift <= 0.1 * shiftMax);
    if (!ok) { // steps that do not shrink (a supply this averaged circuit does not describe): the state before
        scatter(x0);
        phase_ = phase0;
        tick_ = tick0;
        if (!acSrc_.empty()) advanceAc(0.0);
        if (known == 0) remember(2);
    }
    for (std::size_t gi = 0; gi < rt_.size(); ++gi) rt_[gi]->noiseOn = noiseWas[gi] != 0;
    if (dbg) std::fprintf(stderr, "settleSlowRails: %s after %d steps (%.3f s)\n", converged ? "converged" : ok ? "settled to 1/10 of its largest move" : known ? "falls back (remembered)" : "no progress, undone", it,
                          std::chrono::duration<double>(std::chrono::steady_clock::now() - tStart).count());
    return ok;
}

// The supply's averaged operating point (settlePeriodic): the netlist with its mains side cut away at the rectifiers.
// The mains side is everything reached from a mains source node (≥ 40 Hz) through transformers and passives (windings,
// their resistors, centre-tap returns, hum-balance pots); a diode with one terminal there is a rectifier and its other
// terminal (unless ground) a rectifier output. The returned netlist drops every part that touches the mains side and
// drives each rectifier output with a DC source instead ("VSETTLE<k>", value set by the caller), so its DC operating
// point is the circuit with its rails at those values: every consumer, filtered rail, coupling capacitor and bias at DC.
// False when anything other than passives, transformers, sources and diodes touches the mains side.
bool mainsAveragedNetlist(const Netlist& n, Netlist& out, std::vector<std::string>& rectNodes, bool capsBound, std::vector<std::string>* mainsSide) {
    std::set<std::string> ac;
    for (const auto& p : n.parts)
        if (p.type == "V" && p.acAmp != 0 && p.acFreq >= 40.0)
            for (const auto& nd : p.nodes)
                if (nd != "0") ac.insert(nd);
    if (ac.empty()) return false;
    auto passive = [](const std::string& t) { return t == "transformer" || t == "R" || t == "C" || t == "L" || t == "pot" || t == "switch" || t == "V"; };
    std::set<std::string> rect;
    for (bool grew = true; grew;) {
        grew = false;
        for (const auto& p : n.parts) {
            const bool touches = std::any_of(p.nodes.begin(), p.nodes.end(), [&](const std::string& nd) { return ac.count(nd) > 0; });
            if (!touches) continue;
            if (p.type == "D" && p.nodes.size() == 2) {
                for (const auto& nd : p.nodes)
                    if (nd != "0" && !ac.count(nd)) rect.insert(nd);
                continue;
            }
            if (capsBound && p.type == "C") continue; // (§10.49) a capacitor bounds the mains side: it is open at DC
            if (capsBound && (p.type == "vccs" || p.type == "vcvs") && p.nodes.size() == 4 && (p.nodes[2] == "0" || ac.count(p.nodes[2])) &&
                (p.nodes[3] == "0" || ac.count(p.nodes[3]))) { // a source the mains controls (a winding's Norton / Thevenin form)
                for (int k = 0; k < 2; ++k)
                    if (p.nodes[static_cast<std::size_t>(k)] != "0" && ac.insert(p.nodes[static_cast<std::size_t>(k)]).second) grew = true;
                continue;
            }
            if (!passive(p.type)) return false;
            for (const auto& nd : p.nodes)
                if (nd != "0" && ac.insert(nd).second) grew = true;
        }
    }
    // (§10.56) A rectifier output that only diodes touch is the inside of a series diode chain: it joins the mains side,
    // and the rectifier output moves on past the chain's next diode.
    auto diodesOnly = [&](const std::string& nd) {
        if (nd == n.io.input || nd == n.io.output) return false;
        int touching = 0;
        for (const auto& p : n.parts)
            if (std::find(p.nodes.begin(), p.nodes.end(), nd) != p.nodes.end()) {
                if (p.type != "D" || p.nodes.size() != 2) return false;
                ++touching;
            }
        return touching >= 2;
    };
    // A chain that ends at ground or on the mains side (the JC-120's power LED behind its own diode) leads to no reservoir:
    // that output stays as it was.
    for (bool moved = kEnvDiodeChain; moved;) {
        moved = false;
        for (const std::string& mid : std::vector<std::string>(rect.begin(), rect.end())) {
            if (!diodesOnly(mid)) continue;
            std::vector<std::string> next;
            for (const auto& p : n.parts)
                if (p.type == "D" && std::find(p.nodes.begin(), p.nodes.end(), mid) != p.nodes.end())
                    for (const auto& nd : p.nodes)
                        if (nd != "0" && nd != mid && !ac.count(nd)) next.push_back(nd);
            if (next.empty()) continue;
            moved = true;
            ac.insert(mid);
            rect.erase(mid);
            rect.insert(next.begin(), next.end());
        }
    }
    for (const auto& r : rect)
        if (ac.count(r)) return false; // a rectifier output on the mains side: not a supply this can split
    if (rect.empty()) return false;
    if (capsBound) // a mains-side node with a capacitor into the rest (a split reservoir's centre on the winding's tap) is
        for (const auto& p : n.parts) // driven like a rectifier output: the capacitor stays, and the node is held at its mean
            if (p.type == "C" && p.nodes.size() == 2 && (ac.count(p.nodes[0]) > 0) != (ac.count(p.nodes[1]) > 0))
                for (const auto& nd : p.nodes)
                    if (ac.count(nd)) rect.insert(nd);
    if (capsBound)
        for (const auto& r : rect) ac.erase(r);
    if (mainsSide) mainsSide->assign(ac.begin(), ac.end());
    out = n;
    out.parts.clear();
    for (const auto& p : n.parts)
        if (!std::any_of(p.nodes.begin(), p.nodes.end(), [&](const std::string& nd) { return ac.count(nd) > 0; })) out.parts.push_back(p);
    rectNodes.assign(rect.begin(), rect.end());
    for (std::size_t k = 0; k < rectNodes.size(); ++k) {
        Part v;
        v.ref = "VSETTLE" + std::to_string(k);
        v.type = "V";
        v.nodes = {rectNodes[k], "0"};
        v.hasValue = true;
        out.parts.push_back(v);
    }
    out.cuts.erase(std::remove_if(out.cuts.begin(), out.cuts.end(), [&](const std::string& c) { return rect.count(c) > 0 || (capsBound && ac.count(c) > 0); }),
                   out.cuts.end());
    out.probes.clear();
    return true;
}

// Periodic steady state of a transformer-fed supply with zero input (docs §3.10), from a cold start (or the last one):
//  1. Averaged operating point. Each step measures the rectifier outputs' means over one mains period, solves the
//     rest of the circuit at DC with its rails held there (mainsAveragedNetlist), and moves every state that circuit
//     shares by (its DC value − its mean over the period), which keeps the ripple and the mains side's own state. The
//     rails' load changes with it, so the rectifier outputs move: a few steps, until the shift is below 1e-3 of the
//     circuit's largest capacitor voltage. This settles the filtered rails (8.2k into 140 µF, 10k into 290 µF: seconds),
//     the series reservoir's balancers (22 s), coupling capacitors behind megohms (1 s) and the bias network at once.
//  2. Reduced-rank extrapolation of the mains-period map x → Φ(x) (the engine's own cycle) for what the averaged circuit
//     misses (ripple-driven offsets, the cores' DC flux): from K + 2 snapshots at period boundaries, γ minimises
//     |Σ γ_j (x_{j+1} − x_j)| over the capacitor voltages (Σ γ_j = 1) and the state jumps to Σ γ_j x_{j+1}; repeated
//     while the jump exceeds 2e-4 of that scale.
// Ends on a mains-period boundary with the phase back at t = 0: the audio starts on the mains phase a cold start has.
bool Circuit::settlePeriodic() {
    const double step = multirate_ ? 2.0 * T_ : T_;
    double fMains = 0;
    for (const auto& a : acSrc_)
        if (a.amp != 0.0 && a.w >= 2.0 * M_PI * 40.0) fMains = std::max(fMains, a.w / (2.0 * M_PI));
    if (fMains <= 0) return false;
    const long N = std::max(1L, std::lround(1.0 / (fMains * step)));
    const bool dbg = std::getenv("CE_PSS_DEBUG") != nullptr;
    const auto tStart = std::chrono::steady_clock::now();
    // noise off while settling (the state is the noiseless circuit's; the noise streams resume where they were)
    std::vector<std::uint8_t> noiseWas(rt_.size());
    for (std::size_t gi = 0; gi < rt_.size(); ++gi) noiseWas[gi] = rt_[gi]->noiseOn, rt_[gi]->noiseOn = false;
    std::vector<std::pair<double*, std::size_t>> arr;
    stateArrays(arr);
    std::size_t dim = 0;
    for (const auto& a : arr) dim += a.second;
    auto gather = [&](std::vector<double>& x) {
        x.resize(dim);
        std::size_t at = 0;
        for (const auto& [p, n] : arr) std::copy(p, p + n, x.begin() + static_cast<long>(at)), at += n;
    };
    auto scatter = [&](const std::vector<double>& x) {
        std::size_t at = 0;
        for (auto& [p, n] : arr) std::copy(x.begin() + static_cast<long>(at), x.begin() + static_cast<long>(at + n), p), at += n;
        for (auto& rp : rt_) rp->predHist = 0;
        primeConverters();
    };
    // The measure of both steps: every group's capacitor and inductor state voltages (current whether or not their nodes
    // are exported), in volts, unweighted: the rails' and plates' volts count as volts.
    std::vector<std::size_t> fit;
    std::vector<int> fitPart;
    {
        std::size_t at = 0;
        for (const auto& [p, n] : arr) {
            for (std::size_t gi = 0; gi < rt_.size(); ++gi)
                if (p == rt_[gi]->vcLast.data())
                    for (std::size_t k = 0; k < n; ++k) fit.push_back(at + k), fitPart.push_back(prims_[static_cast<std::size_t>(groups_[gi].states[k])].part);
            at += n;
        }
    }
    auto label = [&](std::size_t q) { return fitPart[q] >= 0 ? net_.parts[static_cast<std::size_t>(fitPart[q])].ref.c_str() : "?"; };
    // The mains-fed cores' flux states (their 1 F flux capacitor: V_, vcLast and w entries, companion conductance). In
    // the periodic state the primary carries no DC (it sits across the mains), so with secondaries whose rectifiers
    // draw no net DC ampere-turns (bridges, centre-tapped full-wave) the flux has zero mean; a cold start leaves an
    // offset that the primary's resistance would only remove with Lm/R (20 s on the SVT).
    struct Flux {
        std::size_t v, vc, w;
        double gc;
    };
    std::vector<Flux> fluxes;
    for (const auto& mg : mags_) {
        const int part = devices_[static_cast<std::size_t>(mg.device)].part;
        bool mainsFed = false;
        for (const auto& a : acSrc_)
            if (a.amp != 0.0 && a.w >= 2.0 * M_PI * 40.0)
                for (const auto& nm : net_.parts[static_cast<std::size_t>(part)].nodes) mainsFed = mainsFed || node(nm) == a.node;
        if (!mainsFed) continue;
        for (std::size_t gi = 0; gi < groups_.size(); ++gi)
            for (std::size_t k = 0; k < groups_[gi].states.size(); ++k) {
                const auto& p = prims_[static_cast<std::size_t>(groups_[gi].states[k])];
                if (p.kind != Prim::Cap || p.a != mg.flux || p.b != 0) continue;
                std::size_t at = 0, ow = 0, ovc = 0;
                for (const auto& [ptr, n] : arr) {
                    if (ptr == rt_[gi]->w.data()) ow = at + k;
                    if (ptr == rt_[gi]->vcLast.data()) ovc = at + k;
                    at += n;
                }
                fluxes.push_back({static_cast<std::size_t>(mg.flux), ovc, ow, rt_[gi]->gc[k]});
            }
    }
    // adoptStateByName also takes over a tank / bucket chain / tape: this circuit keeps its own
    auto adoptKeepingBlocks = [&](const Circuit& o) {
        std::vector<std::shared_ptr<SpringTank>> tanks;
        std::vector<std::shared_ptr<BbdLine>> lines;
        std::vector<std::shared_ptr<TapeLoop>> loops;
        for (const auto& b : blocks_) tanks.push_back(b.tank);
        for (const auto& b : bbds_) lines.push_back(b.line);
        for (const auto& t : tapes_) loops.push_back(t.loop);
        adoptStateByName(o);
        for (std::size_t b = 0; b < blocks_.size(); ++b) blocks_[b].tank = tanks[b];
        for (std::size_t b = 0; b < bbds_.size(); ++b) bbds_[b].line = lines[b];
        for (std::size_t t = 0; t < tapes_.size(); ++t) tapes_[t].loop = loops[t];
    };
    bool warm = !pss_.empty(); // started from the last periodic state (reset() restored it)
    // Above 160 kHz a cold settle starts at a lower rate: the same circuit at fs/2^m (80-160 kHz) settles itself, and its
    // state, carried over by part and node name (companion states converted to this rate), is the warm start here. The
    // SVT at 176.4 kHz: about 0.9 s of CPU instead of 2.4 s, then one averaged step at the full rate.
    if (!warm && !multirate_ && !ladder_ && fs_ > 160000.0) {
        double fc = fs_;
        while (fc > 160000.0) fc *= 0.5;
        Circuit coarse;
        CompileOptions co = opt_;
        co.sampleRate = fc;
        co.settleFrom = nullptr;
        co.coldMs = -1;
        co.noise = co.hum = false;
        co.probes.clear();
        co.exportAllNodes = false;
        co.partLevels.clear();
        std::string err;
        bool ok = coarse.compile(net_, co, err);
        if (ok) {
            for (std::size_t c = 0; c < ctlCur_.size(); ++c) coarse.setControlImmediate(static_cast<int>(c), ctlCur_[c]);
            ok = coarse.reset() && !coarse.pss_.empty();
        }
        if (ok) {
            adoptKeepingBlocks(coarse);
            primeConverters();
            warm = true;
        }
        if (dbg) std::fprintf(stderr, "settlePeriodic: %s at %.0f Hz (%.3f s)\n", ok ? "settled" : "could not settle", fc,
                              std::chrono::duration<double>(std::chrono::steady_clock::now() - tStart).count());
    }
    pssQuick_ = !warm && kEnvPssFast; // (§10.53) the cold run only
    if (!warm) runMains(6 * N); // the reservoirs charge
    pssQuick_ = false;
    for (auto& rp : rt_) rp->pssHeld = 0;
    if (dbg) std::fprintf(stderr, "settlePeriodic: %s (%.3f s), N %ld, multirate %d, %zu groups, dim %zu\n", warm ? "warm" : "cold run of 6 periods", std::chrono::duration<double>(std::chrono::steady_clock::now() - tStart).count(), N, multirate_ ? 1 : 0, groups_.size(), dim);
    bool steady = false;        // warm and the first averaged step moved nothing: still periodic
    std::vector<double> xb, xa;
    gather(xb);
    double vmax = 1.0;
    for (std::size_t i : fit) vmax = std::max(vmax, std::abs(xb[i]));
    // 1. averaged operating point
    Netlist hn;
    std::vector<std::string> rect;
    if (mainsAveragedNetlist(net_, hn, rect)) {
        Circuit helper;
        helper.dcOnly_ = kEnvPssFast; // (§10.53) its DC points only
        CompileOptions ho = opt_;
        ho.settleMs = 0;
        ho.coldMs = -1;
        ho.settleFrom = nullptr;
        ho.noise = ho.hum = false;
        ho.probes.clear();
        ho.exportAllNodes = true; // adoptStateByName reads its node voltages
        ho.multirate = false;
        ho.partLevels.clear();
        std::string err;
        bool ok = helper.compile(hn, ho, err);
        if (dbg && !ok) std::fprintf(stderr, "settlePeriodic: averaged circuit: %s\n", err.c_str());
        std::vector<std::pair<std::size_t, std::size_t>> pin; // helper source node, our node
        for (const auto& r : rect) pin.push_back({static_cast<std::size_t>(helper.node(r)), static_cast<std::size_t>(node(r))});
        std::vector<double> xm(dim);
        for (int it = 0; ok && it < 6; ++it) {
            std::fill(xm.begin(), xm.end(), 0.0);
            for (long i = 0; i < N; ++i) {
                multirate_ ? processHost(0.0) : process(0.0);
                std::size_t at = 0;
                for (const auto& [p, n] : arr)
                    for (std::size_t e = 0; e < n; ++e) xm[at++] += p[e];
            }
            for (double& v : xm) v /= static_cast<double>(N);
            gather(xb);
            for (std::size_t c = 0; c < ctlCur_.size(); ++c) helper.setControlImmediate(static_cast<int>(c), ctlCur_[c]);
            for (const auto& [h, m] : pin) helper.knownValue_[h] = helper.V_[h] = xm[m]; // V_ leads the state vector
            if (!helper.reset()) {
                if (dbg) std::fprintf(stderr, "settlePeriodic: averaged circuit: no DC point\n");
                break;
            }
            adoptKeepingBlocks(helper);
            gather(xa);
            for (std::size_t i = 0; i < dim; ++i)
                if (xa[i] != xb[i]) xa[i] = xb[i] + (xa[i] - xm[i]);
            for (const auto& f : fluxes) { // the cores' DC flux to zero
                const double dl = -xm[f.vc];
                xa[f.v] += dl, xa[f.vc] += dl, xa[f.w] += f.gc * dl;
            }
            double shift = 0;
            std::size_t worst = 0;
            for (std::size_t q = 0; q < fit.size(); ++q)
                if (std::abs(xa[fit[q]] - xb[fit[q]]) > shift) shift = std::abs(xa[fit[q]] - xb[fit[q]]), worst = q;
            scatter(xa);
            if (dbg) {
                std::fprintf(stderr, "settlePeriodic: averaged DC %d (%.3f s): largest shift %.3e V (%s);", it,
                             std::chrono::duration<double>(std::chrono::steady_clock::now() - tStart).count(), shift, fit.empty() ? "" : label(worst));
                for (const auto& [h, m] : pin) std::fprintf(stderr, " %s %.3f", nodeNames_[m].c_str(), xm[m]);
                std::fprintf(stderr, "\n");
            }
            if (shift <= 1e-3 * vmax) {
                steady = warm && it == 0;
                break;
            }
            runMains(4 * N); // the rectifier outputs follow their new load
        }
    }
    // 2. extrapolation of the period map
    const int K = 8;
    std::vector<std::vector<double>> X(static_cast<std::size_t>(K + 2));
    std::vector<double> G(static_cast<std::size_t>((K + 1) * (K + 1))), d(static_cast<std::size_t>(K + 1)), s(dim);
    std::vector<int> piv(static_cast<std::size_t>(K + 1));
    bool converged = steady; // a periodic state that the averaged step left where it was needs no extrapolation
    int cycle = 0;
    for (; cycle < 4 && !converged; ++cycle) {
        gather(X[0]);
        for (int j = 1; j <= K + 1; ++j) {
            runMains(N);
            gather(X[static_cast<std::size_t>(j)]);
        }
        auto u = [&](int j, std::size_t q) { return X[static_cast<std::size_t>(j + 1)][fit[q]] - X[static_cast<std::size_t>(j)][fit[q]]; };
        double trace = 0;
        for (int a = 0; a <= K; ++a)
            for (int b = 0; b <= a; ++b) {
                double acc = 0;
                for (std::size_t q = 0; q < fit.size(); ++q) acc += u(a, q) * u(b, q);
                G[static_cast<std::size_t>(a * (K + 1) + b)] = G[static_cast<std::size_t>(b * (K + 1) + a)] = acc;
                if (a == b) trace += acc;
            }
        if (!(trace > 0) || !std::isfinite(trace)) {
            converged = trace == 0.0;
            break;
        }
        for (int a = 0; a <= K; ++a) G[static_cast<std::size_t>(a * (K + 1) + a)] += 1e-13 * trace;
        std::fill(d.begin(), d.end(), 1.0);
        if (!la::luFactor(G.data(), K + 1, piv.data())) break;
        la::luSolve(G.data(), piv.data(), K + 1, d.data());
        double sum = 0;
        for (double v : d) sum += v;
        if (!std::isfinite(sum) || sum == 0.0) break;
        std::fill(s.begin(), s.end(), 0.0);
        for (int j = 0; j <= K; ++j) {
            const double g = d[static_cast<std::size_t>(j)] / sum;
            const auto& xj = X[static_cast<std::size_t>(j + 1)];
            for (std::size_t i = 0; i < dim; ++i) s[i] += g * xj[i];
        }
        const auto& xl = X[static_cast<std::size_t>(K + 1)];
        double jump = 0;
        std::size_t worst = 0;
        bool finite = true;
        for (double v : s) finite = finite && std::isfinite(v);
        if (!finite) break;
        for (std::size_t q = 0; q < fit.size(); ++q)
            if (std::abs(s[fit[q]] - xl[fit[q]]) > jump) jump = std::abs(s[fit[q]] - xl[fit[q]]), worst = q;
        scatter(s);
        if (dbg) std::fprintf(stderr, "settlePeriodic: extrapolation %d (%d periods of %ld samples): largest jump %.3e V (%s)\n", cycle, K + 1, N, jump, fit.empty() ? "" : label(worst));
        converged = jump <= 2e-4 * vmax;
    }
    runMains(N); // the last jump's image under the period map: a state the engine itself produced
    phase_ = 0;
    gather(pss_);
    for (std::size_t gi = 0; gi < rt_.size(); ++gi) rt_[gi]->noiseOn = noiseWas[gi] != 0;
    if (dbg) std::fprintf(stderr, "settlePeriodic: %s after %d extrapolations (%.3f s)\n", converged ? "converged" : "NOT converged", cycle,
                          std::chrono::duration<double>(std::chrono::steady_clock::now() - tStart).count());
    return converged;
}

// End of reset (ENGINE_QUEUE 25). A high-gain positive-feedback loop (a Schmitt comparator around an op-amp macro) has
// a middle solution on which the static loop gain exceeds one. The engine's macro has no dynamics inside that loop, so
// Newton, warm-started there, solves it again every sample, and a relaxation LFO sat at its DC point, creeping off
// through its slow integrator for seconds (Dimension D, CE-1, Small Stone). In the real circuit, and in ngspice, whose
// macro has a pole there, the loop flips within microseconds of power-up. Test: an odd number of real eigenvalues of
// the loop matrix K·∂i/∂v above one at the solution, which flips the sign of det(K·∂i/∂v − I) away from (−1)^nn. Such a
// group's Newton warm start moves to the stable solution of the same sample: states and inputs stay, so the first
// sample lands where the circuit is a moment after power-up. Deterministic: ports in order, pushed first the way the
// circuit's own time-varying sources drive them at t = 0+. Stable operating points are untouched. CE_NOESCAPE: A/B.
void Circuit::escapeUnstableBranches() {
    if (std::getenv("CE_NOESCAPE")) return; // developer A/B
    const Diagnostics keep = diag_;
    for (std::size_t gi = 0; gi < groups_.size(); ++gi) {
        const auto& g = groups_[gi];
        auto& r = *rt_[gi];
        const int nn = r.nn;
        if (r.wdf || nn == 0 || nn > kMaxNlPorts) continue;
        const auto N = static_cast<std::size_t>(nn);
        std::vector<double> A(N * N), M(N * N), Jd(N * N);
        std::vector<int> piv(N);
        auto loopMatrix = [&](const double* v) { // A = K·Jd (Jd block-diagonal by device)
            std::fill(Jd.begin(), Jd.end(), 0.0);
            int o = 0;
            for (int di : g.devices) {
                const auto& d = devices_[static_cast<std::size_t>(di)];
                double cur[3], J[9];
                d.eval(v + o, cur, J);
                for (int a = 0; a < d.nports; ++a)
                    for (int b = 0; b < d.nports; ++b) Jd[static_cast<std::size_t>((o + a) * nn + o + b)] = J[a * d.nports + b];
                o += d.nports;
            }
            for (int i = 0; i < nn; ++i)
                for (int j = 0; j < nn; ++j) {
                    double s = 0;
                    for (int k = 0; k < nn; ++k) s += r.K[static_cast<std::size_t>(i * nn + k)] * Jd[static_cast<std::size_t>(k * nn + j)];
                    A[static_cast<std::size_t>(i * nn + j)] = s;
                }
        };
        auto unstable = [&]() { // sign of det(A − I) against (−1)^nn
            M = A;
            for (int i = 0; i < nn; ++i) M[static_cast<std::size_t>(i * nn + i)] -= 1.0;
            if (!la::luFactor(M.data(), nn, piv.data())) return false;
            int neg = 0;
            for (int i = 0; i < nn; ++i) {
                if (M[static_cast<std::size_t>(i * nn + i)] < 0) ++neg;
                if (piv[static_cast<std::size_t>(i)] != i) ++neg;
            }
            return (neg % 2) != (nn % 2);
        };
        r.regen = false;
        const std::vector<double> v0(r.vp.begin(), r.vp.end());
        loopMatrix(v0.data());
        if (!unstable()) continue;
        r.regen = true;
        r.vpAlt.assign(N, 0.0);
        r.curAlt.assign(N, 0.0);
        // Leave along each port in turn (+ then −, growing pushes): the first push from which Newton converges to a
        // solution of the same sample whose loop matrix passes the test is taken. The dominant eigenvalue is usually
        // a negative-feedback one (an integrator's op-amp), so the unstable direction is not found by power iteration.
        // Which way first: the way the circuit's own time-varying sources (a start-up kick, the mains) push each port
        // at t = 0+, dp/dt = Dp·du/dt over the group's inputs; + where they do not reach.
        std::vector<double> dp(N, 0.0);
        for (const auto& a : acSrc_)
            for (int j = 0; j < r.nu; ++j)
                if (g.inputs[static_cast<std::size_t>(j)] == a.node) {
                    const double dv = acScale(a) * a.amp * a.w * std::cos(std::fmod(a.w * phase_ + a.phase, 2.0 * M_PI));
                    for (int k = 0; k < nn; ++k) dp[static_cast<std::size_t>(k)] += r.Dp[static_cast<std::size_t>(k * (r.ns + r.nu) + r.ns + j)] * dv;
                }
        bool moved = false;
        for (double amp : {0.5, 2.0, 8.0}) {
            for (int k = 0; k < nn && !moved; ++k)
                for (double sg : {dp[static_cast<std::size_t>(k)] < 0 ? -1.0 : 1.0, dp[static_cast<std::size_t>(k)] < 0 ? 1.0 : -1.0}) {
                    std::copy(v0.begin(), v0.end(), r.vp.begin());
                    r.vp[static_cast<std::size_t>(k)] += sg * amp;
                    if (!newton(r, g, r.pPrev.data(), 100)) continue;
                    double dist = 0;
                    for (int i = 0; i < nn; ++i) dist = std::max(dist, std::abs(r.vp[static_cast<std::size_t>(i)] - v0[static_cast<std::size_t>(i)]));
                    loopMatrix(r.vp.data());
                    if (dist > 1e-3 && !unstable()) {
                        moved = true;
                        break;
                    }
                }
            if (moved) break;
        }
        if (moved) {
            r.vpPrev = r.vp;
            r.vpPrev2 = r.vp;
            if (r.curPrev1.size() == N && r.curPrev2.size() == N)
                for (int k = 0; k < nn; ++k) {
                    const double c = r.w[static_cast<std::size_t>(r.ns + r.nu + k)];
                    r.curPrev1[static_cast<std::size_t>(k)] = r.curPrev2[static_cast<std::size_t>(k)] = c;
                }
            r.predHist = 0;
            warnings_.push_back("group " + std::to_string(gi) + " started on the stable side of a positive-feedback loop (its operating point is the unstable middle solution)");
        } else {
            std::copy(v0.begin(), v0.end(), r.vp.begin());
            int o = 0; // currents consistent with the kept warm start
            for (int di : g.devices) {
                const auto& dv = devices_[static_cast<std::size_t>(di)];
                double J[9];
                dv.eval(r.vp.data() + o, r.w.data() + r.ns + r.nu + o, J);
                o += dv.nports;
            }
        }
    }
    diag_ = keep;
}

// Ladder cut streams at the operating point: every slot the cut node's DC current, each converter primed with its
// consumer's DC contribution (cutAcc_ is idle between samples). Control thread (reset / adopt).
void Circuit::primeLadderCuts() noexcept {
    if (!ladderCuts_) return;
    for (int x : lcutNodes_)
        for (int i = 0; i < 32; ++i) {
            cutPlay_[static_cast<std::size_t>(x) * 32 + static_cast<std::size_t>(i)] = cutI_[static_cast<std::size_t>(x)];
            cutNext_[static_cast<std::size_t>(x) * 32 + static_cast<std::size_t>(i)] = 0.0;
        }
    cutKeep_.resize(cutAcc_.size()); // (sized at compile: no allocation on the audio thread's hand-offs)
    std::copy(cutAcc_.begin(), cutAcc_.end(), cutKeep_.begin());
    for (std::size_t gi = 0; gi < groups_.size(); ++gi) {
        if (lcutOf_[gi].empty()) continue;
        std::fill(cutAcc_.begin(), cutAcc_.end(), 0.0);
        cutScale_ = 1.0;
        accumulateCutTerms(gi, false);
        for (int e : lcutOf_[gi]) {
            auto& lc = lcut_[static_cast<std::size_t>(e)];
            const double i0 = cutAcc_[static_cast<std::size_t>(lc.node)];
            lc.buf.fill(i0);
            if (lc.conv >= 0) {
                auto& cv = lcutConv_[static_cast<std::size_t>(lc.conv)];
                for (auto& s : cv.st) s.prime(i0, cv.to > cv.from);
                cv.buf.fill(i0);
            }
        }
    }
    std::copy(cutKeep_.begin(), cutKeep_.end(), cutAcc_.begin());
}

void Circuit::primeConverters() noexcept {
    primeLadderCuts();
    evosPrime(); // docs §10.48
    if (ladder_) {
        for (std::size_t nd = 0; nd < V_.size(); ++nd)
            for (int k = 0; k < 32; ++k) blk_[nd * 32 + static_cast<std::size_t>(k)] = V_[nd];
        for (auto& c : lconv_) {
            for (auto& s : c.st) s.prime(V_[static_cast<std::size_t>(c.node)], c.to > c.from);
            c.buf.fill(V_[static_cast<std::size_t>(c.node)]);
        }
    }
    if (!multirate_) return;
    for (std::size_t n = 0; n < V_.size(); ++n) {
        lo_[n] = hi0_[n] = hi1_[n] = V_[n];
        if (upOf_[n] >= 0) conv_[static_cast<std::size_t>(upOf_[n])].prime(V_[n]);
        if (downOf_[n] >= 0) conv_[static_cast<std::size_t>(downOf_[n])].prime(V_[n]);
    }
}

double Circuit::processHost(double vin) noexcept {
    if (!multirate_) return process(vin);
    if (gliding_ && subCounter_ <= 0) {
        updateControls();
        subCounter_ = refreshInterval_ / 2;
    }
    --subCounter_;
    if (!std::isfinite(vin)) vin = 0;
    vin = std::clamp(vin, -100.0, 100.0);
    if (!acSrc_.empty()) advanceAc(2.0 * T_);
    const auto in = static_cast<std::size_t>(inputNode_);
    lo_[in] = vin;
    if (upOf_[in] >= 0) conv_[static_cast<std::size_t>(upOf_[in])].up(vin, hi0_[in], hi1_[in]);
    if (inputNodeR_ > 0) { // io.inputR: the same up-conversion as the left input
        const auto inR = static_cast<std::size_t>(inputNodeR_);
        lo_[inR] = vinR_;
        if (upOf_[inR] >= 0) conv_[static_cast<std::size_t>(upOf_[inR])].up(vinR_, hi0_[inR], hi1_[inR]);
    }
    double* Vn = V_.data();
    for (std::size_t gi = 0; gi < groups_.size(); ++gi) {
        const auto& g = groups_[gi];
        std::chrono::steady_clock::time_point tg0;
        if (profile_) tg0 = std::chrono::steady_clock::now();
        struct Acc {
            Circuit* c;
            std::size_t gi;
            std::chrono::steady_clock::time_point t0;
            ~Acc() {
                if (c->profile_) c->groupNs_[gi] += std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
            }
        } acc{this, gi, tg0};
        if (g.lowRate) {
            for (int n : g.inputs) Vn[n] = (nodeKind_[static_cast<std::size_t>(n)] == kKnown && !isInputNode(n)) ? Vn[n] : lo_[static_cast<std::size_t>(n)];
            runGroup(gi);
            for (int e : g.exports) {
                const auto u = static_cast<std::size_t>(e);
                lo_[u] = Vn[e];
                if (upOf_[u] >= 0) conv_[static_cast<std::size_t>(upOf_[u])].up(lo_[u], hi0_[u], hi1_[u]);
            }
        } else {
            cutScale_ = 0.5; // two internal samples per host sample: the host-rate side sees their mean current
            for (int k = 0; k < 2; ++k) {
                for (int n : g.inputs)
                    if (!(nodeKind_[static_cast<std::size_t>(n)] == kKnown && !isInputNode(n))) Vn[n] = k ? hi1_[static_cast<std::size_t>(n)] : hi0_[static_cast<std::size_t>(n)];
                processGroup(gi);
                for (int e : g.exports) (k ? hi1_ : hi0_)[static_cast<std::size_t>(e)] = Vn[e];
            }
            cutScale_ = 1.0;
            for (int e : g.exports) {
                const auto u = static_cast<std::size_t>(e);
                if (downOf_[u] >= 0) lo_[u] = conv_[static_cast<std::size_t>(downOf_[u])].down(hi0_[u], hi1_[u]);
            }
        }
    }
    if (!cutI_.empty()) finishCutSample();
    if (!blocks_.empty() || !bbds_.empty() || !tapes_.empty()) runBlocks();
    if (!logicAt_.empty()) runLogic(groups_.size()); // CMOS cells (host rate in a multirate circuit)
    ++tick_;
    diag_.samples += 2;
    return lo_[static_cast<std::size_t>(outputNode_)];
}

// ───────────────────────────── controls ─────────────────────────────
void Circuit::setControlTarget(int c, double v) noexcept {
    if (c < 0 || static_cast<std::size_t>(c) >= ctlTarget_.size() || !std::isfinite(v)) return;
    v = std::clamp(v, 0.0, 1.0);
    const auto& def = net_.controls[static_cast<std::size_t>(c)];
    ctlTarget_[static_cast<std::size_t>(c)] = v;
    if (def.kind != "continuous") ctlCur_[static_cast<std::size_t>(c)] = v; // switches don't glide
    gliding_ = true;
}
void Circuit::setControlImmediate(int c, double v) noexcept {
    if (c < 0 || static_cast<std::size_t>(c) >= ctlTarget_.size() || !std::isfinite(v)) return;
    ctlTarget_[static_cast<std::size_t>(c)] = ctlCur_[static_cast<std::size_t>(c)] = std::clamp(v, 0.0, 1.0);
    gliding_ = true;
    updateControls();
    refreshScaledSources(); // DC operating points see the scaled supply too
}

void Circuit::setSpeakerLoad(double fsHz, double qRes, double leScale) noexcept {
    if (speakers_.empty() && spkDevs_.empty()) return;
    const bool valid = std::isfinite(fsHz) && fsHz > 25.0 && fsHz < 250.0;
    const double fs = valid ? fsHz : 0.0;
    const double q = valid && std::isfinite(qRes) ? std::clamp(qRes, 0.5, 4.0) : 1.4;
    const double le = valid && std::isfinite(leScale) ? std::clamp(leScale, 0.2, 2.5) : 1.0;
    if (std::abs(fs - spkFs_) <= 0.2 && std::abs(q - spkQ_) <= 0.02 && std::abs(le - spkLe_) <= 0.02) return;
    spkFs_ = fs;
    spkQ_ = q;
    spkLe_ = le;
    auto applySlot = [&](int si, double sc) {
        if (si < 0) return;
        auto& sl = slots_[static_cast<std::size_t>(si)];
        sl.scale = sc;
        const double v = slotValue(sl);
        if (std::abs(v - sl.value) > 1e-15 * std::abs(sl.value)) {
            sl.value = v;
            for (int g : slotGroups_[static_cast<std::size_t>(si)]) groupDirty_[static_cast<std::size_t>(g)] = 1;
        }
    };
    for (const auto& sp : speakers_) {
        const double kf = valid ? sp.fs0 / fs : 1.0;
        const double kq = valid ? q / 1.4 : 1.0;
        applySlot(sp.lces, kf / kq);
        applySlot(sp.cmes, kf * kq);
        applySlot(sp.res, kq);
        applySlot(sp.le, valid ? le : 1.0);
        applySlot(sp.le2, valid ? le : 1.0);
    }
    // Electro-mechanical speakers: Kms stays (it is in the law and in a fixed transconductance), so the profile moves the
    // cone mass: Mms × kf² puts the resonance at fs, and Rms ÷ kq scales the impedance peak (Res = Bl²/Rms) as the T/S
    // blocks' Res × kq. The bandwidth then follows physically from the heavier or lighter cone (Qms × kf·kq).
    for (const auto& sd : spkDevs_) {
        const double kf = valid ? sd.fs0 / fs : 1.0;
        const double kq = valid ? q / 1.4 : 1.0;
        applySlot(sd.mms, kf * kf);
        applySlot(sd.rms, kq); // the slot holds 1/Rms (ohms)
        applySlot(sd.le, valid ? le : 1.0);
        applySlot(sd.l2, valid ? le : 1.0);
    }
    syncSpeakerDevices();
    for (std::size_t g = 0; g < groups_.size(); ++g)
        if (groupDirty_[g]) {
            if (!smUpdate(g)) refreshGroup(g, false);
            groupDirty_[g] = 0;
        }
}

bool Circuit::evalSlots(bool force) noexcept {
    bool any = false;
    for (std::size_t s = 0; s < slots_.size(); ++s) {
        auto& sl = slots_[s];
        if (sl.kind == ValueSlot::Const && !force) continue;
        const double v = slotValue(sl);
        if (force || std::abs(v - sl.value) > 1e-12 * std::abs(sl.value)) {
            sl.value = v;
            for (int g : slotGroups_[s]) groupDirty_[static_cast<std::size_t>(g)] = 1;
            any = true;
        }
    }
    return any;
}

void Circuit::updateControls() noexcept {
    if (!gliding_) return;
    bool still = false;
    for (std::size_t c = 0; c < ctlCur_.size(); ++c) {
        double& cur = ctlCur_[c];
        const double t = ctlTarget_[c];
        if (cur == t) continue;
        cur += glideAlpha_ * (t - cur);
        if (std::abs(t - cur) < 2e-5) cur = t;
        else still = true;
    }
    gliding_ = still;
    if (evalSlots(false))
        for (std::size_t g = 0; g < groups_.size(); ++g)
            if (groupDirty_[g]) {
                if (!smUpdate(g)) refreshGroup(g, false);
                groupDirty_[g] = 0;
            }
}

bool Circuit::adoptStateFrom(const Circuit& o) noexcept {
    if ((o.hash_ != hash_ || kEnvForceTopo) && o.netHash_ == netHash_ && &o != this) return adoptAcrossTopology(o); // other switch positions
    if (o.hash_ != hash_ || o.rt_.size() != rt_.size()) return false;
    for (std::size_t gi = 0; gi < rt_.size(); ++gi) {
        auto& a = *rt_[gi];
        const auto& b = *o.rt_[gi];
        if (a.w.size() != b.w.size() || a.vp.size() != b.vp.size()) return false;
        std::copy(b.w.begin(), b.w.end(), a.w.begin());
        std::copy(b.vp.begin(), b.vp.end(), a.vp.begin());
        std::copy(b.vpPrev.begin(), b.vpPrev.end(), a.vpPrev.begin());
        std::copy(b.vpPrev2.begin(), b.vpPrev2.end(), a.vpPrev2.begin());
        std::copy(b.pPrev.begin(), b.pPrev.end(), a.pPrev.begin());
        a.predHist = 0;
        std::copy(b.vcLast.begin(), b.vcLast.end(), a.vcLast.begin());
        std::copy(b.gc.begin(), b.gc.end(), a.gc.begin());
        if (a.uPrev.size() == b.uPrev.size()) std::copy(b.uPrev.begin(), b.uPrev.end(), a.uPrev.begin());
        if (a.xh.size() == b.xh.size()) {
            std::copy(b.xh.begin(), b.xh.end(), a.xh.begin());
            a.xhHead = b.xhHead;
            a.lteWarm = b.lteWarm;
            a.subNext = b.subNext;
            a.subPrev = b.subPrev;
        } else a.lteWarm = 0, a.subNext = 1, a.subPrev = 1;
        if (a.wdf && b.wdf) a.wdf->adoptFrom(*b.wdf);
        // Noise stream (docs §3.9): the generator, the held draw and its countdown, the compensation ring and the 1/f
        // sections, so an edited unit continues its predecessor's noise sample for sample (test_effect_handoffs).
        if (a.noiseOn && b.noiseOn && a.xf.size() == b.xf.size() && a.xr.size() == b.xr.size() && a.flick.size() == b.flick.size()) {
            std::copy(std::begin(b.rng), std::end(b.rng), std::begin(a.rng));
            std::copy(b.xf.begin(), b.xf.end(), a.xf.begin());
            std::copy(b.xr.begin(), b.xr.end(), a.xr.begin());
            a.xHead = b.xHead;
            a.noiseCount = b.noiseCount;
            for (std::size_t f = 0; f < a.flick.size(); ++f) std::copy(std::begin(b.flick[f].s), std::end(b.flick[f].s), std::begin(a.flick[f].s));
        }
    }
    std::copy(o.V_.begin(), o.V_.end(), V_.begin());
    // Time bases: the mains / AC sources' phase and the slow partitions' tick, so an adopted circuit continues on the
    // same mains cycle and LFO schedule as its predecessor (a CE-1 on mains differed after every knob edit).
    phase_ = o.phase_;
    tick_ = o.tick_;
    for (std::size_t gi = 0; gi < rt_.size(); ++gi) {
        auto& a = *rt_[gi];
        const auto& b = *o.rt_[gi];
        if (a.curPrev1.size() == b.curPrev1.size()) std::copy(b.curPrev1.begin(), b.curPrev1.end(), a.curPrev1.begin());
        if (a.curPrev2.size() == b.curPrev2.size()) std::copy(b.curPrev2.begin(), b.curPrev2.end(), a.curPrev2.begin());
    }
    for (std::size_t d = 0; d < devices_.size() && d < o.devices_.size(); ++d)
        std::copy(std::begin(o.devices_[d].hs), std::end(o.devices_[d].hs), std::begin(devices_[d].hs));
    // Take over the tank / bucket chain / tape (no copy, no allocation). Our own fresh ones are parked in retired_
    // (capacity reserved at compile) so their release never runs on the audio thread (test_effect_handoffs).
    auto park = [&](auto& mine, const auto& theirs) {
        if (mine == theirs) return;
        if (mine && retired_.size() < retired_.capacity()) retired_.push_back(std::move(mine));
        else if (mine) return; // no room to park it: keep our own state rather than free on this thread
        mine = theirs;
    };
    for (std::size_t b = 0; b < blocks_.size() && b < o.blocks_.size(); ++b) {
        park(blocks_[b].tank, o.blocks_[b].tank);
        blocks_[b].y = o.blocks_[b].y;
    }
    for (std::size_t b = 0; b < bbds_.size() && b < o.bbds_.size(); ++b) park(bbds_[b].line, o.bbds_[b].line); // the bucket chain
    for (std::size_t b = 0; b < tapes_.size() && b < o.tapes_.size(); ++b) park(tapes_[b].loop, o.tapes_[b].loop); // the tape
    for (std::size_t c = 0; c < logic_.size() && c < o.logic_.size(); ++c) logic_[c].adoptRuntime(o.logic_[c]); // flip-flop states, edges
    if (o.cutI_.size() == cutI_.size()) std::copy(o.cutI_.begin(), o.cutI_.end(), cutI_.begin());
    if (ladder_ && o.ladder_ && o.blk_.size() == blk_.size() && o.lconv_.size() == lconv_.size()) {
        std::copy(o.blk_.begin(), o.blk_.end(), blk_.begin());
        for (std::size_t c = 0; c < lconv_.size(); ++c) lconv_[c] = o.lconv_[c];
        if (ladderCuts_ && o.ladderCuts_ && o.lcut_.size() == lcut_.size() && o.lcutConv_.size() == lcutConv_.size()) {
            std::copy(o.cutPlay_.begin(), o.cutPlay_.end(), cutPlay_.begin());
            std::copy(o.cutNext_.begin(), o.cutNext_.end(), cutNext_.begin());
            for (std::size_t e = 0; e < lcut_.size(); ++e) lcut_[e].buf = o.lcut_[e].buf;
            for (std::size_t c = 0; c < lcutConv_.size(); ++c) lcutConv_[c] = o.lcutConv_[c];
        }
    }
    if (nEvo_) { // docs §10.48: the output streams carry over when the other circuit has the same ones
        bool same = o.nEvo_ == nEvo_ && o.evosM_ == evosM_ && o.evosP_ == evosP_;
        for (int i = 0; i < nEvo_ && same; ++i) same = o.evo_[static_cast<std::size_t>(i)].node == evo_[static_cast<std::size_t>(i)].node && o.evo_[static_cast<std::size_t>(i)].nst == evo_[static_cast<std::size_t>(i)].nst;
        if (same)
            for (int i = 0; i < nEvo_; ++i) {
                auto& a = evo_[static_cast<std::size_t>(i)];
                const auto& b = o.evo_[static_cast<std::size_t>(i)];
                a.s = b.s, a.sub = b.sub, a.ev = b.ev, a.rec = b.rec, a.head = b.head, a.st = b.st, a.zh = b.zh, a.zhHead = b.zhHead;
            }
        else evosPrime();
    }
    if (multirate_ && o.conv_.size() == conv_.size()) {
        std::copy(o.conv_.begin(), o.conv_.end(), conv_.begin());
        std::copy(o.lo_.begin(), o.lo_.end(), lo_.begin());
        std::copy(o.hi0_.begin(), o.hi0_.end(), hi0_.begin());
        std::copy(o.hi1_.begin(), o.hi1_.end(), hi1_.begin());
    }
    // Play from the controls and coupled speaker load the old instance was at and glide to this instance's targets.
    spkFs_ = o.spkFs_;
    spkQ_ = o.spkQ_;
    spkLe_ = o.spkLe_;
    for (std::size_t s = 0; s < slots_.size() && s < o.slots_.size(); ++s) slots_[s].scale = o.slots_[s].scale;
    syncSpeakerDevices();
    std::copy(o.ctlCur_.begin(), o.ctlCur_.end(), ctlCur_.begin());
    evalSlots(true);
    for (std::size_t g = 0; g < groups_.size(); ++g) {
        refreshGroup(g, false);
        groupDirty_[g] = 0;
    }
    gliding_ = true;
    subCounter_ = 0;
    return true;
}

// A reactive element's post-update state carried to another companion conductance with its branch voltage v and current
// i (processGroup: trapezoidal s = G·v + i for C and L, so i = s − G·v; backward Euler s = G·v for C, s = i for L, as
// substepGroup's mapStates). Until 2026-11-11 the hand-offs used s_new = G_new·v + (G_old·v − s) for C and
// (G_old·v + s) − G_new·v for L, an older sign convention: a capacitor's current came back negated (docs §10.29).
static double carriedState(const Prim& to, const Prim& from, double v, double s, double gOld, double gNew) noexcept {
    const bool cap = to.kind == Prim::Cap;
    const double i = from.be ? (cap ? 0.0 : s) : s - gOld * v; // (a backward-Euler capacitor's current is not in its state)
    return to.be ? (cap ? gNew * v : i) : gNew * v + i;
}

// After a part edit the structure differs: carry state by element identity instead. Each capacitor/inductor keeps
// its branch voltage AND current across the edit (carriedState), devices keep
// their Newton warm starts, nodes and controls keep their values by name. Allocation-free (string compares only).
bool Circuit::adoptStateByName(const Circuit& o) noexcept {
    auto partRef = [](const Circuit& c, int part) -> const std::string* {
        return part >= 0 ? &c.net_.parts[static_cast<std::size_t>(part)].ref : nullptr;
    };
    int matched = 0;
    for (std::size_t gi = 0; gi < groups_.size(); ++gi) {
        auto& r = *rt_[gi];
        const auto& g = groups_[gi];
        for (int k = 0; k < r.ns; ++k) {
            const auto& p = prims_[static_cast<std::size_t>(g.states[static_cast<std::size_t>(k)])];
            const std::string* ref = partRef(*this, p.part);
            if (!ref) continue;
            for (std::size_t ogi = 0; ogi < o.groups_.size(); ++ogi) {
                const auto& og = o.groups_[ogi];
                const auto& orr = *o.rt_[ogi];
                for (int ok = 0; ok < orr.ns; ++ok) {
                    const auto& op = o.prims_[static_cast<std::size_t>(og.states[static_cast<std::size_t>(ok)])];
                    const std::string* oref = partRef(o, op.part);
                    if (!oref || *oref != *ref || op.kind != p.kind || o.nodeNames_[static_cast<std::size_t>(op.a)] != nodeNames_[static_cast<std::size_t>(p.a)] ||
                        o.nodeNames_[static_cast<std::size_t>(op.b)] != nodeNames_[static_cast<std::size_t>(p.b)])
                        continue;
                    const double vc = orr.vcLast[static_cast<std::size_t>(ok)], so = orr.w[static_cast<std::size_t>(ok)], go = orr.gc[static_cast<std::size_t>(ok)];
                    r.w[static_cast<std::size_t>(k)] = carriedState(p, op, vc, so, go, r.gc[static_cast<std::size_t>(k)]);
                    r.vcLast[static_cast<std::size_t>(k)] = vc;
                    ++matched;
                }
            }
        }
        // Newton warm starts per device (by part ref and kind)
        int o1 = 0;
        for (int di : g.devices) {
            const auto& d = devices_[static_cast<std::size_t>(di)];
            for (std::size_t ogi = 0; ogi < o.groups_.size(); ++ogi) {
                int o2 = 0;
                for (int odi : o.groups_[ogi].devices) {
                    const auto& od = o.devices_[static_cast<std::size_t>(odi)];
                    if (od.kind == d.kind && od.nports == d.nports && *partRef(o, od.part) == *partRef(*this, d.part))
                        for (int k = 0; k < d.nports; ++k) {
                            const auto a = static_cast<std::size_t>(o1 + k), b = static_cast<std::size_t>(o2 + k);
                            r.vp[a] = o.rt_[ogi]->vp[b];
                            r.vpPrev[a] = o.rt_[ogi]->vpPrev[b];
                            r.vpPrev2[a] = o.rt_[ogi]->vpPrev2[b];
                        }
                    o2 += od.nports;
                }
            }
            o1 += d.nports;
        }
        if (r.wdf) r.wdf->resetFrom(*this, g, r.w.data(), r.vcLast.data());
    }
    for (std::size_t n = 1; n < nodeNames_.size(); ++n)
        for (std::size_t m = 1; m < o.nodeNames_.size(); ++m)
            if (nodeNames_[n] == o.nodeNames_[m] && nodeKind_[n] != kKnown) V_[n] = o.V_[m];
    if (blocks_.size() == o.blocks_.size())
        for (std::size_t b = 0; b < blocks_.size(); ++b) {
            blocks_[b].tank = o.blocks_[b].tank;
            blocks_[b].y = o.blocks_[b].y;
        }
    if (bbds_.size() == o.bbds_.size())
        for (std::size_t b = 0; b < bbds_.size(); ++b) bbds_[b].line = o.bbds_[b].line;
    if (tapes_.size() == o.tapes_.size())
        for (std::size_t b = 0; b < tapes_.size(); ++b) tapes_[b].loop = o.tapes_[b].loop;
    for (std::size_t c = 0; c < net_.controls.size(); ++c)
        for (std::size_t oc = 0; oc < o.net_.controls.size(); ++oc)
            if (net_.controls[c].id == o.net_.controls[oc].id) ctlCur_[c] = o.ctlCur_[oc];
    evalSlots(true);
    for (std::size_t g = 0; g < groups_.size(); ++g) {
        refreshGroup(g, false);
        groupDirty_[g] = 0;
        auto& r = *rt_[g];
        // homotopy anchor consistent with the adopted state
        for (int j = 0; j < r.nu; ++j) r.w[static_cast<std::size_t>(r.ns + j)] = V_[static_cast<std::size_t>(groups_[g].inputs[static_cast<std::size_t>(j)])];
        for (int j = 0; j < r.nuc; ++j) {
            const auto x = static_cast<std::size_t>(groups_[g].cutIn[static_cast<std::size_t>(j)]);
            r.w[static_cast<std::size_t>(r.ns + r.nu + r.nn + j)] = -(cutI_[x] - tranLoadG_[x] * V_[x]);
        }
        for (int k = 0; k < r.nn; ++k) {
            double sum = 0;
            for (int c = 0; c < r.ns + r.nu; ++c) sum += r.Dp[static_cast<std::size_t>(k * (r.ns + r.nu) + c)] * r.w[static_cast<std::size_t>(c)];
            for (int c = 0; c < r.nuc; ++c) sum += r.Dc[static_cast<std::size_t>(k * r.nuc + c)] * r.w[static_cast<std::size_t>(r.ns + r.nu + r.nn + c)];
            r.pPrev[static_cast<std::size_t>(k)] = sum;
        }
        r.predHist = 0;
    }
    if (o.spkFs_ > 0.0) {
        spkFs_ = 0.0;
        setSpeakerLoad(o.spkFs_, o.spkQ_, o.spkLe_);
    }
    primeConverters();
    gliding_ = true;
    subCounter_ = 0;
    return matched > 0;
}

// ───────────────────────────── switch topology (CompileOptions::openSwitches) ─────────────────────────────
bool Circuit::topologyStale(const std::vector<double>* controls) const noexcept {
    // A contact left out that should now close cannot (stale), and one compiled closed that is now open might leave the
    // partition; a contact compiled open that kept its roff (into a high-impedance node, or one that would float) is
    // exact in either position as it stands.
    const auto& ctl = controls ? *controls : ctlCur_;
    for (const auto& s : sw_)
        if ((s.closed || s.removed) && switchClosed(net_, net_.parts[static_cast<std::size_t>(s.part)], ctl) != s.closed) return true;
    return false;
}

int Circuit::openContacts() const noexcept {
    int k = 0;
    for (const auto& s : sw_) k += s.removed ? 1 : 0;
    return k;
}

bool Circuit::recompileTopology() {
    const Netlist n = net_;
    CompileOptions o = opt_;
    o.controls = ctlCur_;
    o.settleFrom = nullptr;
    const auto target = ctlTarget_;
    const double sf = spkFs_, sq = spkQ_, sl = spkLe_, vr = vinR_;
    std::string err;
    recompiling_ = true;
    const bool ok = compile(n, o, err); // ends in reset() at these positions
    recompiling_ = false;
    for (std::size_t c = 0; c < ctlTarget_.size() && c < target.size(); ++c) ctlTarget_[c] = target[c];
    gliding_ = true;
    vinR_ = vr;
    if (sf > 0.0) setSpeakerLoad(sf, sq, sl);
    return ok;
}

// adoptStateFrom for the same netlist, rate and options compiled at other switch positions (a relay or selector moved:
// CircuitUnit rebuilds the circuit off the audio thread and swaps it in). Nodes, prims, devices and slots are the same
// index for index; only the partition differs. Each node keeps its voltage, each capacitor / inductor its branch
// voltage and current (as adoptStateByName), each device its Newton warm starts and history, and the time bases, cut
// currents, blocks, logic and speaker load carry over as in the same-structure hand-off. The new switch positions are
// this circuit's own; continuous controls continue from the old circuit's positions and glide on. Allocation-free.
bool Circuit::adoptAcrossTopology(const Circuit& o) noexcept {
    if (o.V_.size() != V_.size() || o.prims_.size() != prims_.size() || o.devices_.size() != devices_.size() || o.primStateG_.size() != prims_.size() ||
        o.devG_.size() != devices_.size() || o.ctlCur_.size() != ctlCur_.size() || o.slots_.size() != slots_.size())
        return false;
    std::copy(o.V_.begin(), o.V_.end(), V_.begin());
    phase_ = o.phase_;
    tick_ = o.tick_;
    if (o.cutI_.size() == cutI_.size()) std::copy(o.cutI_.begin(), o.cutI_.end(), cutI_.begin());
    for (std::size_t d = 0; d < devices_.size(); ++d) std::copy(std::begin(o.devices_[d].hs), std::end(o.devices_[d].hs), std::begin(devices_[d].hs));
    for (std::size_t s = 0; s < spkTh_.size() && s < o.spkTh_.size(); ++s) spkTh_[s].pPrev = o.spkTh_[s].pPrev;
    // controls: the continuous ones where the old circuit was (they glide on to this one's targets)
    for (std::size_t c = 0; c < ctlCur_.size(); ++c)
        if (net_.controls[c].kind == "continuous") ctlCur_[c] = o.ctlCur_[c];
    spkFs_ = o.spkFs_;
    spkQ_ = o.spkQ_;
    spkLe_ = o.spkLe_;
    for (std::size_t s = 0; s < slots_.size(); ++s) slots_[s].scale = o.slots_[s].scale;
    syncSpeakerDevices();
    evalSlots(true);
    for (std::size_t g = 0; g < groups_.size(); ++g) {
        refreshGroup(g, false);
        groupDirty_[g] = 0;
    }
    for (std::size_t gi = 0; gi < groups_.size(); ++gi) {
        auto& r = *rt_[gi];
        const auto& g = groups_[gi];
        for (int k = 0; k < r.ns; ++k) { // states: branch voltage and current, through the new companion conductance
            const auto pi = static_cast<std::size_t>(g.states[static_cast<std::size_t>(k)]);
            const int og = o.primStateG_[pi], ok = o.primStateK_[pi];
            if (og < 0 || ok < 0) continue;
            const auto& orr = *o.rt_[static_cast<std::size_t>(og)];
            const double vc = orr.vcLast[static_cast<std::size_t>(ok)], so = orr.w[static_cast<std::size_t>(ok)], go = orr.gc[static_cast<std::size_t>(ok)];
            r.w[static_cast<std::size_t>(k)] = carriedState(prims_[pi], o.prims_[pi], vc, so, go, r.gc[static_cast<std::size_t>(k)]);
            r.vcLast[static_cast<std::size_t>(k)] = vc;
        }
        for (int di : g.devices) { // devices: Newton warm starts and the predictor / active-set history, port by port
            const auto d = static_cast<std::size_t>(di);
            const int og = o.devG_[d], oo = o.devPort_[d], mo = devPort_[d];
            if (og < 0 || oo < 0 || mo < 0) continue;
            const auto& orr = *o.rt_[static_cast<std::size_t>(og)];
            for (int k = 0; k < devices_[d].nports; ++k) {
                const auto a = static_cast<std::size_t>(mo + k), b = static_cast<std::size_t>(oo + k);
                if (a >= r.vp.size() || b >= orr.vp.size()) break;
                r.vp[a] = orr.vp[b];
                r.vpPrev[a] = orr.vpPrev[b];
                r.vpPrev2[a] = orr.vpPrev2[b];
                if (a < r.curPrev1.size() && b < orr.curPrev1.size()) r.curPrev1[a] = orr.curPrev1[b];
                if (a < r.curPrev2.size() && b < orr.curPrev2.size()) r.curPrev2[a] = orr.curPrev2[b];
            }
        }
        if (r.wdf) r.wdf->resetFrom(*this, g, r.w.data(), r.vcLast.data());
        // inputs and cut injections as the adopted node voltages and cut currents give them; p from them (the homotopy
        // anchor); the LTE and predictor histories restart (they hold this partition's states)
        for (int j = 0; j < r.nu; ++j) r.w[static_cast<std::size_t>(r.ns + j)] = V_[static_cast<std::size_t>(g.inputs[static_cast<std::size_t>(j)])];
        for (int j = 0; j < r.nuc; ++j) {
            const auto x = static_cast<std::size_t>(g.cutIn[static_cast<std::size_t>(j)]);
            r.w[static_cast<std::size_t>(r.ns + r.nu + r.nn + j)] = -(cutI_[x] - tranLoadG_[x] * V_[x]);
        }
        for (int k = 0; k < r.nn; ++k) {
            double sum = 0;
            for (int c = 0; c < r.ns + r.nu; ++c) sum += r.Dp[static_cast<std::size_t>(k * (r.ns + r.nu) + c)] * r.w[static_cast<std::size_t>(c)];
            for (int c = 0; c < r.nuc; ++c) sum += r.Dc[static_cast<std::size_t>(k * r.nuc + c)] * r.w[static_cast<std::size_t>(r.ns + r.nu + r.nn + c)];
            r.pPrev[static_cast<std::size_t>(k)] = sum;
        }
        r.predHist = 0;
        r.lteWarm = 0;
        r.subNext = 1;
        r.subPrev = 1;
    }
    auto park = [&](auto& mine, const auto& theirs) {
        if (mine == theirs) return;
        if (mine && retired_.size() < retired_.capacity()) retired_.push_back(std::move(mine));
        else if (mine) return;
        mine = theirs;
    };
    for (std::size_t b = 0; b < blocks_.size() && b < o.blocks_.size(); ++b) {
        park(blocks_[b].tank, o.blocks_[b].tank);
        blocks_[b].y = o.blocks_[b].y;
    }
    for (std::size_t b = 0; b < bbds_.size() && b < o.bbds_.size(); ++b) park(bbds_[b].line, o.bbds_[b].line);
    for (std::size_t b = 0; b < tapes_.size() && b < o.tapes_.size(); ++b) park(tapes_[b].loop, o.tapes_[b].loop);
    for (std::size_t c = 0; c < logic_.size() && c < o.logic_.size(); ++c) logic_[c].adoptRuntime(o.logic_[c]);
    // rate converters: carried where this partition has the same ones, else primed from the adopted node voltages
    bool conv = false;
    if (ladder_ && o.ladder_ && o.blk_.size() == blk_.size() && o.lconv_.size() == lconv_.size()) {
        bool same = true;
        for (std::size_t c = 0; c < lconv_.size() && same; ++c)
            same = lconv_[c].node == o.lconv_[c].node && lconv_[c].from == o.lconv_[c].from && lconv_[c].to == o.lconv_[c].to;
        bool sameCuts = ladderCuts_ == o.ladderCuts_ && lcut_.size() == o.lcut_.size() && lcutConv_.size() == o.lcutConv_.size() && cutPlay_.size() == o.cutPlay_.size();
        for (std::size_t e = 0; e < lcut_.size() && sameCuts; ++e)
            sameCuts = lcut_[e].group == o.lcut_[e].group && lcut_[e].node == o.lcut_[e].node && lcut_[e].owner == o.lcut_[e].owner && lcut_[e].conv == o.lcut_[e].conv;
        if (same && sameCuts) {
            std::copy(o.blk_.begin(), o.blk_.end(), blk_.begin());
            for (std::size_t c = 0; c < lconv_.size(); ++c) lconv_[c] = o.lconv_[c];
            if (ladderCuts_) {
                std::copy(o.cutPlay_.begin(), o.cutPlay_.end(), cutPlay_.begin());
                std::copy(o.cutNext_.begin(), o.cutNext_.end(), cutNext_.begin());
                for (std::size_t e = 0; e < lcut_.size(); ++e) lcut_[e].buf = o.lcut_[e].buf;
                for (std::size_t c = 0; c < lcutConv_.size(); ++c) lcutConv_[c] = o.lcutConv_[c];
            }
            conv = true;
        }
    }
    if (multirate_ && o.multirate_ && o.conv_.size() == conv_.size() && o.upOf_ == upOf_ && o.downOf_ == downOf_) {
        std::copy(o.conv_.begin(), o.conv_.end(), conv_.begin());
        std::copy(o.lo_.begin(), o.lo_.end(), lo_.begin());
        std::copy(o.hi0_.begin(), o.hi0_.end(), hi0_.begin());
        std::copy(o.hi1_.begin(), o.hi1_.end(), hi1_.begin());
        conv = true;
    }
    if (!conv && (ladder_ || multirate_)) primeConverters();
    if (nEvo_) { // docs §10.48: the output streams carry over when the other circuit has the same ones
        bool same = o.nEvo_ == nEvo_ && o.evosM_ == evosM_ && o.evosP_ == evosP_;
        for (int i = 0; i < nEvo_ && same; ++i) same = o.evo_[static_cast<std::size_t>(i)].node == evo_[static_cast<std::size_t>(i)].node && o.evo_[static_cast<std::size_t>(i)].nst == evo_[static_cast<std::size_t>(i)].nst;
        if (same)
            for (int i = 0; i < nEvo_; ++i) {
                auto& a = evo_[static_cast<std::size_t>(i)];
                const auto& b = o.evo_[static_cast<std::size_t>(i)];
                a.s = b.s, a.sub = b.sub, a.ev = b.ev, a.rec = b.rec, a.head = b.head, a.st = b.st, a.zh = b.zh, a.zhHead = b.zhHead;
            }
        else evosPrime();
    }
    gliding_ = true;
    subCounter_ = 0;
    return true;
}

// ───────────────────────────── process ─────────────────────────────
void Circuit::processGroup(std::size_t gi) noexcept {
    const auto& g = groups_[gi];
    auto& r = *rt_[gi];
    const int ns = r.ns, nu = r.nu, W = r.W;
    if (W == 0 && g.cutTerms.empty() && !r.wdf) return;
    // (§10.53) settlePeriodic's cold run: a group that failed and held its ports on the last sample, and fails Newton
    // again, is not rescued; the next sample's Newton tries again.
    const bool quick = pssQuick_ && r.pssHeld;
    r.pssHeld = 0;
    double* __restrict w = r.w.data();
    {
        const double* Vn = V_.data();
        const int* in = g.inputs.data();
        for (int j = 0; j < nu; ++j) w[ns + j] = Vn[in[j]];
        const double* gL = tranLoadG_.data();
        for (int j = 0; j < r.nuc; ++j) {
            const auto x = static_cast<std::size_t>(g.cutIn[static_cast<std::size_t>(j)]);
            const double ci = ladderCuts_ ? cutPlay_[x * 32 + static_cast<std::size_t>(ladderSub_)] : cutI_[x]; // see LadderCut
            w[ns + nu + r.nn + j] = -(ci - gL[x] * Vn[x]);
        }
    }
    if (r.noiseOn && --r.noiseCount <= 0) {
        r.noiseCount = r.noiseDiv;
        updateNoise(r, g);
    }
    // LTE control inside an event: the last (substepped) sample says a full step would be over tolerance again, so go
    // straight to substeps instead of solving and rejecting the full step first.
    bool outDone = false; // r.out already holds H·w (the LTE check computed it)
    bool failed = false;  // Newton and every rescue failed: the last accepted port voltages were kept
    // CE_LTESTAT: a substepped sample (kind 0 detected, 1 continued) with m substeps, and the next sample's decision
    auto lteStat = [&](int kind, int m, double ratio) {
        auto bucket = [](double x) { return x <= 1 ? 0 : x <= 2 ? 1 : x <= 4 ? 2 : x <= 8 ? 3 : x <= 27 ? 4 : x <= 64 ? 5 : x <= 512 ? 6 : 7; };
        ++r.stM[kind][std::min(m, 4)];
        ++r.stR[kind][bucket(ratio)];
        if (kind == 1) ++r.stRegime[nonlinearEventAt(r, r.vpPrev.data(), r.vpPrev2.data()) ? 0 : 1];
        r.stLen = kind == 0 ? 1 : r.stLen + 1;
        ++r.stM[2][std::min(r.subNext, 4)];
        ++r.stR[2][bucket(r.lteLastRatio)];
        if (r.subNext <= 1) ++r.stChain[std::min(r.stLen, 64)], r.stLen = 0;
    };
    if (r.lteOn && r.subNext > 1) {
        r.predHist = 0;
        std::chrono::steady_clock::time_point tS0;
        if (profile_) tS0 = std::chrono::steady_clock::now();
        const bool okCont = substepGroup(gi, r.subNext, 2);
        if (profile_) r.subNs += std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - tS0).count();
        if (okCont) {
            ++dg(gi).lteSubsteps;
            ++r.lteEvents;
            pushLteHistory(r);
            const double ratio = r.lteLastRatio;
            const int m = r.subNext;
            r.subNext = r.tb ? r.tbNext : lteSubstepsFromHistory(r); // TR-BDF2: its embedded estimate (§10.47)
            if (kEnvLteStat) lteStat(1, r.subPrev > 1 ? r.subPrev : m, ratio);
            return;
        }
        if (kEnvLteStat && r.stLen) ++r.stChain[std::min(r.stLen, 64)], r.stLen = 0;
        r.subNext = 1;
    }
    if (r.wdf) {
        r.wdf->process(*this, g, w, r.out.data());
    } else if (r.nn > 0) {
        double* p = r.p.data();
        const int sw = ns + nu;
        (void)sw;
        linPorts(r, w, p); // Dp·w + Dc·cut (+ noise): dense, or the sparse solve (SparseLin)
        const bool ssHit = r.ss && r.ss->on && ssSolve(gi, r, p); // GR_LESSONS P2: the certified frozen solve, no Newton
        // Predictor: linear extrapolation of the port voltages (pnjlim keeps it safe on junctions).
        double* __restrict vp = r.vp.data();
        double* __restrict v1 = r.vpPrev.data();
        double* __restrict v2 = r.vpPrev2.data();
        const bool quad = r.nn > r.quadPorts && r.predHist >= 3 && !kEnvNoPredQ; // three samples of history (r.quadPorts: kPredQuadPorts or §10.50)
        if (ssHit) {
        } else if (opt_.predictor && quad) {
            const double* __restrict v3 = r.vpPrev3.data();
            for (int k = 0; k < r.nn; ++k) vp[k] = 3.0 * (v1[k] - v2[k]) + v3[k];
            limitPredicted(r.nn, r.lim.data(), r.limA.data(), r.limB.data(), r.limC.data(), v1, vp);
        } else if (opt_.predictor) {
            for (int k = 0; k < r.nn; ++k) vp[k] = 2.0 * v1[k] - v2[k];
            limitPredicted(r.nn, r.lim.data(), r.limA.data(), r.limB.data(), r.limC.data(), v1, vp);
        }
        // ...corrected (in the Newton kernel) for the part of p's change that is not on its own linear trend
        const int hist = r.predHist;
        r.predHist = 0; // stays 0 if this sample leaves early (step halving)
        if (!ssHit && opt_.predictor && hist >= 2 && (r.ks.valid || r.ksR.valid)) {
            double* __restrict dq = r.dpPred.data();
            const double* __restrict p1 = r.pPrev.data();
            const double* __restrict p2 = r.pPrev2.data();
            // every sample, except small groups already near one iteration a solve (kPredSmallPorts, kPredEveryAboveIt)
            const double th = (r.itAvg > kPredEveryAboveIt || r.nn > kPredSmallPorts ? kEnvPredTh : kPredAttackTh) * r.tol;
            bool step = false;
            for (int k = 0; k < r.nn; ++k) {
                dq[k] = quad ? p[k] - (3.0 * (p1[k] - p2[k]) + r.pPrev3[static_cast<std::size_t>(k)]) : p[k] - (2.0 * p1[k] - p2[k]);
                step |= std::abs(dq[k]) > th * (1.0 + std::abs(vp[k]));
            }
            if (step) r.predDp = dq; // an input step (a pick attack): correct the start
        }
        std::chrono::steady_clock::time_point tN0;
        if (profile_) tN0 = std::chrono::steady_clock::now();
        r.pssNoPtc = quick ? 1 : 0;
        const bool okN = ssHit || newton(r, g, p, opt_.maxIterations);
        r.pssNoPtc = 0;
        bool solved = true; // false: the last accepted port voltages were kept
        r.predDp = nullptr;
        if (profile_) newtonNs_[gi] += std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - tN0).count();
        if (!okN && debugFail_ && diag_.homotopies < 3) {
            std::fprintf(stderr, "homotopy: sample %llu group %zu\n  p    ", static_cast<unsigned long long>(diag_.samples), gi);
            for (int k = 0; k < r.nn; ++k) std::fprintf(stderr, " %.6g", p[k]);
            std::fprintf(stderr, "\n  pPrev");
            for (int k = 0; k < r.nn; ++k) std::fprintf(stderr, " %.6g", r.pPrev[static_cast<std::size_t>(k)]);
            std::fprintf(stderr, "\n  vPrev");
            for (int k = 0; k < r.nn; ++k) std::fprintf(stderr, " %.6g", r.vpPrev[static_cast<std::size_t>(k)]);
            std::fprintf(stderr, "\n");
        }
        if (!okN) {
            // Homotopy in p: the nonlinear equation depends on states and inputs only through p, so the
            // path from last sample's p to this one needs no matrix change and no state rollback.
            ++dg(gi).homotopies;
            bool ok = !quick;
            for (int steps : {4, 16}) {
                if (quick) break;
                std::copy(r.vpPrev.begin(), r.vpPrev.end(), r.vp.begin());
                ok = true;
                for (int s = 1; s <= steps && ok; ++s) {
                    const double lam = static_cast<double>(s) / steps;
                    for (int k = 0; k < r.nn; ++k) r.F[static_cast<std::size_t>(k)] = 0; // scratch
                    double pl[kMaxNlPorts];
                    for (int k = 0; k < r.nn && k < kMaxNlPorts; ++k) pl[k] = r.pPrev[static_cast<std::size_t>(k)] + lam * (p[k] - r.pPrev[static_cast<std::size_t>(k)]);
                    ok = newton(r, g, pl, opt_.maxIterations);
                }
                if (ok) break;
            }
            if (!ok && !quick && rtHalf_[gi] && substepGroup(gi, 2, 0)) { // the half steps finished the sample
                ++dg(gi).substeps;
                if (r.lteOn) pushLteHistory(r);
                return;
            }
            if (!ok && !quick) {
                std::copy(r.vpPrev.begin(), r.vpPrev.end(), r.vp.begin());
                ok = newtonPtc(r, g, p, 200);
                if (ok) ++dg(gi).ptcRescues;
            }
            if (!ok) {
                ++dg(gi).failures;
                if (debugFail_) {
                    std::fprintf(stderr, "newton failure: sample %llu group %zu\n  p   ", static_cast<unsigned long long>(diag_.samples), gi);
                    for (int k = 0; k < r.nn; ++k) std::fprintf(stderr, " %.6g", p[k]);
                    std::fprintf(stderr, "\n  pPrev");
                    for (int k = 0; k < r.nn; ++k) std::fprintf(stderr, " %.6g", r.pPrev[static_cast<std::size_t>(k)]);
                    std::fprintf(stderr, "\n  vpPrev");
                    for (int k = 0; k < r.nn; ++k) std::fprintf(stderr, " %.6g", r.vpPrev[static_cast<std::size_t>(k)]);
                    std::fprintf(stderr, "\n");
                    static int traced = 0;
                    if (traced++ < 1) {
                        std::copy(r.vpPrev.begin(), r.vpPrev.end(), r.vp.begin());
                        traceNewton_ = true;
                        newtonPtc(r, g, p, 200);
                        traceNewton_ = false;
                    }
                }
                std::copy(r.vpPrev.begin(), r.vpPrev.end(), r.vp.begin());
                solved = false;
                failed = true;
                // currents consistent with the kept port voltages
                int o = 0;
                double* cur = &w[ns + nu];
                for (int di : g.devices) {
                    const auto& d = devices_[static_cast<std::size_t>(di)];
                    double Jl[9];
                    d.eval(r.vp.data() + o, cur + o, Jl);
                    o += d.nports;
                }
            }
        }
        // Regenerative loop (ENGINE_QUEUE 25): a solution that jumped away from the last one (a comparator switching) is
        // accepted only if the old branch is gone. Newton restarted from the last solution itself, without the
        // extrapolating predictor, must not find a nearby solution; if it does, the jump was premature (Newton's start,
        // a chord or an attack-predictor correction landed it on the far branch while the near one still existed) and
        // the near solution is kept. The switch is then the first sample past the fold, whatever the solver path.
        if (r.regen && solved) {
            double jump = 0;
            for (int k = 0; k < r.nn; ++k) jump = std::max(jump, std::abs(vp[k] - v1[k]));
            if (jump > kRegenJump) {
                double* __restrict cw = w + ns + nu;
                std::copy(vp, vp + r.nn, r.vpAlt.data());
                std::copy(cw, cw + r.nn, r.curAlt.data());
                std::copy(v1, v1 + r.nn, vp);
                bool near = newton(r, g, p, opt_.maxIterations);
                if (near) {
                    double d2 = 0;
                    for (int k = 0; k < r.nn; ++k) d2 = std::max(d2, std::abs(vp[k] - v1[k]));
                    near = d2 < 0.25 * jump;
                }
                if (!near) {
                    std::copy(r.vpAlt.begin(), r.vpAlt.end(), vp);
                    std::copy(r.curAlt.begin(), r.curAlt.end(), cw);
                }
            }
        }
        r.pdOk = solved; // vp is the accepted solution: linOut reads the port-defined states from it
        r.pssHeld = pssQuick_ && failed ? 1 : 0; // (§10.53)
        if (r.noiseOn) { // shot noise: the port currents the rest of the group sees include it
            const double* __restrict dI = r.dI.data();
            double* __restrict cw = w + ns + nu;
            for (int k = 0; k < r.nn; ++k) cw[k] += dI[k];
        }
        // LTE control: the step just solved stands if its truncation-error estimate is within tolerance; otherwise the
        // sample is re-solved as substeps from the same start (nothing has been committed yet).
        if (r.lteOn && solved) {
            linOut(r, w, r.out.data());
            outDone = true;
            r.pdOk = false;
            const int m = lteSubsteps(r);
            std::chrono::steady_clock::time_point tS0;
            if (profile_ && m > 1) tS0 = std::chrono::steady_clock::now();
            r.subDetect = true;
            const bool okRej = m > 1 && substepGroup(gi, m, 2);
            r.subDetect = false;
            if (profile_ && m > 1) r.subNs += std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - tS0).count();
            if (okRej) {
                ++r.lteDetect;
                ++dg(gi).lteSubsteps;
                ++r.lteEvents;
                pushLteHistory(r);
                const double ratio = r.lteLastRatio;
                r.subNext = r.tb ? r.tbNext : lteSubstepsFromHistory(r); // TR-BDF2: its embedded estimate (§10.47)
                    if (kEnvLteStat) lteStat(0, r.subPrev > 1 ? r.subPrev : m, ratio);
                return;
            }
        }
        double* __restrict pp = r.pPrev.data();
        double* __restrict pp2 = r.pPrev2.data();
        double* __restrict v3 = r.vpPrev3.data();
        double* __restrict pp3 = r.pPrev3.data();
        for (int k = 0; k < r.nn; ++k) {
            v3[k] = v2[k];
            v2[k] = v1[k];
            v1[k] = vp[k];
            pp3[k] = pp2[k];
            pp2[k] = pp[k];
            pp[k] = p[k];
        }
        r.predHist = std::min(hist + 1, 3);
        if (r.ss && !ssHit && --r.ss->wait < 0) ssAfter(gi, r, failed); // GR_LESSONS P2: freeze a linearisation when the ports are quiet
        if (r.nn <= kMaxNlPorts) {
            const double* cw = w + ns + nu;
            for (int k = 0; k < r.nn; ++k) {
                r.curPrev2[static_cast<std::size_t>(k)] = r.curPrev1[static_cast<std::size_t>(k)];
                r.curPrev1[static_cast<std::size_t>(k)] = cw[k];
            }
        }
        for (const auto& [di, o] : g.hyst) devices_[static_cast<std::size_t>(di)].advancePlay(vp[o]);
        if (!g.spk.empty()) advanceSpeakers(g, vp, g.T);
    }
    double* __restrict out = r.out.data();
    if (!r.wdf && !outDone) linOut(r, w, out);
    r.pdOk = false;
    double* __restrict Vn = V_.data();
    const int* ex = g.exports.data();
    if (failed) {
        // A failed sample holds its last port voltages; whatever its inputs did, a non-finite or absurd node voltage or
        // state must not leave the group (a NaN state never recovers, and a rail or cut node feeds every other group):
        // such entries keep their last values.
        for (int e = 0; e < r.ny; ++e)
            if (!(std::abs(out[e]) < 1e9)) out[e] = Vn[ex[e]];
        for (int k = 0; k < ns; ++k)
            if (!(std::abs(out[r.ny + k]) < 1e9)) out[r.ny + k] = r.vcLast[static_cast<std::size_t>(k)];
    }
    for (int e = 0; e < r.ny; ++e) {
        const double v = out[e];
        Vn[ex[e]] = (v > -1e9 && v < 1e9) ? v : 0.0;
    }
    if (!g.cutTerms.empty()) accumulateCutTerms(gi, false); // before the state update: companions need s[n]
    if (!r.wdf) {
        double* __restrict vcl = r.vcLast.data();
        const double* __restrict G2 = r.G2.data();
        const double* __restrict sg = r.sgn.data();
        for (int k = 0; k < ns; ++k) {
            const double vc = out[r.ny + k];
            vcl[k] = vc;
            w[k] = G2[k] * vc + sg[k] * w[k];
        }
    }
    if (rtHalf_[gi]) {
        double* up = r.uPrev.data();
        for (int j = 0; j < nu; ++j) up[j] = w[ns + j];
        for (int j = 0; j < r.nuc; ++j) up[nu + j] = w[ns + nu + r.nn + j];
    }
    r.subPrev = 1;
    if (r.lteOn) pushLteHistory(r);
}

// ───────────────────────────── Small-signal path (GR_LESSONS §4 P2; docs §10.37) ─────────────────────────────
// A group whose ports sit still (silence, a decayed note, a stage the signal does not reach) solves a frozen linear
// system instead of Newton, for as long as that is certified to land within a small fraction σ of the Newton tolerance.
//
// The frozen system. At a converged solution v0 the devices are evaluated once: currents i0, Jacobian Jd0 (block
// diagonal, a device's ports only). The linearised law i_lin(v) = i0 + Jd0·(v − v0) in v = p + K·i gives
//     v_L = v0 + S·(p + q0),   S = (I − K·Jd0)⁻¹,   q0 = K·i0 − v0,
// one nn×nn product per sample and no device evaluation or factorisation. The currents are i_lin(v_L).
//
// The certificate. The exact solution v* = p + K·i(v*) differs from v_L by e = S·K·r(v*), r(v) = i(v) − i_lin(v) the
// linearisation's remainder, and the group's outputs (exports and state voltages, out = H·w, whose current columns Hi
// read i_lin) by G·r(v*), G = Hi·(I + Jd0·S·K). With m_j the largest |(S·K)_kj| / (1 + |v0_k|) and |G_ej| / (1 + |out_e|)
// over the rows, each port j gets a remainder budget ε_j = λ·σ·tol / (n·m_j) and each device a box around v0 inside
// which its remainder stays within the budget of every one of its ports (searched at the freeze: along each port's axis
// by doubling and bisection, then all corners and edge midpoints of the device's box at full and half size, halving
// the widest port while a cross term fails). λ = n, then √n, then 1: the larger shares let ports with no remainder
// (reverse-biased junctions, open clamps) pass theirs to the curved ones, and are kept only if the exact rows pass with
// the remainders R_j sampled in the final boxes: η_k = Σ_j |S·K|_kj·R_j ≤ σ·tol·(1 + |v0_k|) and Σ_j |G_ej|·R_j ≤
// σ·tol·(1 + |out_e|) (at λ = 1 they always do). A sample takes v_L when every port lies at least η_k inside its box:
// the map v ↦ v_L + S·K·r(v) then sends the η-ball around v_L into itself, so the exact solution is within η of v_L.
// Any port outside (or a NaN) ends the stretch and the sample runs Newton. Per sample the test is 2·nn comparisons.
//
// σ = 1e-4. The remainder of a frozen linearisation has one sign over a stretch (it is quadratic), and a long time
// constant integrates it: the dbx 160's log-domain RMS detector turned a 1e-7-level per-sample error (σ = 0.1) into
// −78 dBFS peak on a DI, where tightening Newton's own tolerance moves it by −115 dBFS. At σ = 1e-4 every measured null
// is at or below that (docs §10.37). The sample boxes are not interval bounds: a law that bends between the sampled
// points inside a box would escape them (none of the library's laws does at these box sizes; CE_SSCHECK measures it).
//
// When. A freeze (devices at v0, one LU and its inverse, S·K and G, the box search) is tried after a Newton sample
// once the ports move less per sample than 1/kSsMotion of the last box; the search stops at the first device whose box
// the recent motion would cross within kSsMotion samples. A stretch shorter than kSsShortRun samples, or a freeze
// that does not certify, doubles the wait before the next try (up to kSsMaxBackoff samples). K changing (controls,
// refreshes: deriveFromZ) ends the stretch. Never on groups with hidden device state (hysteresis cores, speakers'
// thermal ΔRe), WDF groups, regenerative loops, or groups whose device-port currents another group reads (cut terms).
//
// Off switch: CE_NOSMALLSIG=1 or CompileOptions::smallSignal = false (Newton on every sample, bit-identical to the
// engine without the path). Developer: CE_SSSIGMA sets σ; CE_SSCHECK=1 runs Newton from v_L on every hit and records
// the worst port / output error in tolerances for describeGroups (the render keeps v_L); CE_SSDIAG=1 prints every
// freeze's box; CE_SSONLY=<group> limits the path to one group.
namespace {
const bool kEnvNoSmallSig = std::getenv("CE_NOSMALLSIG") != nullptr;
const double kSsSigma = std::getenv("CE_SSSIGMA") ? std::atof(std::getenv("CE_SSSIGMA")) : 1e-4;
const bool kEnvSsCheck = std::getenv("CE_SSCHECK") != nullptr;
const bool kEnvSsDiag = std::getenv("CE_SSDIAG") != nullptr; // developer: print every freeze's box
constexpr double kSsMotion = 8.0;     // a freeze needs the ports to move under 1/8 of the box per sample
constexpr std::uint64_t kSsShortRun = 256;
constexpr int kSsMinBackoff = 16, kSsMaxBackoff = 32768;
constexpr double kSsRhoStart = 1e-3, kSsRhoMin = 1e-8, kSsRhoMax = 64.0; // box search range (V)
} // namespace

void Circuit::buildSmallSignal() {
    for (std::size_t gi = 0; gi < rt_.size(); ++gi) {
        auto& r = *rt_[gi];
        r.ss.reset();
        const auto& g = groups_[gi];
        if (!opt_.smallSignal || kEnvNoSmallSig || r.wdf || r.nn <= 0 || r.nn > kMaxNlPorts || !g.hyst.empty() || !g.spk.empty()) continue;
        bool ok = true;
        for (int di : g.devices) {
            const auto k = devices_[static_cast<std::size_t>(di)].kind;
            ok &= k != NlKind::Magnetizing && k != NlKind::Speaker;
        }
        for (const auto& ct : g.cutTerms) ok &= ct.kind != 3; // a device port's current read by another group: not certified here
        if (!ok) continue;
        if (const char* only = std::getenv("CE_SSONLY"); only && std::atoi(only) != static_cast<int>(gi)) continue; // developer: one group
        auto s = std::make_unique<SmallSig>();
        const auto n = static_cast<std::size_t>(r.nn);
        for (auto* v : {&s->v0, &s->i0, &s->q0, &s->lo, &s->hi, &s->rho, &s->rinv, &s->eps}) v->assign(n, 0.0);
        s->Jd0.assign(3 * n, 0.0);
        for (auto* v : {&s->S, &s->A, &s->SK}) v->assign(n * n, 0.0);
        s->G.assign(static_cast<std::size_t>(r.ny + r.ns) * n, 0.0);
        s->piv.assign(n, 0);
        s->backoff = kSsMinBackoff;
        r.ss = std::move(s);
    }
}

bool Circuit::ssSolve(std::size_t gi, GroupRt& r, const double* p) noexcept {
    auto& s = *r.ss;
    const int nn = r.nn;
    double t[kMaxNlPorts], v[kMaxNlPorts];
    const double* __restrict q0 = s.q0.data();
    for (int k = 0; k < nn; ++k) t[k] = p[k] + q0[k];
    gemvB(s.S.data(), nn, nn, t, v, s.v0.data()); // v = S·t + v0
    const double* __restrict lo = s.lo.data();
    const double* __restrict hi = s.hi.data();
    bool in = true;
    for (int k = 0; k < nn; ++k) in &= v[k] >= lo[k] && v[k] <= hi[k]; // NaN fails
    if (!in) {
        s.on = false;
        ++s.exits;
        s.backoff = s.run < kSsShortRun ? std::min(2 * s.backoff, kSsMaxBackoff) : kSsMinBackoff;
        s.wait = s.backoff;
        return false;
    }
    double* __restrict vp = r.vp.data();
    double* __restrict cur = &r.w[static_cast<std::size_t>(r.ns + r.nu)];
    const double* __restrict v0 = s.v0.data();
    const double* __restrict i0 = s.i0.data();
    const double* __restrict J = s.Jd0.data();
    const int* bs = r.bs.data();
    const int* bn = r.bn.data();
    for (int k = 0; k < nn; ++k) {
        const int b0 = bs[k];
        double c = i0[k];
        for (int b = 0; b < bn[k]; ++b) c += J[3 * k + b] * (v[b0 + b] - v0[b0 + b]);
        cur[k] = c;
        vp[k] = v[k];
    }
    ++s.hits;
    ++s.run;
    if (kEnvSsCheck) { // developer: the Newton solution from v_L, compared and then discarded
        double keepC[kMaxNlPorts];
        std::copy(cur, cur + nn, keepC);
        if (newton(r, groups_[gi], p, opt_.maxIterations)) {
            double worst = 0;
            for (int k = 0; k < nn; ++k) worst = std::max(worst, std::abs(vp[k] - v[k]) / (r.tol * (1.0 + std::abs(vp[k]))));
            s.checkWorst = std::max(s.checkWorst, worst);
            { // the outputs: Hi·(i_Newton − i_lin) against tol·(1 + |out|)
                const int nOut = r.ny + r.ns, ci = r.ns + r.nu;
                for (int e = 0; e < nOut; ++e) {
                    double d = 0;
                    for (int j = 0; j < nn; ++j) d += r.H[static_cast<std::size_t>(e) * static_cast<std::size_t>(r.W) + static_cast<std::size_t>(ci + j)] * (cur[j] - keepC[j]);
                    const double we = std::abs(d) / (r.tol * (1.0 + std::abs(r.out[static_cast<std::size_t>(e)])));
                    if (kEnvSsDiag && we > 1.0 && we > s.checkWorst)
                        std::fprintf(stderr, "ssCheck group %zu hit %llu: output row %d off by %.3g x tol (out %.6g)\n", gi, static_cast<unsigned long long>(s.hits), e, we, r.out[static_cast<std::size_t>(e)]);
                    worst = std::max(worst, we);
                }
                s.checkWorst = std::max(s.checkWorst, worst);
            }
            if (kEnvSsDiag && worst > 1.0) {
                std::fprintf(stderr, "ssCheck group %zu hit %llu: worst %.3g x tol\n", gi, static_cast<unsigned long long>(s.hits), worst);
                for (int k = 0; k < nn; ++k) std::fprintf(stderr, "  port %d v_L %.9g newton %.9g box %.9g..%.9g\n", k, v[k], vp[k], lo[k], hi[k]);
            }
        }
        std::copy(v, v + nn, vp);
        std::copy(keepC, keepC + nn, cur);
    }
    return true;
}

void Circuit::ssAfter(std::size_t gi, GroupRt& r, bool failed) noexcept {
    // processGroup calls this after a Newton sample whose countdown (wait) has run out
    (void)gi;
    auto& s = *r.ss;
    s.wait = r.regen ? kSsMaxBackoff : 0; // a regenerative loop never takes the path: look rarely
    if (s.on || failed || r.regen || r.dcMode || r.predHist < 2) return;
    const double* v1 = r.vpPrev.data(); // this sample's solution (the history has shifted)
    const double* v2 = r.vpPrev2.data();
    if (s.hadBox)
        for (int k = 0; k < r.nn; ++k)
            if (kSsMotion * std::abs(v1[k] - v2[k]) > s.rho[static_cast<std::size_t>(k)]) {
                s.wait = kSsMinBackoff; // still moving: look again shortly (cheap)
                return;
            }
    ++s.tries;
    std::chrono::steady_clock::time_point tF;
    if (profile_) tF = std::chrono::steady_clock::now();
    const bool frozen = ssFreeze(r);
    if (profile_) {
        const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - tF).count();
        s.freezeNs += ns;
        s.freezeMaxNs = std::max(s.freezeMaxNs, ns);
    }
    if (!frozen) {
        ++s.rejects;
        s.wait = s.backoff;
        s.backoff = std::min(2 * s.backoff, kSsMaxBackoff);
    }
}

bool Circuit::ssFreeze(GroupRt& r) noexcept {
    auto& s = *r.ss;
    const int nn = r.nn, ndev = static_cast<int>(r.dev.size());
    const double tol = r.tol, sig = kSsSigma;
    const double* __restrict K = r.K.data();
    double* __restrict v0 = s.v0.data();
    double* __restrict i0 = s.i0.data();
    double* __restrict Jd0 = s.Jd0.data();
    const int* bs = r.bs.data();
    const int* bn = r.bn.data();
    std::copy(r.vpPrev.begin(), r.vpPrev.begin() + nn, v0); // the solution just committed
    for (int q = 0, o = 0; q < ndev; ++q) {
        const NlDevice& d = *r.dev[static_cast<std::size_t>(q)];
        double Jl[9];
        d.eval(v0 + o, i0 + o, Jl);
        for (int a = 0; a < d.nports; ++a)
            for (int b = 0; b < 3; ++b) Jd0[3 * (o + a) + b] = b < d.nports ? Jl[a * d.nports + b] : 0.0;
        o += d.nports;
    }
    // A = I − K·Jd0 (Jd0 block diagonal), its LU, S = A⁻¹
    double* __restrict A = s.A.data();
    for (int a = 0; a < nn; ++a)
        for (int c = 0; c < nn; ++c) {
            double x = a == c ? 1.0 : 0.0;
            const int b0 = bs[c];
            for (int m = b0; m < b0 + bn[c]; ++m) x -= K[a * nn + m] * Jd0[3 * m + (c - b0)];
            A[a * nn + c] = x;
        }
    for (int k = 0; k < nn * nn; ++k)
        if (!std::isfinite(A[k])) return false;
    if (!luSmall(A, nn, s.piv.data(), s.rinv.data())) return false;
    double* __restrict S = s.S.data();
    for (int j = 0; j < nn; ++j) {
        double col[kMaxNlPorts];
        for (int k = 0; k < nn; ++k) col[k] = k == j ? 1.0 : 0.0;
        luSolveSmall(A, nn, s.piv.data(), s.rinv.data(), col);
        for (int k = 0; k < nn; ++k) S[k * nn + j] = col[k];
    }
    for (int k = 0; k < nn; ++k) {
        double x = -v0[k];
        for (int j = 0; j < nn; ++j) x += K[k * nn + j] * i0[j];
        s.q0[static_cast<std::size_t>(k)] = x;
    }
    // The error a remainder r makes: at the ports e = S·K·r; in the exports and state voltages (out = H·w, whose current
    // columns Hi read the currents i_lin(v_L) = i(v*) − r − Jd0·e) Hi·(I + Jd0·S·K)·r =: G·r. Both as absolute values,
    // with their column maxima m_j over rows weighted 1 / (1 + |row value|) (ports at v0, outputs at the last sample's).
    double* __restrict SKs = A; // the LU is no longer needed: signed S·K
    double* __restrict SK = s.SK.data();
    double m[kMaxNlPorts];
    for (int j = 0; j < nn; ++j) m[j] = 0.0;
    for (int k = 0; k < nn; ++k) {
        const double wk = 1.0 / (1.0 + std::abs(v0[k]));
        for (int j = 0; j < nn; ++j) {
            double x = 0;
            for (int c = 0; c < nn; ++c) x += S[k * nn + c] * K[c * nn + j];
            SKs[k * nn + j] = x;
            SK[k * nn + j] = std::abs(x);
            m[j] = std::max(m[j], std::abs(x) * wk);
        }
    }
    const int nOut = r.ny + r.ns, W = r.W, ci = r.ns + r.nu;
    double* __restrict G = s.G.data();
    {
        double row[kMaxNlPorts];
        const double* __restrict H = r.H.data();
        const double* __restrict outPrev = r.out.data();
        for (int e = 0; e < nOut; ++e) {
            const double* __restrict hi = H + static_cast<std::size_t>(e) * static_cast<std::size_t>(W) + ci;
            for (int j = 0; j < nn; ++j) row[j] = hi[j]; // Hi·I
            for (int a = 0; a < nn; ++a) { // + Hi·Jd0·S·K, row a of Jd0·SK on the fly
                if (hi[a] == 0.0) continue;
                const int b0 = bs[a];
                for (int mm = b0; mm < b0 + bn[a]; ++mm) {
                    const double f = hi[a] * Jd0[3 * a + (mm - b0)];
                    if (f == 0.0) continue;
                    for (int j = 0; j < nn; ++j) row[j] += f * SKs[mm * nn + j];
                }
            }
            const double we = 1.0 / (1.0 + std::abs(outPrev[e]));
            for (int j = 0; j < nn; ++j) {
                G[static_cast<std::size_t>(e) * static_cast<std::size_t>(nn) + static_cast<std::size_t>(j)] = std::abs(row[j]);
                m[j] = std::max(m[j], std::abs(row[j]) * we);
            }
        }
    }
    int nLive = 0;
    for (int j = 0; j < nn; ++j) nLive += m[j] > 0.0;
    if (nLive == 0) nLive = 1;
    double* __restrict eps = s.eps.data();
    double* __restrict lo = s.lo.data(); // half-widths first, absolute bounds at the end
    double* __restrict hi = s.hi.data();
    double R[kMaxNlPorts];               // per port: the largest remainder sampled inside its device's final box
    // The box of every device for the budgets eps (see the section comment), and R.
    const double* v2 = r.vpPrev2.data();
    bool firstPass = true;
    auto boxes = [&]() -> bool {
        for (int q = 0, o = 0; q < ndev; ++q) {
            const NlDevice& d = *r.dev[static_cast<std::size_t>(q)];
            const int np = d.nports;
            double rmax[3] = {0.0, 0.0, 0.0};
            bool track = false;
            auto bad = [&](const double* dv) {
                double vv[3], ii[3], Jl[9];
                for (int a = 0; a < np; ++a) vv[a] = v0[o + a] + dv[a];
                d.eval(vv, ii, Jl);
                bool b = false;
                for (int c = 0; c < np; ++c) {
                    double rr = ii[c] - i0[o + c];
                    for (int a = 0; a < np; ++a) rr -= Jd0[3 * (o + c) + a] * dv[a];
                    const double ar = std::abs(rr);
                    if (track) rmax[c] = std::max(rmax[c], std::isfinite(ar) ? ar : 1e300);
                    b |= !(ar <= eps[o + c]);
                }
                return b;
            };
            auto okAxis = [&](int a, double x) { // x signed
                double dv[3] = {0.0, 0.0, 0.0};
                dv[a] = x;
                if (bad(dv)) return false;
                dv[a] = 0.5 * x;
                return !bad(dv);
            };
            for (int a = 0; a < np; ++a)
                for (int side = 0; side < 2; ++side) {
                    const double sg = side ? 1.0 : -1.0;
                    double good = 0.0, badX = 0.0, x = kSsRhoStart;
                    if (okAxis(a, sg * x)) {
                        good = x;
                        while (good < kSsRhoMax) {
                            x = 4.0 * good;
                            if (!okAxis(a, sg * x)) {
                                badX = x;
                                break;
                            }
                            good = x;
                        }
                    } else {
                        badX = x;
                        for (;;) {
                            x = 0.25 * badX;
                            if (x < kSsRhoMin) break;
                            if (okAxis(a, sg * x)) {
                                good = x;
                                break;
                            }
                            badX = x;
                        }
                    }
                    if (good > 0.0 && badX > 0.0)
                        for (int it = 0; it < 4; ++it) {
                            const double mid = std::sqrt(good * badX);
                            if (okAxis(a, sg * mid)) good = mid;
                            else badX = mid;
                        }
                    (side ? hi : lo)[o + a] = std::min(good, kSsRhoMax);
                }
            // every corner and edge midpoint (and the axis points), at full and half size; the widest port is halved until
            // they all pass (a cross term: a junction's current against a wide reverse-biased port); the last sweep, all
            // passing, records the remainders R
            int ncomb = 1;
            for (int a = 0; a < np; ++a) ncomb *= 3;
            bool okAll = false;
            for (int tr = 0; tr < 64 && !okAll; ++tr) {
                okAll = true;
                for (int c = 1; c < ncomb && okAll; ++c)
                    for (double f : {1.0, 0.5}) {
                        double dv[3] = {0.0, 0.0, 0.0};
                        for (int a = 0, cc = c; a < np; ++a, cc /= 3) dv[a] = cc % 3 == 0 ? 0.0 : cc % 3 == 1 ? -f * lo[o + a] : f * hi[o + a];
                        if (bad(dv)) {
                            okAll = false;
                            break;
                        }
                    }
                if (!okAll) {
                    int wa = 0;
                    for (int a = 1; a < np; ++a)
                        if (std::max(lo[o + a], hi[o + a]) > std::max(lo[o + wa], hi[o + wa])) wa = a;
                    lo[o + wa] *= 0.5;
                    hi[o + wa] *= 0.5;
                }
            }
            if (okAll) {
                track = true;
                for (int c = 1; c < ncomb; ++c)
                    for (double f : {1.0, 0.5}) {
                        double dv[3] = {0.0, 0.0, 0.0};
                        for (int a = 0, cc = c; a < np; ++a, cc /= 3) dv[a] = cc % 3 == 0 ? 0.0 : cc % 3 == 1 ? -f * lo[o + a] : f * hi[o + a];
                        (void)bad(dv);
                    }
            } else
                for (int a = 0; a < np; ++a) lo[o + a] = hi[o + a] = rmax[a] = 0.0;
            for (int a = 0; a < np; ++a) R[o + a] = rmax[a];
            // the largest budgets give the largest box: a port already moving too fast for it, or a box with no room
            // inside its error bound even at these budgets, ends the attempt here (cheap misses)
            if (firstPass)
                for (int a = 0; a < np; ++a) {
                    const double w = std::min(lo[o + a], hi[o + a]);
                    if (kSsMotion * std::abs(v0[o + a] - v2[o + a]) > w || w <= 2.0 * sig * tol * (1.0 + std::abs(v0[o + a]))) return false;
                }
            o += np;
        }
        return true;
    };
    // Budgets: the equal share σ·tol/(n·m_j) always certifies (Σ_j m_j·ε_j = σ·tol). Larger shares (×n, ×√n) let ports whose
    // remainder is tiny (reverse-biased junctions, idle clamps) give their share to the curved ones; each is kept only if
    // the exact rows pass: η_k = Σ_j |S·K|_kj·R_j ≤ σ·tol·(1 + |v0_k|).
    double eta[kMaxNlPorts];
    bool certified = false;
    for (const double lam : {static_cast<double>(nLive), std::sqrt(static_cast<double>(nLive)), 1.0}) {
        for (int j = 0; j < nn; ++j) eps[j] = m[j] > 0.0 ? lam * sig * tol / (nLive * m[j]) : 1e300;
        if (!boxes()) {
            s.hadBox = false;
            return false;
        }
        firstPass = false;
        certified = true;
        for (int k = 0; k < nn && certified; ++k) {
            double e = 0;
            for (int j = 0; j < nn; ++j) e += SK[k * nn + j] * R[j];
            eta[k] = e;
            certified = e <= sig * tol * (1.0 + std::abs(v0[k]));
        }
        for (int e = 0; e < nOut && certified; ++e) {
            double x = 0;
            for (int j = 0; j < nn; ++j) x += G[static_cast<std::size_t>(e) * static_cast<std::size_t>(nn) + static_cast<std::size_t>(j)] * R[j];
            certified = x <= sig * tol * (1.0 + std::abs(r.out[static_cast<std::size_t>(e)]));
        }
        if (certified || lam == 1.0) break;
    }
    if (!certified) // rounding at the equal share: the column-max bound itself
        for (int k = 0; k < nn; ++k) eta[k] = sig * tol * (1.0 + std::abs(v0[k]));
    // absolute bounds η inside the box; the ports' recent motion must leave the stretch room
    bool ok = true, roomy = true;
    for (int k = 0; k < nn; ++k) {
        const double e = std::max(eta[k], 1e-300);
        const double room = std::min(lo[k], hi[k]) - e;
        s.rho[static_cast<std::size_t>(k)] = std::max(room, 0.0);
        roomy &= room > e;
        ok &= room > e && kSsMotion * std::abs(v0[k] - v2[k]) <= room;
        lo[k] = v0[k] - lo[k] + e;
        hi[k] = v0[k] + hi[k] - e;
    }
    s.hadBox = roomy; // the next attempt's cheap motion pre-check uses this box only if it had room everywhere
    if (kEnvSsDiag) {
        std::fprintf(stderr, "ssFreeze %p nn %d %s:", static_cast<void*>(&r), nn, ok ? "ok" : "rejected");
        for (int k = 0; k < nn; ++k)
            std::fprintf(stderr, " [%d v0 %.4g box %.3g..%.3g mot %.3g eta %.3g]", k, v0[k], lo[k] - v0[k], hi[k] - v0[k], std::abs(v0[k] - v2[k]), eta[k]);
        std::fprintf(stderr, "\n");
    }
    if (!ok) return false;
    s.on = true;
    s.run = 0;
    ++s.freezes;
    return true;
}

// ── Noise (CompileOptions::noise; docs §3.9) ──
// Thermal noise of the group's resistors enters as node current injections (one column per unknown node a resistor
// touches), shot noise as port currents. The response is the node-voltage vector z = M⁻¹·inj over the unknowns the
// exports, states and ports read (Zf, kept columns), gathered into the export/state offsets dOut and the port offsets
// dP (plus K·shot); the shot currents themselves join the port currents. A draw is held for noiseDiv samples (update
// rate 44.1-48 kHz) with the variance of that rate's band, and the hold's droop is compensated on the draw sequence
// (comp). Gaussians: Irwin–Hall of four bytes (variance exact, tails ±3.46σ; the circuit's filtering makes the output
// Gaussian), four per xoshiro256** word pair, two lanes at once. All per-draw arithmetic is float: noise needs no more.
namespace {
typedef float v4f __attribute__((ext_vector_type(4)));
typedef unsigned long long v2u __attribute__((ext_vector_type(2)));
typedef unsigned int v4u __attribute__((ext_vector_type(4)));
inline v4f load4f(const float* p) noexcept {
    v4f v;
    __builtin_memcpy(&v, p, sizeof v);
    return v;
}
inline void store4f(float* p, v4f v) noexcept { __builtin_memcpy(p, &v, sizeof v); }
inline v2u rotl2(v2u x, int k) noexcept { return (x << k) | (x >> (64 - k)); }
// g[0..n) (n a multiple of 4) = unit Gaussians from the two-lane xoshiro256** state st[8] (lane-interleaved).
inline void noiseGauss(std::uint64_t* st, float* g, int n) noexcept {
    v2u s[4];
    __builtin_memcpy(s, st, sizeof s);
    const v2u m8 = 0x00ff00ff00ff00ffull, m16 = 0x0000ffff0000ffffull;
    for (int i = 0; i < n; i += 4) {
        const v2u a = (s[1] << 2) + s[1], b = rotl2(a, 7), r = (b << 3) + b, t = s[1] << 17; // rotl(s1·5, 7)·9
        s[2] ^= s[0];
        s[3] ^= s[1];
        s[1] ^= s[2];
        s[0] ^= s[3];
        s[2] ^= t;
        s[3] = rotl2(s[3], 45);
        v2u h = (r & m8) + ((r >> 8) & m8); // byte pairs
        h = (h & m16) + ((h >> 16) & m16);  // per lane two sums of four bytes, 0..1020
        v4u u;
        __builtin_memcpy(&u, &h, sizeof u);
        store4f(g + i, (__builtin_convertvector(u, v4f) - 510.0f) * 0.0067658776f); // 1/√(4·(256²−1)/12)
    }
    __builtin_memcpy(st, s, sizeof s);
}
// y = A·x with A rows×n row-major in float, n a multiple of 4.
inline void gemvF(const float* __restrict A, int rows, int n, const float* __restrict x, double* __restrict y) noexcept {
    int r = 0;
    for (; r + 4 <= rows; r += 4) {
        const float *a0 = A + r * n, *a1 = a0 + n, *a2 = a1 + n, *a3 = a2 + n;
        v4f s0 = 0, s1 = 0, s2 = 0, s3 = 0;
        for (int c = 0; c < n; c += 4) {
            const v4f xv = load4f(x + c);
            s0 += load4f(a0 + c) * xv;
            s1 += load4f(a1 + c) * xv;
            s2 += load4f(a2 + c) * xv;
            s3 += load4f(a3 + c) * xv;
        }
        y[r] = static_cast<double>((s0.x + s0.y) + (s0.z + s0.w));
        y[r + 1] = static_cast<double>((s1.x + s1.y) + (s1.z + s1.w));
        y[r + 2] = static_cast<double>((s2.x + s2.y) + (s2.z + s2.w));
        y[r + 3] = static_cast<double>((s3.x + s3.y) + (s3.z + s3.w));
    }
    for (; r < rows; ++r) {
        const float* a = A + r * n;
        v4f s0 = 0;
        for (int c = 0; c < n; c += 4) s0 += load4f(a + c) * load4f(x + c);
        y[r] = static_cast<double>((s0.x + s0.y) + (s0.z + s0.w));
    }
}
// gemvF over the kept 4-column chunks only (kNoiseFold): rows in blocks of 4 (block b's chunks cols[off[b] .. off[b+1])),
// the last 0-3 rows one block each. A kept chunk's arithmetic is gemvF's (same lanes, same order).
inline void gemvFChunks(const float* __restrict A, int rows, int n, const float* __restrict x, double* __restrict y, const int* __restrict off,
                        const int* __restrict cols) noexcept {
    int r = 0, b = 0;
    for (; r + 4 <= rows; r += 4, ++b) {
        const float *a0 = A + r * n, *a1 = a0 + n, *a2 = a1 + n, *a3 = a2 + n;
        v4f s0 = 0, s1 = 0, s2 = 0, s3 = 0;
        for (int e = off[b]; e < off[b + 1]; ++e) {
            const int c = cols[e];
            const v4f xv = load4f(x + c);
            s0 += load4f(a0 + c) * xv;
            s1 += load4f(a1 + c) * xv;
            s2 += load4f(a2 + c) * xv;
            s3 += load4f(a3 + c) * xv;
        }
        y[r] = static_cast<double>((s0.x + s0.y) + (s0.z + s0.w));
        y[r + 1] = static_cast<double>((s1.x + s1.y) + (s1.z + s1.w));
        y[r + 2] = static_cast<double>((s2.x + s2.y) + (s2.z + s2.w));
        y[r + 3] = static_cast<double>((s3.x + s3.y) + (s3.z + s3.w));
    }
    for (; r < rows; ++r, ++b) {
        const float* a = A + r * n;
        v4f s0 = 0;
        for (int e = off[b]; e < off[b + 1]; ++e) s0 += load4f(a + cols[e]) * load4f(x + cols[e]);
        y[r] = static_cast<double>((s0.x + s0.y) + (s0.z + s0.w));
    }
}
// x^(1/3) in float within 1e-7 relative over 1e-13 .. 1e13 (std::cbrt is a libm call): a third of the exponent bits as
// the start, then three Newton steps. Zero and denormal inputs give 0 (no shot noise from a cut-off plate).
inline float cbrtNoise(float x) noexcept {
    if (!(x >= 1.17549435e-38f)) return 0.0f;
    std::uint32_t b;
    std::memcpy(&b, &x, sizeof b);
    b = b / 3 + 709921077u; // 0x2a5137a0: exponent and mantissa bits divided by 3 (Kahan's start)
    float y;
    std::memcpy(&y, &b, sizeof y);
    y = y - (y * y * y - x) / (3.0f * y * y);
    y = y - (y * y * y - x) / (3.0f * y * y);
    y = y - (y * y * y - x) / (3.0f * y * y);
    return y;
}
constexpr double kBoltzmann = 1.380649e-23, kElectron = 1.602176634e-19;
} // namespace

void Circuit::updateNoise(GroupRt& r, const Group& g) noexcept {
    const double bw = 0.5 / (g.T * r.noiseDiv); // band of one held draw (Hz)
    const int nn = r.nn, naP = r.naP, nxP = r.nxP;
    float* __restrict gz = r.gz.data();
    noiseGauss(r.rng, gz, (r.nDraw + 3) & ~3);
    // This update's raw draws go to the ring slot xHead (the compensation reads the last kNoiseTaps of them).
    float* __restrict x = r.xr.data() + static_cast<std::size_t>(r.xHead) * static_cast<std::size_t>(nxP);
    int k = 0;
    // A unit white draw plus, with a 1/f bank, its sections (each a unit AR(1) step on a draw of its own).
    auto draw = [&](int fl) {
        float v = gz[k++];
        if (fl >= 0) {
            auto& f = r.flick[static_cast<std::size_t>(fl)];
            for (int j = 0; j < f.n; ++j) {
                f.s[j] = f.a[j] * f.s[j] + f.c[j] * gz[k++];
                v += f.w[j] * f.s[j];
            }
        }
        return v;
    };
    if (kNoiseFold) { // folded: the kept sources' unit draws (their scale and nodes are in Zf's columns)
        const int nt = r.nRAct;
        if (!r.thFl) {
            std::memcpy(x, gz, static_cast<std::size_t>(nt) * sizeof(float));
            k = nt;
        } else
            for (int a = 0; a < nt; ++a) x[a] = draw(r.noiseR[static_cast<std::size_t>(r.rAct[static_cast<std::size_t>(a)])].fl);
    } else {
        std::fill(x, x + naP, 0.0f);
        for (int a = 0; a < r.nRAct; ++a) { // thermal (and op-amp input current noise): a current source a→b
            const auto& q = r.noiseR[static_cast<std::size_t>(r.rAct[static_cast<std::size_t>(a)])];
            const float i = static_cast<float>(q.scale) * draw(q.fl);
            if (q.ka >= 0) x[q.ka] -= i;
            if (q.kb >= 0) x[q.kb] += i;
        }
    }
    if (nn > 0) { // shot noise on the ports, from the device currents and gm at the last solution
        const double* cw = r.w.data() + r.ns + r.nu;
        const double q2 = 2.0 * kElectron * bw, t4 = 4.0 * kBoltzmann * opt_.noiseTempK * 2.5 * bw;
        float* __restrict xi = x + naP;
        for (const auto& sd : r.shotDev) {
            const int o = sd.o;
            if (sd.kind == 0) {
                xi[o] = static_cast<float>(std::sqrt(q2 * std::abs(cw[o]) * sd.var)) * gz[k++];
            } else if (sd.kind == 1) { // ports b→e, b→c: collector noise c→e = (b→e) − (b→c), base noise b→e
                const double ic = -cw[o + 1], ib = cw[o] + cw[o + 1];
                const float nc2 = static_cast<float>(std::sqrt(q2 * std::abs(ic))) * gz[k];
                const float nb = static_cast<float>(std::sqrt(q2 * std::abs(ib))) * gz[k + 1];
                k += 2;
                xi[o] = nc2 + nb;
                xi[o + 1] = -nc2;
            } else { // tube: grid current shot noise; plate: space-charge-reduced shot noise 4kT·2.5·gm
                // gm from the operating point's (reset) by Child's law, gm ∝ Ip^(1/3): no device evaluation per draw
                const auto pk = static_cast<std::size_t>(o + 1);
                const double i0 = r.noiseI0[pk], ip = cw[o + 1];
                const double gm = i0 > 0 && ip > 0 ? r.noiseGm0[pk] * (kNoiseFold ? static_cast<double>(cbrtNoise(static_cast<float>(ip / i0))) : std::cbrt(ip / i0)) : 0.0;
                xi[o] = static_cast<float>(std::sqrt(q2 * std::abs(cw[o]))) * gz[k++];
                xi[o + 1] = static_cast<float>(std::sqrt(t4 * gm)) * draw(sd.fl);
            }
        }
        float* __restrict xv = x + naP + (nxP - naP) / 2; // port voltage noise: op-amp en in series with the input
        for (const auto& v : r.portV) xv[v.port] = static_cast<float>(v.scale) * draw(v.fl);
    }
    // Hold compensation: xf = c0·x[m−3] + Σ ck·(x[m−3+k] + x[m−3−k]) over the ring (a 3-update delay is immaterial).
    float* __restrict xf = r.xf.data();
    if (r.noiseDiv > 1) {
        const float* s[kNoiseTaps];
        for (int j = 0; j < kNoiseTaps; ++j)
            s[j] = r.xr.data() + static_cast<std::size_t>((r.xHead + kNoiseTaps - j) % kNoiseTaps) * static_cast<std::size_t>(nxP); // s[j] = x[m−j]
        const v4f c0 = r.comp[0], c1 = r.comp[1], c2 = r.comp[2], c3 = r.comp[3];
        for (int e = 0; e < nxP; e += 4)
            store4f(xf + e, c0 * load4f(s[3] + e) + c1 * (load4f(s[2] + e) + load4f(s[4] + e)) + c2 * (load4f(s[1] + e) + load4f(s[5] + e)) +
                                c3 * (load4f(s[0] + e) + load4f(s[6] + e)));
        r.xHead = (r.xHead + 1) % kNoiseTaps;
    } else {
        std::copy(x, x + nxP, xf);
    }
    // Responses: node voltages z (thermal), gathered by exports, states and ports; the ports add K·(shot currents).
    double* __restrict z = r.zv.data();
    if (naP > 0) { // z[nz] stays 0
        if (kNoiseFold) gemvFChunks(r.Zf.data(), r.nz, naP, xf, z, r.zcOff.data(), r.zcCol.data());
        else gemvF(r.Zf.data(), r.nz, naP, xf, z);
    }
    double* __restrict dO = r.dOut.data();
    const int ny = r.ny;
    std::copy(z, z + ny, dO);
    const int* gs = r.gSt.data();
    for (int k = 0; k < r.ns; ++k) dO[ny + k] = z[gs[2 * k]] - z[gs[2 * k + 1]];
    if (nn > 0) {
        double* __restrict dP = r.dP.data();
        const int nnP = (nxP - naP) / 2;
        gemvF(r.Kf.data(), nn, nnP, xf + naP, dP);
        const int* gp = r.gPort.data();
        for (int j = 0; j < nn; ++j) {
            dP[j] += z[gp[2 * j]] - z[gp[2 * j + 1]] + static_cast<double>(xf[naP + nnP + j]);
            r.dI[static_cast<std::size_t>(j)] = static_cast<double>(xf[naP + j]);
        }
    }
}

// ── LTE control (docs §3.1) ──
// Trapezoidal LTE ≈ T³x'''/12. T³x''' is estimated from the third difference of the pairwise mean (x[n] + x[n-1]) / 2,
// which is blind to a (-1)^n component: a stiff mode's trapezoidal chatter at fs/2 is a persistent, harmless alternation
// and must not read as error. x is the capacitor voltage or the inductor current. Returns 1 when every state is within
// tolerance, else the substep count that brings the estimate (∝ h³) under it (2 or 4).
namespace {
inline int lteCount(double ratio) noexcept { return !(ratio > 1.0) ? 1 : std::min(kLteMaxSubsteps, ratio <= 8.0 ? 2 : (ratio <= 64.0 ? 4 : 8)); }
} // namespace

namespace {
// LTE estimate: a state's ratio is only computed where it can raise the running maximum. |d3| > worst·tol·(1 − 1e-9)
// lets through every ratio that could, rounding included, so the maximum (and the decision) is the one of all ratios.
constexpr double kLteCandidate = 1.0 - 1e-9;
} // namespace
const double* Circuit::lteSlot(const GroupRt& r, int j) noexcept {
    return r.xh.data() + static_cast<std::size_t>((r.xhHead + j) % 5) * static_cast<std::size_t>(r.ns);
}

int Circuit::lteSubsteps(const GroupRt& r) const noexcept {
    if (r.evosM && kEnvEvosForce) return r.evosM; // docs §10.48 (diagnostic: every sample)
    if (r.lteWarm < 4) return 1;
    const int ns = r.ns;
    const double* out = r.out.data() + r.ny;
    const double* w = r.w.data();
    const double* s0 = lteSlot(r, 0);
    const double* s2 = lteSlot(r, 2);
    const double* s3 = lteSlot(r, 3);
    const double rel = kLteRel * opt_.lteTol;
    double worst = 0;
    for (int k = 0; k < ns; ++k) {
        const auto kk = static_cast<std::size_t>(k);
        const double vc = out[k];
        const double x = r.xInd[kk] ? r.gc[kk] * vc + w[k] : vc; // before the state update: i = G·v + s
        const double h1 = s0[k], h3 = s2[k], h4 = s3[k];
        const double d3 = 0.5 * (x - 2.0 * h1 + 2.0 * h3 - h4);
        const double tol = rel * std::max(std::abs(x), std::abs(h1)) + r.lteAbs[kk];
        const double a = std::abs(d3);
        if (a > worst * tol * kLteCandidate) worst = std::max(worst, a / tol); // divide only where the maximum can move
    }
    const int m = lteCount(worst * (1.0 / 12.0));
    if (kEnvLteStat || r.tb) r.lteLastRatio = worst * (1.0 / 12.0); // TR-BDF2 picks its macro steps from it (§10.47)
    if (!r.lteWho.empty() && m > 1) lteTally(r, false);
    // Over tolerance without a device changing regime is a linear mode ringing near fs/2 (an output transformer's
    // leakage resonance after a pick attack, say): the trapezoidal rule renders it as designed (frequency warping),
    // and substepping it would only cost CPU on clean signals. The full step stands.
    if (r.evosM) return m > 1 && (kEnvEvosNoGate || nonlinearEvent(r)) ? r.evosM : 1; // docs §10.48
    return m > 1 && nonlinearEvent(r) ? m : 1;
}

// After a substepped sample: the same estimate on the solution just finished (history[0]). Inside an event the next
// sample starts straight on substeps instead of solving and rejecting a full step first. No regime gate here, so an
// event's ringing is followed until it is within tolerance.
int Circuit::lteSubstepsFromHistory(const GroupRt& r) const noexcept {
    const int ns = r.ns;
    const double* s0 = lteSlot(r, 0);
    const double* s1 = lteSlot(r, 1);
    const double* s3 = lteSlot(r, 3);
    const double* s4 = lteSlot(r, 4);
    const double rel = kLteRel * opt_.lteTol;
    double worst = 0;
    for (int k = 0; k < ns; ++k) {
        const double x = s0[k], h1 = s1[k], h3 = s3[k], h4 = s4[k];
        const double d3 = 0.5 * (x - 2.0 * h1 + 2.0 * h3 - h4);
        const double tol = rel * std::max(std::abs(x), std::abs(h1)) + r.lteAbs[static_cast<std::size_t>(k)];
        const double a = std::abs(d3);
        if (a > worst * tol * kLteCandidate) worst = std::max(worst, a / tol);
    }
    const int m = lteCount(worst * (1.0 / 12.0));
    if (kEnvLteStat) r.lteLastRatio = worst * (1.0 / 12.0);
    if (!r.lteWho.empty() && m > 1) lteTally(r, true);
    if (r.evosM && kEnvEvosRegime && m > 1 && !kEnvEvosForce && !nonlinearEventAt(r, r.vpPrev.data(), r.vpPrev2.data())) return 1;
    if (r.evosM) return kEnvEvosContM && !kEnvEvosForce ? m : (m > 1 || kEnvEvosForce ? r.evosM : 1); // docs §10.48
    return m;
}

// P1 profiling (CE_LTEWHO): count every state over tolerance at a substep decision (the estimate of lteSubsteps or, on
// history, of lteSubstepsFromHistory, recomputed here per state).
void Circuit::lteTally(const GroupRt& r, bool hist) const noexcept {
    const int ns = r.ns;
    const double rel = kLteRel * opt_.lteTol;
    const double* out = r.out.data() + r.ny;
    for (int k = 0; k < ns; ++k) {
        const auto kk = static_cast<std::size_t>(k);
        double x, h1, h3, h4;
        if (hist) x = lteSlot(r, 0)[k], h1 = lteSlot(r, 1)[k], h3 = lteSlot(r, 3)[k], h4 = lteSlot(r, 4)[k];
        else {
            const double vc = out[k];
            x = r.xInd[kk] ? r.gc[kk] * vc + r.w[kk] : vc;
            h1 = lteSlot(r, 0)[k], h3 = lteSlot(r, 2)[k], h4 = lteSlot(r, 3)[k];
        }
        const double d3 = 0.5 * (x - 2.0 * h1 + 2.0 * h3 - h4);
        const double tol = rel * std::max(std::abs(x), std::abs(h1)) + r.lteAbs[kk];
        if (std::abs(d3) / tol > 12.0) ++r.lteWho[kk];
    }
}

// A device changed regime over the step: some port's incremental conductance moved by more than kLteGate of its size
// between t[n-1] and t[n] (cutoff, grid conduction, a junction turning on, a plate entering the knee).
bool Circuit::nonlinearEvent(const GroupRt& r) const noexcept { return nonlinearEventAt(r, r.vp.data(), r.vpPrev.data()); }
bool Circuit::nonlinearEventAt(const GroupRt& r, const double* va, const double* vb) const noexcept {
    for (std::size_t q = 0; q < r.portDev.size();) {
        const auto& d = devices_[static_cast<std::size_t>(r.portDev[q])];
        double i0[3], i1[3], J0[9], J1[9];
        d.eval(va + q, i0, J0);
        d.eval(vb + q, i1, J1);
        // A speaker's law is the remainder after its rest-point linear parts (Kms0, Bl0, i = ψ) went into the matrix: its
        // own diagonal sits near zero, so the regime test reads the whole incremental slope.
        const bool spk = d.kind == NlKind::Speaker;
        const double off[3] = {spk ? d.p[7] : 0.0, spk ? d.p[0] : 0.0, spk ? 1.0 : 0.0};
        for (int k = 0; k < d.nports; ++k) {
            const double a = J0[k * d.nports + k] + off[k], b = J1[k * d.nports + k] + off[k];
            if (std::abs(a - b) > kLteGate * (std::max(std::abs(a), std::abs(b)) + 1e-6)) return true;
        }
        q += static_cast<std::size_t>(std::max(d.nports, 1));
    }
    return false;
}

void Circuit::pushLteHistory(GroupRt& r) noexcept {
    const int ns = r.ns;
    r.xhHead = (r.xhHead + 4) % 5; // a ring: the oldest slot becomes the newest (no shift of the other four)
    double* h = r.xh.data() + static_cast<std::size_t>(r.xhHead) * static_cast<std::size_t>(ns);
    for (int k = 0; k < ns; ++k) {
        const auto kk = static_cast<std::size_t>(k);
        const double vc = r.vcLast[kk];
        // After the state update: trapezoidal inductor s' = 2G·v + s, so i[n] = s' − G·v; backward Euler s' = i[n].
        h[k] = !r.xInd[kk] ? vc : (r.G2[kk] != r.gc[kk] ? r.w[kk] - r.gc[kk] * vc : r.w[kk]);
    }
    if (r.lteWarm < 5) ++r.lteWarm;
}

bool Circuit::solvePorts(GroupRt& r, const Group& g, const double* p, const double* pAnchor) noexcept {
    if (newton(r, g, p, opt_.maxIterations)) return true;
    for (int steps : {4, 16}) {
        std::copy(r.vpPrev.begin(), r.vpPrev.end(), r.vp.begin());
        bool ok = true;
        for (int s = 1; s <= steps && ok; ++s) {
            const double lam = static_cast<double>(s) / steps;
            double pl[kMaxNlPorts];
            for (int k = 0; k < r.nn && k < kMaxNlPorts; ++k) pl[k] = pAnchor[k] + lam * (p[k] - pAnchor[k]);
            ok = newton(r, g, pl, opt_.maxIterations);
        }
        if (ok) return true;
    }
    return false;
}

namespace {
// Variable-step BDF2 from a step T to h = T/m (ω = h/T = 1/m), through x[n] and x[n-1] (CPU_THEORY.md R2.3):
// x[n+1] = a·x[n] − b·x[n−1] + β·h·x'[n+1], a = (1+ω)²/(1+2ω), b = ω²/(1+2ω), β = (1+ω)/(1+2ω).
inline double bdf2A(int m) noexcept { const double w = 1.0 / m; return (1 + w) * (1 + w) / (1 + 2 * w); }
inline double bdf2B(int m) noexcept { const double w = 1.0 / m; return w * w / (1 + 2 * w); }
inline double bdf2StepScale(int m) noexcept { const double w = 1.0 / m; return 2.0 * (1 + w) / (1 + 2 * w) / m; } // 2βh / T
} // namespace

Circuit::GroupRt* Circuit::subRuntime(std::size_t gi, int m, bool be) noexcept {
    const auto lv = static_cast<std::size_t>(m == 2 ? 0 : (m == 4 ? 1 : 2));
    if (!be && lv == 0) {
        if (!rtHalf_[gi]) return nullptr;
    } else if (gi >= rtBe_.size()) return nullptr;
    auto& slot = be ? rtBe_[gi][lv] : (lv == 0 ? rtHalf_[gi] : rtTr_[gi][lv]);
    if (!slot) return nullptr;
    auto& stale = be ? beStale_[gi][lv] : (lv == 0 ? halfStale_[gi] : trStale_[gi][lv]);
    if (stale) {
        // refreshGroup marks every sub-runtime of the group stale; only this one is brought up to date here.
        const std::uint8_t sh = halfStale_[gi];
        const auto sb = gi < beStale_.size() ? beStale_[gi] : std::array<std::uint8_t, 3>{1, 1, 1};
        const auto st = gi < trStale_.size() ? trStale_[gi] : std::array<std::uint8_t, 3>{1, 1, 1};
        const bool b2 = be && rt_[gi]->bdf2; // read on the full-step runtime, before the swap
        std::swap(rt_[gi], slot);
        if (b2) {
            // variable-step BDF2 at h = T/m after a step T: companion C/(βh) (inductor βh/L), the trapezoidal one at 2βh
            const double T = groups_[gi].T;
            groups_[gi].T = T * bdf2StepScale(m);
            refreshGroup(gi, false);
            groups_[gi].T = T;
        } else {
            groups_[gi].T /= m;
            refreshGroup(gi, false);
            groups_[gi].T *= m;
        }
        std::swap(rt_[gi], slot);
        halfStale_[gi] = sh;
        if (gi < beStale_.size()) beStale_[gi] = sb, trStale_[gi] = st;
        stale = 0;
        slot->subGrid = slot->subPGrid = 0; // new matrices: the kept substep history is no longer this runtime's
    }
    return slot.get();
}

bool Circuit::substepGroup(std::size_t gi, int m, int mode) noexcept {
    if (mode == 2 && rt_[gi]->tb) return substepTrBdf2(gi, m, mode); // docs §10.47
    const auto& g = groups_[gi];
    auto& r = *rt_[gi];
    if (r.nn > kMaxNlPorts) return false;
    // BDF2 start (CompileOptions::lteStart): a step-size change starts on the variable-step BDF2 runtime; a sample that
    // continues the last one's substep count carries on in plain trapezoid (its (v, i) hand-off is exact).
    const bool cont = mode == 2 && r.bdf2 && r.subPrev == m;
    const bool b2 = mode == 2 && r.bdf2 && !cont;
    const int sp = r.nn >= r.subPredPorts ? r.subPred : 0; // the substep predictor's bits for this group (kEnvSubPred; §10.50 hints)
    GroupRt* h0 = subRuntime(gi, m, mode == 2 && !cont);     // the first substep
    GroupRt* h1 = mode == 2 ? subRuntime(gi, m, false) : h0; // the others
    if (!h0 || !h1) return false;
    const auto lv = static_cast<std::size_t>(m == 2 ? 0 : (m == 4 ? 1 : 2));
    auto& lastSlot = lv == 0 ? rtHalf_[gi] : rtTr_[gi][lv]; // the runtime of the last substep (cut terms)
    const int ns = r.ns, nu = r.nu, nn = r.nn, nuc = r.nuc, W = r.W;
    // Map the companion states between step sizes and integrators through each element's (v, i) at the last step.
    // Post-update states: trapezoidal s = G·v + i (capacitor and inductor); backward Euler s = G·v (capacitor) or
    // s = i (inductor). A backward-Euler capacitor's current is not in its state: the substep loop keeps it (iLast).
    auto mapStates = [&](GroupRt& to, const GroupRt& from) {
        for (int k = 0; k < ns; ++k) {
            const auto& pr = prims_[static_cast<std::size_t>(g.states[static_cast<std::size_t>(k)])];
            const auto kk = static_cast<std::size_t>(k);
            const bool ind = pr.kind == Prim::Ind, beTo = pr.be || to.beAll, beFrom = pr.be || from.beAll;
            const double v = from.vcLast[kk];
            if (beTo && !ind) to.w[kk] = to.gc[kk] * v;
            else {
                const double i = !beFrom ? from.w[kk] - from.gc[kk] * v : (ind ? from.w[kk] : from.iLast[kk]);
                to.w[kk] = beTo ? i : to.gc[kk] * v + i;
            }
            to.vcLast[kk] = v;
        }
    };
    // Homotopy anchor of a runtime: the p for which v solves v = p + K·i(v).
    auto anchor = [&](GroupRt& x, const double* v, double* pa) {
        double cur[kMaxNlPorts];
        int o = 0;
        for (int di : g.devices) {
            const auto& d = devices_[static_cast<std::size_t>(di)];
            double Jl[9];
            d.eval(v + o, cur + o, Jl);
            o += d.nports;
        }
        for (int k = 0; k < nn; ++k) {
            double s = v[k];
            for (int j = 0; j < nn; ++j) s -= x.K[static_cast<std::size_t>(k * nn + j)] * cur[j];
            pa[k] = s;
        }
    };
    mapStates(*h0, r);
    if (b2) {
        // BDF2 history from the LTE record of the last two samples (capacitor voltage, inductor current): the companion
        // is i = G·v − G·(a·v[n] − b·v[n−1]) for a capacitor and i = G·v + (a·i[n] − b·i[n−1]) for an inductor, G the
        // runtime's (trapezoidal at 2βh). After the step the runtime's own trapezoidal update leaves s = G·v + i, so the
        // hand-off to the trapezoidal substeps (mapStates) reads the current exactly as from any trapezoidal runtime.
        const double a = bdf2A(m), b = bdf2B(m);
        const double* x0 = lteSlot(r, 0);
        const double* x1 = lteSlot(r, 1);
        for (int k = 0; k < ns; ++k) {
            const auto kk = static_cast<std::size_t>(k);
            const double x = a * x0[k] - b * x1[k];
            h0->w[kk] = r.xInd[kk] ? x : h0->gc[kk] * x;
        }
    }
    // Exact substep-grid history (kSpGrid): a sample that continues the last one's substep count on the same grid finds
    // the last substeps' port voltages still in h1 (read before h0, which may be h1, is set up below). The bitwise match
    // with r.vpPrev proves it is this circuit's last solution (not a hand-off's or an older chain's).
    int grid = 1; // real points on the substep grid in h.vpPrev, vpPrev2, vpPrev3
    if ((sp & kSpGrid) && mode == 2 && r.subPrev == m && h1->subGrid >= 2) {
        bool same = true;
        for (int k = 0; k < nn && same; ++k) same = h1->vpPrev[static_cast<std::size_t>(k)] == r.vpPrev[static_cast<std::size_t>(k)];
        if (same) grid = h1->subGrid;
    }
    int pg = grid >= 2 && h0 == h1 ? std::min(h1->subPGrid, grid) : 0; // consecutive solves of h with p in its history
    std::copy(r.vpPrev.begin(), r.vpPrev.end(), h0->vpPrev.begin());
    if (grid >= 2) { // the backward-Euler / BDF2 start runtime takes the grid points (h0 == h1: already there)
        if (h0 != h1) {
            std::copy(h1->vpPrev2.begin(), h1->vpPrev2.end(), h0->vpPrev2.begin());
            std::copy(h1->vpPrev3.begin(), h1->vpPrev3.end(), h0->vpPrev3.begin());
        }
    } else if ((sp & kSpToward) && mode == 2 && r.subDetect) {
        // a rejected full step: its solution at t[n+1] (still in r.vp) gives the direction; the first substep's linear
        // predictor 2·v[n] − that point is v[n] + (v_full − v[n])/m
        const double f = 1.0 / m;
        for (int k = 0; k < nn; ++k) {
            const auto kk = static_cast<std::size_t>(k);
            h0->vpPrev2[kk] = r.vpPrev[kk] - f * (r.vp[kk] - r.vpPrev[kk]);
        }
    } else if ((sp & kSpSlope) && mode == 2) {
        // the history point one substep before t[n] on the line through the last two samples (kSpSlope): the first
        // substep's linear predictor 2·v[n] − that point is v[n] + (v[n] − v[n−1])/m
        const double f = 1.0 / m;
        for (int k = 0; k < nn; ++k) {
            const auto kk = static_cast<std::size_t>(k);
            h0->vpPrev2[kk] = r.vpPrev[kk] - f * (r.vpPrev[kk] - r.vpPrev2[kk]);
        }
    } else std::copy(r.vpPrev.begin(), r.vpPrev.end(), h0->vpPrev2.begin());
    std::copy(r.curPrev1.begin(), r.curPrev1.end(), h0->curPrev1.begin());
    std::copy(r.curPrev1.begin(), r.curPrev1.end(), h0->curPrev2.begin());
    double pa[kMaxNlPorts];
    bool paValid = false; // the homotopy anchor is built only when a substep's direct Newton fails
    bool ok = true;
    // State the substeps advance outside the runtimes (core hysteresis plays, speaker voice-coil temperatures). A failed
    // substep hands the sample back to the caller's full step, which advances them again: restore them first, so a
    // failed attempt leaves no trace (as the runtimes, inputs, cut terms and exports, which commit only at the end).
    constexpr int kSideMax = 16;
    double side[kSideMax][4];
    const int nSide = static_cast<int>(g.hyst.size() + g.spk.size());
    if (nSide <= kSideMax) {
        int s = 0;
        for (const auto& [di, o] : g.hyst) std::copy(devices_[static_cast<std::size_t>(di)].hs, devices_[static_cast<std::size_t>(di)].hs + 3, side[s++]);
        for (const auto& [ti, o] : g.spk) {
            const auto& t = spkTh_[static_cast<std::size_t>(ti)];
            std::copy(devices_[static_cast<std::size_t>(t.dev)].hs, devices_[static_cast<std::size_t>(t.dev)].hs + 3, side[s]);
            side[s++][3] = t.pPrev;
        }
    }
    auto restoreSide = [&]() {
        if (nSide > kSideMax) return;
        int s = 0;
        for (const auto& [di, o] : g.hyst) std::copy(side[s], side[s] + 3, devices_[static_cast<std::size_t>(di)].hs), ++s;
        for (const auto& [ti, o] : g.spk) {
            auto& t = spkTh_[static_cast<std::size_t>(ti)];
            std::copy(side[s], side[s] + 3, devices_[static_cast<std::size_t>(t.dev)].hs);
            t.pPrev = side[s++][3];
        }
    };
    for (int step = 1; step <= m; ++step) {
        if (step == 2 && h1 != h0) { // the backward-Euler start is done: the rest on trapezoidal companions
            mapStates(*h1, *h0);
            std::copy(h0->vpPrev.begin(), h0->vpPrev.end(), h1->vpPrev.begin());
            std::copy(h0->vpPrev2.begin(), h0->vpPrev2.end(), h1->vpPrev2.begin());
            if (grid >= 3) std::copy(h0->vpPrev3.begin(), h0->vpPrev3.end(), h1->vpPrev3.begin());
            std::copy(h0->curPrev1.begin(), h0->curPrev1.end(), h1->curPrev1.begin());
            std::copy(h0->curPrev2.begin(), h0->curPrev2.end(), h1->curPrev2.begin());
            paValid = false; // a p of the backward-Euler runtime is no anchor for the trapezoidal one
            pg = 0;
        }
        GroupRt& h = step == 1 ? *h0 : *h1;
        const double lam = static_cast<double>(step) / m;
        double* w = h.w.data();
        for (int j = 0; j < nu; ++j) w[ns + j] = r.uPrev[static_cast<std::size_t>(j)] + lam * (r.w[static_cast<std::size_t>(ns + j)] - r.uPrev[static_cast<std::size_t>(j)]);
        for (int j = 0; j < nuc; ++j) {
            const auto c = static_cast<std::size_t>(ns + nu + nn + j);
            w[c] = r.uPrev[static_cast<std::size_t>(nu + j)] + lam * (r.w[c] - r.uPrev[static_cast<std::size_t>(nu + j)]);
        }
        double* p = h.p.data();
        const int sw = ns + nu;
        const bool spLin = h.sl && h.sl->valid && !h.noiseOn; // the sparse linear update (SparseLin), as the full step
        if (spLin) linPorts(h, w, p);
        else {
            gemv(h.Dp.data(), nn, sw, w, p);
            if (nuc > 0)
                for (int k = 0; k < nn; ++k) {
                    double s = 0;
                    for (int c = 0; c < nuc; ++c) s += h.Dc[static_cast<std::size_t>(k * nuc + c)] * w[sw + nn + c];
                    p[k] += s;
                }
        }
        const bool quadS = (sp & kSpQuad) && grid >= 3;
        if (quadS) // quadratic along the substep grid (three real points)
            for (int k = 0; k < nn; ++k) {
                const auto kk = static_cast<std::size_t>(k);
                h.vp[kk] = 3.0 * (h.vpPrev[kk] - h.vpPrev2[kk]) + h.vpPrev3[kk];
            }
        else
            for (int k = 0; k < nn; ++k) h.vp[static_cast<std::size_t>(k)] = 2.0 * h.vpPrev[static_cast<std::size_t>(k)] - h.vpPrev2[static_cast<std::size_t>(k)];
        limitPredicted(nn, h.lim.data(), h.limA.data(), h.limB.data(), h.limC.data(), h.vpPrev.data(), h.vp.data());
        // the full step's predictor correction (processGroup) along this runtime's own p history: the part of p's change
        // off the extrapolated trend, through the runtime's kept factorisation (kSpCorr; pg counts that history)
        double dqS[kMaxNlPorts];
        if ((sp & kSpCorr) && pg >= (quadS ? 3 : 2) && (h.ks.valid || h.ksR.valid)) {
            for (int k = 0; k < nn; ++k) {
                const auto kk = static_cast<std::size_t>(k);
                dqS[k] = quadS ? p[k] - (3.0 * (h.pPrev[kk] - h.pPrev2[kk]) + h.pPrev3[kk]) : p[k] - (2.0 * h.pPrev[kk] - h.pPrev2[kk]);
            }
            h.predDp = dqS;
        }
        const bool okS = newton(h, g, p, opt_.maxIterations);
        h.predDp = nullptr;
        if (!okS) { // homotopy in p from the substep's start, as solvePorts()
            if (!paValid) anchor(h, h.vpPrev.data(), pa);
            bool hok = false;
            for (int steps : {4, 16}) {
                std::copy(h.vpPrev.begin(), h.vpPrev.end(), h.vp.begin());
                hok = true;
                for (int s = 1; s <= steps && hok; ++s) {
                    const double lam2 = static_cast<double>(s) / steps;
                    double pl[kMaxNlPorts];
                    for (int k = 0; k < nn; ++k) pl[k] = pa[k] + lam2 * (p[k] - pa[k]);
                    hok = newton(h, g, pl, opt_.maxIterations);
                }
                if (hok) break;
            }
            if (!hok) {
                ok = false;
                break;
            }
        }
        for (int k = 0; k < nn; ++k) {
            const auto kk = static_cast<std::size_t>(k);
            if (sp & (kSpGrid | kSpQuad)) h.vpPrev3[kk] = h.vpPrev2[kk];
            h.vpPrev2[kk] = h.vpPrev[kk];
            h.vpPrev[kk] = h.vp[kk];
            pa[k] = p[k];
            if (sp & kSpCorr) h.pPrev3[kk] = h.pPrev2[kk], h.pPrev2[kk] = h.pPrev[kk], h.pPrev[kk] = p[k];
        }
        grid = std::min(grid + 1, 3);
        pg = std::min(pg + 1, 3);
        paValid = true;
        for (const auto& [di, o] : g.hyst) devices_[static_cast<std::size_t>(di)].advancePlay(h.vp[static_cast<std::size_t>(o)]);
        if (!g.spk.empty()) advanceSpeakers(g, h.vp.data(), g.T / m);
        for (int k = 0; k < nn && nn <= kMaxNlPorts; ++k) {
            h.curPrev2[static_cast<std::size_t>(k)] = h.curPrev1[static_cast<std::size_t>(k)];
            h.curPrev1[static_cast<std::size_t>(k)] = h.w[static_cast<std::size_t>(ns + nu + k)];
        }
        // State rows every substep; the exported node rows only at the end of the sample.
        if (spLin) linOut(h, w, h.out.data()); // every row (one solve gives them all)
        else if (!h.pdRow.empty()) { // port-defined rows from the substep's solution (GroupRt::pdRow), the rest from Hc
            const int e0 = step == m ? 0 : h.hcY0; // Hc's first hcY0 rows are exports (needed at the end only)
            const int rows = static_cast<int>(h.hcOut.size()) - e0;
            gemv(h.Hc.data() + static_cast<std::size_t>(e0) * static_cast<std::size_t>(W), rows, W, w, h.hcY.data());
            for (int c = 0; c < rows; ++c) h.out[static_cast<std::size_t>(h.hcOut[static_cast<std::size_t>(e0 + c)])] = h.hcY[static_cast<std::size_t>(c)];
            for (std::size_t q = 0; q < h.pdRow.size(); ++q) {
                const int a = h.pdA[q], b = h.pdB[q];
                if (step == m || h.pdRow[q] >= h.ny)
                    h.out[static_cast<std::size_t>(h.pdRow[q])] = (a >= 0 ? h.vp[static_cast<std::size_t>(a)] : 0.0) - (b >= 0 ? h.vp[static_cast<std::size_t>(b)] : 0.0);
            }
        } else if (step == m) gemv(h.H.data(), h.ny + ns, W, w, h.out.data());
        else gemv(h.H.data() + static_cast<std::size_t>(h.ny) * static_cast<std::size_t>(W), ns, W, w, h.out.data() + h.ny);
        if (step == m) {
            double* Vn = V_.data();
            for (int e = 0; e < h.ny; ++e) {
                const double v = h.out[static_cast<std::size_t>(e)];
                Vn[g.exports[static_cast<std::size_t>(e)]] = (v > -1e9 && v < 1e9) ? v : 0.0;
            }
            if (!g.cutTerms.empty()) {
                std::swap(rt_[gi], lastSlot);
                accumulateCutTerms(gi, false);
                std::swap(rt_[gi], lastSlot);
            }
        }
        if (r.evosM) evosRecord(gi, h, w, step, m); // docs §10.48: the output node at this substep
        for (int k = 0; k < ns; ++k) {
            const auto kk = static_cast<std::size_t>(k);
            const double vc = h.out[static_cast<std::size_t>(h.ny + k)];
            if (h.beAll) h.iLast[kk] = h.gc[kk] * vc - w[k]; // backward-Euler capacitor current (i = G·v − s)
            h.vcLast[kk] = vc;
            w[k] = h.G2[kk] * vc + h.sgn[kk] * w[k];
        }
    }
    if (!ok) { // the caller keeps last sample's port voltages, as without step halving
        h1->subGrid = h1->subPGrid = 0;
        restoreSide();
        return false;
    }
    r.subPrev = m;
    h1->subGrid = grid;
    h1->subPGrid = pg;
    // Back to the full-step runtime: states, port history (v at n and n-1), anchor, outputs, inputs.
    GroupRt& hl = *h1;
    mapStates(r, hl);
    for (int k = 0; k < nn; ++k) {
        const auto kk = static_cast<std::size_t>(k);
        r.vpPrev2[kk] = r.vpPrev[kk];
        r.vpPrev[kk] = hl.vp[kk];
        r.vp[kk] = hl.vp[kk];
        r.w[static_cast<std::size_t>(ns + nu + k)] = hl.w[static_cast<std::size_t>(ns + nu + k)];
    }
    anchor(r, r.vp.data(), r.pPrev.data());
    if (nn <= kMaxNlPorts) {
        std::copy(r.curPrev1.begin(), r.curPrev1.end(), r.curPrev2.begin());
        for (int k = 0; k < nn; ++k) r.curPrev1[static_cast<std::size_t>(k)] = hl.w[static_cast<std::size_t>(ns + nu + k)];
    }
    std::copy(hl.out.begin(), hl.out.end(), r.out.begin());
    for (int j = 0; j < nu; ++j) r.uPrev[static_cast<std::size_t>(j)] = r.w[static_cast<std::size_t>(ns + j)];
    for (int j = 0; j < nuc; ++j) r.uPrev[static_cast<std::size_t>(nu + j)] = r.w[static_cast<std::size_t>(ns + nu + nn + j)];
    return true;
}

// ───────────────────────────── TR-BDF2 LTE substeps (docs §10.47; 2026-11-14) ─────────────────────────────
// An LTE event's sample as M macro steps of H = T/M (M = 1 or 2), each one TR-BDF2 step (Bank et al. 1985; Hosea and
// Shampine 1996): a trapezoidal stage to t + γH, then a BDF2 stage through (t, t + γH) to t + H, γ = 2 − √2. With that γ
// both stages have the companion C/(γH/2), the trapezoidal runtime's at γH: one runtime serves the whole sequence, and
// only the BDF2 stage's history differs (s = G·(a·x_γ − b·x_0) for a capacitor, a·i_γ − b·i_0 for an inductor). The
// runtime's own trapezoidal update then leaves s = G·v + i, the state every stage starts from. L-stable, second order and
// self-starting from (v, i) at t[n]: no backward-Euler or history-based start.
// Embedded estimate (Hosea and Shampine's third-order companion): est = H·Σ (b − b̂)_i·f_i over the three stage
// derivatives, b − b̂ = ((√2 − 1)/3, −1/3, γ/3). For x = t³ it is the step's own error, γ(1 − γ)/6·H³·x''' (the
// trapezoid's is H³·x'''/12). It replaces the third-difference history estimate inside the event: the next sample is a
// full step if the trapezoid's predicted error, est·M³/(2γ(1 − γ)), is within tolerance, else one or two macro steps.
namespace {
constexpr double kTbG = 2.0 - M_SQRT2;                                               // γ
constexpr double kTbA = 1.0 / (kTbG * (2.0 - kTbG));                                 // BDF2 stage: x_hist = a·x_γ − b·x_0
constexpr double kTbB = (1.0 - kTbG) * (1.0 - kTbG) / (kTbG * (2.0 - kTbG));
constexpr double kTbE0 = (M_SQRT2 - 1.0) / 3.0, kTbE1 = -1.0 / 3.0, kTbE2 = kTbG / 3.0; // b − b̂
constexpr double kTbTrap = 1.0 / (2.0 * kTbG * (1.0 - kTbG));                       // trapezoid's error constant ÷ TR-BDF2's
} // namespace

Circuit::GroupRt* Circuit::tbRuntime(std::size_t gi, int M) noexcept {
    if (gi >= rtTr_.size() || gi >= rtBe_.size()) return nullptr;
    auto& slot = M == 1 ? rtTr_[gi][2] : rtBe_[gi][2];
    if (!slot) return nullptr;
    auto& stale = M == 1 ? trStale_[gi][2] : beStale_[gi][2];
    if (stale) { // as subRuntime: only this runtime is brought up to date
        const std::uint8_t sh = halfStale_[gi];
        const auto sb = beStale_[gi];
        const auto st = trStale_[gi];
        std::swap(rt_[gi], slot);
        const double T = groups_[gi].T;
        groups_[gi].T = T * kTbG / M;
        refreshGroup(gi, false);
        groups_[gi].T = T;
        std::swap(rt_[gi], slot);
        halfStale_[gi] = sh;
        beStale_[gi] = sb, trStale_[gi] = st;
        stale = 0;
        slot->subGrid = slot->subPGrid = 0;
    }
    return slot.get();
}

bool Circuit::substepTrBdf2(std::size_t gi, int m, int /*mode*/) noexcept {
    const auto& g = groups_[gi];
    auto& r = *rt_[gi];
    if (r.nn > kMaxNlPorts) return false;
    const int mMax = std::clamp(kEnvTbMax, 1, 2);
    // a detected event: one or two macro steps from the full step's ratio (lteCount's boundary 8, scaled by CE_TBSCALE);
    // a continued one: the embedded estimate's decision (tbNext: 2 = one macro step, 4 = two)
    int M = r.subDetect ? (r.lteLastRatio * kEnvTbScale <= 8.0 ? 1 : 2) : m / 2;
    M = std::clamp(M, 1, mMax);
    GroupRt* hp = tbRuntime(gi, M);
    if (!hp) return false;
    GroupRt& h = *hp;
    auto& slot = M == 1 ? rtTr_[gi][2] : rtBe_[gi][2];
    const int ns = r.ns, nu = r.nu, nn = r.nn, nuc = r.nuc, W = r.W;
    const double T = g.T;
    double* x0 = h.xh.data();      // state variables at the macro step's start
    double* xg = x0 + ns;          // ... after its trapezoidal stage
    double* q = x0 + 2 * ns;       // Σ (b − b̂)_i · (capacitor current or inductor voltage) over the stages
    const std::uint8_t* ind = r.xInd.data();
    // (v, i) hand-off from the full-step runtime: both trapezoidal (s = G·v + i after the update)
    for (int k = 0; k < ns; ++k) {
        const auto kk = static_cast<std::size_t>(k);
        const double v = r.vcLast[kk];
        h.w[kk] = h.gc[kk] * v + (r.w[kk] - r.gc[kk] * v);
        h.vcLast[kk] = v;
    }
    auto anchor = [&](GroupRt& x, const double* v, double* pa) {
        double cur[kMaxNlPorts];
        int o = 0;
        for (int di : g.devices) {
            const auto& d = devices_[static_cast<std::size_t>(di)];
            double Jl[9];
            d.eval(v + o, cur + o, Jl);
            o += d.nports;
        }
        for (int k = 0; k < nn; ++k) {
            double s = v[k];
            for (int j = 0; j < nn; ++j) s -= x.K[static_cast<std::size_t>(k * nn + j)] * cur[j];
            pa[k] = s;
        }
    };
    // Newton start: linear extrapolation through the last two solutions at their times (fractions of T): vpPrev2 at tA,
    // vpPrev at tB. A detected event's first stage heads for the rejected full step's solution (CE_TBPRED bit 1).
    double tA = -1.0, tB = 0.0;
    int pg = 0; // consecutive solves of h whose (v, p) pairs are in vpPrev / pPrev, vpPrev2 / pPrev2 (predictor correction)
    // A sample that continues the last one's macro steps on the same runtime finds the last two stage solutions still in
    // h (bit 4; the bitwise match with r.vpPrev proves they are this circuit's last solution): extrapolate from them, and
    // correct along their p (same runtime, so the same v = p + K·i(v) manifold whatever the stage type).
    bool grid = (kEnvTbPred & 4) && !r.subDetect && r.subPrev == 2 * M && h.subGrid >= 2;
    for (int k = 0; k < nn && grid; ++k) grid = h.vpPrev[static_cast<std::size_t>(k)] == r.vpPrev[static_cast<std::size_t>(k)];
    if (grid) tA = -(1.0 - kTbG) / M, pg = 2;
    else std::copy(r.vpPrev.begin(), r.vpPrev.end(), h.vpPrev.begin());
    if (grid) {
    } else if ((kEnvTbPred & 1) && r.subDetect)
        for (int k = 0; k < nn; ++k) {
            const auto kk = static_cast<std::size_t>(k);
            h.vpPrev2[kk] = r.vpPrev[kk] - (r.vp[kk] - r.vpPrev[kk]); // the line through v[n] and v_full, at −T
        }
    else std::copy(r.vpPrev2.begin(), r.vpPrev2.end(), h.vpPrev2.begin());
    std::copy(r.curPrev1.begin(), r.curPrev1.end(), h.curPrev1.begin());
    std::copy(r.curPrev1.begin(), r.curPrev1.end(), h.curPrev2.begin());
    h.subGrid = 0; // set again on success
    double pa[kMaxNlPorts];
    bool paValid = false;
    bool ok = true;
    constexpr int kSideMax = 16; // as substepGroup: state advanced outside the runtimes, restored on failure
    double side[kSideMax][4];
    const int nSide = static_cast<int>(g.hyst.size() + g.spk.size());
    if (nSide <= kSideMax) {
        int s = 0;
        for (const auto& [di, o] : g.hyst) std::copy(devices_[static_cast<std::size_t>(di)].hs, devices_[static_cast<std::size_t>(di)].hs + 3, side[s++]);
        for (const auto& [ti, o] : g.spk) {
            const auto& t = spkTh_[static_cast<std::size_t>(ti)];
            std::copy(devices_[static_cast<std::size_t>(t.dev)].hs, devices_[static_cast<std::size_t>(t.dev)].hs + 3, side[s]);
            side[s++][3] = t.pPrev;
        }
    }
    auto restoreSide = [&]() {
        if (nSide > kSideMax) return;
        int s = 0;
        for (const auto& [di, o] : g.hyst) std::copy(side[s], side[s] + 3, devices_[static_cast<std::size_t>(di)].hs), ++s;
        for (const auto& [ti, o] : g.spk) {
            auto& t = spkTh_[static_cast<std::size_t>(ti)];
            std::copy(side[s], side[s] + 3, devices_[static_cast<std::size_t>(t.dev)].hs);
            t.pPrev = side[s++][3];
        }
    };
    // post-update state of h: x (capacitor voltage / inductor current) and the derivative carrier (current / voltage)
    auto xOf = [&](int k) { const auto kk = static_cast<std::size_t>(k); return ind[kk] ? h.w[kk] - h.gc[kk] * h.vcLast[kk] : h.vcLast[kk]; };
    auto dOf = [&](int k) { const auto kk = static_cast<std::size_t>(k); return ind[kk] ? h.vcLast[kk] : h.w[kk] - h.gc[kk] * h.vcLast[kk]; };
    double* w = h.w.data();
    double tPrevStage = 0.0;
    for (int j = 0; j < M && ok; ++j)
        for (int st = 0; st < 2 && ok; ++st) {
            const bool last = j == M - 1 && st == 1;
            if (st == 0)
                for (int k = 0; k < ns; ++k) x0[k] = xOf(k), q[k] = kTbE0 * dOf(k);
            else
                for (int k = 0; k < ns; ++k) { // the BDF2 stage's history from the stage values
                    const auto kk = static_cast<std::size_t>(k);
                    xg[k] = xOf(k);
                    q[k] += kTbE1 * dOf(k);
                    const double xh = kTbA * xg[k] - kTbB * x0[k];
                    w[k] = ind[kk] ? xh : h.gc[kk] * xh;
                }
            const double t1 = (j + (st == 0 ? kTbG : 1.0)) / M; // stage end, as a fraction of T
            for (int jj = 0; jj < nu; ++jj) w[ns + jj] = r.uPrev[static_cast<std::size_t>(jj)] + t1 * (r.w[static_cast<std::size_t>(ns + jj)] - r.uPrev[static_cast<std::size_t>(jj)]);
            for (int jj = 0; jj < nuc; ++jj) {
                const auto c = static_cast<std::size_t>(ns + nu + nn + jj);
                w[c] = r.uPrev[static_cast<std::size_t>(nu + jj)] + t1 * (r.w[c] - r.uPrev[static_cast<std::size_t>(nu + jj)]);
            }
            double* p = h.p.data();
            const int sw = ns + nu;
            const bool spLin = h.sl && h.sl->valid && !h.noiseOn;
            if (spLin) linPorts(h, w, p);
            else {
                gemv(h.Dp.data(), nn, sw, w, p);
                if (nuc > 0)
                    for (int k = 0; k < nn; ++k) {
                        double s = 0;
                        for (int c = 0; c < nuc; ++c) s += h.Dc[static_cast<std::size_t>(k * nuc + c)] * w[sw + nn + c];
                        p[k] += s;
                    }
            }
            const double rho = (t1 - tB) / (tB - tA);
            for (int k = 0; k < nn; ++k) {
                const auto kk = static_cast<std::size_t>(k);
                h.vp[kk] = h.vpPrev[kk] + rho * (h.vpPrev[kk] - h.vpPrev2[kk]);
            }
            limitPredicted(nn, h.lim.data(), h.limA.data(), h.limB.data(), h.limC.data(), h.vpPrev.data(), h.vp.data());
            double dq[kMaxNlPorts];
            if ((kEnvTbPred & 2) && pg >= 2 && (h.ks.valid || h.ksR.valid)) { // p's change off its trend, through the kept factorisation
                for (int k = 0; k < nn; ++k) {
                    const auto kk = static_cast<std::size_t>(k);
                    dq[k] = p[k] - (h.pPrev[kk] + rho * (h.pPrev[kk] - h.pPrev2[kk]));
                }
                h.predDp = dq;
            }
            const bool okS = newton(h, g, p, opt_.maxIterations);
            h.predDp = nullptr;
            if (!okS) { // homotopy in p from the stage's start, as substepGroup
                if (!paValid) anchor(h, h.vpPrev.data(), pa);
                bool hok = false;
                for (int steps : {4, 16}) {
                    std::copy(h.vpPrev.begin(), h.vpPrev.end(), h.vp.begin());
                    hok = true;
                    for (int s = 1; s <= steps && hok; ++s) {
                        const double lam2 = static_cast<double>(s) / steps;
                        double pl[kMaxNlPorts];
                        for (int k = 0; k < nn; ++k) pl[k] = pa[k] + lam2 * (p[k] - pa[k]);
                        hok = newton(h, g, pl, opt_.maxIterations);
                    }
                    if (hok) break;
                }
                if (!hok) {
                    ok = false;
                    break;
                }
            }
            for (int k = 0; k < nn; ++k) {
                const auto kk = static_cast<std::size_t>(k);
                h.vpPrev2[kk] = h.vpPrev[kk];
                h.vpPrev[kk] = h.vp[kk];
                h.pPrev2[kk] = h.pPrev[kk];
                h.pPrev[kk] = p[k];
                pa[k] = p[k];
            }
            tA = tB, tB = t1;
            pg = std::min(pg + 1, 3);
            paValid = true;
            for (const auto& [di, o] : g.hyst) devices_[static_cast<std::size_t>(di)].advancePlay(h.vp[static_cast<std::size_t>(o)]);
            if (!g.spk.empty()) advanceSpeakers(g, h.vp.data(), (t1 - tPrevStage) * T);
            tPrevStage = t1;
            for (int k = 0; k < nn && nn <= kMaxNlPorts; ++k) {
                h.curPrev2[static_cast<std::size_t>(k)] = h.curPrev1[static_cast<std::size_t>(k)];
                h.curPrev1[static_cast<std::size_t>(k)] = h.w[static_cast<std::size_t>(ns + nu + k)];
            }
            // state rows every stage; the exported node rows only at the end of the sample (as substepGroup)
            if (spLin) linOut(h, w, h.out.data());
            else if (!h.pdRow.empty()) {
                const int e0 = last ? 0 : h.hcY0;
                const int rows = static_cast<int>(h.hcOut.size()) - e0;
                gemv(h.Hc.data() + static_cast<std::size_t>(e0) * static_cast<std::size_t>(W), rows, W, w, h.hcY.data());
                for (int c = 0; c < rows; ++c) h.out[static_cast<std::size_t>(h.hcOut[static_cast<std::size_t>(e0 + c)])] = h.hcY[static_cast<std::size_t>(c)];
                for (std::size_t qq = 0; qq < h.pdRow.size(); ++qq) {
                    const int a = h.pdA[qq], b = h.pdB[qq];
                    if (last || h.pdRow[qq] >= h.ny)
                        h.out[static_cast<std::size_t>(h.pdRow[qq])] = (a >= 0 ? h.vp[static_cast<std::size_t>(a)] : 0.0) - (b >= 0 ? h.vp[static_cast<std::size_t>(b)] : 0.0);
                }
            } else if (last) gemv(h.H.data(), h.ny + ns, W, w, h.out.data());
            else gemv(h.H.data() + static_cast<std::size_t>(h.ny) * static_cast<std::size_t>(W), ns, W, w, h.out.data() + h.ny);
            if (last) {
                double* Vn = V_.data();
                for (int e = 0; e < h.ny; ++e) {
                    const double v = h.out[static_cast<std::size_t>(e)];
                    Vn[g.exports[static_cast<std::size_t>(e)]] = (v > -1e9 && v < 1e9) ? v : 0.0;
                }
                if (!g.cutTerms.empty()) {
                    std::swap(rt_[gi], slot);
                    accumulateCutTerms(gi, false);
                    std::swap(rt_[gi], slot);
                }
            }
            for (int k = 0; k < ns; ++k) {
                const auto kk = static_cast<std::size_t>(k);
                const double vc = h.out[static_cast<std::size_t>(h.ny + k)];
                h.vcLast[kk] = vc;
                w[k] = h.G2[kk] * vc + h.sgn[kk] * w[k];
            }
            if (st == 1)
                for (int k = 0; k < ns; ++k) q[k] += kTbE2 * dOf(k);
        }
    if (!ok) {
        restoreSide();
        return false;
    }
    // Embedded estimate of the last macro step (H = T/M) against the LTE control's per-state tolerance; as a full
    // trapezoidal step's ratio (× M³ · kTbTrap) it sets the next sample: a full step, or one or two macro steps.
    const double rel = kLteRel * opt_.lteTol;
    double worst = 0;
    for (int k = 0; k < ns; ++k) {
        const auto kk = static_cast<std::size_t>(k);
        const double est = (2.0 / kTbG) * (ind[kk] ? h.gc[kk] * q[k] : q[k] / h.gc[kk]);
        const double x1 = xOf(k);
        const double tol = rel * std::max(std::abs(x1), std::abs(x0[k])) + r.lteAbs[kk];
        worst = std::max(worst, std::abs(est) / tol);
        if (!r.lteWho.empty() && std::abs(est) / tol * static_cast<double>(M * M * M) * kTbTrap > 1.0) ++r.lteWho[kk]; // CE_LTEWHO
    }
    const double rhat = worst * static_cast<double>(M * M * M) * kTbTrap;
    r.lteLastRatio = rhat;
    r.tbNext = !(rhat * kEnvTbFull > 1.0) ? 1 : (rhat * kEnvTbScale <= 8.0 || mMax < 2 ? 2 : 4);
    r.subPrev = 2 * M;
    h.subGrid = 2;
    // back to the full-step runtime, as substepGroup
    for (int k = 0; k < ns; ++k) {
        const auto kk = static_cast<std::size_t>(k);
        const double v = h.vcLast[kk];
        r.w[kk] = r.gc[kk] * v + (h.w[kk] - h.gc[kk] * v);
        r.vcLast[kk] = v;
    }
    for (int k = 0; k < nn; ++k) {
        const auto kk = static_cast<std::size_t>(k);
        r.vpPrev2[kk] = r.vpPrev[kk];
        r.vpPrev[kk] = h.vp[kk];
        r.vp[kk] = h.vp[kk];
        r.w[static_cast<std::size_t>(ns + nu + k)] = h.w[static_cast<std::size_t>(ns + nu + k)];
    }
    anchor(r, r.vp.data(), r.pPrev.data());
    if (nn <= kMaxNlPorts) {
        std::copy(r.curPrev1.begin(), r.curPrev1.end(), r.curPrev2.begin());
        for (int k = 0; k < nn; ++k) r.curPrev1[static_cast<std::size_t>(k)] = h.w[static_cast<std::size_t>(ns + nu + k)];
    }
    std::copy(h.out.begin(), h.out.end(), r.out.begin());
    for (int jj = 0; jj < nu; ++jj) r.uPrev[static_cast<std::size_t>(jj)] = r.w[static_cast<std::size_t>(ns + jj)];
    for (int jj = 0; jj < nuc; ++jj) r.uPrev[static_cast<std::size_t>(nu + jj)] = r.w[static_cast<std::size_t>(ns + nu + nn + jj)];
    return true;
}
// ───────────────────────────── end of TR-BDF2 LTE substeps ─────────────────────────────

// Current each consumer element draws from its cut node(s) this sample (dc: capacitors open, inductors at 1 kS).
void Circuit::runGroup(std::size_t gi) noexcept {
    if (!logicAt_.empty() && !logicAt_[gi].empty()) runLogic(gi); // CMOS cells whose drive this group reads first
    const auto& g = groups_[gi];
    if (g.div <= 1 || (tick_ + static_cast<std::uint64_t>(g.phase)) % static_cast<std::uint64_t>(g.div) == 0) {
        if (!g.unread) processGroup(gi); // docs §10.60: a group nothing outside reads is not solved
        return;
    }
    for (const auto& [nd, i] : cutHold_[gi]) cutAdd(gi, nd, i); // held consumer currents
}

void Circuit::accumulateCutTerms(std::size_t gi, bool dc) noexcept {
    const auto& g = groups_[gi];
    auto& r = *rt_[gi];
    const double* w = r.w.data();
    auto* hold = !dc && g.div > 1 && gi < cutHold_.size() ? &cutHold_[gi] : nullptr;
    if (hold) hold->clear();
    for (const auto& t : g.cutTerms) {
        double i = 0;
        switch (t.kind) {
            case 0: {
                const auto& p = prims_[static_cast<std::size_t>(t.index)];
                const double val = p.slot >= 0 ? slots_[static_cast<std::size_t>(p.slot)].value : p.fixed;
                i = (V_[static_cast<std::size_t>(t.node)] - (t.other > 0 ? V_[static_cast<std::size_t>(t.other)] : 0.0)) / val;
                cutAdd(gi, t.node, cutScale_ * i); // already oriented node -> other
                if (hold) hold->push_back({t.node, cutScale_ * i});
                continue;
            }
            case 1:
            case 2: {
                const auto k = static_cast<std::size_t>(t.state);
                const double vc = r.out[static_cast<std::size_t>(r.ny) + k];
                if (dc) i = t.kind == 1 ? 0.0 : kDcInductorG * vc;
                else i = t.kind == 1 ? r.gc[k] * vc - w[k] : r.gc[k] * vc + w[k]; // a→b
                break;
            }
            case 3: i = w[r.ns + r.nu + t.index]; break; // from→to
            case 4: {
                const auto& p = prims_[static_cast<std::size_t>(t.index)];
                i = p.fixed * ((p.c > 0 ? V_[static_cast<std::size_t>(p.c)] : 0.0) - (p.d > 0 ? V_[static_cast<std::size_t>(p.d)] : 0.0));
                break;
            }
        }
        cutAdd(gi, t.node, cutScale_ * t.sign * i);
        if (hold) hold->push_back({t.node, cutScale_ * t.sign * i});
    }
}

void Circuit::railDraws() noexcept {
    // Headroom gate: a rail at or below the output (dead, or not yet charged) supplies nothing; full draw above ~0.4 V.
    if (drawsDone_) return;
    drawsDone_ = true;
    auto gate = [](double h) {
        if (h >= 2.2) return 1.0;
        if (h <= -1.8) return 0.0;
        return 1.0 / (1.0 + std::exp(-(h - 0.2) / 0.05));
    };
    for (const auto& ra : railAmps_) {
        const double io = cutAcc_[static_cast<std::size_t>(ra.out)]; // current the output sources into its loads
        const double vo = V_[static_cast<std::size_t>(ra.out)];
        if (ra.vp > 0) cutAcc_[static_cast<std::size_t>(ra.vp)] += gate(V_[static_cast<std::size_t>(ra.vp)] - vo) * (std::max(io, 0.0) + ra.iq);
        if (ra.vn > 0) cutAcc_[static_cast<std::size_t>(ra.vn)] -= gate(vo - V_[static_cast<std::size_t>(ra.vn)]) * (std::max(-io, 0.0) + ra.iq);
    }
}

void Circuit::finishCutSample() noexcept {
    if (!railAmps_.empty()) railDraws();
    drawsDone_ = false;
    for (std::size_t n = 0; n < cutI_.size(); ++n) {
        cutI_[n] = cutAcc_[n];
        cutAcc_[n] = 0.0;
    }
}

bool Circuit::settleBbdDc(const std::vector<std::uint8_t>* skip) noexcept {
    // The block outputs are known nodes: set them to the DC map of the sensed input and re-solve until they agree
    // (an input that depends on the output only through cap-coupled feedback settles at once).
    bool ok = true;
    for (int it = 0; it < 8; ++it) {
        double worst = 0;
        for (auto& b : bbds_) {
            const double y = b.line->dcOut(V_[static_cast<std::size_t>(b.in)]);
            worst = std::max(worst, std::abs(y - V_[static_cast<std::size_t>(b.src1)]));
            V_[static_cast<std::size_t>(b.src1)] = y;
            if (b.src2 >= 0) V_[static_cast<std::size_t>(b.src2)] = y;
        }
        if (worst < 1e-7) break;
        if (!cutI_.empty()) dcCutIterations(skip);
        for (std::size_t gi = 0; gi < groups_.size(); ++gi) {
            if (skip && (*skip)[gi]) continue;
            if (!solveGroupDc(gi)) ok = false;
            initGroupStates(gi);
        }
    }
    for (auto& b : bbds_)
        b.line->reset(V_[static_cast<std::size_t>(b.in)], b.ctl > 0 ? V_[static_cast<std::size_t>(b.ctl)] : 0.0,
                      b.clockCtl >= 0 ? ctlCur_[static_cast<std::size_t>(b.clockCtl)] : 0.5);
    return ok;
}

void Circuit::runBlocks() noexcept {
    for (auto& t : tapes_) {
        double out[3] = {0, 0, 0};
        const double i = (V_[static_cast<std::size_t>(t.recA)] - V_[static_cast<std::size_t>(t.recX)]) / t.rrec;
        t.loop->process(i, t.ctl >= 0 ? ctlCur_[static_cast<std::size_t>(t.ctl)] : 1.0, out);
        for (int h = 0; h < 3; ++h)
            if (t.src[h] >= 0) V_[static_cast<std::size_t>(t.src[h])] = out[h];
    }
    for (auto& b : bbds_) {
        const double y = b.line->process(V_[static_cast<std::size_t>(b.in)], b.ctl > 0 ? V_[static_cast<std::size_t>(b.ctl)] : 0.0,
                                         b.clockCtl >= 0 ? ctlCur_[static_cast<std::size_t>(b.clockCtl)] : 0.5);
        V_[static_cast<std::size_t>(b.src1)] = y; // drives the outputs from the next sample on (the line predicts it)
        if (b.src2 >= 0) V_[static_cast<std::size_t>(b.src2)] = y;
    }
    for (auto& b : blocks_) {
        const double drive = (V_[static_cast<std::size_t>(b.senseA)] - V_[static_cast<std::size_t>(b.senseB)]) / b.rin; // coil current
        b.y = b.tank->process(drive);
        V_[static_cast<std::size_t>(b.src)] = b.y; // drives the output transducer from the next sample on
    }
}

// Voice-coil thermal network per speaker (docs §10.21): Ctv·Tv' = P − (Tv − Tm)/Rtv, Ctm·Tm' = (Tv − Tm)/Rtv − Tm/Rtm (Tm ≡ 0
// without a magnet stage), P = Re(T)·i² per coil, trapezoidal over the step just solved; ΔRe = Re·α·Tv for the next one.
void Circuit::advanceSpeakers(const Group& g, const double* vp, double dt) noexcept {
    for (const auto& [ti, o] : g.spk) {
        auto& t = spkTh_[static_cast<std::size_t>(ti)];
        auto& d = devices_[static_cast<std::size_t>(t.dev)];
        const double x = vp[o], psi = vp[o + 2];
        const double xc = std::clamp(x, d.p[5], d.p[6]);
        const double cur = psi / (1.0 + xc * (d.p[11] + xc * d.p[12]));
        const double pw = (t.re + d.hs[0]) * cur * cur / t.coils;
        const double gv = 1.0 / t.rtv, gm = t.rtm > 0 ? 1.0 / t.rtm : 0.0, h = 0.5 * dt;
        const double a11 = -gv / t.ctv, a12 = gv / t.ctv, a21 = t.rtm > 0 ? gv / t.ctm : 0.0, a22 = t.rtm > 0 ? -(gv + gm) / t.ctm : 0.0;
        const double tv = d.hs[1], tm = d.hs[2], q = h * (pw + t.pPrev) / t.ctv;
        const double r1 = tv + h * (a11 * tv + a12 * tm) + q, r2 = tm + h * (a21 * tv + a22 * tm);
        const double m11 = 1.0 - h * a11, m12 = -h * a12, m21 = -h * a21, m22 = 1.0 - h * a22, det = m11 * m22 - m12 * m21;
        const double tvn = (r1 * m22 - m12 * r2) / det, tmn = t.rtm > 0 ? (m11 * r2 - m21 * r1) / det : 0.0;
        d.hs[1] = std::isfinite(tvn) ? tvn : 0.0;
        d.hs[2] = std::isfinite(tmn) ? tmn : 0.0;
        d.hs[0] = t.re * t.alpha * d.hs[1];
        t.pPrev = std::isfinite(pw) ? pw : 0.0;
    }
}

void Circuit::syncSpeakerDevices() noexcept {
    for (const auto& sd : spkDevs_)
        if (sd.le >= 0) devices_[static_cast<std::size_t>(sd.dev)].p[13] = sd.p13 * slots_[static_cast<std::size_t>(sd.le)].scale;
}

// fmod(x, 2π), bit-identical and without the libm call: the remainder x − k·c (c the double 2π) is exactly representable,
// so one fma with the right quotient k returns it exactly; trunc(x/c) is corrected when it lands one off.
static inline double fmod2Pi(double x) noexcept {
    constexpr double c = 2.0 * M_PI;
    if (!(std::abs(x) < 0x1p52)) return std::fmod(x, c);
    const double k = std::trunc(x / c);
    double r = std::fma(-k, c, x);
    if (x >= 0.0) {
        if (r < 0.0) r = std::fma(-(k - 1.0), c, x);
        else if (r >= c) r = std::fma(-(k + 1.0), c, x);
    } else {
        if (r > 0.0) r = std::fma(-(k + 1.0), c, x);
        else if (r <= -c) r = std::fma(-(k - 1.0), c, x);
    }
    return r;
}

void Circuit::advanceAc(double dt) noexcept {
    phase_ += dt;
    double lastW = -1.0, lastPh = 0.0, lastS = 0.0; // heaters share the mains' frequency and phase: one sine for all
    for (const auto& a : acSrc_) {
        if (a.w != lastW || a.phase != lastPh) {
            lastS = std::sin(fmod2Pi(a.w * phase_ + a.phase));
            lastW = a.w;
            lastPh = a.phase;
        }
        V_[static_cast<std::size_t>(a.node)] = acScale(a) * (a.dc + a.amp * lastS);
    }
}

void Circuit::refreshScaledSources() noexcept {
    for (const auto& a : acSrc_) {
        if (a.ctl < 0) continue;
        const double v = acScale(a) * (a.dc + a.amp * std::sin(std::fmod(a.w * phase_ + a.phase, 2.0 * M_PI)));
        knownValue_[static_cast<std::size_t>(a.node)] = v;
        V_[static_cast<std::size_t>(a.node)] = v;
    }
}

double Circuit::process(double vin) noexcept {
    if (ladder_) return processLadder(vin);
    if (gliding_ && subCounter_ <= 0) {
        updateControls();
        subCounter_ = refreshInterval_;
    }
    --subCounter_;
    if (!std::isfinite(vin)) vin = 0;
    V_[static_cast<std::size_t>(inputNode_)] = std::clamp(vin, -100.0, 100.0);
    if (inputNodeR_ > 0) V_[static_cast<std::size_t>(inputNodeR_)] = vinR_; // io.inputR (clamped in setInputR)
    if (!acSrc_.empty()) advanceAc(T_);
    if (lanesArmed_) {
        runLaneSample(); // intra-unit lanes (docs §10.33): the same groups, bit-identical
    } else if (profile_) {
        for (std::size_t g = 0; g < groups_.size(); ++g) {
            const auto t0 = std::chrono::steady_clock::now();
            runGroup(g);
            groupNs_[g] += std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
        }
    } else
        for (std::size_t g = 0; g < groups_.size(); ++g) runGroup(g);
    ++tick_;
    if (!cutI_.empty()) finishCutSample();
    if (!blocks_.empty() || !bbds_.empty() || !tapes_.empty()) runBlocks();
    if (!logicAt_.empty()) runLogic(groups_.size()); // CMOS cells after the sample
    ++diag_.samples;
    if (nEvo_) // docs §10.48: the output node(s) from the band-limited stream
        for (int i = 0; i < nEvo_; ++i) V_[static_cast<std::size_t>(evo_[static_cast<std::size_t>(i)].node)] = evosEmit(evo_[static_cast<std::size_t>(i)]);
    return V_[static_cast<std::size_t>(outputNode_)];
}

// One base-rate sample of the rate ladder: groups in topological order, each 2^level times, its inputs taken at its own
// level (the producer's samples, or a half-band cascade of them), its exports kept per sub-sample for its consumers.
// Cut currents from a group at level L count 1/2^L per sub-sample, so the owner sees their mean. Rails and other known
// nodes are held over the base sample, as in the 2x multirate mode.
void Circuit::runLadderConv(LadderConv& c, const double* src) noexcept {
    double a[32], b[32]; // ping-pong stage buffers (no copies between stages)
    int n = 1 << c.from;
    const double* in = src;
    double* out = a;
    for (auto& s : c.st) {
        if (c.to > c.from) {
            for (int i = 0; i < n; ++i) s.up(in[i], out[2 * i], out[2 * i + 1]);
            n *= 2;
        } else {
            n /= 2;
            for (int i = 0; i < n; ++i) out[i] = s.down(in[2 * i], in[2 * i + 1]);
        }
        in = out;
        out = out == a ? b : a;
    }
    for (int i = 0; i < n; ++i) c.buf[static_cast<std::size_t>(i)] = in[i];
}

double Circuit::processLadder(double vin) noexcept {
    if (gliding_ && subCounter_ <= 0) {
        updateControls();
        subCounter_ = refreshInterval_;
    }
    --subCounter_;
    if (!std::isfinite(vin)) vin = 0;
    double* Vn = V_.data();
    const auto in = static_cast<std::size_t>(inputNode_);
    Vn[in] = blk_[in * 32] = std::clamp(vin, -100.0, 100.0);
    if (inputNodeR_ > 0) Vn[static_cast<std::size_t>(inputNodeR_)] = blk_[static_cast<std::size_t>(inputNodeR_) * 32] = vinR_;
    if (!acSrc_.empty()) advanceAc(T_);
    for (std::size_t gi = 0; gi < groups_.size(); ++gi) {
        const auto& g = groups_[gi];
        const int n = 1 << g.level;
        auto& ins = ladderIn_[gi];
        for (const auto& [x, c] : ins)
            if (c >= 0) runLadderConv(lconv_[static_cast<std::size_t>(c)], &blk_[static_cast<std::size_t>(x) * 32]);
        std::chrono::steady_clock::time_point t0;
        if (profile_) t0 = std::chrono::steady_clock::now();
        cutScale_ = 1.0 / n;
        const bool acSub = n > 1 && ladderAc_[gi]; // mains read by a faster group: evaluated at its sub-sample times
        for (int k = 0; k < n; ++k) {
            if (acSub) {
                const double t = phase_ - T_ + T_ * (k + 1) / n;
                double lastW = -1.0, lastPh = 0.0, lastS = 0.0;
                auto put = [&](const AcSource& a) {
                    if (a.w != lastW || a.phase != lastPh) {
                        lastS = std::sin(fmod2Pi(a.w * t + a.phase));
                        lastW = a.w;
                        lastPh = a.phase;
                    }
                    V_[static_cast<std::size_t>(a.node)] = acScale(a) * (a.dc + a.amp * lastS);
                };
                // only the sources this group reads; the last sub-sample writes every AC node, as before, so the
                // groups after this one read the same values (bit-identical)
                if (k == n - 1)
                    for (const auto& a : acSrc_) put(a);
                else
                    for (int ai : ladderAcIdx_[gi]) put(acSrc_[static_cast<std::size_t>(ai)]);
            }
            for (const auto& [x, c] : ins)
                Vn[x] = c >= 0 ? lconv_[static_cast<std::size_t>(c)].buf[static_cast<std::size_t>(k)] : blk_[static_cast<std::size_t>(x) * 32 + static_cast<std::size_t>(k)];
            ladderSub_ = k;
            if (ladderCuts_)
                for (int e : lcutOf_[gi]) lcutSave_[static_cast<std::size_t>(e)] = cutAcc_[static_cast<std::size_t>(lcut_[static_cast<std::size_t>(e)].node)];
            runGroup(gi);
            if (ladderCuts_)
                for (int e : lcutOf_[gi]) {
                    auto& lc = lcut_[static_cast<std::size_t>(e)];
                    lc.buf[static_cast<std::size_t>(k)] = (cutAcc_[static_cast<std::size_t>(lc.node)] - lcutSave_[static_cast<std::size_t>(e)]) * n;
                }
            for (int e : g.exports) blk_[static_cast<std::size_t>(e) * 32 + static_cast<std::size_t>(k)] = Vn[e];
        }
        cutScale_ = 1.0;
        if (ladderCuts_) // this group's cut currents, at their owners' levels, for the owners' next base sample
            for (int e : lcutOf_[gi]) {
                auto& lc = lcut_[static_cast<std::size_t>(e)];
                const double* src = lc.buf.data();
                if (lc.conv >= 0) {
                    auto& cv = lcutConv_[static_cast<std::size_t>(lc.conv)];
                    runLadderConv(cv, src);
                    src = cv.buf.data();
                }
                const int m = 1 << groups_[static_cast<std::size_t>(lc.owner)].level;
                double* dst = &cutNext_[static_cast<std::size_t>(lc.node) * 32];
                for (int i = 0; i < m; ++i) dst[i] += src[i];
            }
        if (profile_) groupNs_[gi] += std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
    }
    ++tick_;
    if (!cutI_.empty()) finishCutSample();
    if (ladderCuts_)
        for (int x : lcutNodes_) {
            double* nx = &cutNext_[static_cast<std::size_t>(x) * 32];
            double* pl = &cutPlay_[static_cast<std::size_t>(x) * 32];
            for (int i = 0; i < 32; ++i) {
                pl[i] = nx[i];
                nx[i] = 0.0;
            }
        }
    if (!blocks_.empty() || !bbds_.empty() || !tapes_.empty()) runBlocks();
    ++diag_.samples;
    const auto out = static_cast<std::size_t>(outputNode_);
    if (outConv_ >= 0) {
        auto& c = lconv_[static_cast<std::size_t>(outConv_)];
        runLadderConv(c, &blk_[out * 32]);
        Vn[out] = c.buf[0];
    }
    if (outConvR_ >= 0) {
        auto& c = lconv_[static_cast<std::size_t>(outConvR_)];
        runLadderConv(c, &blk_[static_cast<std::size_t>(outputNodeR_) * 32]);
        Vn[outputNodeR_] = c.buf[0];
    }
    return Vn[out];
}

void Circuit::processBlock(const double* in, double* out, int n) noexcept {
    const ScopedFlushDenormals ftz;
    for (int i = 0; i < n; ++i) out[i] = process(in[i]);
}

std::string Circuit::describeGroups() const {
    std::ostringstream o;
    for (std::size_t gi = 0; gi < groups_.size(); ++gi) {
        const auto& g = groups_[gi];
        o << "group " << gi << ": " << solverName(g.solver) << ", " << g.unknowns.size() << " unknowns, " << g.states.size()
          << " states, " << g.nn << " NL ports, " << g.inputs.size() << " inputs, " << g.exports.size() << " exports";
        const auto& r = *rt_[gi];
        if (g.nn > 0) { // Newton tolerance (docs §10.42): the group's, or per port where tube ports sit beside others
            if (r.ptol.empty()) o << ", tol " << r.tol;
            else o << ", tol " << r.tol << " / tube ports " << *std::max_element(r.ptol.begin(), r.ptol.end()) << " ("
                   << std::count_if(r.ptol.begin(), r.ptol.end(), [&](double t) { return t != r.tol; }) << " of " << g.nn << ")";
        }
        if (r.solves) {
            o << ", newton " << static_cast<double>(r.iters) / static_cast<double>(r.solves) << " it avg / " << r.maxIt << " max [";
            for (int h = 0; h < 6; ++h) o << (h ? " " : "") << std::lround(1000.0 * static_cast<double>(r.itHist[h]) / static_cast<double>(r.solves)) / 10.0;
            o << " % at 1/2/3/4-6/7-12/>12 it]";
        }
        if (r.lteEvents) { // P1 profiling: LTE events and the substep runtimes' Newton (full-step runtime above)
            o << ", lte " << r.lteEvents << " events (" << r.lteDetect << " detected on a full step); substep time " << r.subNs * 1e-6 << " ms; substep newton";
            auto sub = [&](const char* name, const GroupRt* x) {
                if (!x || !x->solves) return;
                o << " " << name << " " << static_cast<double>(x->iters) / static_cast<double>(x->solves) << " it x " << x->solves << " [";
                for (int q = 0; q < 6; ++q) o << (q ? " " : "") << std::lround(1000.0 * static_cast<double>(x->itHist[q]) / static_cast<double>(x->solves)) / 10.0;
                o << "]";
            };
            sub("T/2", rtHalf_[gi].get());
            if (gi < rtTr_.size()) sub("T/4", rtTr_[gi][1].get());
            if (gi < rtBe_.size()) sub("start/2", rtBe_[gi][0].get()), sub("start/4", rtBe_[gi][1].get());
            if (gi < rtBe_.size() && r.tb) sub("tb-gT", rtTr_[gi][2].get()), sub("tb-gT/2", rtBe_[gi][2].get()); // docs §10.47
            if (gi < rtBe_.size() && !r.tb) sub("T/8", rtTr_[gi][2].get()), sub("start/8", rtBe_[gi][2].get()); // docs §10.62
            if (rtHalf_[gi] && !rtHalf_[gi]->ptol.empty() && !r.ptol.empty() && rtHalf_[gi]->ptol != r.ptol) // docs §10.64
                o << " (substep tube-port tolerance x" << *std::max_element(rtHalf_[gi]->ptol.begin(), rtHalf_[gi]->ptol.end()) / *std::max_element(r.ptol.begin(), r.ptol.end()) << ")";
            else if (rtHalf_[gi] && rtHalf_[gi]->tol != r.tol) o << " (substep tolerance x" << rtHalf_[gi]->tol / r.tol << ", docs §10.64)";
            if (kEnvLteStat) {
                o << "; ltestat";
                const char* kn[3] = {" detected", " continued", " next"};
                for (int k = 0; k < 3; ++k) {
                    o << kn[k] << " m1/2/3/4 " << r.stM[k][1] << "/" << r.stM[k][2] << "/" << r.stM[k][3] << "/" << r.stM[k][4] << " ratio";
                    for (int b = 0; b < 8; ++b) o << (b ? "," : " ") << r.stR[k][b];
                }
                o << " regime " << r.stRegime[0] << "/" << r.stRegime[1] << " chains";
                for (int b = 1; b < 65; ++b) if (r.stChain[b]) o << " " << b << ":" << r.stChain[b];
            }
            if (!r.lteWho.empty()) { // CE_LTEWHO: the states over tolerance, most often first
                std::vector<std::pair<std::uint32_t, int>> v;
                for (int k = 0; k < r.ns; ++k) if (r.lteWho[static_cast<std::size_t>(k)]) v.push_back({r.lteWho[static_cast<std::size_t>(k)], k});
                std::sort(v.rbegin(), v.rend());
                o << "; over tolerance:";
                for (const auto& [c, k] : v) {
                    const auto& pr = prims_[static_cast<std::size_t>(g.states[static_cast<std::size_t>(k)])];
                    o << " " << (pr.part >= 0 ? net_.parts[static_cast<std::size_t>(pr.part)].ref : std::string("?")) << "#" << k << "=" << c;
                }
            }
        }
        if (r.ss && (r.ss->hits || r.ss->tries)) { // GR_LESSONS P2
            const auto& q = *r.ss;
            o << ", small-signal " << q.hits << " samples (" << std::lround(1000.0 * static_cast<double>(q.hits) / static_cast<double>(std::max<std::uint64_t>(1, q.hits + r.solves))) / 10.0
              << " %), " << q.freezes << " freezes / " << q.tries << " tries, " << q.exits << " exits";
            if (profile_ && q.tries) o << ", freeze " << std::lround(q.freezeNs / static_cast<double>(q.tries)) << " ns avg / " << std::lround(q.freezeMaxNs) << " max";
            if (kEnvSsCheck) o << ", check worst " << q.checkWorst << " x tol";
        }
        if (g.lowRate) o << ", host rate";
        if (g.level > 0) o << ", rate x" << (1 << g.level);
        if (g.div > 1) o << ", slow (1/" << g.div << ")";
        o << "; parts:";
        for (int p : g.parts) o << " " << net_.parts[static_cast<std::size_t>(p)].ref;
        o << "\n";
    }
    return o.str();
}

double Circuit::flopsPerSample() const noexcept {
    double f = 0;
    const double avgIt = diag_.samples ? static_cast<double>(diag_.newtonIterations) / static_cast<double>(diag_.samples) : 2.0;
    for (std::size_t gi = 0; gi < groups_.size(); ++gi) {
        const auto& r = *rt_[gi];
        const double nn = r.nn;
        f += 2.0 * (r.ny + r.ns) * r.W + 2.0 * nn * (r.ns + r.nu);
        if (nn > 0) f += (avgIt / std::max<std::size_t>(1, groups_.size())) * (2.0 / 3.0 * nn * nn * nn + 4 * nn * nn + 40 * nn);
    }
    return f;
}

} // namespace rig::ce

namespace rig::ce {

std::vector<std::complex<double>> Circuit::smallSignalResponse(const std::vector<double>& freqs, int outNode) const {
    // Exact discrete-time small-signal response at the present operating point, including the approximate cuts:
    // every group is linearised to s' = A s + B [u; c], [y; vc; i_port] = C s + D [u; c], where c are the cut
    // injections (one sample late: c = -z^-1 (I_load - G_est V)). Per frequency the groups are swept in order and
    // the cut currents iterated to their fixed point (the one-sample couplings are contractions by construction).
    using cd = std::complex<double>;
    const std::size_t N = nodeNames_.size();
    std::vector<cd> res;
    struct Lin {
        int ns, nu, nc, ny, nn;
        std::vector<double> A, B, C, D; // B: ns×(nu+nc); C: (ny+ns+nn)×ns; D: (ny+ns+nn)×(nu+nc)
    };
    std::vector<Lin> lin(groups_.size());
    for (std::size_t gi = 0; gi < groups_.size(); ++gi) {
        const auto& g = groups_[gi];
        const auto& r = *rt_[gi];
        Lin L;
        L.ns = r.ns;
        L.nu = r.nu;
        L.nc = r.nuc;
        L.ny = r.ny;
        L.nn = r.nn;
        const int ns = r.ns, nu = r.nu, nn = r.nn, nc = r.nuc, W = r.W, sw = ns + nu, sx = ns + nu + nc;
        auto colW = [&](int c) { return c < sw ? c : sw + nn + (c - sw); }; // [s;u;c] column -> W column
        // vp = Lsu·[s;u;c], i = Jf·vp
        std::vector<double> Jf(static_cast<std::size_t>(nn * nn), 0.0), Lsu(static_cast<std::size_t>(nn * sx), 0.0);
        if (nn > 0) {
            int o = 0;
            std::vector<double> cur(static_cast<std::size_t>(nn));
            for (int di : g.devices) {
                const auto& d = devices_[static_cast<std::size_t>(di)];
                double Jl[9];
                d.eval(r.vp.data() + o, cur.data() + o, Jl);
                for (int a2 = 0; a2 < d.nports; ++a2)
                    for (int b2 = 0; b2 < d.nports; ++b2) Jf[static_cast<std::size_t>((o + a2) * nn + o + b2)] = Jl[a2 * d.nports + b2];
                o += d.nports;
            }
            std::vector<double> Mm(static_cast<std::size_t>(nn * nn));
            for (int a2 = 0; a2 < nn; ++a2)
                for (int b2 = 0; b2 < nn; ++b2) {
                    double sum = a2 == b2 ? 1.0 : 0.0;
                    for (int k = 0; k < nn; ++k) sum -= r.K[static_cast<std::size_t>(a2 * nn + k)] * Jf[static_cast<std::size_t>(k * nn + b2)];
                    Mm[static_cast<std::size_t>(a2 * nn + b2)] = sum;
                }
            std::vector<int> piv(static_cast<std::size_t>(nn));
            if (!la::luFactor(Mm.data(), nn, piv.data())) return {};
            std::vector<double> col(static_cast<std::size_t>(nn));
            for (int c = 0; c < sx; ++c) {
                for (int k = 0; k < nn; ++k)
                    col[static_cast<std::size_t>(k)] = c < sw ? r.Dp[static_cast<std::size_t>(k * sw + c)] : r.Dc[static_cast<std::size_t>(k * nc + (c - sw))];
                la::luSolve(Mm.data(), piv.data(), nn, col.data());
                for (int k = 0; k < nn; ++k) Lsu[static_cast<std::size_t>(k * sx + c)] = col[static_cast<std::size_t>(k)];
            }
        }
        // rows of [y; vc] (from H) and the port currents i = Jf·Lsu, as functions of [s; u; c]
        const int rows = r.ny + ns;
        std::vector<double> R(static_cast<std::size_t>((rows + nn) * sx), 0.0);
        for (int e = 0; e < rows; ++e) {
            const double* h = &r.H[static_cast<std::size_t>(e * W)];
            for (int c = 0; c < sx; ++c) {
                double sum = h[colW(c)];
                for (int k = 0; k < nn; ++k) {
                    double jl = 0;
                    for (int m = 0; m < nn; ++m) jl += Jf[static_cast<std::size_t>(k * nn + m)] * Lsu[static_cast<std::size_t>(m * sx + c)];
                    sum += h[sw + k] * jl;
                }
                R[static_cast<std::size_t>(e * sx + c)] = sum;
            }
        }
        for (int k = 0; k < nn; ++k)
            for (int c = 0; c < sx; ++c) {
                double jl = 0;
                for (int m = 0; m < nn; ++m) jl += Jf[static_cast<std::size_t>(k * nn + m)] * Lsu[static_cast<std::size_t>(m * sx + c)];
                R[static_cast<std::size_t>((rows + k) * sx + c)] = jl;
            }
        const int nx = nu + nc, outRows = rows + nn;
        L.A.assign(static_cast<std::size_t>(ns * ns), 0.0);
        L.B.assign(static_cast<std::size_t>(ns * nx), 0.0);
        L.C.assign(static_cast<std::size_t>(outRows * ns), 0.0);
        L.D.assign(static_cast<std::size_t>(outRows * nx), 0.0);
        for (int k = 0; k < ns; ++k) {
            for (int c = 0; c < ns; ++c) L.A[static_cast<std::size_t>(k * ns + c)] = r.G2[static_cast<std::size_t>(k)] * R[static_cast<std::size_t>((r.ny + k) * sx + c)] + (k == c ? r.sgn[static_cast<std::size_t>(k)] : 0.0);
            for (int c = 0; c < nx; ++c) L.B[static_cast<std::size_t>(k * nx + c)] = r.G2[static_cast<std::size_t>(k)] * R[static_cast<std::size_t>((r.ny + k) * sx + ns + c)];
        }
        for (int e = 0; e < outRows; ++e) {
            for (int c = 0; c < ns; ++c) L.C[static_cast<std::size_t>(e * ns + c)] = R[static_cast<std::size_t>(e * sx + c)];
            for (int c = 0; c < nx; ++c) L.D[static_cast<std::size_t>(e * nx + c)] = R[static_cast<std::size_t>(e * sx + ns + c)];
        }
        lin[gi] = std::move(L);
    }
    const bool cuts = !cutI_.empty();
    std::vector<std::vector<cd>> lu(groups_.size());
    std::vector<std::vector<int>> pv(groups_.size());
    std::vector<std::vector<cd>> S(groups_.size()), Y(groups_.size()), X(groups_.size());
    for (double f : freqs) {
        std::vector<cd> V(N, cd(0.0)), I(N, cd(0.0)), Inew(N, cd(0.0));
        // Factor (zI − A) per group for this frequency.
        for (std::size_t gi = 0; gi < groups_.size(); ++gi) {
            const auto& L = lin[gi];
            const int ns = L.ns;
            const cd z = std::polar(1.0, 2.0 * M_PI * f * groups_[gi].T); // per-group rate (converters assumed flat)
            auto& M = lu[gi];
            auto& P = pv[gi];
            M.assign(static_cast<std::size_t>(ns * ns), cd(0.0));
            P.assign(static_cast<std::size_t>(ns), 0);
            for (int a2 = 0; a2 < ns; ++a2)
                for (int c = 0; c < ns; ++c) M[static_cast<std::size_t>(a2 * ns + c)] = (a2 == c ? z : cd(0.0)) - L.A[static_cast<std::size_t>(a2 * ns + c)];
            for (int k = 0; k < ns; ++k) {
                int p = k;
                for (int rr = k + 1; rr < ns; ++rr)
                    if (std::abs(M[static_cast<std::size_t>(rr * ns + k)]) > std::abs(M[static_cast<std::size_t>(p * ns + k)])) p = rr;
                P[static_cast<std::size_t>(k)] = p;
                if (p != k) // LINPACK-style: earlier multipliers stay put; the solve applies the swaps progressively
                    for (int c = k; c < ns; ++c) std::swap(M[static_cast<std::size_t>(k * ns + c)], M[static_cast<std::size_t>(p * ns + c)]);
                const cd piv = M[static_cast<std::size_t>(k * ns + k)];
                for (int rr = k + 1; rr < ns; ++rr) {
                    const cd fct = M[static_cast<std::size_t>(rr * ns + k)] / piv;
                    M[static_cast<std::size_t>(rr * ns + k)] = fct;
                    for (int c = k + 1; c < ns; ++c) M[static_cast<std::size_t>(rr * ns + c)] -= fct * M[static_cast<std::size_t>(k * ns + c)];
                }
            }
        }
        // One sweep of the groups in order, with the cut injections taken from the previous sweep's state (I: consumer
        // currents, Vprev: the owner's own cut-node voltage). The sweep is an affine map of that state.
        std::vector<cd> Vprev(N, cd(0.0));
        auto sweepOnce = [&]() {
            V.assign(N, cd(0.0));
            V[static_cast<std::size_t>(inputNode_)] = 1.0; // known DC sources are AC ground
            if (inputNodeR_ > 0) V[static_cast<std::size_t>(inputNodeR_)] = 1.0; // io.inputR: the common-mode (L = R) response
            std::fill(Inew.begin(), Inew.end(), cd(0.0));
            for (std::size_t gi = 0; gi < groups_.size(); ++gi) {
                const auto& g = groups_[gi];
                const auto& L = lin[gi];
                const int ns = L.ns, nx = L.nu + L.nc;
                const cd zinv = std::polar(1.0, -2.0 * M_PI * f * g.T);
                auto& x = X[gi];
                x.assign(static_cast<std::size_t>(nx), cd(0.0));
                for (int j = 0; j < L.nu; ++j) x[static_cast<std::size_t>(j)] = V[static_cast<std::size_t>(g.inputs[static_cast<std::size_t>(j)])];
                // The owner's own cut-node voltage comes from its previous sweep (it is solved below).
                for (int j = 0; j < L.nc; ++j) {
                    const auto nd = static_cast<std::size_t>(g.cutIn[static_cast<std::size_t>(j)]);
                    x[static_cast<std::size_t>(L.nu + j)] = -zinv * (I[nd] - tranLoadG_[nd] * Vprev[nd]);
                }
                auto& st = S[gi];
                st.assign(static_cast<std::size_t>(ns), cd(0.0));
                for (int a2 = 0; a2 < ns; ++a2) {
                    cd b2 = 0.0;
                    for (int c = 0; c < nx; ++c) b2 += L.B[static_cast<std::size_t>(a2 * nx + c)] * x[static_cast<std::size_t>(c)];
                    st[static_cast<std::size_t>(a2)] = b2;
                }
                const auto& M = lu[gi];
                const auto& P = pv[gi];
                for (int k = 0; k < ns; ++k) {
                    const int p = P[static_cast<std::size_t>(k)];
                    if (p != k) std::swap(st[static_cast<std::size_t>(k)], st[static_cast<std::size_t>(p)]);
                    for (int rr = k + 1; rr < ns; ++rr) st[static_cast<std::size_t>(rr)] -= M[static_cast<std::size_t>(rr * ns + k)] * st[static_cast<std::size_t>(k)];
                }
                for (int k = ns - 1; k >= 0; --k) {
                    cd sum = st[static_cast<std::size_t>(k)];
                    for (int c = k + 1; c < ns; ++c) sum -= M[static_cast<std::size_t>(k * ns + c)] * st[static_cast<std::size_t>(c)];
                    st[static_cast<std::size_t>(k)] = sum / M[static_cast<std::size_t>(k * ns + k)];
                }
                const int outRows = L.ny + ns + L.nn;
                auto& y = Y[gi];
                y.assign(static_cast<std::size_t>(outRows), cd(0.0));
                for (int e = 0; e < outRows; ++e) {
                    cd v = 0.0;
                    for (int c = 0; c < ns; ++c) v += L.C[static_cast<std::size_t>(e * ns + c)] * st[static_cast<std::size_t>(c)];
                    for (int c = 0; c < nx; ++c) v += L.D[static_cast<std::size_t>(e * nx + c)] * x[static_cast<std::size_t>(c)];
                    y[static_cast<std::size_t>(e)] = v;
                }
                for (int e = 0; e < L.ny; ++e) V[static_cast<std::size_t>(g.exports[static_cast<std::size_t>(e)])] = y[static_cast<std::size_t>(e)];
                // This group's cut-term currents (as accumulateCutTerms, linearised).
                const auto& r = *rt_[gi];
                for (const auto& t : g.cutTerms) {
                    cd i = 0.0;
                    switch (t.kind) {
                        case 0: {
                            const auto& pr = prims_[static_cast<std::size_t>(t.index)];
                            const double val = pr.slot >= 0 ? slots_[static_cast<std::size_t>(pr.slot)].value : pr.fixed;
                            Inew[static_cast<std::size_t>(t.node)] += (V[static_cast<std::size_t>(t.node)] - (t.other > 0 ? V[static_cast<std::size_t>(t.other)] : cd(0.0))) / val;
                            continue;
                        }
                        case 1:
                        case 2: {
                            const auto k = static_cast<std::size_t>(t.state);
                            const cd vc = y[static_cast<std::size_t>(L.ny) + k];
                            i = t.kind == 1 ? r.gc[k] * vc - st[k] : r.gc[k] * vc + st[k];
                            break;
                        }
                        case 3: i = y[static_cast<std::size_t>(L.ny + ns + t.index)]; break;
                        case 4: {
                            const auto& pr = prims_[static_cast<std::size_t>(t.index)];
                            i = pr.fixed * ((pr.c > 0 ? V[static_cast<std::size_t>(pr.c)] : cd(0.0)) - (pr.d > 0 ? V[static_cast<std::size_t>(pr.d)] : cd(0.0)));
                            break;
                        }
                    }
                    Inew[static_cast<std::size_t>(t.node)] += t.sign * i;
                }
            }
            if (!railAmps_.empty()) {
                auto gate = [](double h, double& dg) {
                    if (h >= 2.2) { dg = 0.0; return 1.0; }
                    if (h <= -1.8) { dg = 0.0; return 0.0; }
                    const double g = 1.0 / (1.0 + std::exp(-(h - 0.2) / 0.05));
                    dg = g * (1.0 - g) / 0.05;
                    return g;
                };
                for (const auto& ra : railAmps_) {
                    const auto out = static_cast<std::size_t>(ra.out);
                    const double io0 = out < cutI_.size() ? cutI_[out] : 0.0;
                    const double vo0 = V_[out];
                    const cd dio = Inew[out], dvo = V[out];
                    if (ra.vp > 0) {
                        const auto vp = static_cast<std::size_t>(ra.vp);
                        double dg = 0.0;
                        const double g = gate(V_[vp] - vo0, dg);
                        const double s = io0 > 1e-15 ? 1.0 : (io0 < -1e-15 ? 0.0 : 0.5);
                        Inew[vp] += dg * (std::max(io0, 0.0) + ra.iq) * (V[vp] - dvo) + g * s * dio;
                    }
                    if (ra.vn > 0) {
                        const auto vn = static_cast<std::size_t>(ra.vn);
                        double dg = 0.0;
                        const double g = gate(vo0 - V_[vn], dg);
                        const double s = io0 < -1e-15 ? -1.0 : (io0 > 1e-15 ? 0.0 : -0.5);
                        Inew[vn] -= dg * (std::max(-io0, 0.0) + ra.iq) * (dvo - V[vn]) + g * s * dio;
                    }
                }
            }
        };
        if (!cuts) {
            sweepOnce();
            res.push_back(V[static_cast<std::size_t>(outNode)]);
            continue;
        }
        // The cut couplings make the sweep an affine map s' = M s + b of the state s = [I; Vprev] at the cut nodes.
        // Iterating it to the fixed point (as this did up to 2026-11) diverges wherever M's spectral radius exceeds 1,
        // which a stable circuit allows: a relaxation LFO linearised at its (unstable) balance point, say, coupled through
        // a shared rail (DMM, RE-201: AC "errors" of thousands of dB). Solve (I − M) s = b directly instead: M from
        // 2K + 1 sweeps, then one complex LU. Where the iteration converged, this is the same fixed point.
        std::vector<int> cn;               // cut nodes (owned by some group)
        std::vector<std::uint8_t> ownExp;  // the owner exports the node (else its Vprev term is 0, as before)
        for (const auto& g : groups_)
            for (int nd : g.cutIn) {
                if (std::find(cn.begin(), cn.end(), nd) != cn.end()) continue;
                cn.push_back(nd);
                ownExp.push_back(std::find(g.exports.begin(), g.exports.end(), nd) != g.exports.end() ? 1 : 0);
            }
        const int K = static_cast<int>(cn.size()), K2 = 2 * K;
        auto runState = [&](const std::vector<cd>& s, std::vector<cd>& out) {
            std::fill(I.begin(), I.end(), cd(0.0));
            std::fill(Vprev.begin(), Vprev.end(), cd(0.0));
            for (int k = 0; k < K; ++k) {
                const auto nd = static_cast<std::size_t>(cn[static_cast<std::size_t>(k)]);
                I[nd] = s[static_cast<std::size_t>(k)];
                Vprev[nd] = ownExp[static_cast<std::size_t>(k)] ? s[static_cast<std::size_t>(K + k)] : cd(0.0);
            }
            sweepOnce();
            for (int k = 0; k < K; ++k) {
                const auto nd = static_cast<std::size_t>(cn[static_cast<std::size_t>(k)]);
                out[static_cast<std::size_t>(k)] = Inew[nd];
                out[static_cast<std::size_t>(K + k)] = ownExp[static_cast<std::size_t>(k)] ? V[nd] : cd(0.0);
            }
        };
        std::vector<cd> sv(static_cast<std::size_t>(K2), cd(0.0)), b(static_cast<std::size_t>(K2)), colv(static_cast<std::size_t>(K2));
        std::vector<cd> Mx(static_cast<std::size_t>(K2) * static_cast<std::size_t>(K2));
        runState(sv, b);
        for (int j = 0; j < K2; ++j) {
            sv[static_cast<std::size_t>(j)] = 1.0;
            runState(sv, colv);
            sv[static_cast<std::size_t>(j)] = 0.0;
            for (int i = 0; i < K2; ++i)
                Mx[static_cast<std::size_t>(i * K2 + j)] = (i == j ? cd(1.0) : cd(0.0)) - (colv[static_cast<std::size_t>(i)] - b[static_cast<std::size_t>(i)]);
        }
        // Gaussian elimination with partial pivoting on [I − M | b].
        for (int k = 0; k < K2; ++k) {
            int p = k;
            for (int r2 = k + 1; r2 < K2; ++r2)
                if (std::abs(Mx[static_cast<std::size_t>(r2 * K2 + k)]) > std::abs(Mx[static_cast<std::size_t>(p * K2 + k)])) p = r2;
            if (p != k) {
                for (int c = 0; c < K2; ++c) std::swap(Mx[static_cast<std::size_t>(k * K2 + c)], Mx[static_cast<std::size_t>(p * K2 + c)]);
                std::swap(b[static_cast<std::size_t>(k)], b[static_cast<std::size_t>(p)]);
            }
            const cd piv = Mx[static_cast<std::size_t>(k * K2 + k)];
            if (std::abs(piv) < 1e-300) continue; // a cut state that nothing reads: leave it at 0
            for (int r2 = k + 1; r2 < K2; ++r2) {
                const cd fct = Mx[static_cast<std::size_t>(r2 * K2 + k)] / piv;
                if (fct == cd(0.0)) continue;
                for (int c = k; c < K2; ++c) Mx[static_cast<std::size_t>(r2 * K2 + c)] -= fct * Mx[static_cast<std::size_t>(k * K2 + c)];
                b[static_cast<std::size_t>(r2)] -= fct * b[static_cast<std::size_t>(k)];
            }
        }
        for (int k = K2 - 1; k >= 0; --k) {
            cd sum = b[static_cast<std::size_t>(k)];
            for (int c = k + 1; c < K2; ++c) sum -= Mx[static_cast<std::size_t>(k * K2 + c)] * sv[static_cast<std::size_t>(c)];
            const cd piv = Mx[static_cast<std::size_t>(k * K2 + k)];
            sv[static_cast<std::size_t>(k)] = std::abs(piv) < 1e-300 ? cd(0.0) : sum / piv;
        }
        runState(sv, colv);
        res.push_back(V[static_cast<std::size_t>(outNode)]);
    }
    return res;
}

} // namespace rig::ce

namespace rig::ce {

// ───────────────────────────── Intra-unit lanes (docs §10.33) ─────────────────────────────
// Two cores on one circuit, within each sample. Groups that share no node value within a sample (they are linked only
// by cut currents, which are read one sample late, or not at all) may run at the same time; every pair that does share
// one (a consumer reading its cut node or input, or an earlier group reading a later one's last-sample value) keeps
// group order. compile() plans two lanes from that graph and estimated group costs, and only when the critical path
// shortens by enough to pay for the synchronisation. The audio thread runs lane A; an RtPool task (laneHelper) runs
// lane B. Every lane-B group is taken by claim (CAS of its sample number): whichever thread gets it first runs it, so
// the audio thread runs whatever the helper has not started and never waits on a helper that is not running.
// Bit-identity: a group's arithmetic depends only on its own state and on the nodes it shares (ordered as above); its
// noise draws come from its own generator; its cut currents go to a per-group record that is added to cutAcc_ in group
// order after the sample (the sequential order of every sum); its solver counters go to per-group Diagnostics.
namespace {
const bool kEnvNoLanes = std::getenv("CE_NOLANES") != nullptr;  // developer A/B: never plan lanes
const char* const kEnvLanesB = std::getenv("CE_LANES");        // developer: lane B = these groups ("7,8,9"), no heuristic
const bool kEnvLaneDiag = std::getenv("CE_LANEDIAG") != nullptr; // print each compile's lane analysis (stderr)
// Cost model (ns per sample, fitted to CE_PROFILE on the JC-120, Ryzen 9 9900X): the linear update's flops and a Newton
// solve's at about 1.5 factorisations per sample, plus per-group and per-port overheads.
constexpr double kLaneFlopsPerNs = 6.0, kLaneGroupNs = 40.0, kLaneNlGroupNs = 150.0, kLanePortNs = 30.0;
// Synchronisation: a cross-lane dependency (a cache-line hand-off and the spin that sees it), and the per-sample start
// and end (stamp, done words, cut records). Lanes engage when the estimated two-lane sample is at least
// max(kLaneMinGainNs, kLaneMinGainFrac · sequential) shorter, the helper's lane has at least kLaneMinWorkNs, and the
// circuit is heavy: an estimated kLaneMinLoad of one core at its rate. The helper spins on a second core for the whole
// block, so a light unit is not worth it: on the 9900X the RE-201 (est. 24 % of a core, two lanes 80 %) measured
// 18.3 -> 15.8 %, the Memory Man (27 %, 86 %) 17.5 -> 16.1 %, the SSL 4000E dynamics (64 %, 87 %) 35.0 -> 31.7 %.
constexpr double kLaneSyncNs = 150.0, kLaneSampleNs = 400.0, kLaneMinGainNs = 800.0, kLaneMinGainFrac = 0.20, kLaneMinWorkNs = 800.0;
constexpr double kLaneMinLoad = 0.30;
// The helper detaches after this long without a new sample, or waiting on one dependency (the audio thread preempted).
// 30 µs detached the 9900X's helper in 27 % of 64-frame blocks (vCPU stalls under WSL2); 100 µs in 2 %, 300 µs in 0.04 %.
const std::int64_t kLaneIdleNs = std::getenv("CE_LANEIDLE") ? std::atoll(std::getenv("CE_LANEIDLE")) : 250000;

inline void laneRelax() noexcept {
#if defined(__aarch64__) || defined(__arm64__)
    __asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(_M_X64)
    _mm_pause();
#endif
}
inline std::int64_t laneNowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
// The audio thread's floating-point control (flush-to-zero, rounding): the helper runs its groups in the same mode.
inline std::uint64_t laneFpMode() noexcept {
#if defined(__aarch64__)
    std::uint64_t v;
    asm volatile("mrs %0, fpcr" : "=r"(v));
    return v;
#elif defined(__x86_64__) || defined(_M_X64)
    return _mm_getcsr();
#else
    return 0;
#endif
}
inline void laneSetFpMode(std::uint64_t v) noexcept {
#if defined(__aarch64__)
    asm volatile("msr fpcr, %0" ::"r"(v));
#elif defined(__x86_64__) || defined(_M_X64)
    _mm_setcsr(static_cast<unsigned>(v));
#else
    (void)v;
#endif
}
} // namespace

void Circuit::cutAdd(std::size_t gi, int node, double x) noexcept {
    if (!laneDuo_) {
        cutAcc_[static_cast<std::size_t>(node)] += x;
        return;
    }
    auto& L = *lane_;
    auto* r = L.rec + L.recOff[gi];
    int& n = r[0].first;
    if (n < L.recCap[gi]) r[1 + n++] = {node, x};
}

void Circuit::buildLanes() {
    lane_.reset();
    laneDiag_.clear();
    const std::size_t G = groups_.size();
    // Not planned: one group; the rate ladder and the 2x multirate mode (their own sample loops); CMOS logic cells
    // (they run between groups and write nodes).
    if (G < 2 || !opt_.lanes || !net_.lanes || kEnvNoLanes || ladder_ || multirate_ || !logic_.empty()) return;
    // Who writes and who reads each node within a sample.
    std::vector<std::vector<int>> writers(nodeNames_.size());
    for (std::size_t gi = 0; gi < G; ++gi)
        for (int e : groups_[gi].exports) writers[static_cast<std::size_t>(e)].push_back(static_cast<int>(gi));
    std::vector<std::set<int>> dep(G);
    auto touch = [&](std::size_t gi, int nd) {
        if (nd <= 0 || static_cast<std::size_t>(nd) >= writers.size()) return;
        for (int w : writers[static_cast<std::size_t>(nd)]) {
            const auto wi = static_cast<std::size_t>(w);
            if (wi != gi) dep[std::max(wi, gi)].insert(static_cast<int>(std::min(wi, gi)));
        }
    };
    for (std::size_t gi = 0; gi < G; ++gi) {
        const auto& g = groups_[gi];
        for (int x : g.inputs) touch(gi, x);
        for (int x : g.cutIn) touch(gi, x);
        for (int x : g.exports) touch(gi, x); // two writers of one node (never in a valid partition): keep their order
        for (const auto& t : g.cutTerms) {
            touch(gi, t.node);
            touch(gi, t.other);
            if (t.kind == 4) {
                const auto& p = prims_[static_cast<std::size_t>(t.index)];
                touch(gi, p.c);
                touch(gi, p.d);
            }
        }
    }
    std::vector<double> cost(G);
    double seq = 0;
    for (std::size_t gi = 0; gi < G; ++gi) {
        const auto& g = groups_[gi];
        const double nn = g.nn, ns = static_cast<double>(g.states.size()), nu = static_cast<double>(g.inputs.size());
        const double ny = static_cast<double>(g.exports.size()), W = ns + nu + nn + static_cast<double>(g.cutIn.size());
        double f = 2.0 * (ny + ns) * W + 2.0 * nn * (ns + nu);
        if (nn > 0) f += 1.5 * (2.0 / 3.0 * nn * nn * nn + 4.0 * nn * nn + 40.0 * nn);
        double c = kLaneGroupNs + f / kLaneFlopsPerNs + (nn > 0 ? kLaneNlGroupNs + kLanePortNs * nn : 0.0);
        if (g.div > 1) c /= g.div;
        cost[gi] = c;
        seq += c;
    }
    // Estimated sample with lane assignment b (1 = lane B): each lane runs its groups in group order.
    std::vector<double> fin(G);
    auto simulate = [&](const std::vector<std::uint8_t>& b) {
        double t[2] = {0.0, 0.0};
        bool anyB = false;
        for (std::size_t gi = 0; gi < G; ++gi) {
            const int l = b[gi];
            anyB |= l == 1;
            double st = t[l];
            for (int d : dep[gi]) st = std::max(st, fin[static_cast<std::size_t>(d)] + (b[static_cast<std::size_t>(d)] != l ? kLaneSyncNs : 0.0));
            fin[gi] = st + cost[gi];
            t[l] = fin[gi];
        }
        return std::max(t[0], t[1]) + (anyB ? kLaneSampleNs : 0.0);
    };
    std::vector<std::uint8_t> best(G, 0);
    double bestT = seq;
    if (kEnvLanesB && *kEnvLanesB) {
        for (const char* q = kEnvLanesB; *q;) {
            const int gi = std::atoi(q);
            if (gi >= 0 && static_cast<std::size_t>(gi) < G) best[static_cast<std::size_t>(gi)] = 1;
            while (*q && *q != ',') ++q;
            if (*q == ',') ++q;
        }
        bestT = simulate(best);
    } else {
        // Starts: everything on lane A, and a two-processor list schedule (upward rank, earliest finish); each improved
        // by single-group moves until none helps.
        std::vector<std::vector<int>> succ(G);
        for (std::size_t gi = 0; gi < G; ++gi)
            for (int d : dep[gi]) succ[static_cast<std::size_t>(d)].push_back(static_cast<int>(gi));
        std::vector<double> rank(G);
        for (std::size_t k = G; k-- > 0;) {
            double m = 0;
            for (int s : succ[k]) m = std::max(m, kLaneSyncNs + rank[static_cast<std::size_t>(s)]);
            rank[k] = cost[k] + m;
        }
        std::vector<int> order(G);
        std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(), [&](int a, int c) { return rank[static_cast<std::size_t>(a)] > rank[static_cast<std::size_t>(c)]; });
        std::vector<std::uint8_t> ls(G, 0);
        {
            double t[2] = {0.0, 0.0};
            std::vector<double> f(G, 0.0);
            for (int gi : order) {
                double eft[2];
                for (int l = 0; l < 2; ++l) {
                    double st = t[l];
                    for (int d : dep[static_cast<std::size_t>(gi)])
                        st = std::max(st, f[static_cast<std::size_t>(d)] + (ls[static_cast<std::size_t>(d)] != l ? kLaneSyncNs : 0.0));
                    eft[l] = st + cost[static_cast<std::size_t>(gi)];
                }
                const int l = eft[1] < eft[0] ? 1 : 0;
                ls[static_cast<std::size_t>(gi)] = static_cast<std::uint8_t>(l);
                f[static_cast<std::size_t>(gi)] = eft[l];
                t[l] = eft[l];
            }
        }
        for (const auto& start : {std::vector<std::uint8_t>(G, 0), ls}) {
            auto b = start;
            double bt = simulate(b);
            for (bool improved = true; improved;) {
                improved = false;
                for (std::size_t gi = 0; gi < G; ++gi) {
                    b[gi] ^= 1;
                    const double v = simulate(b);
                    if (v < bt - 1e-9) {
                        bt = v;
                        improved = true;
                    } else {
                        b[gi] ^= 1;
                    }
                }
            }
            if (bt < bestT) bestT = bt, best = b;
        }
    }
    double workB = 0;
    for (std::size_t gi = 0; gi < G; ++gi)
        if (best[gi]) workB += cost[gi];
    const bool forced = kEnvLanesB && *kEnvLanesB;
    const double load = seq * 1e-9 * fs_; // estimated share of one core, sequential
    const bool engage = workB > 0 && G < 256 &&
                        (forced || (seq - bestT >= std::max(kLaneMinGainNs, kLaneMinGainFrac * seq) && workB >= kLaneMinWorkNs && load >= kLaneMinLoad));
    if (kEnvLaneDiag) {
        std::string a, b;
        for (std::size_t gi = 0; gi < G; ++gi) (best[gi] ? b : a) += " " + std::to_string(gi) + ":" + std::to_string(static_cast<int>(cost[gi]));
        std::fprintf(stderr, "lanes %s: %zu groups, est. sequential %.0f ns, two lanes %.0f ns (%.0f %%), lane B work %.0f ns, load %.0f %%: %s\n  A:%s\n  B:%s\n",
                     net_.id.c_str(), G, seq, bestT, 100.0 * bestT / std::max(seq, 1e-9), workB, 100.0 * load, engage ? "ENGAGED" : "sequential", a.c_str(),
                     b.c_str());
        std::string d;
        for (std::size_t gi = 0; gi < G; ++gi) {
            if (dep[gi].empty()) continue;
            d += " " + std::to_string(gi) + "<-";
            for (int x : dep[gi]) d += std::to_string(x) + ",";
            d.pop_back();
        }
        std::fprintf(stderr, "  deps:%s\n", d.c_str());
    }
    if (!engage) return;
    auto L = std::make_unique<LanePlan>();
    L->inB = best;
    L->cost = cost;
    L->seqCost = seq;
    L->parCost = bestT;
    L->deps.resize(G);
    L->depsB.resize(G);
    L->recOff.resize(G);
    L->recCap.resize(G);
    L->needDone.assign(G, 0);
    int off = 0;
    for (std::size_t gi = 0; gi < G; ++gi) {
        L->deps[gi].assign(dep[gi].begin(), dep[gi].end());
        if (best[gi]) {
            L->laneB.push_back(static_cast<int>(gi));
            for (int d : dep[gi])
                if (!best[static_cast<std::size_t>(d)]) L->needDone[static_cast<std::size_t>(d)] = 1;
        } else {
            L->laneA.push_back(static_cast<int>(gi));
            for (int d : dep[gi])
                if (best[static_cast<std::size_t>(d)]) L->depsB[gi].push_back(d);
        }
    }
    for (int lane = 0; lane < 2; ++lane) // lane A's regions, then lane B's
        for (std::size_t gi = 0; gi < G; ++gi) {
            if (best[gi] != lane) continue;
            // At most one accumulateCutTerms (or the held currents, as many) per group and sample; twice that for margin.
            L->recOff[gi] = off;
            L->recCap[gi] = 2 * static_cast<int>(groups_[gi].cutTerms.size());
            off += (1 + L->recCap[gi] + 3) / 4 * 4; // the count and the entries, whole cache lines
        }
    L->recLines.assign(static_cast<std::size_t>(off / 4), LanePlan::RecLine{});
    L->rec = L->recLines.empty() ? nullptr : L->recLines[0].e;
    L->claim.reset(new LanePlan::Word[G]);
    L->done.reset(new LanePlan::Word[G]);
    lane_ = std::move(L);
    laneDiag_.assign(G, LaneDiag{});
}

bool Circuit::beginLanes() noexcept {
    if (!lane_ || lanesArmed_ || profile_ || debugFail_) return false;
    auto& L = *lane_;
    L.fpMode = laneFpMode();
    L.present.store(0, std::memory_order_relaxed);
    L.stamp.store(L.seq << 1, std::memory_order_release); // open; no two-lane sample yet
    lanesArmed_ = true;
    ++L.blocks;
    L.blockDuo = false;
    L.blockSolo = 0;
    return true;
}

void Circuit::endLanes() noexcept {
    if (!lanesArmed_) return;
    lanesArmed_ = false;
    lane_->stamp.store(LanePlan::kClosed, std::memory_order_release); // a waiting helper returns
    laneDuo_ = false;
    if (lane_->blockDuo) lane_->soloLead += lane_->blockSolo;
    mergeLaneDiag();
}

void Circuit::mergeLaneDiag() noexcept {
    for (auto& x : laneDiag_) {
        auto& d = x.d;
        diag_.newtonIterations += d.newtonIterations;
        diag_.homotopies += d.homotopies;
        diag_.failures += d.failures;
        diag_.refreshes += d.refreshes;
        diag_.substeps += d.substeps;
        diag_.lteSubsteps += d.lteSubsteps;
        diag_.ptcRescues += d.ptcRescues;
        diag_.activeSolves += d.activeSolves;
        diag_.activeMisses += d.activeMisses;
        diag_.maxIterations = std::max(diag_.maxIterations, d.maxIterations);
        diag_.worstResidual = std::max(diag_.worstResidual, d.worstResidual);
        d = Diagnostics{};
    }
}

bool Circuit::laneEnsure(std::size_t gi, std::uint64_t s) noexcept {
    auto& L = *lane_;
    auto& dn = L.done[gi].v;
    if (dn.load(std::memory_order_acquire) >= s) return false;
    auto& cl = L.claim[gi].v;
    std::uint64_t e = cl.load(std::memory_order_relaxed);
    if (e < s && cl.compare_exchange_strong(e, s, std::memory_order_acq_rel)) {
        // Lane-A dependencies are done (they come earlier in group order); lane-B ones are claimed here or waited for.
        for (int d : L.deps[gi])
            if (L.inB[static_cast<std::size_t>(d)]) laneEnsure(static_cast<std::size_t>(d), s);
        runGroup(gi);
        dn.store(s, std::memory_order_release);
        ++L.steals;
        return true;
    }
    while (dn.load(std::memory_order_acquire) < s) laneRelax(); // the helper is running it
    return false;
}

void Circuit::runLaneSample() noexcept {
    auto& L = *lane_;
    const std::size_t G = groups_.size();
    const std::uint64_t s = ++L.seq;
    if (!laneDuo_) {
        if (L.present.load(std::memory_order_acquire) == 0) { // no helper attached yet: the plain loop
            ++L.solo;
            ++L.blockSolo;
            for (std::size_t g = 0; g < G; ++g) runGroup(g);
            return;
        }
        // Two lanes from here to the end of the block (a helper that detaches leaves every group to this thread).
        laneDuo_ = true;
        L.blockDuo = true;
    }
    ++L.duo;
    L.stamp.store((s << 1) | 1u, std::memory_order_release);
    for (int gi : L.laneA) {
        const auto g = static_cast<std::size_t>(gi);
        for (int d : L.depsB[g]) laneEnsure(static_cast<std::size_t>(d), s);
        runGroup(g);
        if (L.needDone[g]) L.done[g].v.store(s, std::memory_order_release);
    }
    // The helper went through lane B (every group there is done: by the helper, or by this thread, which claimed it).
    // Otherwise the groups from its cursor on are claimed and run here, or waited for while the helper runs them.
    if (L.doneB.load(std::memory_order_acquire) != s) {
        const std::uint64_t cur = L.cursorB.load(std::memory_order_acquire);
        const std::size_t k0 = (cur >> 8) == s ? static_cast<std::size_t>(cur & 0xffu) : 0;
        for (std::size_t k = k0; k < L.laneB.size(); ++k) laneEnsure(static_cast<std::size_t>(L.laneB[k]), s);
    }
    // The sample's cut currents in group order: the sums of the sequential loop, term for term.
    for (std::size_t g = 0; g < G; ++g) {
        auto* r = L.rec + L.recOff[g];
        const int n = r[0].first;
        for (int k = 1; k <= n; ++k) cutAcc_[static_cast<std::size_t>(r[k].first)] += r[k].second;
        r[0].first = 0;
    }
}

void Circuit::laneHelper(void* p) noexcept {
    auto& c = *static_cast<Circuit*>(p);
    if (!c.lane_) return;
    auto& L = *c.lane_;
    std::uint64_t x = L.stamp.load(std::memory_order_acquire);
    if (x == LanePlan::kClosed) return; // the block is over (the join reclaimed this task)
    const std::uint64_t fpSaved = laneFpMode();
    laneSetFpMode(L.fpMode);
    // Samples up to the stamp read before attaching are finished: a two-lane sample starts only once present is set.
    std::uint64_t last = x >> 1, ran = 0;
    L.present.store(1, std::memory_order_seq_cst);
    L.attaches.fetch_add(1, std::memory_order_relaxed);
    std::int64_t t0 = laneNowNs();
    int spins = 0;
    for (bool live = true; live;) {
        x = L.stamp.load(std::memory_order_acquire);
        if (x == LanePlan::kClosed) break;
        const std::uint64_t s = x >> 1;
        if (!(x & 1u) || s <= last) {
            laneRelax();
            if ((++spins & 63) == 0 && laneNowNs() - t0 > kLaneIdleNs) {
                L.idleDetaches.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            continue;
        }
        last = s;
        for (std::size_t k = 0; k < L.laneB.size() && live; ++k) {
            const int gi = L.laneB[k];
            L.cursorB.store((s << 8) | k, std::memory_order_release);
            auto& cl = L.claim[static_cast<std::size_t>(gi)].v;
            if (cl.load(std::memory_order_relaxed) >= s) continue; // the audio thread took it
            bool ready = true;
            for (int d : L.deps[static_cast<std::size_t>(gi)]) {
                const auto& dn = L.done[static_cast<std::size_t>(d)].v;
                if (dn.load(std::memory_order_acquire) >= s) continue;
                const std::int64_t w0 = laneNowNs();
                for (int n = 0; dn.load(std::memory_order_acquire) < s;) {
                    if (cl.load(std::memory_order_relaxed) >= s) {
                        ready = false;
                        break;
                    }
                    laneRelax();
                    if ((++n & 63) == 0 && laneNowNs() - w0 > kLaneIdleNs) { // the audio thread is not running: detach
                        L.waitDetaches.fetch_add(1, std::memory_order_relaxed);
                        ready = live = false;
                        break;
                    }
                }
                if (!ready) break;
            }
            if (!ready) continue;
            std::uint64_t e = cl.load(std::memory_order_relaxed);
            if (e >= s || !cl.compare_exchange_strong(e, s, std::memory_order_acq_rel)) continue;
            c.runGroup(static_cast<std::size_t>(gi));
            L.done[static_cast<std::size_t>(gi)].v.store(s, std::memory_order_release);
            ++ran;
        }
        if (live) L.doneB.store(s, std::memory_order_release);
        t0 = laneNowNs();
        spins = 0;
    }
    L.present.store(0, std::memory_order_release);
    L.helperGroups.fetch_add(ran, std::memory_order_relaxed);
    laneSetFpMode(fpSaved);
}

Circuit::LaneStats Circuit::laneStats() const noexcept {
    LaneStats st;
    if (!lane_) return st;
    st.duoSamples = lane_->duo;
    st.soloSamples = lane_->solo;
    st.steals = lane_->steals;
    st.helperGroups = lane_->helperGroups.load(std::memory_order_relaxed);
    st.blocks = lane_->blocks;
    st.attaches = lane_->attaches.load(std::memory_order_relaxed);
    st.idleDetaches = lane_->idleDetaches.load(std::memory_order_relaxed);
    st.waitDetaches = lane_->waitDetaches.load(std::memory_order_relaxed);
    st.soloBeforeAttach = lane_->soloLead;
    return st;
}

std::string Circuit::describeLanes() const {
    if (!lane_) return "lanes: none (sequential)\n";
    const auto& L = *lane_;
    std::ostringstream o;
    auto list = [&](const std::vector<int>& v) {
        std::string s;
        for (int g : v) s += " " + std::to_string(g);
        return s;
    };
    o << "lanes: A {" << list(L.laneA) << " } B {" << list(L.laneB) << " }; est. " << static_cast<int>(L.seqCost) << " ns sequential, "
      << static_cast<int>(L.parCost) << " ns on two lanes\n";
    return o.str();
}


// ───────────────────────────── Event-local oversampling (docs §10.48; 2026-11-14) ─────────────────────────────
// CE_EVOS=1: a group the rate ladder would raise to level L (L ≤ 2) stays at the base rate (compile) with its LTE control on,
// and every LTE decision over one substep takes M = 2^L substeps (lteSubsteps, lteSubstepsFromHistory), so an event (a
// power tube cutting off into the output transformer's leakage) is solved at the ladder's rate and the rest of the time at
// the base rate. The output node's stream at M× the base rate is band-limited by construction:
//   - inside a substepped sample, the substeps' own values (evosRecord);
//   - elsewhere, a Kaiser-windowed sinc interpolation of the base-rate samples, with P samples of lookahead: it puts
//     nothing above the base Nyquist into the stream where the base-rate samples are clean;
//   - the base-rate sample itself at the end of every interval;
// then a linear-phase Kaiser FIR down to the base rate (CE_EVOS_IIR=1: the ladder's own half-band stages). The output is
// therefore delayed by exactly P − 1 + Q base samples (8 at the defaults); every other node is read undelayed.
// Netlists opt in with "multirate": {"rates": {...}, "events": true} (Netlist::rateEvents); CE_EVOS overrides (0 / 1).
void Circuit::evosPlan(const CompileOptions& opt) {
    nEvo_ = 0;
    evo_.clear();
    evosM_ = 1;
    evosGroup_.assign(groups_.size(), 0);
    int L = 0;
    for (std::size_t gi = 0; gi < groups_.size(); ++gi)
        if (groups_[gi].evos > 0 && rt_[gi] && rt_[gi]->lteOn) {
            L = std::max(L, groups_[gi].evos);
            evosGroup_[gi] = 1;
        }
    if (L == 0) return;
    evo_.assign(2, EvosOut{});
    evosM_ = 1 << L;
    evosP_ = std::clamp(kEnvEvosP, 1, (EvosOut::kRing - 2) / 2);
    for (std::size_t gi = 0; gi < groups_.size(); ++gi) {
        if (!evosGroup_[gi]) continue;
        auto& r = *rt_[gi];
        r.evosM = evosM_;
        r.tb = false; // the TR-BDF2 sequences (docs §10.47) do not record the stream
    }
    for (int node : {outputNode_, outputNodeR_}) {
        if (node <= 0) continue;
        for (std::size_t gi = 0; gi < groups_.size(); ++gi) {
            if (!evosGroup_[gi]) continue;
            const auto& ex = groups_[gi].exports;
            const auto it = std::find(ex.begin(), ex.end(), node);
            if (it == ex.end()) continue;
            auto& e = evo_[static_cast<std::size_t>(nEvo_++)];
            e = EvosOut{};
            e.node = node;
            e.group = static_cast<int>(gi);
            e.exportIdx = static_cast<int>(it - ex.begin());
            e.nst = L;
            e.st.assign(static_cast<std::size_t>(L), HalfbandStage{});
            for (int lv = L; lv >= 1; --lv) e.st[static_cast<std::size_t>(L - lv)].design(opt.ladderBaseStage + lv); // down: the highest stage first
        }
    }
    // Interpolation taps: phase j (sub-position j/M after s[n−1]) weights s[n−1+i], i = −P+1 … P, by the ideal low-pass at
    // fc of the base Nyquist under a Kaiser window of half-width P; each phase normalised to unit DC gain.
    const int P = evosP_, M = evosM_;
    evosH_.assign(static_cast<std::size_t>((M - 1) * 2 * P), 0.0);
    auto i0 = [](double x) { // modified Bessel function of the first kind, order 0
        double sum = 1.0, t = 1.0;
        for (int k = 1; k < 60; ++k) {
            t *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += t;
        }
        return sum;
    };
    const double fc = kEnvEvosFc, beta = kEnvEvosBeta;
    for (int j = 1; j < M; ++j) {
        const double f = static_cast<double>(j) / M;
        double sum = 0;
        for (int i = -P + 1; i <= P; ++i) {
            const double x = i - f, u = x / P;
            const double sinc = x == 0.0 ? 1.0 : std::sin(M_PI * fc * x) / (M_PI * fc * x);
            const double win = std::abs(u) < 1.0 ? i0(beta * std::sqrt(1.0 - u * u)) / i0(beta) : 0.0;
            const double h = fc * sinc * win;
            evosH_[static_cast<std::size_t>((j - 1) * 2 * P + (i + P - 1))] = h;
            sum += h;
        }
        for (int i = 0; i < 2 * P; ++i) evosH_[static_cast<std::size_t>((j - 1) * 2 * P + i)] /= sum;
    }
    // Decimator: N = 2·M·Q + 1 taps at M×, the ideal low-pass at the base Nyquist under a Kaiser window, unit DC gain.
    evosQ_ = std::clamp(kEnvEvosQ, 1, (static_cast<int>(EvosOut{}.zh.size()) - 1) / (2 * M));
    const int N = 2 * M * evosQ_ + 1, c0 = M * evosQ_;
    evosD_.assign(static_cast<std::size_t>(N), 0.0);
    double dsum = 0;
    for (int k = 0; k < N; ++k) {
        const double x = static_cast<double>(k - c0) / M, u = static_cast<double>(k - c0) / c0; // x in base samples
        const double sinc = x == 0.0 ? 1.0 : std::sin(M_PI * kEnvEvosDFc * x) / (M_PI * kEnvEvosDFc * x);
        const double win = i0(kEnvEvosDBeta * std::sqrt(std::max(0.0, 1.0 - u * u))) / i0(kEnvEvosDBeta);
        evosD_[static_cast<std::size_t>(k)] = sinc * win;
        dsum += sinc * win;
    }
    for (auto& h : evosD_) h /= dsum;
}

void Circuit::evosPrime() noexcept {
    for (int i = 0; i < nEvo_; ++i) {
        auto& e = evo_[static_cast<std::size_t>(i)];
        const double v = V_[static_cast<std::size_t>(e.node)];
        e.s.fill(v);
        e.sub.fill(v);
        e.rec.fill(v);
        e.ev.fill(0);
        e.head = 0;
        e.zh.fill(v);
        e.zhHead = 0;
        for (int k = 0; k < e.nst; ++k) e.st[static_cast<std::size_t>(k)].prime(v, false);
    }
}

// The output node's value at substep `step` (1 … m − 1) of runtime h, from the same rows the substep computes for its
// states (h.out holds every row on the sparse path; else the port-defined row or the export's row of Hc / H).
void Circuit::evosRecord(std::size_t gi, GroupRt& h, const double* w, int step, int m) noexcept {
    if (step >= m || step > EvosOut::kMaxM) return;
    const bool spLin = h.sl && h.sl->valid && !h.noiseOn;
    const int W = h.W;
    for (int i = 0; i < nEvo_; ++i) {
        auto& e = evo_[static_cast<std::size_t>(i)];
        if (e.group != static_cast<int>(gi)) continue;
        const int x = e.exportIdx;
        double v = 0.0;
        bool done = false;
        if (spLin) v = h.out[static_cast<std::size_t>(x)], done = true;
        else if (!h.pdRow.empty()) {
            for (std::size_t q = 0; q < h.pdRow.size() && !done; ++q)
                if (h.pdRow[q] == x) {
                    const int a = h.pdA[q], b = h.pdB[q];
                    v = (a >= 0 ? h.vp[static_cast<std::size_t>(a)] : 0.0) - (b >= 0 ? h.vp[static_cast<std::size_t>(b)] : 0.0);
                    done = true;
                }
            for (std::size_t c = 0; c < h.hcOut.size() && !done; ++c)
                if (h.hcOut[c] == x) {
                    const double* row = &h.Hc[c * static_cast<std::size_t>(W)];
                    for (int k = 0; k < W; ++k) v += row[k] * w[k];
                    done = true;
                }
        }
        if (!done) {
            const double* row = &h.H[static_cast<std::size_t>(x) * static_cast<std::size_t>(W)];
            for (int k = 0; k < W; ++k) v += row[k] * w[k];
        }
        e.rec[static_cast<std::size_t>(step - 1)] = (v > -1e9 && v < 1e9) ? v : 0.0;
    }
}

double Circuit::evosEmit(EvosOut& e) noexcept {
    constexpr int R = EvosOut::kRing, MM = EvosOut::kMaxM;
    const int M = evosM_, P = evosP_;
    const auto& r = *rt_[static_cast<std::size_t>(e.group)];
    e.head = (e.head + 1) % R;
    const int c = e.head;
    e.s[static_cast<std::size_t>(c)] = V_[static_cast<std::size_t>(e.node)];
    const bool ev = r.subPrev == M; // this sample was solved as M substeps (an LTE event)
    e.ev[static_cast<std::size_t>(c)] = ev ? 1 : 0;
    if (ev)
        for (int j = 0; j < M - 1; ++j) e.sub[static_cast<std::size_t>(c * MM + j)] = e.rec[static_cast<std::size_t>(j)];
    // the interval (n − 1, n] with n = c − (P − 1): its interpolation reads s up to n − 1 + P = c
    const int n = (c - (P - 1) + R) % R;
    double z[MM], y[MM];
    if (e.ev[static_cast<std::size_t>(n)]) {
        for (int j = 0; j < M - 1; ++j) z[j] = e.sub[static_cast<std::size_t>(n * MM + j)];
    } else {
        double win[2 * 16];
        for (int i = 0; i < 2 * P; ++i) win[i] = e.s[static_cast<std::size_t>((n - P + i + R) % R)]; // s[n−P] … s[n−1+P]
        for (int j = 0; j < M - 1; ++j) {
            const double* h = &evosH_[static_cast<std::size_t>(j * 2 * P)];
            double acc = 0.0;
            for (int i = 0; i < 2 * P; ++i) acc += h[i] * win[i];
            z[j] = acc;
        }
    }
    z[M - 1] = e.s[static_cast<std::size_t>(n)];
    if (!kEnvEvosIir) { // linear-phase decimator: one output per base sample, centred on an interval's end (delay Q)
        constexpr int Z = static_cast<int>(sizeof(e.zh) / sizeof(double));
        for (int j = 0; j < M; ++j) e.zh[static_cast<std::size_t>((e.zhHead + 1 + j) % Z)] = z[j];
        e.zhHead = (e.zhHead + M) % Z;
        const int N = static_cast<int>(evosD_.size());
        double acc = 0.0;
        for (int k = 0; k < N; ++k) acc += evosD_[static_cast<std::size_t>(k)] * e.zh[static_cast<std::size_t>((e.zhHead - k + Z) % Z)];
        return acc;
    }
    int len = M;
    double* in = z;
    double* out = y;
    for (int k = 0; k < e.nst; ++k) {
        len /= 2;
        auto& st = e.st[static_cast<std::size_t>(k)];
        for (int i = 0; i < len; ++i) out[i] = st.down(in[2 * i], in[2 * i + 1]);
        std::swap(in, out);
    }
    return in[0];
}


// ───────────────────────────── Nodal charge states (docs §10.51; 2026-11-14) ─────────────────────────────
// CPU_THEORY R6.2: 17-23 % of a tube group's trapezoidal states sit at eigenvalue -1 with no path to any output. They are
// capacitor loops: a loop's capacitors stamp R columns (±1 in the KCL rows of their terminals; ground, known and input
// nodes stamp nothing) that are linearly dependent, so the solve sees the states only through A·s, A the capacitors'
// incidence on the group's KCL rows, and the loop's circulating combination is invisible. nodalPlan finds them exactly:
// a spanning forest of the group's eligible capacitors over the vertices {KCL unknowns} ∪ {datum: everything else},
// built breadth-first from the datum (short loops); every other eligible capacitor (co-tree) closes a loop, and its
// column equals Σ γ_t·(tree column t) with γ = ±1 along the forest path between its ends. Eligible: trapezoidal
// capacitors (not "be"), not read one by one elsewhere (cut terms, a transformer core's flux node).
// What this buys (CE_NODALINFO=1 prints it per group, after the linear update has been chosen): a co-tree column costs
// nn + rows multiply-adds in a dense linear update, and its 1-2 nonzeros in the sparse one (SparseLin, every big group of
// every heavy amp since §10.44); folding it into the tree states costs Σ|γ| adds per sample. See §10.51.
namespace {
const bool kEnvNodalInfo = std::getenv("CE_NODALINFO") != nullptr;
} // namespace

void Circuit::nodalPlan() {
    for (auto& g : groups_) g.nodCo.clear(), g.nodOff.clear(), g.nodT.clear(), g.nodS.clear();
    if (!kEnvNodalInfo) return; // a diagnostic only (§10.51): the default compile does no work here
    std::vector<std::uint8_t> fluxNode(nodeNames_.size(), 0);
    for (const auto& mg : mags_)
        if (mg.flux > 0) fluxNode[static_cast<std::size_t>(mg.flux)] = 1;
    for (auto& g : groups_) {
        const int ns = static_cast<int>(g.states.size());
        g.nodCo.assign(static_cast<std::size_t>(ns), 0);
        g.nodOff.assign(static_cast<std::size_t>(ns) + 1, 0);
        g.nodT.clear();
        g.nodS.clear();
        if (g.solver == SolverKind::Wdf || ns < 2) continue;
        std::map<int, int> vx; // KCL unknown -> vertex (1..); everything else is the datum, vertex 0
        for (int u : g.unknowns)
            if (nodeKind_[static_cast<std::size_t>(u)] == kFree) vx.emplace(u, static_cast<int>(vx.size()) + 1);
        auto vOf = [&](int nd) {
            const auto it = vx.find(nd);
            return it == vx.end() ? 0 : it->second;
        };
        std::vector<std::uint8_t> elig(static_cast<std::size_t>(ns), 0);
        std::vector<int> ea(static_cast<std::size_t>(ns), 0), eb(static_cast<std::size_t>(ns), 0);
        for (int k = 0; k < ns; ++k) {
            const auto& p = prims_[static_cast<std::size_t>(g.states[static_cast<std::size_t>(k)])];
            if (p.kind != Prim::Cap || p.be) continue;
            if ((p.a > 0 && fluxNode[static_cast<std::size_t>(p.a)]) || (p.b > 0 && fluxNode[static_cast<std::size_t>(p.b)])) continue;
            elig[static_cast<std::size_t>(k)] = 1;
            ea[static_cast<std::size_t>(k)] = vOf(p.a);
            eb[static_cast<std::size_t>(k)] = vOf(p.b);
        }
        for (const auto& t : g.cutTerms)
            if ((t.kind == 1 || t.kind == 2) && t.state >= 0 && t.state < ns) elig[static_cast<std::size_t>(t.state)] = 0;
        const int nv = static_cast<int>(vx.size()) + 1;
        std::vector<std::vector<int>> adj(static_cast<std::size_t>(nv));
        for (int k = 0; k < ns; ++k)
            if (elig[static_cast<std::size_t>(k)] && ea[static_cast<std::size_t>(k)] != eb[static_cast<std::size_t>(k)]) {
                adj[static_cast<std::size_t>(ea[static_cast<std::size_t>(k)])].push_back(k);
                adj[static_cast<std::size_t>(eb[static_cast<std::size_t>(k)])].push_back(k);
            }
        // breadth-first forest from the datum, then from each vertex not reached (states in order: deterministic)
        std::vector<int> par(static_cast<std::size_t>(nv), -2), parE(static_cast<std::size_t>(nv), -1), depth(static_cast<std::size_t>(nv), 0);
        std::vector<std::uint8_t> tree(static_cast<std::size_t>(ns), 0);
        for (int root = 0; root < nv; ++root) {
            if (par[static_cast<std::size_t>(root)] != -2) continue;
            par[static_cast<std::size_t>(root)] = -1;
            std::vector<int> q{root};
            for (std::size_t h = 0; h < q.size(); ++h) {
                const int x = q[h];
                for (int k : adj[static_cast<std::size_t>(x)]) {
                    const int y = ea[static_cast<std::size_t>(k)] == x ? eb[static_cast<std::size_t>(k)] : ea[static_cast<std::size_t>(k)];
                    if (par[static_cast<std::size_t>(y)] != -2) continue;
                    par[static_cast<std::size_t>(y)] = x;
                    parE[static_cast<std::size_t>(y)] = k;
                    depth[static_cast<std::size_t>(y)] = depth[static_cast<std::size_t>(x)] + 1;
                    tree[static_cast<std::size_t>(k)] = 1;
                    q.push_back(y);
                }
            }
        }
        for (int k = 0; k < ns; ++k) {
            g.nodOff[static_cast<std::size_t>(k) + 1] = g.nodOff[static_cast<std::size_t>(k)];
            if (!elig[static_cast<std::size_t>(k)] || tree[static_cast<std::size_t>(k)]) continue;
            g.nodCo[static_cast<std::size_t>(k)] = 1;
            // e_a − e_b = Σ over the path's steps x → y of (e_x − e_y); a tree edge t stepped from its a end to its b end is
            // +A_t, the other way −A_t (A_t = e_{a_t} − e_{b_t}, the datum's e = 0)
            int x = ea[static_cast<std::size_t>(k)], y = eb[static_cast<std::size_t>(k)];
            auto step = [&](int from, int t, double sense) { // the edge t from vertex `from` to its parent, sense +1 (x side) / −1 (y side)
                const double s = (ea[static_cast<std::size_t>(t)] == from ? 1.0 : -1.0) * sense;
                g.nodT.push_back(t);
                g.nodS.push_back(static_cast<signed char>(s > 0 ? 1 : -1));
            };
            while (x != y) {
                if (depth[static_cast<std::size_t>(x)] >= depth[static_cast<std::size_t>(y)]) {
                    step(x, parE[static_cast<std::size_t>(x)], 1.0);
                    x = par[static_cast<std::size_t>(x)];
                } else {
                    step(y, parE[static_cast<std::size_t>(y)], -1.0);
                    y = par[static_cast<std::size_t>(y)];
                }
            }
            g.nodOff[static_cast<std::size_t>(k) + 1] = static_cast<int>(g.nodT.size());
        }
    }
}

void Circuit::nodalInfo() const {
    if (!kEnvNodalInfo) return;
    for (std::size_t gi = 0; gi < groups_.size(); ++gi) {
        const auto& g = groups_[gi];
        const auto& r = *rt_[gi];
        int nco = 0, nzCo = 0, gam = 0, caps = 0, nzAll = 0;
        for (std::size_t k = 0; k < g.states.size(); ++k) {
            const auto& p = prims_[static_cast<std::size_t>(g.states[k])];
            int nz = 0;
            for (int nd : {p.a, p.b})
                if (nd > 0 && r.unkOf[static_cast<std::size_t>(nd)] >= 0 && r.kcl[static_cast<std::size_t>(r.unkOf[static_cast<std::size_t>(nd)])]) ++nz;
            nzAll += nz;
            caps += p.kind == Prim::Cap;
            if (!g.nodCo.empty() && g.nodCo[k]) ++nco, nzCo += nz, gam += g.nodOff[k + 1] - g.nodOff[k];
        }
        const bool sparse = r.sl && r.sl->valid;
        const int rows = !r.pdRow.empty() ? static_cast<int>(r.hcOut.size()) : r.ny + r.ns;
        std::fprintf(stderr, "nodal group %zu: %s, n %d, ns %d (caps %d), nn %d, W %d: %d loops (%.0f %%); R state nonzeros %d, in loops %d; fold %d adds; "
                             "dense saving %d, sparse saving %d multiply-adds per linear update\n",
                     gi, r.wdf ? "wdf" : sparse ? "sparse" : "dense", r.n, r.ns, caps, r.nn, r.W, nco, r.ns ? 100.0 * nco / r.ns : 0.0, nzAll, nzCo, gam,
                     nco * (r.nn + rows), nzCo);
    }
}

} // namespace rig::ce
