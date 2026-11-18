#pragma once
// Double-precision 2x polyphase half-band (allpass pair) for multirate partition boundaries
// (docs/CIRCUIT_ENGINE.md §3.7). Coefficients: Headroom's HalfbandDesign (HIIR-style elliptic polyphase design).
// Boundary nodes carry DC bias (4.5 V, 180 V plates), so states are double and primed at the operating point.
#include <array>
#include <vector>

#include "../dsp/AmpModeling.hpp"

namespace rig::ce {

class Halfband2x {
public:
    static constexpr int NC = 10;
    Halfband2x() {
        static const std::vector<double> c = amp::HalfbandDesign::compute(NC, 0.035);
        for (int i = 0; i < NC; ++i) coef_[static_cast<std::size_t>(i)] = c[static_cast<std::size_t>(i)];
    }
    void up(double in, double& o0, double& o1) noexcept {
        double even = in, odd = in;
        for (int i = 0; i < NC; i += 2) {
            even = section(ux_, uy_, even, i);
            if (i + 1 < NC) odd = section(ux_, uy_, odd, i + 1);
        }
        o0 = even;
        o1 = odd;
    }
    double down(double i0, double i1) noexcept {
        double a = i1, b = i0;
        for (int i = 0; i < NC; i += 2) {
            a = section(dx_, dy_, a, i);
            if (i + 1 < NC) b = section(dx_, dy_, b, i + 1);
        }
        return 0.5 * (a + b);
    }
    // Settle both directions at a constant (the DC operating point) so a cold start produces no transient.
    void prime(double dc) noexcept {
        double a, b;
        for (int i = 0; i < 400; ++i) {
            up(dc, a, b);
            down(dc, dc);
        }
    }

private:
    double section(std::array<double, NC>& x, std::array<double, NC>& y, double in, int i) noexcept {
        const auto u = static_cast<std::size_t>(i);
        const double out = (in - y[u]) * coef_[u] + x[u];
        x[u] = in;
        y[u] = out;
        return out;
    }
    std::array<double, NC> coef_{}, ux_{}, uy_{}, dx_{}, dy_{};
};

// Rate-ladder converter stage (docs §3.7): the double-precision twin of amp::HalfbandIIR with the coefficient set of one
// CircuitOversampler stage (1: host to 2x, 10 coefficients; 2: 6; 3: 4; 4 and up: 6). A ladder group at 16x above a 4x
// unit therefore sees the input filtering of a whole circuit at 16x, in double precision, primed at the operating point.
class HalfbandStage {
public:
    static constexpr int kMax = 10;
    void design(int stage) {
        const int nc = stage <= 1 ? 10 : (stage == 2 ? 6 : (stage == 3 ? 4 : 6));
        const double tr = stage <= 1 ? 0.035 : (stage == 3 ? 0.30 : 0.20);
        const auto c = amp::HalfbandDesign::compute(nc, tr);
        nc_ = nc;
        for (int i = 0; i < nc; ++i) coef_[static_cast<std::size_t>(i)] = c[static_cast<std::size_t>(i)];
        x_.fill(0.0);
        y_.fill(0.0);
    }
    // The ladder's stage designs have 4, 6 or 10 coefficients: those run as unrolled copies (no loop or branch per
    // section; the same operations in the same order).
    void up(double in, double& o0, double& o1) noexcept {
        switch (nc_) {
            case 4: return upN<4>(in, o0, o1);
            case 6: return upN<6>(in, o0, o1);
            case 10: return upN<10>(in, o0, o1);
            default: return upN<0>(in, o0, o1);
        }
    }
    double down(double i0, double i1) noexcept {
        switch (nc_) {
            case 4: return downN<4>(i0, i1);
            case 6: return downN<6>(i0, i1);
            case 10: return downN<10>(i0, i1);
            default: return downN<0>(i0, i1);
        }
    }
    // Settle at a constant (the DC operating point), as an up or a down stage.
    void prime(double dc, bool upStage) noexcept {
        double a, b;
        for (int i = 0; i < 400; ++i) {
            if (upStage) up(dc, a, b);
            else down(dc, dc);
        }
    }

private:
    template <int N>
    void upN(double in, double& o0, double& o1) noexcept {
        const int nc = N > 0 ? N : nc_;
        double even = in, odd = in;
        for (int i = 0; i < nc; i += 2) {
            even = section(even, i);
            if (i + 1 < nc) odd = section(odd, i + 1);
        }
        o0 = even;
        o1 = odd;
    }
    template <int N>
    double downN(double i0, double i1) noexcept {
        const int nc = N > 0 ? N : nc_;
        double a = i1, b = i0;
        for (int i = 0; i < nc; i += 2) {
            a = section(a, i);
            if (i + 1 < nc) b = section(b, i + 1);
        }
        return 0.5 * (a + b);
    }
    double section(double in, int i) noexcept {
        const auto u = static_cast<std::size_t>(i);
        const double out = (in - y_[u]) * coef_[u] + x_[u];
        x_[u] = in;
        y_[u] = out;
        return out;
    }
    int nc_ = 0;
    std::array<double, kMax> coef_{}, x_{}, y_{};
};

} // namespace rig::ce
