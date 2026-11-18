#pragma once
// Nonlinear device laws (docs/CIRCUIT_ENGINE.md §4). Each device is a set of ports; port k has a controlling
// voltage v_k and a current i_k. eval() fills the currents and the local Jacobian ∂i/∂v (row-major np×np).
// The SPICE exporter (Netlist.cpp) writes the very same equations for ngspice, so the validation compares
// solvers, not model differences.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "ModelLibrary.hpp"

namespace rig::ce {

inline constexpr double kGmin = 1e-12; // SPICE-style junction gmin (S)
// Largest coupled nonlinear system one solve group may hold (ENGINE_QUEUE items 13 and 17; was 24, then 48). Groups up
// to 24 ports use the size-specialised Newton/LU kernels, larger ones the same kernel at a run-time size (factorisation
// scratch preallocated per group, so the audio thread's stack holds only O(kMaxNlPorts) arrays); the generic Newton,
// pseudo-transient and homotopy fallbacks size their small scratch arrays by this constant. At most 128 (live-row masks).
inline constexpr int kMaxNlPorts = 96;

// exp() with a linear continuation above x = 40: Newton never overflows on wild intermediate iterates, and the
// converged solutions of real circuits never reach the continuation (e^40 · Is ≫ any real current).
inline double limexp(double x, double& d) noexcept {
    if (x < -40.0) { // < 4.3e-18: below double resolution next to the -1 / gmin terms it is added to
        d = 0.0;
        return 0.0;
    }
    if (x < 40.0) {
        const double e = std::exp(x);
        d = e;
        return e;
    }
    // exp(40), as a literal: a function-local static initialised from std::exp is guarded (a lock on first use), and
    // the first junction past the limit is evaluated on the audio thread.
    constexpr double e40 = 2.3538526683702e+17;
    d = e40;
    return e40 * (1.0 + x - 40.0);
}
inline double softplus(double z, double& sig) noexcept {
    const double e = std::exp(-std::abs(z));
    sig = z >= 0 ? 1.0 / (1.0 + e) : e / (1.0 + e);
    return std::max(z, 0.0) + std::log1p(e);
}

enum class NlKind : std::uint8_t { Diode, DiodePair, Bjt, Ota, RailClamp, Triode, Pentode, VacDiode, Magnetizing, Neon, LightDrive, Ldr, Jfet, OutHalf,
                                   OtaHalf, LampTh, CdsRate, CdsCell, DiodeLight, AbsI, GainCell, ElLight, RmsSq, RmsLog, CmosOut,
                                   MosHalf, Zener, OutLim, Ad536, Speaker, ZenerPair };

// CMOS 4000B channel law (docs/CIRCUIT_ENGINE.md §10.14): square law with mobility degradation θ, channel-length
// modulation λ and a softplus subthreshold tail κ. vds ≥ 0. p: β, Vt, θ, λ, κ. Returns Id; d* = partial derivatives.
inline double cmosId(double vgs, double vds, const double* p, double& dVgs, double& dVds) noexcept {
    double sg;
    const double vov = p[4] * softplus((vgs - p[1]) / p[4], sg);
    const double den = 1.0 / (1.0 + p[2] * vov), be = p[0] * den, dbe = -p[0] * p[2] * den * den;
    const double m = std::min(vds, vov), q = vov * m - 0.5 * m * m, c = 1.0 + p[3] * vds;
    dVgs = (dbe * q + be * m) * c * sg;
    dVds = be * (vov - m) * c + be * q * p[3];
    return be * q * c;
}

// Koren triode as separable 1-D tables (docs §3.6): Ip = A(vpk)·h(z), z = kp/mu + kp·(vgk + vgo)·C(vpk),
//   A = (2/kg1)(vpk/kp)^ex,  C = 1/sqrt(kvb + vpk²),  h(z) = softplus(z)^ex;  Ig = gs·(gk·softplus(vgk/gk))^1.5.
// Cubic Hermite on value + first + second derivative where needed; exact formulas outside the table ranges.
struct KorenTables {
    static constexpr int NZ = 4096, NV = 4096, NG = 8192, NA = 8192;
    static constexpr double ZMIN = -60.0, ZMAX = 60.0, VMAX = 800.0, GMIN = -4.0, GMAX = 24.0;
    // Pentode plate collection atan(vpk/kvb) over AMIN..AMAX (plates swing past twice B+ at flyback).
    static constexpr double AMIN = -200.0, AMAX = 1400.0;
    double mu, ex, kg1, kp, kvb, gs, gk;
    std::vector<double> h, dh, d2h, a, da, c, dc, g, dg, d2g, at, dat, d2at;
    // Two-lane tables (docs §10.51, exactmisc; "Two-lane tube laws" below): each knot's values together, 4 doubles a knot,
    // so a lookup's two knots are one or two cache lines and each pair of Hermite evaluations that share a knot interval
    // reads its operands as 2-wide vectors: ac {a, c, da, dc}; gK {g, dg, d2g, 0}; hK {h, dh, d2h, 0}; atK {at, dat, d2at, 0}.
    std::vector<double> ac, gK, hK, atK;
    bool lanes = false; // built (korenTables) and not switched off (CE_NOTUBELANES)
    static double hermite(double t, double step, double y0, double y1, double m0, double m1) noexcept {
        const double t2 = t * t, t3 = t2 * t;
        return (2 * t3 - 3 * t2 + 1) * y0 + (t3 - 2 * t2 + t) * step * m0 + (-2 * t3 + 3 * t2) * y1 + (t3 - t2) * step * m1;
    }
    static double hermiteD(double t, double step, double y0, double y1, double m0, double m1) noexcept {
        const double t2 = t * t;
        return ((6 * t2 - 6 * t) * y0 + (3 * t2 - 4 * t + 1) * step * m0 + (-6 * t2 + 6 * t) * y1 + (3 * t2 - 2 * t) * step * m1) / step;
    }
};

struct NlPort {
    int cp = 0, cn = 0;     // controlling voltage v = V[cp] - V[cn]
    int from = 0, to = 0;   // current i leaves `from`, enters `to` (through the device)
    double maxStep = 1e30;  // Newton step limit on this port (V)
};

// Diode with SPICE breakdown (ngspice level-1 law): forward / reverse is·(e^{v/nVt} − 1); below −xbv the breakdown branch
// −is·exp(−(xbv + v)/(nbv·Vt)). xbv is ngspice's BV adjusted so the current at −BV is IBV (computed at compile time with
// ngspice's own iteration and thermal voltage). p: is, nVt, 1/(nVt), xbv, 1/(nbv·Vt), the region boundary max(xbv, 3·nVt)
[[gnu::always_inline]] inline void zenerLaw(const double* p, double v, double& i, double& g) noexcept {
    const double is = p[0];
    if (v < -p[5]) { // p[5] = max(xbv, 3·nVt): ngspice's forward region always wins above −3·n·Vt
        double d;
        const double e = limexp(-(p[3] + v) * p[4], d);
        i = -is * e + kGmin * v;
        g = is * p[4] * d + kGmin;
        return;
    }
    double d;
    const double e = limexp(v * p[2], d);
    i = is * (e - 1.0) + kGmin * v;
    g = is * p[2] * d + kGmin;
}

struct NlDevice {
    // Hysteresis play update after a converged sample (magnetizing devices with p[2] > 0).
    void advancePlay(double lambda) noexcept {
        for (int j = 0; j < 3; ++j) {
            const double r = p[3] * (j + 1) / 3.0;
            const double z = (lambda - hs[j]) / r, z8 = (z * z) * (z * z) * (z * z) * (z * z);
            hs[j] = lambda - r * z / std::sqrt(std::sqrt(std::sqrt(1.0 + z8)));
        }
    }
    NlKind kind = NlKind::Diode;
    int part = -1;  // netlist part index
    int nports = 1;
    NlPort port[3];
    double p[14] = {};
    bool pnp = false;
    const KorenTables* lut = nullptr; // triode: tabulated plate/grid laws (exact formulas when null)
    double dcLinear = 0.0;            // DC solve: linear part moved into the matrix (magnetizing law: 1/Lm)
    // Exact linear split (Circuit::compile): a conductance gShift[k] stamped across port k's terminals in the linear
    // matrix and subtracted here, so the total current is unchanged. Used on junctions that are the only elements
    // at a node (series diode strings), whose node would otherwise have an empty row in the DK matrix.
    double gShift[3] = {};
    bool hasShift = false;
    // Bit k: port k touches an island that no linear element anchors, not even through a capacitor (a floating
    // transformer winding between bridge diodes). Its voltage is then set by the junction currents alone, so the
    // junction is current-driven (Circuit: kLimPnjF).
    std::uint8_t floatingPorts = 0;
    mutable double ws = 0.0;          // ZenerPair: the last midpoint solution (the next evaluation's warm start)
    double hs[3] = {};                // magnetizing: hysteresis play-operator states (λ at the centre of each play);
                                      // speaker: ΔRe (Ω), coil and magnet temperature rise (K)

