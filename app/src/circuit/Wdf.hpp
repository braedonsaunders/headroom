#pragma once
// Wave digital filter realisation of a solve group (docs/CIRCUIT_ENGINE.md §3.4).
//
// The group's passive two-terminal elements become adapted WDF leaves (R: b = 0, C: b = a[n-1],
// L: b = -a[n-1]). Series-parallel reduction turns them into a binary tree of 3-port adaptors; whatever is
// not series-parallel (bridges, controlled sources, nodes pinned to known voltages) becomes one R-type
// adaptor whose scattering matrix comes from its nodal analysis (Werner et al. 2015): S = 2·Q − I with
// Q = ∂v_ports/∂a, plus Su for the known node voltages. The single nonlinear one-port (diode or
// antiparallel pair) sits at the R-type's adapted root port (R = Thévenin resistance), so the root
// reflection is explicit: Wright-omega closed form + one Newton polish.
//
// Same trapezoidal discretisation as the DK solver; the cross-solver test requires agreement to ~1e-9.
#include <cmath>
#include <string>
#include <vector>

#include "Devices.hpp"

namespace rig::ce {

class Circuit;
struct Group;

// Wright omega ω(x): ω + ln ω = x. D'Angelo et al. (DAFx 2019) style initial guess + 2 Newton steps.
inline double wrightOmega(double x) noexcept {
    double w;
    if (x < -2.0) {
        w = std::exp(x);
    } else if (x < 1.0) {
        // cubic fit around the knee
        w = 0.5671432904 + x * (0.3618963 + x * (0.0631 + x * (-0.0042)));
        if (w < 1e-3) w = 1e-3;
    } else {
        w = x - std::log(x);
    }
    // One Newton step: the root solver polishes the full diode equation afterwards anyway.
    const double f = w + std::log(w) - x;
    w -= f / (1.0 + 1.0 / w);
    return w > 0 ? w : 1e-300;
}

class WdfGroup {
public:
    // A group can be realised as a WDF if it has at most one nonlinear one-port of diode type.
    static bool eligible(const Circuit& c, const Group& g);
    bool build(const Circuit& c, const Group& g, std::string& error);
    void refresh(const Circuit& c, const Group& g) noexcept;
    // w = [states(unused); inputs; NL current], out = exports (first ny entries).
    void process(Circuit& c, const Group& g, double* w, double* out) noexcept;
    // Initialise leaf states from DK history currents s (same ordering as g.states) and branch voltages.
    void resetFrom(const Circuit& c, const Group& g, const double* s, const double* vc) noexcept;
    void adoptFrom(const WdfGroup& o) noexcept;
    // The inverse of resetFrom: the leaves' states as DK history currents s and branch voltages vc (the group's vcLast
    // and w are not kept up to date while the WDF runs). Circuit::settleSlowRails (docs §10.49).
    void stateTo(const Circuit& c, const Group& g, double* s, double* vc) const noexcept;

    int coreSize() const noexcept { return static_cast<int>(ports_.size()); }
    int adaptorCount() const noexcept { return static_cast<int>(tree_.size()); }
    int seriesParallelAdaptors() const noexcept {
        int k = 0;
        for (const auto& t : tree_) k += t.kind != TNode::Leaf;
        return k;
    }

private:
    struct TNode {
        enum Kind : unsigned char { Leaf, Series, Parallel };
        Kind kind = Leaf;
        int c1 = -1, c2 = -1;
        double p1 = 1, p2 = 1; // child polarities
        int prim = -1;         // leaf
        unsigned char elem = 0; // 0 R, 1 C, 2 L
        double R = 1, G = 1;
        double z = 0;          // C/L state (last incident wave)
        double a = 0, b = 0;   // incident (down), reflected (up)
        double iLast = 0;
    };
    struct Port {
        int tree = -1; // subtree root (-1 = the nonlinear root)
        int x = 0, y = 0;
    };
    struct PathStep {
        int leaf;
        double sign;
    };
    struct ExportPath {
        int startCore = -1;  // core unknown index, or -1 with startInput / ground
        int startInput = -1; // input index
        std::vector<PathStep> steps;
    };

    std::vector<TNode> tree_;
    std::vector<int> post_;      // post-order (children before parents)
    std::vector<Port> ports_;    // core ports; root port (if any) is index rootPort_
    int rootPort_ = -1;
    int rootDevice_ = -1;
    // core MNA
    std::vector<int> coreNodes_; // node ids (unknowns of the core)
    std::vector<int> coreOf_;    // node -> core idx
    std::vector<int> inOf_;      // node -> group input idx
    int nu_ = 0;
    std::vector<double> S_, Su_, Qn_, Qnu_; // ports×ports, ports×nu, coreNodes×ports, coreNodes×nu
    std::vector<double> aPort_, bPort_;
    std::vector<ExportPath> exports_;
    std::vector<int> leafOfState_;   // group state k -> leaf tree index
    std::vector<double> leafSign_;   // orientation of leaf vs prim
    // scratch for refresh (sized at build)
    std::vector<double> M_, rhs_, X_;
    std::vector<int> piv_;
    double Rroot_ = 1;
    double is_ = 1e-14, nvt_ = 0.026, logIsRonNvt_ = 0;
    bool pair_ = false;
    double gShift_ = 0; // NlDevice::gShift[0] of the root (its linear split leaf), subtracted from the root law
    double gmin_ = kGmin; // the root diode's gmin (Devices.hpp: gmin/m on a series string of m junctions)
    double vRoot_ = 0, vRoot2_ = 0, invNvt_ = 1.0 / 0.026;
    std::vector<int> ctrlPrims_;     // controlled sources inside the core
    std::vector<int> corePrimsUnused_;
    friend class Circuit;
};

} // namespace rig::ce
