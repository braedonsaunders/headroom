#pragma once
// The dense port-space Newton kernel and its helpers (limiters, small dense LU, Woodbury factor / step, the sparse nodal
// step's interface), shared by Engine.cpp (the interpreter) and the ahead-of-time specialised Newton kernels that
// tools/circuit/circuit_codegen generates for the shipped netlists (docs/CIRCUIT_ENGINE.md §10.45). Internal to the
// circuit engine: nothing outside src/circuit and the generated kernels includes it.
#include "Codegen.hpp"
#include "Devices.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <utility>
#include <vector>

namespace rig::ce::nk {

// Developer A/B switches the kernel reads (read once at load; see Engine.cpp for the others).
inline const bool kEnvOldLim = std::getenv("CE_OLDLIM") != nullptr;
inline const bool kEnvLimWalk = std::getenv("CE_LIMWALK") != nullptr;
// Chord confirmation is tried only after a full step within this many tolerances (2026-11-10: was 300). Measured
// against a CE_TOL=1e-10 reference, the accepted chord steps are the engine's largest error term: Twin, DI at gain 2,
// -140 dB at 300, -193 dB at 30, -214 dB without the chord. Quality first (decision 2026-11-10): 30, for
// about 5-7 % more CPU on a steady JCM800 than 150.
inline const double kEnvChordGate = std::getenv("CE_CHORDGATE") ? std::atof(std::getenv("CE_CHORDGATE")) : 30.0;
// Bit-identical large-group shortcuts (same arithmetic, same order): linOut starts its sparse right-hand side from linPorts'
// state and input columns, and the Newton residual check sums |K|·|Jd| only where the bound at 1 does not already decide.
// CE_NOBIGEXACT=1: the full sums (A/B).
inline const bool kEnvNoBigExact = std::getenv("CE_NOBIGEXACT") != nullptr;
inline const double kSpWeight = std::getenv("CE_SPWEIGHT") ? std::atof(std::getenv("CE_SPWEIGHT")) : 1.0; // developer: sparse Newton per-solve cost weight
// Ahead-of-time specialised kernels (Codegen.hpp, docs §10.44): bit-identical to the interpreter. CE_NOCODEGEN=1: never bound.
inline const bool kEnvNoCodegen = std::getenv("CE_NOCODEGEN") != nullptr;
inline const double kEnvXChordGate = std::getenv("CE_XCHORDGATE") ? std::atof(std::getenv("CE_XCHORDGATE")) : 1000.0;
inline const double kEnvXChordDj = std::getenv("CE_XCHORDDJ") ? std::atof(std::getenv("CE_XCHORDDJ")) : 0.1; // Jacobian-change bound

// kLimPnjF: kLimPnj on a single diode junction whose island floats even through capacitors (NlDevice::floatingPorts):
// also a current-space step down from forward bias (limitPort).
enum LimKind : std::uint8_t { kLimNone = 0, kLimPnj = 1, kLimRail = 2, kLimSat = 3, kLimStep = 4, kLimZener = 5, kLimPnjF = 6, kLimZenerF = 7, kLimPair = 8 };
// Newton acceptance (newtonKernel): the residual a step leaves, r = F + (K·Jd − I)·d with the iterate's device Jacobian,
// measured as the step it still asks for, |r_k| / (1 + Σ_j |K_kj|·max|Jd_j|) (the row's Jacobian scale; a raw volt
// residual is rounding noise on near-floating nodes, K ~ 5e8 Ω), must stay within this many Newton tolerances
// newtonTol·(1 + |v|). Converged solves across the library read at most 7e-4 (SSL dynamics, JC-120, SVT, Mesa, JH-1,
// 5150, JCM800, Twin, 1176, SLO-100 driven); steps from a stale chord factorisation or a cancelled sparse nodal solve
// read 1e4-3e6 (JC-120 rail-node chorus op-amp, residual 2e16 V).
constexpr double kNewtonResidualGate = 1.0;
// Floating zener junctions (kLimZenerF, newtonKernel's portRatio) also converge on their current: |g·Δv| ≤ this +
// tol·|i| (A). CE_NOFLTCONV=1 restores the plain voltage test and kLimZener (developer A/B).
constexpr double kFltAbsI = 1e-12;
inline const bool kEnvNoFltConv = std::getenv("CE_NOFLTCONV") != nullptr;
inline const bool kEnvNoTubeJct = std::getenv("CE_NOTUBEJCT") != nullptr; // developer A/B: junctions in tube groups on the voltage test only (§10.42)

// ───────────────────────────── per-group runtime storage ─────────────────────────────
// Live-row set of a Woodbury factorisation (bit k = port k live), up to 128 ports.
struct LiveMask {
    std::uint64_t w[2] = {0, 0};
    void set(int k) noexcept { w[k >> 6] |= std::uint64_t{1} << (k & 63); }
    bool operator==(const LiveMask& o) const noexcept { return w[0] == o.w[0] && w[1] == o.w[1]; }
};
static_assert(kMaxNlPorts <= 128, "LiveMask holds 128 ports");
struct KaCache { // K's live columns, gathered once per live-row set (K changes only at control rate)
    LiveMask mask;
    bool valid = false;
    std::vector<double> KA; // nn × live, contiguous; sized nn² at build (up to kMaxNlPorts²)
};
// The Newton kernel's last accepted factorisation of I − K·Jd (LU, pivots, live rows, the device Jacobians it was built
// from), kept between samples for the predictor's input correction (see processGroup).
struct KernelState {
    bool valid = false;
    bool xchord = false; // try the kept factorisation as the first chord step (kEnvXChord)
    double chordGate = 0; // the in-solve chord's gate for this group (the circuit's "newton.chordGate", docs §10.50); 0 = kEnvChordGate
    int na = 0;
    bool sparse = false; // the kept factorisation is the group's SparseLu (values still in sp->v)
    LiveMask mask;
    std::vector<double> LU, rinv, Jd; // Jd: the kernel's two device-Jacobian buffers, 2 × nn × 3
    std::vector<int> piv, live;
};
// Sparse nodal Newton step for large groups (above 24 ports). The port-space Jacobian I − K·Jd is dense (K couples
// every port through the linear network), but with K = C·M⁻¹·B (C: port-voltage rows, B: port-current columns of the
// group's MNA matrix M) the step (I − K·Jd)·d = F is d = F + C·x with (M − B·Jd·C)·x = B·Jd·F: one solve with the
// sparse nodal Jacobian. Its pivot order (threshold Markowitz) and fill pattern are fixed at reset() on the control
// thread; per factorisation only the values are refreshed (no allocation, no pivot search). A pivot that comes out
// small against its row falls back to the dense path for that factorisation.
// The values live packed (spPack): the fill pattern is exactly the diagonal plus L's and U's entries, stored as
// [L by column | U by row | diagonal], and the elimination is a precompiled list of target positions (opDst), so a
// factorisation runs the same operations in the same order as on an n×n array, over a few KB instead of n² doubles.
// Static / live split (spPack, spSnapshot): an entry is live once a device stamp reaches it (a stamp position, or the
// target of an update with a live operand). Every operation on a static entry, and every static pivot, scaling and
// pivot test, depends on M alone, so it runs once per change of M (spSnapshot, into snap); a factorisation starts
// from snap and runs only the live operations, in their original order. Each entry therefore sees exactly the same
// operations in the same order as the full elimination (bit-identical); about a quarter of the work goes.
struct SparseLu {
    bool ok = false;
    int n = 0;                          // matrix size (group unknowns + branches)
    double cost = 0;                    // multiply-adds per factorisation + two steps (vs the dense estimate)
    std::vector<double> v;              // packed values: L (lOff order), U (uOff order), then the diagonal
    std::vector<int> opDst;             // per pivot k, L entry e (row i), U entry u (column j): packed position of (i, j)
    bool stale = true, snapOk = false;  // snap out of date (M changed: set by every writer of Mraw); static pivots pass
    std::vector<double> snap, cmaxS;    // static values (live entries as they stood when they went live); per pivot:
                                        // largest |static L entry| before scaling (its test's static part)
    std::vector<std::uint8_t> opLive, lLive, kFl; // per update / L entry: live; per pivot: 1 pivot live, 2 test live
    std::vector<int> dOff, dIdx;        // per pivot: L entries scaled live
    std::vector<int> rOff, rE, oOff, oQ, oU; // per pivot: rows (L entry) with live updates; per row: target, U entry
    std::vector<int> bRows;             // back substitution: the rows whose x the step reads (xP / xN closure), descending
    std::vector<int> pat;               // permuted positions of the fill pattern (diagonal + L + U; its size feeds cost)
    std::vector<int> mPos, mSrc;        // M's structural entries: packed position <- index into Mraw
    std::vector<std::uint8_t> mStruct;  // n×n original coordinates: entry belongs to M's pattern (refresh check)
    std::vector<int> lOff, lIdx;        // pivot k: rows i > k below it (column k of L)
    std::vector<int> uOff, uIdx;        // pivot k: columns j > k right of it (row k of U)
    std::vector<int> stOff, stPos;      // per (port, block member) pair a*3+m: packed positions of −B·Jd·C entries
    std::vector<double> stSign;
    std::vector<int> pFrom, pTo, xP, xN; // per port: permuted rows of its current injection, permuted cols of its sense
    std::vector<double> inv, rhs, x;
    const double* Mraw = nullptr;       // the group's GroupRt::Mraw
    const cg::LuKernel* cg = nullptr;   // generated kernel for this exact structure (Codegen.hpp; null: interpreter)
};

// The sparse nodal factorisation and step (Engine.cpp; generated kernels call the same functions).
void spSnapshot(SparseLu& s) noexcept;
bool spFactor(SparseLu& s, const double (*Jd)[3], const int* live, int na, int nports, const int* bn) noexcept;
void spStep(SparseLu& s, const double (*Jd)[3], const int* live, int na, int nports, const int* bs, const int* bn, const double* F, double* d) noexcept;
void spSolvePacked(const SparseLu& s, const double* __restrict lVal, const double* __restrict uVal, double* __restrict b, double* __restrict x,
                   const std::vector<int>& rows) noexcept;

// SPICE-style junction limiting (pnjlim) and rail-clamp limiting of one port's Newton update.
[[gnu::always_inline]] inline double limitPort(std::uint8_t kind, double A, double B, double C, double vold, double vnew) noexcept {
    switch (kind) {
        case kLimPnj: {
            const double vt = A, vcrit = B;
            auto pnj = [&](double vn, double vo) {
                // Limit forward-bias increases (exp explodes upward); downward steps shrink exp smoothly and
                // must never hit SPICE3f5's non-monotone arg <= 0 -> vcrit cliff.
                if (vn > vcrit && vn - vo > 2.0 * vt) {
                    if (vo > 0) {
                        const double arg = 1.0 + (vn - vo) / vt;
                        vn = vo + vt * std::log(arg);
                    } else {
                        vn = vt * std::log(std::max(vn / vt, 1e-30));
                    }
                }
                return vn;
            };
            if (C > 0) return vnew >= 0 ? pnj(vnew, vold) : -pnj(-vnew, -vold); // symmetric pair
            const double sj = C < 0 ? -1.0 : 1.0;                                 // pnp junction sign
            return sj * pnj(sj * vnew, sj * vold);
        }
        case kLimPnjF: {
            // A current-driven junction (its island floats: NlDevice::floatingPorts). Upward: pnjlim, as kLimPnj.
            // Downward from forward bias, voltage-space Newton approaches a small target current one nVt per
            // iteration (the bridge of a transformer supply turning off took 15-40 iterations), so the step is taken
            // in current instead: the linearised current i + g·d mapped back through the law, v = vold + nVt·ln(1 +
            // d/nVt) (pnjlim's own formula, the exact inverse for i = Is(e^(v/nVt) − 1)). When that current is below
            // −Is, the step goes at least 8·nVt down (the current ÷ 3000; a jump straight to 0 V made an LED in a
            // transistor's collector cycle). Only steps of more than nVt/2 are mapped, so converging steps return
            // bit-exact. C > 0: the mapping is on (newtonKernel passes 0 once the port's limited steps reverse).
            const double vt = A, vcrit = B;
            if (vnew >= vold) {
                if (vnew > vcrit && vnew - vold > 2.0 * vt) return vold > 0 ? vold + vt * std::log(1.0 + (vnew - vold) / vt) : vt * std::log(std::max(vnew / vt, 1e-30));
                return vnew;
            }
            if (C > 0 && vold > 0 && vold - vnew > 0.5 * vt) { // C: 1 = on, 0 = switched off (newtonKernel)
                const double arg = 1.0 + (vnew - vold) / vt;
                return arg > 0 ? vold + vt * std::log(arg) : std::min(vnew, vold - 8.0 * vt);
            }
            return vnew;
        }
        case kLimZenerF: {
            // kLimZener on a junction whose island floats (a back-to-back zener clamp's midpoint): in breakdown as
            // kLimZener; forward and below the knee as kLimPnjF (C = ±knee: the sign carries kLimPnjF's on flag)
            const double vt = A, vcrit = B, knee = std::abs(C);
            auto pnj = [&](double vn, double vo) {
                if (vn > vcrit && vn - vo > 2.0 * vt) {
                    if (vo > 0) vn = vo + vt * std::log(1.0 + (vn - vo) / vt);
                    else vn = vt * std::log(std::max(vn / vt, 1e-30));
                }
                return vn;
            };
            if (vnew < std::min(0.0, -knee + 10.0 * vt)) {
                const double x = -(vnew + knee), xl = pnj(x, -(vold + knee));
                return xl == x ? vnew : -(xl + knee);
            }
            if (vnew >= vold) return pnj(vnew, vold);
            if (C > 0 && vold > 0 && vold - vnew > 0.5 * vt) {
                const double arg = 1.0 + (vnew - vold) / vt;
                return arg > 0 ? vold + vt * std::log(arg) : std::min(vnew, vold - 8.0 * vt);
            }
            return vnew;
        }
        case kLimZener: {
            // ngspice DIOlimit: pnjlim on the forward junction (A = nVt, B = vcrit); in breakdown (below −C + 10·A) the
            // same limiting on the reversed coordinate −(v + C), C = the adjusted breakdown voltage
            const double vt = A, vcrit = B;
            auto pnj = [&](double vn, double vo) {
                if (vn > vcrit && vn - vo > 2.0 * vt) {
                    if (vo > 0) vn = vo + vt * std::log(1.0 + (vn - vo) / vt);
                    else vn = vt * std::log(std::max(vn / vt, 1e-30));
                }
                return vn;
            };
            if (vnew < std::min(0.0, -C + 10.0 * vt)) {
                const double x = -(vnew + C), xl = pnj(x, -(vold + C));
                return xl == x ? vnew : -(xl + C); // untouched steps return bit-exact (a limiter "firing" blocks convergence)
            }
            return pnj(vnew, vold);
        }
        case kLimPair: {
            // A back-to-back junction pair (NlKind::ZenerPair) conducts only past +B (the first junction's breakdown) or
            // −C: pnjlim on the distance past that knee, A = the pair's exponential scale (nbv·Vt + n·Vt); untouched
            // steps return bit-exact.
            const double vt = A;
            auto pnj = [&](double x, double xo) {
                const double base = std::max(xo, 0.0);
                return x > 0 && x - base > 2.0 * vt ? base + vt * std::log(1.0 + (x - base) / vt) : x;
            };
            if (vnew > B) {
                const double x = vnew - B, xl = pnj(x, vold - B);
                return xl == x ? vnew : xl + B;
            }
            if (vnew < -C) {
                const double x = -(vnew + C), xl = pnj(x, -(vold + C));
                return xl == x ? vnew : -(xl + C);
            }
            return vnew;
        }
        case kLimRail: {
            const double vhi = A, vlo = B, vc = C;
            if (vnew > vhi && vnew - std::max(vold, vhi) > 2.0 * vc) vnew = std::max(vold, vhi) + vc * std::log1p((vnew - std::max(vold, vhi)) / vc);
            if (vnew < vlo && std::min(vold, vlo) - vnew > 2.0 * vc) vnew = std::min(vold, vlo) - vc * std::log1p((std::min(vold, vlo) - vnew) / vc);
            return vnew;
        }
        case kLimSat: {
            // Saturating port (tanh transconductance): an iterate may not jump from one saturated side to the
            // other beyond the linear region (A = 3/gm-scale), where the derivative carries no information.
            const double lin = A;
            if (vold * vnew < 0.0 && std::abs(vnew) > lin) return std::copysign(lin, vnew);
            if (std::abs(vnew) > lin && std::abs(vold) > lin && std::abs(vnew - vold) > 4.0 * lin && std::abs(vnew) > std::abs(vold))
                return vold + std::copysign(4.0 * lin, vnew - vold); // walking further into saturation: bounded steps
            return vnew;
        }
        case kLimStep: {
            // Damped Newton for tube ports, only where the law bends: an iterate may move at most A volts when it
            // ends above B (a grid entering conduction, B = -1 V; a plate or screen entering its knee is the mirror
            // case, flagged by C < 0: limited when it ends below |B|). Elsewhere the law is smooth and steps are free.
            // An iterate arriving from outside the bend region (a plate falling from flyback, a grid rising from
            // cut-off) lands at most A inside it in one step instead of walking there A volts at a time.
            const double dv = vnew - vold;
            if (std::abs(dv) <= A) return vnew;
            const bool oldLim = kEnvOldLim; // developer A/B
            const bool bend = oldLim || (C >= 0 ? vnew > B : vnew < -B);
            if (!bend) return vnew;
            const bool walk = kEnvLimWalk; // developer A/B: the old walk
            if (!walk && !oldLim && (C >= 0 ? vold <= B : vold >= -B)) return C >= 0 ? std::min(vnew, B + A) : std::max(vnew, -B - A);
            return vold + std::copysign(A, dv);
        }
        default: return vnew;
    }
}

// Dense solve with partial pivoting for compile-time sizes (fully unrolled by the compiler for NN <= 6).
template <int NN>
inline bool solveFixed(double (&A)[NN * NN], double (&b)[NN]) noexcept {
    // Closed forms for the common tiny systems (one division, no pivot branches); pivoting otherwise.
    if constexpr (NN == 1) {
        if (!(std::abs(A[0]) > 1e-300)) return false;
        b[0] /= A[0];
        return true;
    } else if constexpr (NN == 2) {
        const double det = A[0] * A[3] - A[1] * A[2];
        const double scale = std::abs(A[0] * A[3]) + std::abs(A[1] * A[2]);
        if (std::abs(det) > 1e-12 * scale && scale > 0) {
            const double id = 1.0 / det;
            const double x0 = (A[3] * b[0] - A[1] * b[1]) * id;
            const double x1 = (A[0] * b[1] - A[2] * b[0]) * id;
            b[0] = x0;
            b[1] = x1;
            return true;
        }
    }
    for (int k = 0; k < NN; ++k) {
        int p = k;
        double best = std::abs(A[k * NN + k]);
        for (int r = k + 1; r < NN; ++r)
            if (std::abs(A[r * NN + k]) > best) {
                best = std::abs(A[r * NN + k]);
                p = r;
            }
        if (!(best > 1e-300)) return false;
        if (p != k) {
            for (int c = k; c < NN; ++c) std::swap(A[k * NN + c], A[p * NN + c]);
            std::swap(b[k], b[p]);
        }
        const double inv = 1.0 / A[k * NN + k];
        for (int r = k + 1; r < NN; ++r) {
            const double f = A[r * NN + k] * inv;
            for (int c = k + 1; c < NN; ++c) A[r * NN + c] -= f * A[k * NN + c];
            b[r] -= f * b[k];
        }
    }
    for (int r = NN - 1; r >= 0; --r) {
        double s = b[r];
        for (int c = r + 1; c < NN; ++c) s -= A[r * NN + c] * b[c];
        b[r] = s / A[r * NN + r];
    }
    return true;
}

// Dense y = A·x for the per-sample linear updates (A row-major rows×n). Eight (then four) rows at a time share each x
// load; each row accumulates in a four-lane vector, lane l summing the columns c ≡ l (mod 4) in order, then the lanes
// fold as (l0 + l2) + (l1 + l3): the plain dot loop does not vectorise (FP reassociation). gemvB adds a bias after each
// row's sum (y = A·x + b: the held noise offsets, without a second pass over y).
// (2026-11-12) The four-lane accumulator replaces two two-lane ones (columns c, c+1 and c+2, c+3):
// each lane runs the same multiply-adds in the same order, so the result is bit-identical (checked on 20 000 random
// shapes on arm64 and x86-64-v3), while x86-64-v3 gets 256-bit FMAs and eight independent chains per block: a 56×50
// product 222 → 93 ns on the Ryzen 9 9900X, 187 → 144 ns on an M4 Pro.
typedef double v2d __attribute__((ext_vector_type(2)));
typedef double v4d __attribute__((ext_vector_type(4)));
inline v2d load2(const double* p) noexcept {
    v2d v;
    __builtin_memcpy(&v, p, sizeof v);
    return v;
}
inline v4d load4(const double* p) noexcept {
    v4d v;
    __builtin_memcpy(&v, p, sizeof v);
    return v;
}
template <int R, bool Bias>
inline void gemvRows(const double* __restrict A, int r, int n, const double* __restrict x, double* __restrict y,
                     const double* __restrict b) noexcept {
    const int n2 = n & ~1, n4 = n & ~3;
    const double* a[R];
    for (int i = 0; i < R; ++i) a[i] = A + (r + i) * n;
    v4d q[R];
    for (int i = 0; i < R; ++i) q[i] = 0;
    int c = 0;
    for (; c < n4; c += 4) {
        const v4d xv = load4(x + c);
        for (int i = 0; i < R; ++i) q[i] += load4(a[i] + c) * xv;
    }
    v2d s[R];
    for (int i = 0; i < R; ++i) s[i] = q[i].lo;
    for (; c < n2; c += 2) {
        const v2d xv = load2(x + c);
        for (int i = 0; i < R; ++i) s[i] += load2(a[i] + c) * xv;
    }
    for (int i = 0; i < R; ++i) {
        const v2d t2 = s[i] + q[i].hi;
        double t = t2.x + t2.y;
        if (n2 < n) t += a[i][n2] * x[n2];
        if constexpr (Bias) t += b[r + i];
        y[r + i] = t;
    }
}
// The last one to three rows: each its own two-lane accumulator (lane l sums the columns c ≡ l (mod 2)), run side by
// side so the chains overlap.
template <int R, bool Bias>
inline void gemvTail(const double* __restrict A, int r, int n, const double* __restrict x, double* __restrict y,
                     const double* __restrict b) noexcept {
    const int n2 = n & ~1;
    const double* a[R];
    v2d s[R];
    for (int i = 0; i < R; ++i) {
        a[i] = A + (r + i) * n;
        s[i] = 0;
    }
    for (int c = 0; c < n2; c += 2) {
        const v2d xv = load2(x + c);
        for (int i = 0; i < R; ++i) s[i] += load2(a[i] + c) * xv;
    }
    for (int i = 0; i < R; ++i) {
        double t = s[i].x + s[i].y;
        if (n2 < n) t += a[i][n2] * x[n2];
        if constexpr (Bias) t += b[r + i];
        y[r + i] = t;
    }
}
template <bool Bias>
inline void gemvT(const double* __restrict A, int rows, int n, const double* __restrict x, double* __restrict y,
                  const double* __restrict b) noexcept {
    int r = 0;
    for (; r + 8 <= rows; r += 8) gemvRows<8, Bias>(A, r, n, x, y, b);
    for (; r + 4 <= rows; r += 4) gemvRows<4, Bias>(A, r, n, x, y, b);
    switch (rows - r) {
        case 3: gemvTail<3, Bias>(A, r, n, x, y, b); break;
        case 2: gemvTail<2, Bias>(A, r, n, x, y, b); break;
        case 1: gemvTail<1, Bias>(A, r, n, x, y, b); break;
        default: break;
    }
}
inline void gemv(const double* __restrict A, int rows, int n, const double* __restrict x, double* __restrict y) noexcept {
    gemvT<false>(A, rows, n, x, y, nullptr);
}
inline void gemvB(const double* __restrict A, int rows, int n, const double* __restrict x, double* __restrict y,
                  const double* __restrict b) noexcept {
    gemvT<true>(A, rows, n, x, y, b);
}

// LU with partial pivoting at a run-time size n (row stride n) that keeps its factors (pivots, reciprocal pivots) for
// further right-hand sides, and the matching solve.
inline bool luSmall(double* A, int n, int* piv, double* rinv) noexcept {
    for (int k = 0; k < n; ++k) {
        int p = k;
        double best = std::abs(A[k * n + k]);
        for (int r = k + 1; r < n; ++r)
            if (std::abs(A[r * n + k]) > best) {
                best = std::abs(A[r * n + k]);
                p = r;
            }
        if (!(best > 1e-300)) return false;
        piv[k] = p;
        if (p != k)
            for (int c = 0; c < n; ++c) std::swap(A[k * n + c], A[p * n + c]);
        const double inv = 1.0 / A[k * n + k];
        rinv[k] = inv;
        for (int r = k + 1; r < n; ++r) {
            const double f = (A[r * n + k] *= inv);
            if (f == 0.0) continue;
            for (int c = k + 1; c < n; ++c) A[r * n + c] -= f * A[k * n + c];
        }
    }
    return true;
}
inline void luSolveSmall(const double* A, int n, const int* piv, const double* rinv, double* b) noexcept {
    for (int k = 0; k < n; ++k) // whole rows were swapped during the factorisation: permute b first
        if (piv[k] != k) std::swap(b[k], b[piv[k]]);
    for (int r = 1; r < n; ++r) {
        double s = b[r];
        for (int c = 0; c < r; ++c) s -= A[r * n + c] * b[c];
        b[r] = s;
    }
    // Back substitution sums each row from the far end, so the term with b[r+1] (computed just before) comes last and
    // the rest of the row overlaps the previous row; summed from r+1 up, every row's whole FMA chain waited on b[r+1].
    // 1.5-2x faster at 11-24 live ports; the sum order (rounding only) differs from the near-end-first form.
    for (int r = n - 1; r >= 0; --r) {
        double s = b[r];
        for (int c = n - 1; c > r; --c) s -= A[r * n + c] * b[c];
        b[r] = s * rinv[r];
    }
}

// The same at compile-time sizes, dispatched by size (one out-of-line copy per size). From 12 up both run as
// straight-line code (docs §10.30, 2026-11-12): luSmall's operations in luSmall's order, step K then
// K + 1 (pivot search, row swap, scaling, updates), so the factors are bit-identical, without the loop control, the
// strided pivot-search loop and the inner loops' run-time trip counts. Real JC-120 power-amp Jacobians (18 ports, 10.4
// row swaps per factorisation): 505 -> 388 ns on the Ryzen 9 9900X, 583 -> 430 ns on the M4 Pro; 16 ports 342 -> 243 ns.
// The solve keeps luSolveSmall's sum orders; unrolled, every product fuses into its multiply-subtract (bit-identical on
// x86-64-v3; on arm64 the loop form leaves some products unfused, so the two differ in the last bit: −213 dB rms on a
// JC-120 DI render). CE_LULOOP=1: the loop forms (developer A/B).
const bool kEnvLuLoop = std::getenv("CE_LULOOP") != nullptr;
#if defined(__clang__)
#define CE_UNROLL_LU _Pragma("clang loop unroll(full)")
#elif defined(__GNUC__)
#define CE_UNROLL_LU _Pragma("GCC unroll 32")
#else
#define CE_UNROLL_LU
#endif
template <int N, int K>
[[gnu::always_inline]] inline bool luStepFrom(double* __restrict A, int* __restrict piv, double* __restrict rinv) noexcept {
    if constexpr (K == N) {
        return true;
    } else {
        int p = K;
        double best = std::abs(A[K * N + K]);
        CE_UNROLL_LU
        for (int r = K + 1; r < N; ++r)
            if (std::abs(A[r * N + K]) > best) {
                best = std::abs(A[r * N + K]);
                p = r;
            }
        if (!(best > 1e-300)) return false;
        piv[K] = p;
        if (p != K) {
            double* __restrict x = A + K * N;
            double* __restrict y = A + p * N;
            CE_UNROLL_LU
            for (int c = 0; c < N; ++c) std::swap(x[c], y[c]);
        }
        const double inv = 1.0 / A[K * N + K];
        rinv[K] = inv;
        const double* uk = A + K * N;
        CE_UNROLL_LU
        for (int r = K + 1; r < N; ++r) {
            double* lr = A + r * N;
            const double f = (lr[K] *= inv);
            if (f == 0.0) continue;
            CE_UNROLL_LU
            for (int c = K + 1; c < N; ++c) lr[c] -= f * uk[c];
        }
        return luStepFrom<N, K + 1>(A, piv, rinv);
    }
}
template <int N>
[[gnu::noinline]] bool luFixedN(double* __restrict A, int* __restrict piv, double* __restrict rinv) noexcept {
    if constexpr (N >= 12)
        if (!kEnvLuLoop) return luStepFrom<N, 0>(A, piv, rinv);
    return luSmall(A, N, piv, rinv);
}
template <int N>
[[gnu::noinline]] void luSolveFixedN(const double* __restrict A, const int* __restrict piv, const double* __restrict rinv, double* __restrict b) noexcept {
    if constexpr (N >= 12) {
        if (!kEnvLuLoop) {
            CE_UNROLL_LU
            for (int k = 0; k < N; ++k) { // the row swaps, unconditionally (a swap with itself is a no-op)
                const int q = piv[k];
                const double t = b[q];
                b[q] = b[k];
                b[k] = t;
            }
            CE_UNROLL_LU
            for (int r = 1; r < N; ++r) {
                double s = b[r];
                CE_UNROLL_LU
                for (int c = 0; c < r; ++c) s -= A[r * N + c] * b[c];
                b[r] = s;
            }
            CE_UNROLL_LU
            for (int r = N - 1; r >= 0; --r) {
                double s = b[r];
                CE_UNROLL_LU
                for (int c = N - 1; c > r; --c) s -= A[r * N + c] * b[c];
                b[r] = s * rinv[r];
            }
            return;
        }
    }
    luSolveSmall(A, N, piv, rinv, b);
}
#undef CE_UNROLL_LU
template <int... Ns>
inline bool luDispatch(std::integer_sequence<int, Ns...>, int n, double* A, int* piv, double* rinv) noexcept {
    bool ok = false, hit = false;
    ((n == Ns + 1 ? (ok = luFixedN<Ns + 1>(A, piv, rinv), hit = true) : false), ...);
    return hit ? ok : luSmall(A, n, piv, rinv);
}
template <int... Ns>
inline void luSolveDispatch(std::integer_sequence<int, Ns...>, int n, const double* A, const int* piv, const double* rinv, double* b) noexcept {
    bool hit = false;
    ((n == Ns + 1 ? (luSolveFixedN<Ns + 1>(A, piv, rinv, b), hit = true) : false), ...);
    if (!hit) luSolveSmall(A, n, piv, rinv, b);
}
inline bool luAny(int n, double* A, int* piv, double* rinv) noexcept { return luDispatch(std::make_integer_sequence<int, 24>{}, n, A, piv, rinv); }
inline void luSolveAny(int n, const double* A, const int* piv, const double* rinv, double* b) noexcept {
    luSolveDispatch(std::make_integer_sequence<int, 24>{}, n, A, piv, rinv, b);
}

// wbFactor's pivot-free elimination at a compile-time size NA, as straight-line code (step K, then K + 1): the same
// operations in the same order as the loop in wbFactor, so the factors are bit-identical. Clang keeps the loop form
// at these sizes (a scalar remainder loop of ~9 instructions per multiply-add behind a run-time alias check); unrolled,
// the 5150 / DR103 use 9-12 % fewer instructions and their large groups' Newton solves run 5-11 % faster
// (work/circuit-netlists/COST_BREAKDOWN.md, "Compiler and hand-kernel audit"). Below about 12 live rows the loop
// form is as fast, so wbFactor uses this from NA = 12 up. False: a pivot test failed (wbFactor then pivots).
#if defined(__clang__)
#define CE_UNROLL_FULL _Pragma("clang loop unroll(full)")
#elif defined(__GNUC__)
#define CE_UNROLL_FULL _Pragma("GCC unroll 32")
#else
#define CE_UNROLL_FULL
#endif
template <int NA, int K>
[[gnu::always_inline]] inline bool wbEliminateFrom(double* __restrict LU, int* piv, double* rinv) noexcept {
    if constexpr (K == NA) {
        return true;
    } else {
        const double* uk = LU + K * NA;
        double rmax = 0;
        CE_UNROLL_FULL
        for (int c = K; c < NA; ++c) rmax = std::max(rmax, std::abs(uk[c]));
        const double pv = uk[K];
        if (!(std::abs(pv) > 0.1 * rmax) || !(std::abs(pv) > 1e-300)) return false;
        piv[K] = K;
        const double inv = 1.0 / pv;
        rinv[K] = inv;
        CE_UNROLL_FULL
        for (int r = K + 1; r < NA; ++r) {
            double* lr = LU + r * NA;
            const double f = (lr[K] *= inv);
            CE_UNROLL_FULL
            for (int c = K + 1; c < NA; ++c) lr[c] -= f * uk[c];
        }
        return wbEliminateFrom<NA, K + 1>(LU, piv, rinv);
    }
}
#undef CE_UNROLL_FULL

// Woodbury factor / step at compile-time sizes (NN ports, NA live rows): one specialised copy per (NN, NA), chosen
// through a table, so the per-iteration algebra of every group runs fully unrolled at its own size. NN = NA = 0 is the
// run-time-size copy (groups above 24 ports): the same arithmetic in the same order, sizes from nDyn / naDyn.
template <int NN, int NA>
[[gnu::noinline]] bool wbFactor(const double (*Jd)[3], const int* live, const int* bs, const int* bn, const double* __restrict KA, double* __restrict LU, int* piv,
                                double* rinv, int naDyn = NA) noexcept {
    constexpr int CA = NA > 0 ? NA : kMaxNlPorts;
    const int na = NA > 0 ? NA : naDyn;
    auto build = [&] {
        for (int a = 0; a < na; ++a) {
            const int ka = live[a], b0 = bs[ka], np = bn[ka];
            double row[CA];
            for (int b = 0; b < na; ++b) row[b] = a == b ? 1.0 : 0.0;
            for (int m = 0; m < np; ++m) {
                const double c = Jd[ka][m];
                const double* kr = KA + (b0 + m) * na;
                for (int b = 0; b < na; ++b) row[b] -= c * kr[b];
            }
            for (int b = 0; b < na; ++b) LU[a * na + b] = row[b];
        }
    };
    build();
    // I − Jd·K is usually diagonally dominant (diagonal ≥ 1): eliminate in place without the pivot search (a serial
    // compare chain per column); if any pivot is small against its row, rebuild the matrix and use partial pivoting.
    if constexpr (NA >= 12) {
        if (wbEliminateFrom<NA, 0>(LU, piv, rinv)) return true;
        build();
        return luSmall(LU, na, piv, rinv);
    }
    for (int k = 0; k < na; ++k) {
        double rmax = 0;
        for (int c = k; c < na; ++c) rmax = std::max(rmax, std::abs(LU[k * na + c]));
        const double pv = LU[k * na + k];
        if (!(std::abs(pv) > 0.1 * rmax) || !(std::abs(pv) > 1e-300)) {
            build();
            return luSmall(LU, na, piv, rinv);
        }
        piv[k] = k;
        const double inv = 1.0 / pv;
        rinv[k] = inv;
        for (int r = k + 1; r < na; ++r) {
            const double f = (LU[r * na + k] *= inv);
            for (int c = k + 1; c < na; ++c) LU[r * na + c] -= f * LU[k * na + c];
        }
    }
    return true;
}
template <int NN, int NA>
[[gnu::noinline]] void wbStep(const double (*JdF)[3], const int* live, const int* bs, const int* bn, const double* __restrict KA, const double* __restrict LU,
                              const int* piv, const double* rinv, const double* __restrict F, double* __restrict d, int nDyn = NN, int naDyn = NA) noexcept {
    constexpr int CA = NA > 0 ? NA : kMaxNlPorts;
    const int na = NA > 0 ? NA : naDyn, n = NN > 0 ? NN : nDyn;
    double y[CA];
    for (int a = 0; a < na; ++a) {
        const int ka = live[a], b0 = bs[ka], np = bn[ka];
        double s = 0;
        for (int m = 0; m < np; ++m) s += JdF[ka][m] * F[b0 + m];
        y[a] = s;
    }
    luSolveSmall(LU, na, piv, rinv, y);
    for (int k = 0; k < n; ++k) {
        double s = F[k];
        for (int b = 0; b < na; ++b) s += KA[k * na + b] * y[b];
        d[k] = s;
    }
}
using WbFactorFn = bool (*)(const double (*)[3], const int*, const int*, const int*, const double*, double*, int*, double*, int) noexcept;
using WbStepFn = void (*)(const double (*)[3], const int*, const int*, const int*, const double*, const double*, const int*, const double*, const double*, double*, int,
                          int) noexcept;
// wbFactor's arithmetic does not depend on NN, so every table points at the NN = 24 copies: one copy per live-row
// count instead of one per (NN, NA).
template <int NN, int... A>
constexpr std::array<WbFactorFn, sizeof...(A)> wbFactorTable(std::integer_sequence<int, A...>) { return {&wbFactor<24, A + 1>...}; }
template <int NN, int... A>
constexpr std::array<WbStepFn, sizeof...(A)> wbStepTable(std::integer_sequence<int, A...>) { return {&wbStep<NN, A + 1>...}; }

// Fixed-size Newton kernel. The Jacobian of the residual F(vp) = p + K·i(vp) − vp is J = K·Jd − I, where Jd is
// block-diagonal by device, so J costs nn²·(ports per device) instead of nn³.
//
// Low-rank step (exact): ports whose device rows of Jd are negligible (a grid below conduction, a device in cut-off)
// contribute nothing to K·Jd, so J = K[:,A]·Jd[A,:] − I with A the live rows. The Newton step solves
// (I − Jd[A,:]·K[:,A])·y = Jd[A,:]·F and d = F + K[:,A]·y (Woodbury): the same voltage-space iterate from an
// |A|×|A| system. A row is dropped only when its largest entry times K's largest column entry is below 1e-9, so the
// Jacobian moves by < 1e-9 and the residual (always exact) decides convergence.
//
// Chord confirmation: most samples converge in two iterations, the second only confirming the first. From the
// second iteration on, the step is first taken with the previous iterate's factorisation (one triangular solve, no
// new factorisation); if that step already meets the tolerance it is accepted (it differs from the Newton step by
// O(|Δ_prev|·|Δ|), far below the tolerance). Otherwise it is discarded and the full Newton step is taken at the same
// point with the devices already evaluated, so the iterates are exactly those of plain Newton.
// F = K·cur + p − vp and max |F| (newtonKernel). Out of line, one copy per size NN (NN = 0: run-time n), shared by the
// interpreter and the specialised kernels (docs §10.45): the dot product is reassociated (it vectorises), and the
// compiler's order for it could differ between an inlined copy in one kernel and another, so every kernel calls this one.
template <int NN>
[[gnu::noinline]] double nkResidual(const double* __restrict K, const double* __restrict cur, const double* __restrict p, const double* __restrict vp,
                                    double* __restrict F, int nDyn) noexcept {
    const int n = NN > 0 ? NN : nDyn;
    double maxF = 0;
    for (int k = 0; k < n; ++k) {
        double s = 0;
        {
            // a dot product of fixed length: reassociation lets it vectorise (the sum differs only in rounding)
#pragma clang fp reassociate(on)
            for (int j = 0; j < n; ++j) s += K[k * n + j] * cur[j];
        }
        s += p[k] - vp[k];
        F[k] = s;
        maxF = std::max(maxF, std::abs(s));
    }
    return maxF;
}

// The Newton acceptance's residual check (newtonKernel's residualOk) on big groups, with the in-order sums' decisions
// (docs §10.66). Row k passes when |s| <= gate·tk·(1 + |v|), s = F_k − d_k + Σ_j K_kj·di_j, or failing that when |s| <=
// gate·rn·tk·(1 + |v|), rn = 1 + Σ_j |K_kj|·gmax_j. The in-order sums are a serial chain of n multiply-adds per row
// (half the cost of an iteration's convergence test on the 5150). Here the same sums are reassociated (they vectorise)
// and carry a rounding bound: any summation order of these terms is within γ·(|s0| + Σ_j |K_kj|·|di_j|) of the exact sum,
// γ = (n + 2)·u, and Σ_j |K_kj|·|di_j| <= B = Σ_j kcol_j·|di_j| (kcol: K's column maxima). A row whose fast sum is more
// than twice that bound inside or outside its threshold gets the in-order sum's decision; the few others (und) go to the
// in-order row. False: some row fails for certain.
inline const bool kEnvNoResFast = std::getenv("CE_NORESFAST") != nullptr;
constexpr int kResFastMinPorts = 16;
template <int NN>
[[gnu::noinline]] bool nkResFast(const double* __restrict K, const double* __restrict kcol, const double* __restrict di,
                                 const double* __restrict gmax, const double* __restrict F, const double* __restrict d,
                                 const double* __restrict vp, const double* ptol, double tol, double resGate, int nDyn, int* __restrict und,
                                 int& nund) noexcept {
    const int n = NN > 0 ? NN : nDyn;
    // 4·γ on each side, γ = (n + 2)·2^-53 (generous: the in-order and the fast sum are each within γ of the exact sum);
    // the absolute term covers flush-to-zero underflow
    const double c = 8.0 * (n + 2) * 0x1p-53;
    double B = 0;
    {
#pragma clang fp reassociate(on)
        for (int j = 0; j < n; ++j) B += kcol[j] * std::abs(di[j]);
    }
    if (!(B < 1e300)) {
        for (int k = 0; k < n; ++k) und[k] = k;
        nund = n;
        return true;
    }
    int nu = 0;
    for (int k = 0; k < n; ++k) {
        const double* __restrict Kr = K + static_cast<std::ptrdiff_t>(k) * n;
        const double tk = ptol ? ptol[k] : tol;
        const double s0 = F[k] - d[k];
        double t = 0;
        {
#pragma clang fp reassociate(on)
            for (int j = 0; j < n; ++j) t += Kr[j] * di[j];
        }
        const double as = std::abs(s0 + t), E = c * (std::abs(s0) + B) + 1e-290, vk = 1.0 + std::abs(vp[k] + d[k]);
        const double thr1 = resGate * 1.0 * tk * vk;
        if (as + 2.0 * E <= thr1) continue;
        if (!(as - 2.0 * E > thr1)) {
            und[nu++] = k;
            continue;
        }
        double rn = 0;
        {
#pragma clang fp reassociate(on)
            for (int j = 0; j < n; ++j) rn += std::abs(Kr[j]) * gmax[j];
        }
        rn += 1.0;
        const double thrLo = resGate * (rn * (1.0 - c)) * tk * vk, thrHi = resGate * (rn * (1.0 + c)) * tk * vk;
        if (as + 2.0 * E <= thrLo) continue;
        if (as - 2.0 * E > thrHi) return false;
        und[nu++] = k;
    }
    nund = nu;
    return true;
}

// newtonKernel's portRatio quotients |d_k| / (tk·(1 + |v_k|)) for all ports (big groups, docs §10.66): an elementwise
// loop, so the divisions vectorise; each quotient is the correctly rounded one, as in the scalar form.
template <int NN>
[[gnu::noinline]] void nkQuot(const double* __restrict d, const double* __restrict vn, const double* ptol, double tol, int nDyn,
                              double* __restrict q) noexcept {
    const int n = NN > 0 ? NN : nDyn;
    if (ptol)
        for (int k = 0; k < n; ++k) q[k] = std::abs(d[k]) / (ptol[k] * (1.0 + std::abs(vn[k])));
    else
        for (int k = 0; k < n; ++k) q[k] = std::abs(d[k]) / (tol * (1.0 + std::abs(vn[k])));
}

// Group structure (docs §10.45). The kernel's loops and branches over devices and ports read the group's structure: each
// device's law (kind), port count, first port and exact-split shift flag, each port's limiter kind, and which ports also
// converge on their current (currentConv below). DynSpec takes it from the arguments at run time (the interpreter, any
// circuit). StaticSpec<G> takes it from a circuit_codegen-generated G as compile-time constants: every loop over devices
// or ports becomes straight-line code, each device law and limiter is inlined without its kind switch, and the per-call
// current-convergence scan is gone. Every floating-point operation, operand and order is the same in both (the same
// source; only what decides control flow is constant), so a specialised kernel is bit-identical to the interpreter.
struct DynSpec {
    static constexpr bool kStatic = false;
};
template <class G>
struct StaticSpec {
    static constexpr bool kStatic = true;
    using Gen = G;
};
// Two-lane tube pairs (docs §10.51, exactmisc): in a static spec, consecutive triodes (and consecutive pentodes) are paired
// at compile time and each pair is evaluated in one two-lane pass (NlDevice::evalTriodePair / evalPentodePair, bit-identical
// per device). partner[q] >= 0: q leads a pair with that device; <= -2: q follows (evaluated with its leader); -1: alone.
// CE_NOTUBEPAIRS=1 evaluates every device alone (the same results).
inline const bool kEnvNoTubePairs = std::getenv("CE_NOTUBEPAIRS") != nullptr;
template <class G>
struct NkPairs {
    static constexpr std::array<int, static_cast<std::size_t>(G::ndev)> partner = [] {
        std::array<int, static_cast<std::size_t>(G::ndev)> a{};
        for (int q = 0; q < G::ndev; ++q) a[static_cast<std::size_t>(q)] = -1;
        for (NlKind kd : {NlKind::Triode, NlKind::Pentode}) {
            int open = -1;
            for (int q = 0; q < G::ndev; ++q) {
                if (G::kind[q] != kd || G::shift[q]) continue;
                if (open < 0) open = q;
                else {
                    a[static_cast<std::size_t>(open)] = q;
                    a[static_cast<std::size_t>(q)] = -2 - open;
                    open = -1;
                }
            }
        }
        return a;
    }();
};
// The generated structure G (circuit_codegen) provides: n, ndev; per device kind[], np[], off[], shift[]; per port lim[],
// bn[], dev[] (device index), ccA[] (a floating zener junction: always on its current), ccB[] (a diode or zener junction:
// on its current in a group with per-port tolerances).

// Calls f(q, o) for every device q with first port o: compile-time constants (std::integral_constant) for a static spec.
template <class S, class F>
[[gnu::always_inline]] inline void nkForDevs(const NlDevice* const* devs, int ndev, F&& f) noexcept {
    if constexpr (S::kStatic) {
        using G = typename S::Gen;
        [&]<int... Q>(std::integer_sequence<int, Q...>) {
            (f(std::integral_constant<int, Q>{}, std::integral_constant<int, G::off[Q]>{}), ...);
        }(std::make_integer_sequence<int, G::ndev>{});
        (void)devs;
        (void)ndev;
    } else {
        int o = 0;
        for (int q = 0; q < ndev; ++q) {
            f(q, o);
            o += devs[q]->nports;
        }
    }
}
// Calls f(k) for every port k < n, in order (compile-time constants for a static spec).
template <class S, class F>
[[gnu::always_inline]] inline void nkForPorts(int n, F&& f) noexcept {
    if constexpr (S::kStatic) {
        [&]<int... K>(std::integer_sequence<int, K...>) { (f(std::integral_constant<int, K>{}), ...); }(std::make_integer_sequence<int, S::Gen::n>{});
        (void)n;
    } else {
        for (int k = 0; k < n; ++k) f(k);
    }
}

template <int NN, class S = DynSpec>
inline bool newtonKernel(const NlDevice* const* devs, int ndev, const int* bs, const int* bn, const double* __restrict K,
                         const double* __restrict p, double* __restrict vpOut, double* __restrict curOut, const std::uint8_t* lim,
                         const double* limA, const double* limB, const double* limC, int maxIt, double tol, int& itersOut,
                         double& residualOut, const double* kcol = nullptr, bool chordOn = true, KaCache* kac = nullptr, int nDyn = NN,
                         KernelState* ks = nullptr, const double* dpPred = nullptr, double* luScratch = nullptr,
                         SparseLu* sp = nullptr, const double* ptol = nullptr) noexcept {
    // ptol: per-port tolerances where a group's ports differ (GroupRt::ptol; tol is then their smallest), else null
    // NN = 0: the run-time-size copy for groups above 24 ports (n = nDyn <= kMaxNlPorts), same algorithm and arithmetic
    constexpr int CAP = NN > 0 ? NN : kMaxNlPorts;
    const int n = [&] {
        if constexpr (S::kStatic) return S::Gen::n;
        else return NN > 0 ? NN : nDyn;
    }();
    // structure accessors: the port's device-block size and limiter kind, a device's port count (constants when static)
    auto bnOf = [&](auto k) -> int {
        if constexpr (S::kStatic) return S::Gen::bn[k];
        else return bn[k];
    };
    auto npOf = [&](auto q) -> int {
        if constexpr (S::kStatic) return S::Gen::np[q];
        else return devs[q]->nports;
    };
    auto evalDev = [&](auto q, const double* v, double* i, double* J) {
        if constexpr (S::kStatic) devs[q]->evalAs(S::Gen::kind[q], S::Gen::shift[q], S::Gen::np[q], v, i, J);
        else devs[q]->eval(v, i, J);
    };
    double vp[CAP], cur[CAP], F[CAP], d[CAP];
    // device Jacobians, double-buffered: the one a factorisation was built from (JdF) stays while the next iterate is
    // evaluated into the other buffer (a pointer flip instead of a copy)
    // With a KernelState the factorisation lives there (kept for the next sample's predictor), else on the stack; the
    // run-time-size copy (up to kMaxNlPorts² doubles) uses the caller's preallocated luScratch instead of the stack.
    double JdBuf[2][CAP][3], LUL[NN > 0 ? NN * NN : 1], rinvL[CAP];
    int pivL[CAP], liveL[CAP];
    double (*const JdB0)[3] = JdBuf[0];
    double (*const JdB1)[3] = JdBuf[1];
    double* __restrict const LU = ks ? ks->LU.data() : NN > 0 ? LUL : luScratch;
    double* __restrict const rinv = ks ? ks->rinv.data() : rinvL;
    int* __restrict const piv = ks ? ks->piv.data() : pivL;
    int* __restrict const live = ks ? ks->live.data() : liveL;
    double (*Jd)[3] = JdB0;
    double (*JdF)[3] = JdB1;
    double KAloc[NN > 0 ? NN * NN : 1]; // KA: K's live columns, n×na contiguous (run-time size: kac only)
    const double* KA = KAloc;
    int na = n;
    LiveMask lastMask;
    bool luSparse = false, liveSet = false; // last factorisation: sparse nodal (sp); live[] holds its live ports
    bool denseOnly = false; // a sparse step failed the residual check: dense factorisations for the rest of this solve
    // Sparse nodal step when its fixed cost beats the dense one at this live-port count (run-time-size kernel only)
    auto sparseWanted = [&](int nLive) {
        if constexpr (NN > 0 && NN < 13) {
            (void)nLive;
            return false;
        } else {
            if (!sp || !sp->ok || denseOnly) return false;
            const double a = nLive, nd = n;
            const double dense = nLive < n ? a * a * a / 3.0 + 4.0 * a * a + 2.0 * nd * a : nd * nd * nd / 3.0 + 4.0 * nd * nd;
            return kSpWeight * sp->cost < dense;
        }
    };
    auto factorLive = [&]() -> bool {
        if constexpr (NN > 0) {
            static constexpr auto fTab = wbFactorTable<NN>(std::make_integer_sequence<int, (NN > 1 ? NN - 1 : 1)>{});
            return fTab[static_cast<std::size_t>(na - 1)](Jd, live, bs, bn, KA, LU, piv, rinv, na);
        } else {
            // wbFactor's arithmetic does not depend on NN: up to 24 live rows, the unrolled fixed-NA copies (shared with
            // the 24-port kernel) run the same operations in the same order
            static constexpr auto fTab = wbFactorTable<24>(std::make_integer_sequence<int, 24>{});
            if (na <= 24) return fTab[static_cast<std::size_t>(na - 1)](Jd, live, bs, bn, KA, LU, piv, rinv, na);
            return wbFactor<0, 0>(Jd, live, bs, bn, KA, LU, piv, rinv, na);
        }
    };
    auto stepLive = [&](const double (*JdUse)[3]) {
        if constexpr (NN > 0) {
            static constexpr auto sTab = wbStepTable<NN>(std::make_integer_sequence<int, (NN > 1 ? NN - 1 : 1)>{});
            sTab[static_cast<std::size_t>(na - 1)](JdUse, live, bs, bn, KA, LU, piv, rinv, F, d, n, na);
        } else {
            static constexpr auto sTab = wbStepTable<0>(std::make_integer_sequence<int, 24>{}); // fixed NA, run-time n
            if (na <= 24) sTab[static_cast<std::size_t>(na - 1)](JdUse, live, bs, bn, KA, LU, piv, rinv, F, d, n, na);
            else wbStep<0, 0>(JdUse, live, bs, bn, KA, LU, piv, rinv, F, d, n, na);
        }
    };
    bool haveLU = false;
    bool xTry = false;        // this solve's first chord step uses the kept factorisation (cross-sample chord)
    double lastRatio = 1e30; // last full step's largest |Δ| / tolerance: the chord is tried only when it was close
    double lsc[CAP];          // per-port scale of the tube step limit (halved when limited steps alternate)
    signed char lsg[CAP];     // sign of the last limited step
    bool lsInit = false;      // set on the first full step (a solve accepted by a chord step never reads them)
    const double chordGate = ks && ks->chordGate > 0 ? ks->chordGate : kEnvChordGate; // docs §10.50
    for (int k = 0; k < n; ++k) vp[k] = vpOut[k];
    // Floating zener junctions (a back-to-back clamp's midpoint, NlDevice::floatingPorts): the island's voltage is set
    // by leakage currents alone, so the port converges on its current (SPICE's abstol test) as well as on its voltage.
    double fltG[CAP];
    bool anyFlt = false;
    // In a group with tube ports (ptol set, docs §10.42) every diode and zener junction converges this way too: one that
    // carries no current (a flyback clamp across an output-tube plate, reverse biased at hundreds of volts) otherwise
    // holds the plate it shares a voltage with to the junction tolerance; a conducting one still meets the voltage test
    // first (|g·Δv| <= tol·|i| is tol·nVt there).
    // fltG is read only when anyFlt: fill it only then (the per-device fill was a library call per device and solve)
    if constexpr (S::kStatic) {
        // the structure decides every port's test: no scan (ccA: floating zener; ccB: diode / zener junction)
        using G = typename S::Gen;
        if constexpr (G::anyCcA || G::anyCcB) {
            if (!kEnvNoFltConv) {
                const bool useB = ptol && !kEnvNoTubeJct;
                nkForPorts<S>(n, [&](auto k) {
                    const bool f = G::ccA[k] || (G::ccB[k] && useB);
                    fltG[k] = f ? devs[G::dev[k]]->gShift[k - G::off[G::dev[k]]] : -1.0;
                    anyFlt |= f;
                });
            }
        }
    } else {
        auto currentConv = [&](const NlDevice& dv, int a) {
            if (kEnvNoFltConv) return false;
            if (dv.kind == NlKind::Zener && (dv.floatingPorts >> a & 1u)) return true;
            return ptol && !kEnvNoTubeJct && (dv.kind == NlKind::Diode || dv.kind == NlKind::DiodePair || dv.kind == NlKind::Zener);
        };
        if (!kEnvNoFltConv)
            for (int q = 0; q < ndev && !anyFlt; ++q)
                for (int a = 0; a < devs[q]->nports; ++a) anyFlt |= currentConv(*devs[q], a);
        if (anyFlt) {
            anyFlt = false;
            int o = 0;
            for (int q = 0; q < ndev; ++q) {
                const NlDevice& dv = *devs[q];
                for (int a = 0; a < dv.nports; ++a) {
                    const bool f = currentConv(dv, a);
                    fltG[o + a] = f ? dv.gShift[a] : -1.0;
                    anyFlt |= f;
                }
                o += dv.nports;
            }
        }
    }
    auto portRatio = [&](int k, double dk, double vnew) {
        const double tk = ptol ? ptol[k] : tol;
        const double rv = std::abs(dk) / (tk * (1.0 + std::abs(vnew)));
        if (!anyFlt || fltG[k] < 0 || rv <= 1.0) return rv;
        const double g = Jd[k][0] + fltG[k], i = cur[k] + fltG[k] * vp[k];
        return std::min(rv, std::abs(g * dk) / (kFltAbsI + tk * std::abs(i)));
    };
    // Accept the step d from vp (currents to first order through the Jacobian at vp): quadratic convergence leaves
    // O(κ·Δ²) error, so the devices are not evaluated again (saves one exp/tanh per port per sample).
    auto accept = [&](int it, double maxF) {
        if (ks) {
            ks->valid = haveLU;
            ks->na = na;
            ks->mask = lastMask;
            ks->sparse = luSparse;
            if (haveLU) // the device Jacobians the kept factorisation was built from
                for (int k = 0; k < n; ++k)
                    for (int b = 0; b < 3; ++b) ks->Jd[static_cast<std::size_t>(3 * k + b)] = JdF[k][b];
        }
        nkForDevs<S>(devs, ndev, [&](auto q, auto o) {
            const int np = npOf(q);
            for (int a = 0; a < np; ++a) {
                double di = 0;
                for (int b = 0; b < np; ++b) di += Jd[o + a][b] * d[o + b];
                curOut[o + a] = cur[o + a] + di;
            }
        });
        for (int k = 0; k < n; ++k) vpOut[k] = vp[k] + d[k];
        itersOut = it + 1;
        residualOut = maxF;
    };
    // d from the stored factorisation for the residual F
    auto stepFromLU = [&](const double (*JdUse)[3]) {
        if constexpr (NN == 0 || NN >= 13)
            if (luSparse) {
                spStep(*sp, JdUse, liveSet && na < n ? live : nullptr, na, n, bs, bn, F, d);
                return;
            }
        if (na < n) {
            if (na > 0) {
                stepLive(JdUse);
                return;
            }
            for (int k = 0; k < n; ++k) d[k] = F[k]; // no live rows: the step is the residual itself
        } else {
            for (int k = 0; k < n; ++k) d[k] = -F[k];
            if constexpr (NN > 0) luSolveFixedN<NN>(LU, piv, rinv, d);
            else luSolveSmall(LU, n, piv, rinv, d);
        }
    };
    // Predictor correction (processGroup): the extrapolated port voltages solve the extrapolated p to first order, so
    // move them by (I − K·Jd)⁻¹·(p − p_extrapolated) with the last sample's factorisation, limited like a Newton step.
    // An input step (a pick attack) then starts Newton next to the solution instead of a sample's worth of step away.
    if (ks) {
        const bool spKept = ks->sparse && sp && sp->ok;
        if (dpPred && ks->valid && (spKept || (!ks->sparse && (ks->na == n || ks->na == 0 || (kac && kac->valid && kac->mask == ks->mask))))) {
            na = ks->na;
            luSparse = spKept;
            liveSet = true;
            if (!luSparse && na < n && na > 0) KA = kac->KA.data();
            for (int k = 0; k < n; ++k) F[k] = dpPred[k];
            stepFromLU(reinterpret_cast<const double (*)[3]>(ks->Jd.data()));
            for (int k = 0; k < n; ++k) {
                const double vfull = vp[k] + d[k];
                vp[k] = lim[k] ? limitPort(lim[k], limA[k], limB[k], limC[k], vp[k], vfull) : vfull;
            }
            na = n;
            luSparse = liveSet = false;
        }
        // Cross-sample chord: the first iteration steps with the kept factorisation (and the device Jacobians it was built
        // from); a step that does not pass the chord's acceptance test falls through to a fresh factorisation.
        if ((NN == 0 || NN > 2) && ks->xchord && chordOn && ks->valid && (spKept || (!ks->sparse && (ks->na == n || ks->na == 0 || (kac && kac->valid && kac->mask == ks->mask))))) {
            na = ks->na;
            luSparse = spKept;
            liveSet = true;
            lastMask = ks->mask;
            if (!luSparse && na < n && na > 0) KA = kac->KA.data();
            for (int k = 0; k < n; ++k)
                for (int b = 0; b < 3; ++b) JdF[k][b] = ks->Jd[static_cast<std::size_t>(3 * k + b)];
            haveLU = true;
            lastRatio = 0.0;
            xTry = true;
        }
        ks->valid = false; // set again by accept() from this call's factorisation
    }
    // The residual a step d from vp leaves to first order with this iterate's device Jacobian, F + (K·Jd − I)·d (what
    // accept() hands on: vp + d and cur + Jd·d), as the step it still asks for (kNewtonResidualGate). A small step is not
    // proof of convergence when the step was solved with a stale or inaccurate matrix: a chord factorisation from an
    // iterate where a stiff port was still off, or the sparse nodal step d = F + C·x, which cancels catastrophically
    // when |F| is huge (both seen on an op-amp rail clamp driven to 1e11 S: residual 2e16 V accepted as converged, the
    // JC-120's supply-cut chorus op-amp under LTE substeps, wave-e/roland-jc-120.notes.md session 7).
    double resGate = xTry ? kNewtonResidualGate / kEnvXChordGate : kNewtonResidualGate; // tightened for the cross-sample chord's attempt
    const bool ratioFast = (NN == 0 || NN >= kResFastMinPorts) && !kEnvNoResFast && n >= kResFastMinPorts;
    // One row of the residual check (in order: the sums' rounding is the decision's)
    auto residualRow = [&](int k, const double* di, const double* gmax) -> bool {
        const double tk = ptol ? ptol[k] : tol;
        double s = F[k] - d[k], rn = 1.0;
        if (!kEnvNoBigExact) {
            for (int j = 0; j < n; ++j) s += K[k * n + j] * di[j];
            // rn >= 1 and rounding is monotone: a residual within the gate at rn = 1 is within it at rn
            if (std::abs(s) <= resGate * 1.0 * tk * (1.0 + std::abs(vp[k] + d[k]))) return true;
            for (int j = 0; j < n; ++j) rn += std::abs(K[k * n + j]) * gmax[j];
        } else {
            for (int j = 0; j < n; ++j) s += K[k * n + j] * di[j], rn += std::abs(K[k * n + j]) * gmax[j];
        }
        return std::abs(s) <= resGate * rn * tk * (1.0 + std::abs(vp[k] + d[k]));
    };
    auto residualOk = [&]() {
        double di[CAP], gmax[CAP];
        nkForDevs<S>(devs, ndev, [&](auto q, auto o) {
            const int np = npOf(q);
            for (int a = 0; a < np; ++a) {
                double t = 0, g = 0;
                for (int b = 0; b < np; ++b) t += Jd[o + a][b] * d[o + b], g = std::max(g, std::abs(Jd[o + a][b]));
                di[o + a] = t;
                gmax[o + a] = g;
            }
        });
        if constexpr (NN > 0 && NN < kResFastMinPorts) {
            for (int k = 0; k < n; ++k) {
                const double tk = ptol ? ptol[k] : tol;
                double s = F[k] - d[k], rn = 1.0;
                if (!kEnvNoBigExact) {
                    for (int j = 0; j < n; ++j) s += K[k * n + j] * di[j];
                    // rn >= 1 and rounding is monotone: a residual within the gate at rn = 1 is within it at rn
                    if (std::abs(s) <= resGate * 1.0 * tk * (1.0 + std::abs(vp[k] + d[k]))) continue;
                    for (int j = 0; j < n; ++j) rn += std::abs(K[k * n + j]) * gmax[j];
                } else {
                    for (int j = 0; j < n; ++j) s += K[k * n + j] * di[j], rn += std::abs(K[k * n + j]) * gmax[j];
                }
                if (!(std::abs(s) <= resGate * rn * tk * (1.0 + std::abs(vp[k] + d[k])))) return false;
            }
        } else {
            if constexpr (NN == 0 || NN >= kResFastMinPorts) {
                // the same decisions from vectorised sums with a rounding bound; only undecided rows run the in-order sums
                // below (nkResFast, docs §10.66)
                if (kcol && !kEnvNoBigExact && !kEnvNoResFast && n >= kResFastMinPorts) {
                    int und[CAP], nu = 0;
                    if (!nkResFast<NN>(K, kcol, di, gmax, F, d, vp, ptol, tol, resGate, n, und, nu)) return false;
                    for (int u = 0; u < nu; ++u)
                        if (!residualRow(und[u], di, gmax)) return false;
                    return true;
                }
            }
            for (int k = 0; k < n; ++k)
                if (!residualRow(k, di, gmax)) return false;
        }
        return true;
    };
    for (int it = 0;; ++it) {
        if (Jd == JdF) Jd = Jd == JdB0 ? JdB1 : JdB0;
        nkForDevs<S>(devs, ndev, [&](auto q, auto o) {
            if constexpr (S::kStatic) { // two-lane tube pairs (docs §10.51): the follower is evaluated with its leader
                using G = typename S::Gen;
                constexpr int pr = NkPairs<G>::partner[static_cast<std::size_t>(decltype(q)::value)];
                if constexpr (pr <= -2) {
                    if (!kEnvNoTubePairs && devs[q]->lut && devs[-2 - pr]->lut && devs[q]->lut->lanes && devs[-2 - pr]->lut->lanes) return;
                } else if constexpr (pr >= 0) {
                    const NlDevice& d0 = *devs[q];
                    const NlDevice& d1 = *devs[pr];
                    if (!kEnvNoTubePairs && d0.lut && d1.lut && d0.lut->lanes && d1.lut->lanes) {
                        constexpr int o1 = G::off[pr], np = G::np[decltype(q)::value];
                        double Jl[9], Jl1[9];
                        if constexpr (G::kind[decltype(q)::value] == NlKind::Triode)
                            NlDevice::evalTriodePair(d0, d1, vp + o, vp + o1, cur + o, cur + o1, Jl, Jl1);
                        else NlDevice::evalPentodePair(d0, d1, vp + o, vp + o1, cur + o, cur + o1, Jl, Jl1);
                        for (int a = 0; a < np; ++a)
                            for (int b = 0; b < np; ++b) Jd[o + a][b] = Jl[a * np + b], Jd[o1 + a][b] = Jl1[a * np + b];
                        return;
                    }
                }
            }
            double Jl[9];
            evalDev(q, vp + o, cur + o, Jl);
            const int np = npOf(q);
            for (int a = 0; a < np; ++a)
                for (int b = 0; b < np; ++b) Jd[o + a][b] = Jl[a * np + b];
        });
        // F = K·cur + p − vp, the dot products in one out-of-line copy per size (nkResidual) that the interpreter and every
        // specialised kernel call: the reassociated sum's order is the compiler's choice, so it is made once (docs §10.45)
        const double maxF = nkResidual<NN>(K, cur, p, vp, F, n);
        if (!(maxF < 1e30)) return false;
        if (it >= maxIt) return false;
        if constexpr (NN > 0 && NN <= 2) {
            // closed-form tiny systems: plain Newton
            double J[NN * NN];
            for (int k = 0; k < n; ++k) d[k] = -F[k];
            for (int a = 0; a < NN; ++a)
                for (int b = 0; b < NN; ++b) {
                    double t = a == b ? -1.0 : 0.0;
                    for (int m = 0; m < NN; ++m)
                        if (bs[m] == bs[b]) t += K[a * NN + m] * Jd[m][b - bs[b]];
                    J[a * NN + b] = t;
                }
            if (!solveFixed<NN>(J, d)) return false;
        } else {
            if (xTry && kcol) {
                // the kept factorisation stands in only while the device Jacobians it was built from still describe this
                // iterate: Σ_k max_b |ΔJd[k][b]|·max|K[:,k]| bounds the change of K·Jd (a port turning on fails it)
                double dj = 0;
                for (int k = 0; k < n; ++k) {
                    double m = 0;
                    for (int b = 0; b < bn[k]; ++b) m = std::max(m, std::abs(Jd[k][b] - JdF[k][b]));
                    dj += m * kcol[k];
                }
                if (!(dj <= kEnvXChordDj)) haveLU = false, xTry = false, resGate = kNewtonResidualGate;
            } else if (xTry) haveLU = false, xTry = false, resGate = kNewtonResidualGate;
            if (haveLU && chordOn && lastRatio <= chordGate) {
                stepFromLU(JdF);
                bool small = true;
                for (int k = 0; k < n && small; ++k) {
                    const double vfull = vp[k] + d[k];
                    const double rk = portRatio(k, d[k], vfull);
                    if (lim[k] && limitPort(lim[k], limA[k], limB[k], limC[k], vp[k], vfull) != vfull && !(anyFlt && fltG[k] >= 0 && rk <= 1.0)) small = false;
                    small = small && rk <= 1.0;
                }
                // an iterate already at the tolerance's residual needs no check (it solves the equations itself)
                if (small && (maxF <= tol * resGate || residualOk())) {
                    accept(it, maxF);
                    return true;
                }
                resGate = kNewtonResidualGate; // the cross-sample attempt is the first chord only
                xTry = false;
            }
            // factorise at this iterate
            na = n;
            luSparse = liveSet = false;
            if (n >= 4 && kcol && (NN > 0 || kac)) {
                liveSet = true;
                na = 0;
                LiveMask mask;
                nkForPorts<S>(n, [&](auto k) {
                    double m = 0;
                    const int nb = bnOf(k);
                    for (int b = 0; b < nb; ++b) m = std::max(m, std::abs(Jd[k][b]));
                    if (m * kcol[k] > 1e-9) {
                        live[na++] = k;
                        mask.set(k);
                    }
                });
                lastMask = mask;
                if (na < n && na > 0 && !sparseWanted(na)) {
                    double* dst = kac ? kac->KA.data() : KAloc;
                    if (!(kac && kac->valid && kac->mask == mask)) {
                        for (int k = 0; k < n; ++k)
                            for (int b = 0; b < na; ++b) dst[k * na + b] = K[k * n + live[b]];
                        if (kac) {
                            kac->mask = mask;
                            kac->valid = true;
                        }
                    }
                    KA = dst;
                }
            }
            if constexpr (NN == 0 || NN >= 13)
                if (sparseWanted(na)) {
                    luSparse = spFactor(*sp, Jd, liveSet && na < n ? live : nullptr, na, n, bn);
                    if (!luSparse && na < n && na > 0) { // dense fallback for this factorisation: gather K's live columns
                        double* dst = kac ? kac->KA.data() : KAloc;
                        if (!(kac && kac->valid && kac->mask == lastMask)) {
                            for (int k = 0; k < n; ++k)
                                for (int b = 0; b < na; ++b) dst[k * na + b] = K[k * n + live[b]];
                            if (kac) {
                                kac->mask = lastMask;
                                kac->valid = true;
                            }
                        }
                        KA = dst;
                    }
                }
            if (luSparse) {
            } else if (na < n) {
                if (na > 0 && !factorLive()) return false;
            } else {
                // J = K·Jd − I, device block by device block (Jd is block-diagonal)
                double* J = LU;
                nkForDevs<S>(devs, ndev, [&](auto q, auto o0) {
                    const int np = npOf(q);
                    if (np == 1) {
                        const double j00 = Jd[o0][0];
                        for (int a = 0; a < n; ++a) J[a * n + o0] = K[a * n + o0] * j00;
                    } else if (np == 2) {
                        const double j00 = Jd[o0][0], j01 = Jd[o0][1], j10 = Jd[o0 + 1][0], j11 = Jd[o0 + 1][1];
                        for (int a = 0; a < n; ++a) {
                            const double k0 = K[a * n + o0], k1 = K[a * n + o0 + 1];
                            J[a * n + o0] = k0 * j00 + k1 * j10;
                            J[a * n + o0 + 1] = k0 * j01 + k1 * j11;
                        }
                    } else {
                        for (int a = 0; a < n; ++a) {
                            const double k0 = K[a * n + o0], k1 = K[a * n + o0 + 1], k2 = K[a * n + o0 + 2];
                            for (int b = 0; b < 3; ++b) J[a * n + o0 + b] = k0 * Jd[o0][b] + k1 * Jd[o0 + 1][b] + k2 * Jd[o0 + 2][b];
                        }
                    }
                });
                for (int a = 0; a < n; ++a) J[a * n + a] -= 1.0;
                if constexpr (NN > 0) {
                    if (!luFixedN<NN>(LU, piv, rinv)) return false;
                } else if (!luSmall(LU, n, piv, rinv)) return false;
            }
            haveLU = true;
            JdF = Jd; // the Jacobian just factorised; the next evaluation moves to the other buffer
            stepFromLU(JdF);
        }
        if (!lsInit) {
            for (int k = 0; k < n; ++k) {
                lsc[k] = 1.0;
                lsg[k] = 0;
            }
            lsInit = true;
        }
        bool limited = false;
        double ratio = 0;
        if constexpr (NN > 0 && NN < kResFastMinPorts) {
            // a plain loop on every spec: unrolled per port (constant limiter kinds) it measured slower on the 9900X (the
            // per-port copies of limitPort and portRatio outgrow the loop's branch-predicted form)
            for (int k = 0; k < n; ++k) {
                const double vold = vp[k];
                const double vfull = vold + d[k];
                double vnew = vfull;
                const std::uint8_t lk = lim[k];
                if (lk) {
                    // tube ports: a limited step that reverses the previous limited step halves this port's limit
                    // (an iterate cycling across a knee converges instead of bouncing at the full limit)
                    // floating junctions: a limited step that reverses the previous one switches the current-space step
                    // off for the rest of this solve (lsc 0; a junction inside a transistor's loop otherwise cycled
                    // between the mapped step down and pnjlim's way back up)
                    vnew = limitPort(lk, lk == kLimStep ? limA[k] * lsc[k] : limA[k], limB[k],
                                     lk == kLimPnjF ? lsc[k] : lk == kLimZenerF ? (lsc[k] > 0 ? limC[k] : -limC[k]) : limC[k], vold, vfull);
                    if (vnew != vfull && (lk == kLimStep || lk == kLimPnjF || lk == kLimZenerF)) {
                        const signed char sg = vnew > vold ? 1 : -1;
                        if (lsg[k] == -sg) lsc[k] = lk == kLimStep ? 0.5 * lsc[k] : 0.0;
                        lsg[k] = sg;
                    }
                }
                // a floating zener junction whose full step moves no current that matters does not hold the solve back
                limited |= vnew != vfull && !(anyFlt && fltG[k] >= 0 && portRatio(k, vfull - vold, vfull) <= 1.0);
                d[k] = vnew - vold;
                ratio = std::max(ratio, portRatio(k, d[k], vnew));
            }
        } else {
            double vn[(NN == 0 || NN >= kResFastMinPorts) ? CAP : 1]; // the limited iterates, for nkQuot
            // a plain loop on every spec: unrolled per port (constant limiter kinds) it measured slower on the 9900X (the
            // per-port copies of limitPort and portRatio outgrow the loop's branch-predicted form)
            for (int k = 0; k < n; ++k) {
                const double vold = vp[k];
                const double vfull = vold + d[k];
                double vnew = vfull;
                const std::uint8_t lk = lim[k];
                if (lk) {
                    // tube ports: a limited step that reverses the previous limited step halves this port's limit
                    // (an iterate cycling across a knee converges instead of bouncing at the full limit)
                    // floating junctions: a limited step that reverses the previous one switches the current-space step
                    // off for the rest of this solve (lsc 0; a junction inside a transistor's loop otherwise cycled
                    // between the mapped step down and pnjlim's way back up)
                    vnew = limitPort(lk, lk == kLimStep ? limA[k] * lsc[k] : limA[k], limB[k],
                                     lk == kLimPnjF ? lsc[k] : lk == kLimZenerF ? (lsc[k] > 0 ? limC[k] : -limC[k]) : limC[k], vold, vfull);
                    if (vnew != vfull && (lk == kLimStep || lk == kLimPnjF || lk == kLimZenerF)) {
                        const signed char sg = vnew > vold ? 1 : -1;
                        if (lsg[k] == -sg) lsc[k] = lk == kLimStep ? 0.5 * lsc[k] : 0.0;
                        lsg[k] = sg;
                    }
                }
                // a floating zener junction whose full step moves no current that matters does not hold the solve back
                limited |= vnew != vfull && !(anyFlt && fltG[k] >= 0 && portRatio(k, vfull - vold, vfull) <= 1.0);
                d[k] = vnew - vold;
                if constexpr (NN == 0 || NN >= kResFastMinPorts) {
                    if (ratioFast) {
                        vn[k] = vnew;
                        continue;
                    }
                }
                ratio = std::max(ratio, portRatio(k, d[k], vnew));
            }
            if constexpr (NN == 0 || NN >= kResFastMinPorts) {
                if (ratioFast) {
                    // portRatio's quotients, divided four at a time (nkQuot), then its floating-junction test and the
                    // maximum in port order: the same values and the same maximum (docs §10.66)
                    double rq[CAP];
                    nkQuot<NN>(d, vn, ptol, tol, n, rq);
                    for (int k = 0; k < n; ++k) {
                        double rv = rq[k];
                        if (anyFlt && fltG[k] >= 0 && !(rv <= 1.0)) {
                            const double tk = ptol ? ptol[k] : tol;
                            const double g = Jd[k][0] + fltG[k], i = cur[k] + fltG[k] * vp[k];
                            rv = std::min(rv, std::abs(g * d[k]) / (kFltAbsI + tk * std::abs(i)));
                        }
                        ratio = std::max(ratio, rv);
                    }
                }
            }
        }
        bool rejected = false; // a small step whose residual check failed: no chord from this factorisation
        if (ratio <= 1.0 && !limited) {
            if (maxF <= tol || residualOk()) {
                accept(it, maxF);
                return true;
            }
            if (luSparse) { // the sparse step cancelled: solve this iterate again, densely
                denseOnly = true;
                lastRatio = 1e30;
                continue;
            }
            rejected = true;
        }
        lastRatio = limited || rejected ? 1e30 : ratio;
        for (int k = 0; k < n; ++k) vp[k] += d[k];
    }
}

} // namespace rig::ce::nk