    // v: port voltages; i: currents; J: np×np row-major ∂i/∂v.
    [[gnu::always_inline]] inline void eval(const double* v, double* i, double* J) const noexcept {
        evalLaw(v, i, J);
        if (hasShift)
            for (int k = 0; k < nports; ++k) {
                i[k] -= gShift[k] * v[k];
                J[k * nports + k] -= gShift[k];
            }
    }
    // eval with the kind, the shift flag and the port count given by the caller: a specialised Newton kernel
    // (NewtonKernel.hpp, docs §10.45) passes them as constants, so the law's switch and the shift test fold away. The
    // same operations in the same order as eval.
    [[gnu::always_inline]] inline void evalAs(NlKind kd, bool shift, int np, const double* v, double* i, double* J) const noexcept {
        evalLawAs(kd, v, i, J);
        if (shift)
            for (int k = 0; k < np; ++k) {
                i[k] -= gShift[k] * v[k];
                J[k * np + k] -= gShift[k];
            }
    }
    [[gnu::always_inline]] inline void evalLaw(const double* v, double* i, double* J) const noexcept { evalLawAs(kind, v, i, J); }
    [[gnu::always_inline]] inline void evalLawAs(NlKind kd, const double* v, double* i, double* J) const noexcept {
        switch (kd) {
            case NlKind::Diode: {
                const double is = p[0], inv = p[2]; // p[2] = 1/(n·Vt)
                const double gmin = kGmin * (1.0 - p[3]); // p[3] = 1 − 1/m on a series string of m junctions, else 0
                double d;
                const double e = limexp(v[0] * inv, d);
                i[0] = is * (e - 1.0) + gmin * v[0];
                J[0] = is * inv * d + gmin;
                return;
            }
            case NlKind::DiodePair: {
                // One exponential: the reverse diode's term is the reciprocal of the forward one.
                const double is = p[0], inv = p[2];
                const double x = std::abs(v[0]) * inv;
                double d1;
                const double e1 = limexp(x, d1);
                const double e2 = x < 40.0 ? 1.0 / e1 : 0.0;
                const double sgn = v[0] < 0 ? -1.0 : 1.0;
                i[0] = sgn * is * (e1 - e2) + kGmin * v[0];
                J[0] = is * inv * (d1 + e2) + kGmin;
                return;
            }
            case NlKind::Bjt: {
                // Ebers–Moll transport. Ports: 0 = b→e (controls vbe), 1 = b→c (controls vbc).
                const double s = pnp ? -1.0 : 1.0;
                const double vbe = s * v[0], vbc = s * v[1];
                // p: is, bf, br, 1/bf, 1/br, is/Vt
                const double is = p[0], ibf = p[3], ibr = p[4], isvt = p[5];
                constexpr double ivt = 1.0 / kVt;
                double df, dr;
                const double ef = limexp(vbe * ivt, df), er = limexp(vbc * ivt, dr);
                const double ict = is * (ef - er);
                const double ibe = is * ibf * (ef - 1.0), ibc = is * ibr * (er - 1.0);
                const double gf = isvt * df, gr = isvt * dr;
                // Gummel-Poon extensions (SPICE names; all off by default): Early voltage VAF through the base charge
                // qb = 1/(1 − vbc/VAF), and the non-ideal base currents ISE/NE (b-e recombination: low-current β
                // droop) and ISC/NC (b-c leakage: with IS/BR it is ICBO, which β multiplies into germanium's ICEO).
                const double ivaf = p[10], q = 1.0 - vbc * ivaf;
                const double icc = ict * q;
                double gbe = 0, gbc = 0, ile = 0, ilc = 0;
                if (p[6] > 0) {
                    double dl;
                    ile = p[6] * (limexp(vbe * p[7], dl) - 1.0);
                    gbe = p[6] * p[7] * dl;
                }
                if (p[8] > 0) {
                    double dl;
                    ilc = p[8] * (limexp(vbc * p[9], dl) - 1.0);
                    gbc = p[8] * p[9] * dl;
                }
                // Germanium collector-base leakage ICBO (p[11], A): thermally generated, bounded current,
                // icbo·tanh(vbc/vleak) (reverse: −ICBO; it saturates within vleak, p[12]); β multiplies it into ICEO.
                if (p[11] > 0) {
                    const double t = std::tanh(vbc / p[12]);
                    ilc += p[11] * t;
                    gbc += p[11] / p[12] * (1.0 - t * t);
                }
                i[0] = s * (icc + ibe + ile + kGmin * vbe);
                i[1] = s * (-icc + ibc + ilc + kGmin * vbc);
                const double dIdf = gf * q, dIdr = -gr * q - ict * ivaf; // ∂icc/∂vbe, ∂icc/∂vbc
                J[0] = dIdf + gf * ibf + gbe + kGmin;
                J[1] = dIdr;
                J[2] = -dIdf;
                J[3] = -dIdr + gr * ibr + gbc + kGmin;
                return;
            }
            case NlKind::Ota: {
                // Op-amp input stage: i = Imax·tanh(gm·v/Imax) into the dominant-pole node (slew = Imax/C).
                const double imax = p[0], k = p[1]; // k = gm / imax
                const double z = k * v[0], az = std::abs(z);
                double t;
                if (az <= 0.03) {
                    const double z2 = z * z;
                    t = z * (1.0 - z2 * ((1.0 / 3.0) - z2 * ((2.0 / 15.0) - (17.0 / 315.0) * z2)));
                } else if (az >= 19.0) {
                    t = z >= 0.0 ? 1.0 : -1.0;
                } else {
                    t = std::tanh(z);
                }
                i[0] = imax * t;
                J[0] = imax * k * (1.0 - t * t);
                return;
            }
            case NlKind::RailClamp: {
                const double ic = p[0], vhi = p[1], vlo = p[2], vc = p[3];
                // Inside the swing both exponentials are below e^-36 (< 1e-16·ic): the clamp is open.
                if (v[0] < vhi - 36.0 * vc && v[0] > vlo + 36.0 * vc) {
                    i[0] = 0.0;
                    J[0] = 0.0;
                    return;
                }
                double d1, d2;
                const double e1 = limexp((v[0] - vhi) / vc, d1), e2 = limexp((vlo - v[0]) / vc, d2);
                i[0] = ic * (e1 - e2);
                J[0] = ic / vc * (d1 + d2);
                return;
            }
            case NlKind::VacDiode: {
                // Vacuum rectifier: Child–Langmuir i = K·s^1.5, s = knee·softplus(v/knee) (perveance K).
                const double k = p[0], kn = p[1];
                double sg;
                const double s = kn * softplus(v[0] / kn, sg), rs = std::sqrt(s);
                i[0] = k * s * rs + kGmin * v[0];
                J[0] = 1.5 * k * rs * sg + kGmin;
                return;
            }
            case NlKind::Magnetizing: {
                // Core magnetizing law (transformer): MMF i = (λsat/Lm)·sinh(λ/λsat); incremental L = Lm/cosh(λ/λsat).
                const double lm = p[0], ls = p[1];
                const double x = std::clamp(v[0] / ls, -40.0, 40.0);
                i[0] = ls / lm * std::sinh(x) - dcLinear * v[0];
                J[0] = std::cosh(x) / lm - dcLinear;
                // Hysteresis (p[2] = coercive MMF hc, p[3] = play width w): a Prandtl–Ishlinskii sum of three smooth
                // play operators (widths w/3, 2w/3, w; weight hc/3 each). Inside a play the operator is reversible
                // (extra incremental permeance); past it the play centre follows λ, tracing a loop whose area is the
                // per-cycle core loss. The play centres advance once per converged sample (Circuit), never during
                // Newton, so the law stays a smooth function of λ within a sample. Off at DC (dcLinear set).
                if (p[2] > 0 && dcLinear == 0.0)
                    for (int j = 0; j < 3; ++j) {
                        const double r = p[3] * (j + 1) / 3.0;
                        const double z = (v[0] - hs[j]) / r, z8 = (z * z) * (z * z) * (z * z) * (z * z);
                        const double q = 1.0 / std::sqrt(std::sqrt(std::sqrt(1.0 + z8))); // (1+z^8)^(-1/8)
                        i[0] += p[2] / 3.0 * z * q;
                        J[0] += p[2] / 3.0 * q / (1.0 + z8) / r; // d/dz [z(1+z^8)^(-1/8)] = (1+z^8)^(-9/8)
                    }
                return;
            }
            case NlKind::Neon:
            case NlKind::LightDrive: {
                // Neon lamp: i = sgn(v)·(s/Rs)·softplus((|v| − Vb)/s). LightDrive injects k·|i_neon| into the light node.
                const double vb = p[0], rs = p[1], sm = p[2], k = kd == NlKind::LightDrive ? p[3] : 1.0;
                const double a = std::abs(v[0]);
                double sg;
                const double sp = softplus((a - vb) / sm, sg);
                const double mag = sm / rs * sp, dmag = sg / rs;
                if (kd == NlKind::Neon) {
                    i[0] = (v[0] < 0 ? -mag : mag) + kGmin * v[0];
                    J[0] = dmag + kGmin;
                } else {
                    i[0] = k * mag;
                    J[0] = k * dmag * (v[0] < 0 ? -1.0 : 1.0);
                }
                return;
            }
            case NlKind::Ldr: {
                // Photoresistor: i = G(λ)·v, G = 1/Rdark + max(λ,0)^γ/Ron. Port 1 senses the light node, carries no current.
                const double rdark = p[0], ron = p[1], gam = p[2];
                constexpr double eps = 1e-4; // regularised power law: finite slope at λ = 0 (same in the SPICE export)
                const double lam = std::max(v[1], 0.0);
                const double G = 1.0 / rdark + (std::pow(lam + eps, gam) - std::pow(eps, gam)) / ron;
                const double dG = v[1] > 0 ? gam * std::pow(lam + eps, gam - 1.0) / ron : 0.0;
                i[0] = G * v[0];
                i[1] = 0.0;
                J[0] = G;
                J[1] = dG * v[0];
                J[2] = 0.0;
                J[3] = 0.0;
                return;
            }
            case NlKind::OtaHalf: {
                // Real OTA (CA3080 / LM13700), one output half (docs/CIRCUIT_ENGINE.md §10.12, DEVICES.md §1.3).
                // Ports: 0 = vd (inp − inn; optional input base current), 1 = v_abc (abc − V−, senses only: the abc
                // junction itself is a Diode device), 2 = output headroom u (V+ − out for the sourcing half, out − V−
                // for the sinking half; injects the output current). i = IABC(v_abc)·σ(±vd/Vt)·c(u)·(1 + u/VA) with
                // IABC = IS·mirror·(e^{v_abc/nVt} − 1) and compliance c(u) = ½[1 + tanh((u − usat)/w)]. The two halves'
                // difference is IABC·tanh(vd/2Vt) away from the rails.
                // p: IS·mirror, 1/(n·Vt), sign (±1), 1/Vt, usat, 1/w, 1/VA, 1/bf
                double dex;
                const double ex = limexp(v[1] * p[1], dex);
                const double iabc = p[0] * (ex - 1.0), diabc = p[0] * p[1] * dex;
                const double x = p[2] * v[0] * p[3], ax = std::abs(x);
                double s, ds;
                if (ax >= 38.0) {
                    s = x >= 0.0 ? 1.0 : 0.0;
                    ds = 0.0;
                } else {
                    const double em = std::exp(-ax);
                    const double s0 = 1.0 / (1.0 + em);
                    s = x >= 0.0 ? s0 : em * s0;
                    ds = s0 * (em * s0) * p[2] * p[3];
                }
                const double zc = (v[2] - p[4]) * p[5];
                double c = 1.0, dc = 0.0;
                if (zc < 19.0) {
                    const double th = zc <= -19.0 ? -1.0 : std::tanh(zc);
                    c = 0.5 * (1.0 + th);
                    dc = 0.5 * p[5] * (1.0 - th * th);
                }
                const double e = 1.0 + v[2] * p[6];
                const double ce = c * e;
                i[0] = iabc * s * p[7];
                i[1] = 0.0;
                i[2] = iabc * s * ce;
                J[0] = iabc * ds * p[7];
                J[1] = diabc * s * p[7];
                J[2] = 0.0;
                J[3] = J[4] = J[5] = 0.0;
                J[6] = iabc * ds * ce;
                J[7] = diabc * s * ce;
                J[8] = iabc * s * (dc * e + c * p[6]);
                return;
            }
            case NlKind::LampTh: {
                // Incandescent lamp with a thermal state (DEVICES.md §4.2). θ = T/T_hot (state node, 1 F); R = R_hot·θ^α.
                // Ports: 0 = lamp terminals (current v/R), 1 = θ (the current LEAVING θ is −dθ/dt =
                // −[P/P_rated − (θ⁴ − θa⁴)/(1 − θa⁴)]/τ0, so an exact linear split can anchor θ at DC), 2 = light node
                // (injects G_L·exp(−b(1/θ − 1)) into it, Wien form).
                // p: 1/R_hot, α, θa, 1/τ0, 1/P_rated, b = Tλ/T_hot, G_L, 1/(1 − θa⁴)
                const double th = std::max(v[1], 0.02);
                // Below the clamp the slopes are taken at the clamp (not zero), so Newton from θ = 0 still climbs.
                const double g = p[0] * std::pow(th, -p[1]), dg = -p[1] * g / th;
                const double vv = v[0];
                double dph;
                const double ph = limexp(-p[5] * (1.0 / th - 1.0), dph);
                const double t3 = th * th * th;
                i[0] = g * vv + kGmin * vv;
                i[1] = -p[3] * (vv * vv * g * p[4] - (t3 * th - p[2] * p[2] * p[2] * p[2]) * p[7]); // leaves θ (see port note)
                i[2] = p[6] * ph;
                J[0] = g + kGmin;
                J[1] = vv * dg;
                J[2] = 0.0;
                J[3] = -p[3] * 2.0 * vv * g * p[4];
                J[4] = -p[3] * (vv * vv * dg * p[4] - 4.0 * t3 * p[7]);
                J[5] = 0.0;
                J[6] = 0.0;
                J[7] = p[6] * dph * p[5] / (th * th);
                J[8] = 0.0;
                return;
            }
            case NlKind::CdsRate: {
                // CdS cell carrier state n (DEVICES.md §4.2): dn/dt = kr/(1 + mr·h)·(c·λ − n|n|) − n/τl. Bimolecular:
                // attack 1/(2·kr·n) is faster when bright; release from n0 is n0/(1 + kr·n0·t) (fast, then slow).
                // Ports: 0 = light λ (senses), 1 = n (current leaves n: −dn/dt, so the exact linear split applies; the
                // linear leak −n/τl is a resistor on the node), 2 = history h (senses; only when nports == 3).
                // p: kr, couple, (unused), mr
                const double lam = std::max(v[0], 0.0), nv = v[1], hp = nports == 3 ? std::max(v[2], 0.0) : 0.0;
                const double den = 1.0 / (1.0 + p[3] * hp), k = p[0] * den;
                const double drive = p[1] * lam - nv * std::abs(nv);
                const int np = nports;
                for (int q = 0; q < np * np; ++q) J[q] = 0.0;
                i[0] = 0.0;
                i[1] = -k * drive;
                J[np + 0] = v[0] > 0 ? -k * p[1] : 0.0;
                J[np + 1] = 2.0 * k * std::abs(nv);
                if (np == 3) {
                    i[2] = 0.0;
                    J[np + 2] = v[2] > 0 ? p[0] * p[3] * den * den * drive : 0.0;
                }
                return;
            }
            case NlKind::CdsCell: {
                // CdS cell conductance: G = 1/Rdark + ((n+ε)^{2γ} − ε^{2γ})/Ron / (1 + mh·h). The steady state n² = c·λ makes
                // the static law G ∝ (c·λ)^γ, as the single-node ldr. Ports: 0 = cell terminals (injects G·v), 1 = n,
                // 2 = h (sense; h only when nports == 3). p: 1/Rdark, 1/Ron, 2γ, mh
                constexpr double eps = 1e-4;
                const double nv = std::max(v[1], 0.0), hp = nports == 3 ? std::max(v[2], 0.0) : 0.0;
                const double pw = std::pow(nv + eps, p[2]);
                const double P = pw - std::pow(eps, p[2]), dP = v[1] > 0 ? p[2] * pw / (nv + eps) : 0.0;
                const double H = 1.0 / (1.0 + p[3] * hp);
                const double G = p[0] + P * p[1] * H;
                const int np = nports;
                for (int q = 0; q < np * np; ++q) J[q] = 0.0;
                i[0] = G * v[0];
                i[1] = 0.0;
                J[0] = G;
                J[1] = v[0] * dP * p[1] * H;
                if (np == 3) {
                    i[2] = 0.0;
                    J[2] = v[2] > 0 ? -v[0] * P * p[1] * p[3] * H * H : 0.0;
                }
                return;
            }
            case NlKind::DiodeLight: {
                // LED emission: the junction's Shockley current (same IS, n as its Diode device) scaled into the light
                // node: i = k·IS·(e^{v/nVt} − 1), k = G_L/iref. Port 0 senses the junction, injects 0 → light.
                double d;
                const double e = limexp(v[0] * p[2], d);
                i[0] = p[3] * p[0] * (e - 1.0);
                J[0] = p[3] * p[0] * p[2] * d;
                return;
            }
            case NlKind::ElLight: {
                // Electroluminescent panel light (T4B; DEVICES.md §4.2): Φ = k·|i|·exp(−b/√(|v_Cp| + vε)), the
                // displacement current through the panel (sensed across its electrode resistance Rs) weighted by the
                // Alfrey–Taylor field activation, injected into the light node. Ports: 0 = Rs voltage (senses),
                // 1 = Cp voltage (senses), 2 = light node (0 → light). p: k·G_L/Rs, b, vε, δ (|i| rounding, volts on Rs)
                const double vr = v[0], r = std::sqrt(vr * vr + p[3] * p[3]);
                const double ac = std::sqrt(v[1] * v[1] + p[3] * p[3]) + p[2], sq = std::sqrt(ac);
                const double h = std::exp(-p[1] / sq), dh = h * p[1] / (2.0 * sq * ac) * (v[1] / (ac - p[2]));
                for (int q = 0; q < 9; ++q) J[q] = 0.0;
                i[0] = i[1] = 0.0;
                i[2] = p[0] * r * h;
                J[6] = p[0] * vr / r * h;
                J[7] = p[0] * r * dh;
                return;
            }
            case NlKind::AbsI: {
                // Compander full-wave rectifier (NE570 Fig. 9/10): the input current v/R1 into the VREF summing node,
                // rectified, charges the CRECT pin: i = (sqrt(v² + δ²) − δ)/R1 injected into CRECT. p: 1/R1, δ
                const double r = std::sqrt(v[0] * v[0] + p[1] * p[1]);
                i[0] = (r - p[1]) * p[0];
                J[0] = v[0] / r * p[0];
                return;
            }
            case NlKind::GainCell: {
                // Compander ΔG cell (NE570): IOUT = IG·tanh(IIN/I1) into the op-amp summing node, IIN = v0/R2 (ΔG input
                // above VREF), IG = 2·max(v1, 0)/R5 (the averaged rectifier current through R5, mirrored ×2). The
                // translinear cell is exact (IOUT = IIN·IG/I1) up to |IIN| = I1; tanh is the assumed soft limit.
                // Ports: 0 = ΔG input (senses), 1 = R5 voltage (senses), 2 = injection into INV IN. p: 2/R5, 1/R2, 1/I1
                const double ig = std::max(v[1], 0.0) * p[0], dig = v[1] > 0 ? p[0] : 0.0;
                const double x = v[0] * p[1] * p[2], ax = std::abs(x);
                double th;
                if (ax <= 0.03) {
                    const double x2 = x * x;
                    th = x * (1.0 - x2 * ((1.0 / 3.0) - x2 * ((2.0 / 15.0) - (17.0 / 315.0) * x2)));
                } else if (ax >= 19.0) {
                    th = x >= 0.0 ? 1.0 : -1.0;
                } else {
                    th = std::tanh(x);
                }
                for (int q = 0; q < 9; ++q) J[q] = 0.0;
                i[0] = i[1] = 0.0;
                i[2] = ig * th;
                J[6] = ig * (1.0 - th * th) * p[1] * p[2];
                J[7] = dig * th;
                return;
            }
            case NlKind::Ad536: {
                // AD536A rms-to-DC converter (datasheet Fig. 6; docs §10.17). Ports: 0 = VIN − COM (senses; the absolute-value
                // stage's current I1 = |VIN|/R4), 1 = +VS − CAV (senses I3 = v/R1, the current mirror's averaged input
                // across R1; injects the squarer / divider current I4 = I1²/I3 from CAV to −VS), 2 = the dB node (1 Ω to
                // COM; injects the log of I3 against IREF from COM into it). The external CAV and R1 average I4, so I3
                // settles at I1 rms (τ = R1·CAV, 25 ms/µF). I1² is computed as VIN²/R4² (the rectifier's sign drops out of
                // the square), plus the input floor i0²; I3 is kept positive smoothly (scale δ); I4 has an optional soft
                // ceiling (crest factor). dB node: −k·ln(I3/IREF), k = 20·(mV/dB)/ln10.
                // p: 1/R4², i0², 1/R1, δ, I4max (0 = off), k, IREF
                const double i3 = v[1] * p[2], r = std::sqrt(i3 * i3 + p[3] * p[3]);
                const double i3e = 0.5 * (i3 + r), di3e = 0.5 * (1.0 + i3 / r) * p[2]; // ∂I3e/∂v1
                const double S = v[0] * v[0] * p[0] + p[1];
                const double raw = S / i3e;
                double g = 1.0, gp = 1.0;
                if (p[4] > 0) {
                    g = 1.0 / (1.0 + raw / p[4]);
                    gp = g * g; // d(raw·g)/d raw
                }
                for (int q = 0; q < 9; ++q) J[q] = 0.0;
                i[0] = 0.0;
                i[1] = raw * g;
                i[2] = -p[5] * std::log(i3e / p[6]);
                J[3] = gp * 2.0 * v[0] * p[0] / i3e;
                J[4] = -gp * raw / i3e * di3e;
                J[7] = -p[5] / i3e * di3e;
                return;
            }
            case NlKind::Speaker: {
                // Electro-mechanical loudspeaker (docs §10.21; ModelLibrary.hpp speakerDerived). Ports (all sensed against
                // ground except the coil): 0 = x, excursion in mm (injects the force residual out of the velocity node u),
                // 1 = u, cone velocity in m/s (injects the EMF residual out of the flux node ψ), 2 = ψ = flux / Le0 in A
                // (injects the coil-current residual from the coil node to pin −). The compiler stamps the rest-point
                // linear parts (Bl0 both ways, Kms0, i = ψ) as primitives; this law is the exact remainder:
                //   i = ψ / r(x),  r = Le(x)/Le0 = 1 + lx1·xc + lx2·xc²,  xc = clamp(x, xlo, xhi)
                //   Bl(x) = Bl0·(1 + b1·xc + … + b4·xc⁴),  F_k = K0·x·(1 + k1·x + k2·x² + k4·x⁴)  (K0 in N/mm)
                //   F_rel = ½·i²·dLe/dx (the reluctance force; zero outside the clamp, where Le is held)
                //   i0 = F_k − K0·x − (Bl·i − Bl0·ψ) − F_rel,  i1 = (Bl − Bl0)·u + ΔRe·i,  i2 = i − ψ
                // ΔRe (hs[0]) is the voice coil's thermal rise, integrated after each sample (Circuit::advanceSpeakers).
                // p: Bl0, b1, b2, b3, b4, xlo, xhi, K0, k1, k2, k4, lx1, lx2, 500·Le0 (½·Le0 per mm → per m)
                const double x = v[0], u = v[1], psi = v[2];
                const bool in = x > p[5] && x < p[6];
                const double xc = in ? x : (x <= p[5] ? p[5] : p[6]);
                const double blr = 1.0 + xc * (p[1] + xc * (p[2] + xc * (p[3] + xc * p[4])));
                const double bl = p[0] * blr, dbl = in ? p[0] * (p[1] + xc * (2.0 * p[2] + xc * (3.0 * p[3] + xc * 4.0 * p[4]))) : 0.0;
                const double r = 1.0 + xc * (p[11] + xc * p[12]), dr = in ? p[11] + 2.0 * xc * p[12] : 0.0, d2r = in ? 2.0 * p[12] : 0.0;
                const double ir = 1.0 / r, cur = psi * ir, diPsi = ir, diX = -cur * dr * ir;
                const double x2 = x * x;
                const double fk = p[7] * x * (1.0 + x * (p[8] + x * (p[9] + x2 * p[10])));
                const double dfk = p[7] * (1.0 + x * (2.0 * p[8] + x * (3.0 * p[9] + x2 * 5.0 * p[10])));
                const double fr = p[13] * dr * cur * cur;
                const double dfrX = p[13] * (d2r * cur * cur + 2.0 * dr * cur * diX), dfrPsi = p[13] * dr * 2.0 * cur * diPsi;
                const double dre = hs[0];
                i[0] = fk - p[7] * x - (bl * cur - p[0] * psi) - fr;
                i[1] = (bl - p[0]) * u + dre * cur;
                i[2] = cur - psi;
                J[0] = dfk - p[7] - (dbl * cur + bl * diX) - dfrX;
                J[1] = 0.0;
                J[2] = -(bl * diPsi - p[0]) - dfrPsi;
                J[3] = dbl * u + dre * diX;
                J[4] = bl - p[0];
                J[5] = dre * diPsi;
                J[6] = diX;
                J[7] = 0.0;
                J[8] = diPsi - 1.0;
                return;
            }
            case NlKind::Zener: zenerLaw(p, v[0], i[0], J[0]); return;
            case NlKind::ZenerPair: {
                // Two zener junctions in series with opposite polarity and nothing else at their midpoint (a back-to-back
                // clamp), solved as one two-terminal port (Circuit::compile; docs §10.29). p[0..5]: the first junction,
                // p[6..11]: the second, each as NlKind::Zener's p. With u the first junction's voltage, KCL at the
                // eliminated midpoint is f(u) = I1(u) + I2(u + v) = 0, and the port current (into the first junction's
                // outer end) is i = I2(u + v). f rises monotonically (each law has its gmin slope), with the root in
                // [min(0, −v), max(0, −v)]: a safeguarded Newton from the last solution (ws) solves it to rounding.
                // di/dv = g1·g2/(g1 + g2), the series slope at the solution: the composite is the series pair exactly.
                const double vv = v[0];
                double lo = std::min(0.0, -vv), hi = std::max(0.0, -vv);
                double u = std::clamp(ws, lo, hi), i1, g1, i2, g2;
                for (int it = 0; it < 200; ++it) {
                    zenerLaw(p, u, i1, g1);
                    zenerLaw(p + 6, u + vv, i2, g2);
                    const double f = i1 + i2;
                    if (f > 0) hi = u;
                    else if (f < 0) lo = u;
                    else break;
                    double un = u - f / (g1 + g2);
                    if (!(un > lo && un < hi)) un = 0.5 * (lo + hi); // outside the bracket: bisect
                    const double du = un - u;
                    u = un;
                    if (std::abs(du) <= 1e-15 * (std::abs(u) + std::abs(vv)) + 1e-300 || hi - lo <= 1e-15 * (std::abs(lo) + std::abs(hi))) {
                        zenerLaw(p, u, i1, g1);
                        zenerLaw(p + 6, u + vv, i2, g2);
                        break;
                    }
                }
                ws = u;
                i[0] = 0.5 * (i2 - i1); // = I2 = −I1 at the root (their mean: the residual split evenly)
                J[0] = g1 * g2 / (g1 + g2);
                return;
            }
            case NlKind::OutLim: {
                // Op-amp output current limit (model "ilim"): i = ilim·tanh(v/(ilim·rout)) from the internal output
                // source to the pin. p: ilim, 1/(ilim·rout)
                const double z = v[0] * p[1];
                const double t = std::abs(z) >= 19.0 ? (z > 0 ? 1.0 : -1.0) : std::tanh(z);
                i[0] = p[0] * t;
                J[0] = p[0] * p[1] * (1.0 - t * t);
                return;
            }
            case NlKind::CmosOut: {
                // Buffered CMOS output stage (4000B; §10.14). Ports: 0 = λ (the cell's normalised drive, senses), 1 = VDD − out
                // (injects VDD → out), 2 = out − VSS (injects out → VSS). The open-circuit level is VSS + λ·(VDD − VSS), exactly
                // rail to rail and linear in λ so a band-limited λ edge stays band-limited; the current toward it is the
                // full-drive channel law of the drop Δ = λ·v1 − (1 − λ)·v2 (symmetric source / sink), split smoothly between
                // the rails: i_VDD = (i + √(i² + ε²))/2, i_VSS = (√(i² + ε²) − i)/2 (ε/2 = quiescent current).
                // p: β, Vt, θ, λclm, κ, ε
                const double lam = v[0], v1 = v[1], v2 = v[2], vs = v1 + v2;
                const double dl = lam * v1 - (1.0 - lam) * v2, ad = std::abs(dl), sgn = dl < 0 ? -1.0 : 1.0;
                double gVgs, gVds;
                const double g = cmosId(vs, ad, p, gVgs, gVds);
                const double cur = sgn * g;
                const double dCdD = gVds, dCdVs = sgn * gVgs; // ∂i/∂Δ, ∂i/∂(VDD − VSS)
                const double r = std::sqrt(cur * cur + p[5] * p[5]), a1 = 0.5 * (1.0 + cur / r), a2 = 0.5 * (cur / r - 1.0);
                const double di[3] = {dCdD * vs, dCdD * lam + dCdVs, -dCdD * (1.0 - lam) + dCdVs};
                i[0] = 0.0;
                i[1] = 0.5 * (r + cur);
                i[2] = 0.5 * (r - cur);
                for (int k = 0; k < 3; ++k) {
                    J[k] = 0.0;
                    J[3 + k] = a1 * di[k];
                    J[6 + k] = a2 * di[k];
                }
                return;
            }
            case NlKind::MosHalf: {
                // One channel of an unbuffered CMOS inverter (4069UB / 4049UB): ports 0 = gate drive vgs (senses; for the
                // p-channel VDD − in), 1 = vds (VDD − out for p, out − VSS for n; injects the drain current). Source and drain
                // swap for vds < 0. p: β, Vt, θ, λ, κ
                double dg, dd;
                if (v[1] >= 0.0) {
                    i[1] = cmosId(v[0], v[1], p, dg, dd);
                    J[2] = dg;
                    J[3] = dd;
                } else {
                    i[1] = -cmosId(v[0] - v[1], -v[1], p, dg, dd);
                    J[2] = -dg;
                    J[3] = dg + dd;
                }
                i[0] = 0.0;
                J[0] = J[1] = 0.0;
                return;
            }
            case NlKind::RmsSq: {
                // RMS detector (THAT 2252 / dbx RMS; docs/CIRCUIT_ENGINE.md "vca and rms"): the rectifier and the 2·log
                // stage reduce to the squared input current, which the log-domain filter (RmsLog) antilogs. Iin is read
                // from the internal transimpedance node (x = −r0·Iin). The positive half passes the Q1/Q2 mirror,
                // ratio m = exp((v_sym − v_in − vsym)/VT) (1 when trimmed); the input-bias floor adds iflr².
                // i0 = (m²·max(Iin,0)² + min(Iin,0)² + iflr²)/Iref², injected into a 1 Ω node (so its volts are the
                // normalised mean-square drive). Ports: 0 = x − gnd (senses, injects 0 → sq), 1 = sym − in (senses).
                // p: 1/r0, vsym, 1/VT, iflr², 1/Iref²
                const double x = -v[0] * p[0];
                double m2 = 1.0, dm2 = 0.0;
                if (x > 0) {
                    double d;
                    m2 = limexp(2.0 * (v[1] - p[1]) * p[2], d);
                    dm2 = 2.0 * p[2] * d;
                }
                i[0] = (m2 * x * x + p[3]) * p[4];
                i[1] = 0.0;
                J[0] = -2.0 * m2 * x * p[0] * p[4];
                J[1] = dm2 * x * x * p[4];
                J[2] = J[3] = 0.0;
                return;
            }
            case NlKind::RmsLog: {
                // RMS detector log-domain filter (2252 Q6 into the CAP pin): the emitter current is the antilog of
                // Vlog − V6, i = sq·Iref²·k²/IBIAS·exp(y/VT) with y = (V_gnd + vofs) − V_cap, sourced from V+ into CAP.
                // The external CT and the timing current IT drawn from CAP make CT·dV/dt = i − IT, so exp(−y/VT) is the
                // RC average of Iin² with τ = CT·VT/IT, and the steady state is −y = 2VT·ln(Irms/Iin0), Iin0 =
                // √(IBIAS·IT)/k (datasheet). IBIAS is the pin-2 diode-string current, read from its junction voltage.
                // Ports: 0 = sq (senses), 1 = y (ko − cap, ko = gnd + vofs; injects V+ → cap), 2 = ibias − V− (senses).
                // p: Iref²·k², (unused), 1/VT, IS_bias, 1/(nb·VT)
                double db, dd;
                const double ib = p[3] * limexp(v[2] * p[4], db);
                const double sq = std::max(v[0], 0.0);
                const double e = limexp(v[1] * p[2], dd);
                const double k = p[0] / std::max(ib, 1e-30);
                const double cur = sq * k * e;
                for (int q = 0; q < 9; ++q) J[q] = 0.0;
                i[0] = i[2] = 0.0;
                i[1] = cur;
                J[3] = v[0] > 0 ? k * e : 0.0;
                J[4] = sq * k * dd * p[2];
                J[5] = ib > 1e-30 ? -cur * (p[3] * db * p[4]) / ib : 0.0;
                return;
            }
            case NlKind::OutHalf: {
                // Op-amp push-pull output half (supply pins on circuit nodes): i = g·(σ·v + sqrt(v² + δ²))/2 with
                // v = x − out. The sourcing half (σ = +1) injects V+ → out, the sinking half (σ = −1) out → V−; their
                // difference is g·v (open-loop output resistance 1/g), their overlap at v = 0 is the quiescent current.
                const double g = p[0], sg = p[1], d2 = p[2];
                const double r = std::sqrt(v[0] * v[0] + d2);
                i[0] = 0.5 * g * (sg * v[0] + r);
                J[0] = 0.5 * g * (sg + v[0] / r);
                return;
            }
            case NlKind::Jfet: {
                // SPICE JFET level 1 (Shichman-Hodges, B = 1), drain/source symmetric. Ports: 0 = g→s (vgs; the gate
                // current igs + igd), 1 = d→s (vds; the drain current id − igd). p: vto, beta, lambda, is, 1/(n·Vt).
                // P-channel: every voltage and current negated (vto keeps the SPICE sign, negative for depletion).
                const double sg = pnp ? -1.0 : 1.0;
                const double vgs = sg * v[0], vds = sg * v[1], vgd = vgs - vds;
                const double vto = p[0], beta = p[1], lam = p[2], is = p[3], inv = p[4];
                double d1, d2;
                const double egs = limexp(vgs * inv, d1), egd = limexp(vgd * inv, d2);
                const double igs = is * (egs - 1.0) + kGmin * vgs, igd = is * (egd - 1.0) + kGmin * vgd;
                const double ggs = is * inv * d1 + kGmin, ggd = is * inv * d2 + kGmin;
                // channel: normal mode (vds >= 0) on vgs; inverse mode on vgd with the terminals swapped
                const bool inverse = vds < 0;
                double vgst = (inverse ? vgd : vgs) - vto, dvg = 1.0;
                const double u = inverse ? -vds : vds;
                // Optional subthreshold smoothing (model "ksub" = κ, DEVICES.md §5.2): vgst → κ·softplus(vgst/κ), an
                // exponential tail below pinch-off instead of level 1's hard corner. κ = 0: exact SPICE level 1.
                if (p[5] > 0) vgst = p[5] * softplus(vgst / p[5], dvg);
                double id = 0, gm = 0, gds = 0; // ∂id/∂vgst, ∂id/∂u in the active orientation
                if (vgst > 0) {
                    const double bp = beta * (1.0 + lam * u);
                    if (u < vgst) {
                        id = bp * u * (2.0 * vgst - u);
                        gm = 2.0 * bp * u;
                        gds = beta * lam * u * (2.0 * vgst - u) + 2.0 * bp * (vgst - u);
                    } else {
                        id = bp * vgst * vgst;
                        gm = 2.0 * bp * vgst;
                        gds = beta * lam * vgst * vgst;
                    }
                    gm *= dvg;
                }
                double idn, didvgs, didvds; // drain current (d→s) and its derivatives in (vgs, vds)
                if (!inverse) {
                    idn = id;
                    didvgs = gm;
                    didvds = gds;
                } else { // id(vgs, vds) = −f(vgs − vds − vto, −vds)
                    idn = -id;
                    didvgs = -gm;
                    didvds = gm + gds;
                }
                i[0] = sg * (igs + igd);
                i[1] = sg * (idn - igd);
                J[0] = ggs + ggd;
                J[1] = -ggd;
                J[2] = didvgs - ggd;
                J[3] = didvds + ggd;
                return;
            }
            case NlKind::Pentode: {
                if (lut) {
                    if (lut->lanes) evalPentodeLut2(v, i, J); // docs §10.51: the same operations, two lanes
                    else evalPentodeLut(v, i, J);
                    return;
                }
                // Koren pentode / beam tetrode. Ports: 0 = g1→k (grid current), 1 = p→k (plate), 2 = g2→k (screen).
                const double mu = p[0], ex = p[1], kg1 = p[2], kg2 = p[3], kp = p[4], kvb = p[5], gs = p[6], gk = p[7];
                const double vg1 = v[0], vpk = v[1], vg2 = v[2];
                for (int q = 0; q < 9; ++q) J[q] = 0.0;
                double sg;
                const double s = gk * softplus(vg1 / gk, sg), rs = std::sqrt(s);
                i[0] = gs * s * rs;
                J[0] = 1.5 * gs * rs * sg;
                // Space current Q = E1^ex with Koren's smooth E1 (the same cut-off for plate and screen). The plate
                // collects (2/kg1)·Q·atan(vpk/kvb); the screen takes Q/kg2 plus what the plate fails to collect
                // below its knee, (2/kg1)·Q·(π/2 − atan): cathode current ≈ Q·(π/kg1 + 1/kg2), independent of the
                // plate voltage, as in a real pentode. (Koren's separate (vg1 + vg2/mu)^1.5 screen law goes to zero
                // at ordinary class-AB bias while the plate still conducts.)
                i[2] = 0.0;
                if (vg2 <= 1e-3) {
                    i[1] = 0.0;
                    return;
                }
                const double z = kp * (1.0 / mu + vg1 / vg2);
                double sig;
                const double sp = softplus(z, sig);
                const double e1 = vg2 / kp * sp;
                const double at = std::atan(vpk / kvb), dat = (1.0 / kvb) / (1.0 + (vpk / kvb) * (vpk / kvb));
                if (e1 <= 0.0) {
                    i[1] = 0.0;
                    return;
                }
                const double pw = std::pow(e1, ex - 1.0);
                const double Q = pw * e1, dQ1 = ex * pw * sig, dQ2 = ex * pw * (sp / kp - sig * vg1 / vg2);
                const double P = 2.0 / kg1 * Q;
                i[1] = P * at;
                J[3] = 2.0 / kg1 * dQ1 * at; // ∂Ip/∂vg1
                J[4] = P * dat;              // ∂Ip/∂vpk
                J[5] = 2.0 / kg1 * dQ2 * at; // ∂Ip/∂vg2
                const double c = 1.0 / kg2 + 2.0 / kg1 * (0.5 * M_PI - at);
                i[2] = Q * c;
                J[6] = dQ1 * c;       // ∂Ig2/∂vg1
                J[7] = -P * dat;      // ∂Ig2/∂vpk
                J[8] = dQ2 * c;       // ∂Ig2/∂vg2
                return;
            }
            case NlKind::Triode: {
                if (lut) {
                    if (lut->lanes) evalTriodeLut2(v, i, J); // docs §10.51: the same operations, two lanes
                    else evalTriodeLut(v, i, J);
                    return;
                }
                // Ports: 0 = grid g→k (controls vgk, grid current), 1 = plate p→k (controls vpk, plate current).
                const double mu = p[0], ex = p[1], kg1 = p[2], kp = p[3], kvb = p[4], gs = p[5], gk = p[6];
                const double vgk = v[0], vpk = v[1];
                // Grid: Ig = G·s^1.5, s = knee·softplus(vgk/knee)
                double sg;
                const double s = gk * softplus(vgk / gk, sg);
                const double rs = std::sqrt(s);
                i[0] = gs * s * rs;
                J[0] = 1.5 * gs * rs * sg;
                J[1] = 0.0;
                // Plate (Koren): E1 = vpk/kp·softplus(kp(1/mu + (vgk + vgo)/√(kvb+vpk²))), Ip = 2·E1^ex/kg1. vgo = p[7]:
                // contact-potential offset of the plate law (card "vgo", 0 = Koren's law exactly; the grid law keeps vgk).
                const double vgp = vgk + p[7];
                const double q2 = kvb + vpk * vpk, q = std::sqrt(q2);
                const double z = kp * (1.0 / mu + vgp / q);
                double sig;
                const double sp = softplus(z, sig);
                const double e1 = vpk / kp * sp;
                if (e1 <= 0.0) {
                    i[1] = 0.0;
                    J[2] = J[3] = 0.0;
                    return;
                }
                const double pw = std::pow(e1, ex - 1.0);
                i[1] = 2.0 / kg1 * pw * e1;
                const double dI = 2.0 / kg1 * ex * pw;
                const double de_dvg = vpk * sig / q;
                const double de_dvp = sp / kp - vpk * sig * vgp * vpk / (q2 * q);
                J[2] = dI * de_dvg;
                J[3] = dI * de_dvp;
                return;
            }
        }
    }

    // Grid-conduction law from the table (shared by triodes and pentodes): Ig = gs·(gk·softplus(vgk/gk))^1.5.
    static void gridLut(const KorenTables& T, double vgk, double& ig, double& gg) noexcept {
        if (vgk <= KorenTables::GMIN) {
            ig = 0.0;
            gg = 0.0;
        } else if (vgk >= KorenTables::GMAX) {
            double sg;
            const double s = T.gk * softplus(vgk / T.gk, sg), rs = std::sqrt(s);
            ig = T.gs * s * rs;
            gg = 1.5 * T.gs * rs * sg;
        } else {
            constexpr double step = (KorenTables::GMAX - KorenTables::GMIN) / (KorenTables::NG - 1);
            const double u = (vgk - KorenTables::GMIN) / step;
            const int k = std::min(static_cast<int>(u), KorenTables::NG - 2);
            const double t = u - k;
            const auto q = static_cast<std::size_t>(k);
            ig = KorenTables::hermite(t, step, T.g[q], T.g[q + 1], T.dg[q], T.dg[q + 1]);
            gg = KorenTables::hermite(t, step, T.dg[q], T.dg[q + 1], T.d2g[q], T.d2g[q + 1]);
        }
    }
    // h(z) = softplus(z)^ex and h'(z); false below ZMIN (h = 0 to double precision for any real ex).
    static bool hLut(const KorenTables& T, double z, double& h, double& dh) noexcept {
        if (z <= KorenTables::ZMIN) return false;
        if (z >= KorenTables::ZMAX) {
            double sig;
            const double sp = softplus(z, sig);
            h = std::pow(sp, T.ex);
            dh = T.ex * h / sp * sig;
            return true;
        }
        constexpr double zstep = (KorenTables::ZMAX - KorenTables::ZMIN) / (KorenTables::NZ - 1);
        const double u = (z - KorenTables::ZMIN) / zstep;
        const int k = std::min(static_cast<int>(u), KorenTables::NZ - 2);
        const double t = u - k;
        const auto q = static_cast<std::size_t>(k);
        h = KorenTables::hermite(t, zstep, T.h[q], T.h[q + 1], T.dh[q], T.dh[q + 1]);
        dh = KorenTables::hermite(t, zstep, T.dh[q], T.dh[q + 1], T.d2h[q], T.d2h[q + 1]);
        return true;
    }
    // A(v) = (2/kg1)·(v/kp)^ex and A'(v) for v > 0 (exact below 4 V where v^ex is not smooth, and above VMAX).
    static void aLut(const KorenTables& T, double v, double& A, double& dA) noexcept {
        if (v < 4.0 || v >= KorenTables::VMAX) {
            A = 2.0 / T.kg1 * std::pow(v / T.kp, T.ex);
            dA = A * T.ex / v;
            return;
        }
        constexpr double vstep = KorenTables::VMAX / (KorenTables::NV - 1);
        const double u = v / vstep;
        const int k = std::min(static_cast<int>(u), KorenTables::NV - 2);
        const double t = u - k;
        const auto q = static_cast<std::size_t>(k);
        A = KorenTables::hermite(t, vstep, T.a[q], T.a[q + 1], T.da[q], T.da[q + 1]);
        dA = KorenTables::hermiteD(t, vstep, T.a[q], T.a[q + 1], T.da[q], T.da[q + 1]);
    }

    // Tabulated pentode (same law as the exact branch of eval): the space current factorises as
    // Q·(2/kg1) = A(vg2)·h(z), z = kp·(1/mu + vg1/vg2), with the triode's A and h tables; the plate collection
    // atan(vpk/kvb) is a third table. Ports: 0 = g1→k, 1 = p→k, 2 = g2→k.
    void evalPentodeLut(const double* v, double* i, double* J) const noexcept {
        const KorenTables& T = *lut;
        const double vg1 = v[0], vpk = v[1], vg2 = v[2];
        for (int q = 1; q < 9; ++q) J[q] = 0.0;
        gridLut(T, vg1, i[0], J[0]);
        i[1] = i[2] = 0.0;
        if (vg2 <= 1e-3) return;
        const double inv = 1.0 / vg2;
        double h, dh;
        if (!hLut(T, T.kp * (1.0 / T.mu + vg1 * inv), h, dh)) return;
        double A, dA;
        aLut(T, vg2, A, dA);
        const double P = A * h;                                  // (2/kg1)·Q
        const double dP1 = A * dh * T.kp * inv;                  // ∂P/∂vg1
        const double dP2 = dA * h - A * dh * T.kp * vg1 * inv * inv; // ∂P/∂vg2
        double at, dat;
        if (vpk <= KorenTables::AMIN || vpk >= KorenTables::AMAX) {
            const double u = vpk / p[5];
            at = std::atan(u);
            dat = (1.0 / p[5]) / (1.0 + u * u);
        } else {
            constexpr double astep = (KorenTables::AMAX - KorenTables::AMIN) / (KorenTables::NA - 1);
            const double u = (vpk - KorenTables::AMIN) / astep;
            const int k = std::min(static_cast<int>(u), KorenTables::NA - 2);
            const double t = u - k;
            const auto q = static_cast<std::size_t>(k);
            at = KorenTables::hermite(t, astep, T.at[q], T.at[q + 1], T.dat[q], T.dat[q + 1]);
            dat = KorenTables::hermite(t, astep, T.dat[q], T.dat[q + 1], T.d2at[q], T.d2at[q + 1]);
        }
        i[1] = P * at;
        J[3] = dP1 * at;
        J[4] = P * dat;
        J[5] = dP2 * at;
        // screen: Q·(1/kg2 + (2/kg1)(π/2 − atan)) with Q = P·kg1/2
        const double c = 0.5 * T.kg1 / p[3] + (0.5 * M_PI - at);
        i[2] = P * c;
        J[6] = dP1 * c;
        J[7] = -P * dat;
        J[8] = dP2 * c;
    }

    void evalTriodeLut(const double* v, double* i, double* J) const noexcept {
        const KorenTables& T = *lut;
        const double vgk = v[0], vpk = v[1];
        gridLut(T, vgk, i[0], J[0]);
        J[1] = 0.0;
        if (vpk <= 0.0) {
            i[1] = J[2] = J[3] = 0.0;
            return;
        }
        double A, dA, C, dC;
        constexpr double vstep = KorenTables::VMAX / (KorenTables::NV - 1);
        if (vpk < 4.0 || vpk >= KorenTables::VMAX) { // v^ex is not smooth near 0: exact there
            const double q2 = T.kvb + vpk * vpk, q = std::sqrt(q2);
            A = 2.0 / T.kg1 * std::pow(vpk / T.kp, T.ex);
            dA = A * T.ex / vpk;
            C = 1.0 / q;
            dC = -vpk / (q2 * q);
        } else {
            const double u = vpk / vstep;
            const int k = std::min(static_cast<int>(u), KorenTables::NV - 2);
            const double t = u - k;
            const auto q = static_cast<std::size_t>(k);
            A = KorenTables::hermite(t, vstep, T.a[q], T.a[q + 1], T.da[q], T.da[q + 1]);
            dA = KorenTables::hermiteD(t, vstep, T.a[q], T.a[q + 1], T.da[q], T.da[q + 1]);
            C = KorenTables::hermite(t, vstep, T.c[q], T.c[q + 1], T.dc[q], T.dc[q + 1]);
            dC = KorenTables::hermiteD(t, vstep, T.c[q], T.c[q + 1], T.dc[q], T.dc[q + 1]);
        }
        const double vgp = vgk + p[7]; // contact-potential offset (card "vgo"; 0: Koren exactly)
        const double z = T.kp / T.mu + T.kp * vgp * C;
        double h, dh;
        if (z <= KorenTables::ZMIN) {
            i[1] = J[2] = J[3] = 0.0;
            return;
        }
        if (z >= KorenTables::ZMAX) {
            double sig;
            const double sp = softplus(z, sig);
            h = std::pow(sp, T.ex);
            dh = T.ex * h / sp * sig;
        } else {
            constexpr double zstep = (KorenTables::ZMAX - KorenTables::ZMIN) / (KorenTables::NZ - 1);
            const double u = (z - KorenTables::ZMIN) / zstep;
            const int k = std::min(static_cast<int>(u), KorenTables::NZ - 2);
            const double t = u - k;
            const auto q = static_cast<std::size_t>(k);
            h = KorenTables::hermite(t, zstep, T.h[q], T.h[q + 1], T.dh[q], T.dh[q + 1]);
            dh = KorenTables::hermite(t, zstep, T.dh[q], T.dh[q + 1], T.d2h[q], T.d2h[q + 1]);
        }
        i[1] = A * h;
        J[2] = A * dh * T.kp * C;
        J[3] = dA * h + A * dh * T.kp * vgp * dC;
    }

    // ── Two-lane tube laws (docs §10.51; 2026-11-14) ──
    // evalTriodeLut / evalPentodeLut with every pair of table lookups that share a knot interval evaluated as one 2-wide
    // vector: (A, C) and (A', C') on the plate-voltage tables, (Ig, gg) on the grid table, (h, h') on the z table and the
    // pentode's (atan, atan') on its plate-collection table. Each lane's expression is the scalar code's (hermiteL is
    // hermite's text on vector operands, the basis computed once on the scalar t), so the clang fmuladd contraction is the
    // same and each lane rounds exactly as the scalar code: bit-identical (bench hashes, docs §10.51). The knots come from
    // the interleaved tables (KorenTables::ac, gK, hK, atK); the out-of-table branches are the scalar code's.
    typedef double lane2 __attribute__((ext_vector_type(2)));
    [[gnu::always_inline]] static lane2 ld2(const double* q) noexcept {
        lane2 x;
        __builtin_memcpy(&x, q, sizeof x);
        return x;
    }
    [[gnu::always_inline]] static lane2 hermiteL(double t, double step, lane2 y0, lane2 y1, lane2 m0, lane2 m1) noexcept {
        const double t2 = t * t, t3 = t2 * t;
        return (2 * t3 - 3 * t2 + 1) * y0 + (t3 - 2 * t2 + t) * step * m0 + (-2 * t3 + 3 * t2) * y1 + (t3 - t2) * step * m1;
    }
    [[gnu::always_inline]] static lane2 hermiteDL(double t, double step, lane2 y0, lane2 y1, lane2 m0, lane2 m1) noexcept {
        const double t2 = t * t;
        return ((6 * t2 - 6 * t) * y0 + (3 * t2 - 4 * t + 1) * step * m0 + (-6 * t2 + 6 * t) * y1 + (3 * t2 - 2 * t) * step * m1) / step;
    }
    // (value, derivative) of a {f, f', f''} knot table at t in knot q: hermite(f, f') and hermite(f', f'') in one
    [[gnu::always_inline]] static lane2 knot3(const double* K, std::size_t q, double t, double step) noexcept {
        const double* k0 = K + 4 * q;
        return hermiteL(t, step, ld2(k0), ld2(k0 + 4), ld2(k0 + 1), ld2(k0 + 5));
    }
    [[gnu::always_inline]] static void gridLut2(const KorenTables& T, double vgk, double& ig, double& gg) noexcept {
        if (vgk <= KorenTables::GMIN || vgk >= KorenTables::GMAX) {
            gridLut(T, vgk, ig, gg);
            return;
        }
        constexpr double step = (KorenTables::GMAX - KorenTables::GMIN) / (KorenTables::NG - 1);
        const double u = (vgk - KorenTables::GMIN) / step;
        const int k = std::min(static_cast<int>(u), KorenTables::NG - 2);
        const lane2 r = knot3(T.gK.data(), static_cast<std::size_t>(k), u - k, step);
        ig = r.x;
        gg = r.y;
    }
    void evalTriodeLut2(const double* v, double* i, double* J) const noexcept {
        const KorenTables& T = *lut;
        const double vgk = v[0], vpk = v[1];
        gridLut2(T, vgk, i[0], J[0]);
        J[1] = 0.0;
        if (vpk <= 0.0) {
            i[1] = J[2] = J[3] = 0.0;
            return;
        }
        double A, dA, C, dC;
        constexpr double vstep = KorenTables::VMAX / (KorenTables::NV - 1);
        if (vpk < 4.0 || vpk >= KorenTables::VMAX) { // v^ex is not smooth near 0: exact there
            const double q2 = T.kvb + vpk * vpk, q = std::sqrt(q2);
            A = 2.0 / T.kg1 * std::pow(vpk / T.kp, T.ex);
            dA = A * T.ex / vpk;
            C = 1.0 / q;
            dC = -vpk / (q2 * q);
        } else {
            const double u = vpk / vstep;
            const int k = std::min(static_cast<int>(u), KorenTables::NV - 2);
            const double t = u - k;
            const double* k0 = T.ac.data() + 4 * static_cast<std::size_t>(k);
            const lane2 y0 = ld2(k0), m0 = ld2(k0 + 2), y1 = ld2(k0 + 4), m1 = ld2(k0 + 6);
            const lane2 ac = hermiteL(t, vstep, y0, y1, m0, m1), dac = hermiteDL(t, vstep, y0, y1, m0, m1);
            A = ac.x;
            C = ac.y;
            dA = dac.x;
            dC = dac.y;
        }
        const double vgp = vgk + p[7]; // contact-potential offset (card "vgo"; 0: Koren exactly)
        const double z = T.kp / T.mu + T.kp * vgp * C;
        double h, dh;
        if (z <= KorenTables::ZMIN) {
            i[1] = J[2] = J[3] = 0.0;
            return;
        }
        if (z >= KorenTables::ZMAX) {
            double sig;
            const double sp = softplus(z, sig);
            h = std::pow(sp, T.ex);
            dh = T.ex * h / sp * sig;
        } else {
            constexpr double zstep = (KorenTables::ZMAX - KorenTables::ZMIN) / (KorenTables::NZ - 1);
            const double u = (z - KorenTables::ZMIN) / zstep;
            const int k = std::min(static_cast<int>(u), KorenTables::NZ - 2);
            const lane2 r = knot3(T.hK.data(), static_cast<std::size_t>(k), u - k, zstep);
            h = r.x;
            dh = r.y;
        }
        i[1] = A * h;
        J[2] = A * dh * T.kp * C;
        J[3] = dA * h + A * dh * T.kp * vgp * dC;
    }
    void evalPentodeLut2(const double* v, double* i, double* J) const noexcept {
        const KorenTables& T = *lut;
        const double vg1 = v[0], vpk = v[1], vg2 = v[2];
        for (int q = 1; q < 9; ++q) J[q] = 0.0;
        gridLut2(T, vg1, i[0], J[0]);
        i[1] = i[2] = 0.0;
        if (vg2 <= 1e-3) return;
        const double inv = 1.0 / vg2;
        double h, dh;
        {
            const double z = T.kp * (1.0 / T.mu + vg1 * inv);
            if (z <= KorenTables::ZMIN) return;
            if (z >= KorenTables::ZMAX) {
                if (!hLut(T, z, h, dh)) return;
            } else {
                constexpr double zstep = (KorenTables::ZMAX - KorenTables::ZMIN) / (KorenTables::NZ - 1);
                const double u = (z - KorenTables::ZMIN) / zstep;
                const int k = std::min(static_cast<int>(u), KorenTables::NZ - 2);
                const lane2 r = knot3(T.hK.data(), static_cast<std::size_t>(k), u - k, zstep);
                h = r.x;
                dh = r.y;
            }
        }
        double A, dA;
        aLut(T, vg2, A, dA);
        const double P = A * h;                                  // (2/kg1)·Q
        const double dP1 = A * dh * T.kp * inv;                  // ∂P/∂vg1
        const double dP2 = dA * h - A * dh * T.kp * vg1 * inv * inv; // ∂P/∂vg2
        double at, dat;
        if (vpk <= KorenTables::AMIN || vpk >= KorenTables::AMAX) {
            const double u = vpk / p[5];
            at = std::atan(u);
            dat = (1.0 / p[5]) / (1.0 + u * u);
        } else {
            constexpr double astep = (KorenTables::AMAX - KorenTables::AMIN) / (KorenTables::NA - 1);
            const double u = (vpk - KorenTables::AMIN) / astep;
            const int k = std::min(static_cast<int>(u), KorenTables::NA - 2);
            const lane2 r = knot3(T.atK.data(), static_cast<std::size_t>(k), u - k, astep);
            at = r.x;
            dat = r.y;
        }
        i[1] = P * at;
        J[3] = dP1 * at;
        J[4] = P * dat;
        J[5] = dP2 * at;
        // screen: Q·(1/kg2 + (2/kg1)(π/2 − atan)) with Q = P·kg1/2
        const double c = 0.5 * T.kg1 / p[3] + (0.5 * M_PI - at);
        i[2] = P * c;
        J[6] = dP1 * c;
        J[7] = -P * dat;
        J[8] = dP2 * c;
    }
    // Two tubes of one kind in one pass (docs §10.51): the lanes are the two devices (each with its own tables and
    // constants). The divisions and the law's own arithmetic run as 2-wide vectors over the devices, the table pairs as
    // above per device; each lane's expression is evalTriodeLut2's / evalPentodeLut2's, so each device's result is
    // bit-identical. When a lane leaves a table range (cut-off, grid conduction past GMAX, low plate) both devices take
    // the one-device path. Both devices must have tables with lanes on (the caller checks).
    static void evalTriodePair(const NlDevice& d0, const NlDevice& d1, const double* v0, const double* v1, double* i0, double* i1, double* J0,
                               double* J1) noexcept {
        const KorenTables& T0 = *d0.lut;
        const KorenTables& T1 = *d1.lut;
        const lane2 vg = {v0[0], v1[0]}, vp = {v0[1], v1[1]};
        if (!(vp.x >= 4.0 && vp.x < KorenTables::VMAX && vp.y >= 4.0 && vp.y < KorenTables::VMAX)) {
            d0.evalTriodeLut2(v0, i0, J0);
            d1.evalTriodeLut2(v1, i1, J1);
            return;
        }
        // plate tables first (C decides z)
        constexpr double vstep = KorenTables::VMAX / (KorenTables::NV - 1);
        const lane2 u = vp / vstep;
        lane2 A, dA, C, dC;
        {
            const int k = std::min(static_cast<int>(u.x), KorenTables::NV - 2);
            const double t = u.x - k;
            const double* k0 = T0.ac.data() + 4 * static_cast<std::size_t>(k);
            const lane2 y0 = ld2(k0), m0 = ld2(k0 + 2), y1 = ld2(k0 + 4), m1 = ld2(k0 + 6);
            const lane2 ac = hermiteL(t, vstep, y0, y1, m0, m1), dac = hermiteDL(t, vstep, y0, y1, m0, m1);
            A.x = ac.x, C.x = ac.y, dA.x = dac.x, dC.x = dac.y;
        }
        {
            const int k = std::min(static_cast<int>(u.y), KorenTables::NV - 2);
            const double t = u.y - k;
            const double* k0 = T1.ac.data() + 4 * static_cast<std::size_t>(k);
            const lane2 y0 = ld2(k0), m0 = ld2(k0 + 2), y1 = ld2(k0 + 4), m1 = ld2(k0 + 6);
            const lane2 ac = hermiteL(t, vstep, y0, y1, m0, m1), dac = hermiteDL(t, vstep, y0, y1, m0, m1);
            A.y = ac.x, C.y = ac.y, dA.y = dac.x, dC.y = dac.y;
        }
        const lane2 kp = {T0.kp, T1.kp}, mu = {T0.mu, T1.mu};
        const lane2 vgp = vg + lane2{d0.p[7], d1.p[7]}; // contact-potential offset (card "vgo"; 0: Koren exactly)
        const lane2 z = kp / mu + kp * vgp * C;
        if (!(z.x > KorenTables::ZMIN && z.x < KorenTables::ZMAX && z.y > KorenTables::ZMIN && z.y < KorenTables::ZMAX)) {
            d0.evalTriodeLut2(v0, i0, J0);
            d1.evalTriodeLut2(v1, i1, J1);
            return;
        }
        // grid
        if (vg.x > KorenTables::GMIN && vg.x < KorenTables::GMAX && vg.y > KorenTables::GMIN && vg.y < KorenTables::GMAX) {
            constexpr double step = (KorenTables::GMAX - KorenTables::GMIN) / (KorenTables::NG - 1);
            const lane2 ug = (vg - KorenTables::GMIN) / step;
            const int k0 = std::min(static_cast<int>(ug.x), KorenTables::NG - 2), k1 = std::min(static_cast<int>(ug.y), KorenTables::NG - 2);
            const lane2 r0 = knot3(T0.gK.data(), static_cast<std::size_t>(k0), ug.x - k0, step);
            const lane2 r1 = knot3(T1.gK.data(), static_cast<std::size_t>(k1), ug.y - k1, step);
            i0[0] = r0.x, J0[0] = r0.y, i1[0] = r1.x, J1[0] = r1.y;
        } else {
            gridLut2(T0, vg.x, i0[0], J0[0]);
            gridLut2(T1, vg.y, i1[0], J1[0]);
        }
        J0[1] = J1[1] = 0.0;
        constexpr double zstep = (KorenTables::ZMAX - KorenTables::ZMIN) / (KorenTables::NZ - 1);
        const lane2 uz = (z - KorenTables::ZMIN) / zstep;
        const int q0 = std::min(static_cast<int>(uz.x), KorenTables::NZ - 2), q1 = std::min(static_cast<int>(uz.y), KorenTables::NZ - 2);
        const lane2 r0 = knot3(T0.hK.data(), static_cast<std::size_t>(q0), uz.x - q0, zstep);
        const lane2 r1 = knot3(T1.hK.data(), static_cast<std::size_t>(q1), uz.y - q1, zstep);
        const lane2 h = {r0.x, r1.x}, dh = {r0.y, r1.y};
        const lane2 ip = A * h, j2 = A * dh * kp * C, j3 = dA * h + A * dh * kp * vgp * dC;
        i0[1] = ip.x, i1[1] = ip.y, J0[2] = j2.x, J1[2] = j2.y, J0[3] = j3.x, J1[3] = j3.y;
    }
    static void evalPentodePair(const NlDevice& d0, const NlDevice& d1, const double* v0, const double* v1, double* i0, double* i1, double* J0,
                                double* J1) noexcept {
        const KorenTables& T0 = *d0.lut;
        const KorenTables& T1 = *d1.lut;
        const lane2 vg1 = {v0[0], v1[0]}, vpk = {v0[1], v1[1]}, vg2 = {v0[2], v1[2]};
        auto inA = [](double x) { return x >= 4.0 && x < KorenTables::VMAX; }; // vg2: aLut's table (and > 1e-3)
        auto inP = [](double x) { return x > KorenTables::AMIN && x < KorenTables::AMAX; };
        auto inG = [](double x) { return x > KorenTables::GMIN && x < KorenTables::GMAX; };
        if (!(inA(vg2.x) && inA(vg2.y) && inP(vpk.x) && inP(vpk.y) && inG(vg1.x) && inG(vg1.y))) {
            d0.evalPentodeLut2(v0, i0, J0);
            d1.evalPentodeLut2(v1, i1, J1);
            return;
        }
        const lane2 inv = 1.0 / vg2;
        const lane2 kp = {T0.kp, T1.kp}, mu = {T0.mu, T1.mu};
        const lane2 z = kp * (1.0 / mu + vg1 * inv);
        if (!(z.x > KorenTables::ZMIN && z.x < KorenTables::ZMAX && z.y > KorenTables::ZMIN && z.y < KorenTables::ZMAX)) {
            d0.evalPentodeLut2(v0, i0, J0);
            d1.evalPentodeLut2(v1, i1, J1);
            return;
        }
        for (int q = 1; q < 9; ++q) J0[q] = J1[q] = 0.0;
        {
            constexpr double step = (KorenTables::GMAX - KorenTables::GMIN) / (KorenTables::NG - 1);
            const lane2 ug = (vg1 - KorenTables::GMIN) / step;
            const int k0 = std::min(static_cast<int>(ug.x), KorenTables::NG - 2), k1 = std::min(static_cast<int>(ug.y), KorenTables::NG - 2);
            const lane2 r0 = knot3(T0.gK.data(), static_cast<std::size_t>(k0), ug.x - k0, step);
            const lane2 r1 = knot3(T1.gK.data(), static_cast<std::size_t>(k1), ug.y - k1, step);
            i0[0] = r0.x, J0[0] = r0.y, i1[0] = r1.x, J1[0] = r1.y;
        }
        lane2 h, dh;
        {
            constexpr double zstep = (KorenTables::ZMAX - KorenTables::ZMIN) / (KorenTables::NZ - 1);
            const lane2 uz = (z - KorenTables::ZMIN) / zstep;
            const int q0 = std::min(static_cast<int>(uz.x), KorenTables::NZ - 2), q1 = std::min(static_cast<int>(uz.y), KorenTables::NZ - 2);
            const lane2 r0 = knot3(T0.hK.data(), static_cast<std::size_t>(q0), uz.x - q0, zstep);
            const lane2 r1 = knot3(T1.hK.data(), static_cast<std::size_t>(q1), uz.y - q1, zstep);
            h = lane2{r0.x, r1.x}, dh = lane2{r0.y, r1.y};
        }
        lane2 A, dA;
        {
            constexpr double vstep = KorenTables::VMAX / (KorenTables::NV - 1);
            const lane2 u = vg2 / vstep;
            const int k0 = std::min(static_cast<int>(u.x), KorenTables::NV - 2), k1 = std::min(static_cast<int>(u.y), KorenTables::NV - 2);
            const double t0 = u.x - k0, t1 = u.y - k1;
            const auto q0 = static_cast<std::size_t>(k0), q1 = static_cast<std::size_t>(k1);
            A.x = KorenTables::hermite(t0, vstep, T0.a[q0], T0.a[q0 + 1], T0.da[q0], T0.da[q0 + 1]);
            dA.x = KorenTables::hermiteD(t0, vstep, T0.a[q0], T0.a[q0 + 1], T0.da[q0], T0.da[q0 + 1]);
            A.y = KorenTables::hermite(t1, vstep, T1.a[q1], T1.a[q1 + 1], T1.da[q1], T1.da[q1 + 1]);
            dA.y = KorenTables::hermiteD(t1, vstep, T1.a[q1], T1.a[q1 + 1], T1.da[q1], T1.da[q1 + 1]);
        }
        const lane2 P = A * h;                                     // (2/kg1)·Q
        const lane2 dP1 = A * dh * kp * inv;                       // ∂P/∂vg1
        const lane2 dP2 = dA * h - A * dh * kp * vg1 * inv * inv;  // ∂P/∂vg2
        lane2 at, dat;
        {
            constexpr double astep = (KorenTables::AMAX - KorenTables::AMIN) / (KorenTables::NA - 1);
            const lane2 u = (vpk - KorenTables::AMIN) / astep;
            const int k0 = std::min(static_cast<int>(u.x), KorenTables::NA - 2), k1 = std::min(static_cast<int>(u.y), KorenTables::NA - 2);
            const lane2 r0 = knot3(T0.atK.data(), static_cast<std::size_t>(k0), u.x - k0, astep);
            const lane2 r1 = knot3(T1.atK.data(), static_cast<std::size_t>(k1), u.y - k1, astep);
            at = lane2{r0.x, r1.x}, dat = lane2{r0.y, r1.y};
        }
        const lane2 ip = P * at, j3 = dP1 * at, j4 = P * dat, j5 = dP2 * at;
        // screen: Q·(1/kg2 + (2/kg1)(π/2 − atan)) with Q = P·kg1/2
        const lane2 c = 0.5 * lane2{T0.kg1, T1.kg1} / lane2{d0.p[3], d1.p[3]} + (0.5 * M_PI - at);
        const lane2 is = P * c, j6 = dP1 * c, j7 = -P * dat, j8 = dP2 * c;
        i0[1] = ip.x, i1[1] = ip.y, i0[2] = is.x, i1[2] = is.y;
        J0[3] = j3.x, J1[3] = j3.y, J0[4] = j4.x, J1[4] = j4.y, J0[5] = j5.x, J1[5] = j5.y;
        J0[6] = j6.x, J1[6] = j6.y, J0[7] = j7.x, J1[7] = j7.y, J0[8] = j8.x, J1[8] = j8.y;
    }
    // ── end of two-lane tube laws ──
};

// Shared, immutable tables per parameter set (built on the control thread at compile time).
const KorenTables* korenTables(const double* p);

// Small dense linear algebra on caller-owned storage (no allocation).
namespace la {

// In-place LU with partial pivoting (row-major, n×n). Returns false if singular.
inline bool luFactor(double* a, int n, int* piv) noexcept {
    for (int k = 0; k < n; ++k) {
        int p = k;
        double best = std::abs(a[k * n + k]);
        for (int r = k + 1; r < n; ++r) {
            const double x = std::abs(a[r * n + k]);
            if (x > best) {
                best = x;
                p = r;
            }
        }
        piv[k] = p;
        if (!(best > 1e-300)) return false;
        if (p != k)
            for (int c = 0; c < n; ++c) std::swap(a[k * n + c], a[p * n + c]);
        const double inv = 1.0 / a[k * n + k];
        for (int r = k + 1; r < n; ++r) {
            const double f = (a[r * n + k] *= inv);
            if (f != 0.0)
                for (int c = k + 1; c < n; ++c) a[r * n + c] -= f * a[k * n + c];
        }
    }
    return true;
}
inline void luSolve(const double* lu, const int* piv, int n, double* b) noexcept {
    for (int k = 0; k < n; ++k)
        if (piv[k] != k) std::swap(b[k], b[piv[k]]);
    for (int r = 1; r < n; ++r) {
        double s = b[r];
        for (int c = 0; c < r; ++c) s -= lu[r * n + c] * b[c];
        b[r] = s;
    }
    for (int r = n - 1; r >= 0; --r) {
        double s = b[r];
        for (int c = r + 1; c < n; ++c) s -= lu[r * n + c] * b[c];
        b[r] = s / lu[r * n + r];
    }
}
// Solve A·x = b in place for small dense systems (A destroyed). Returns false if singular.
inline bool solveInPlace(double* a, double* b, int n, int* piv) noexcept {
    if (!luFactor(a, n, piv)) return false;
    luSolve(a, piv, n, b);
    return true;
}

} // namespace la

} // namespace rig::ce
