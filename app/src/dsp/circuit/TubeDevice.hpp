#pragma once
#include <algorithm>
#include <array>
#include <cmath>

namespace rig::circuit {

// 12AX7 grid conduction law shared by every triode model in Headroom (the fast
// amp stages in AmpModeling.hpp and this circuit-reference device):
//   Ig(vgk) = G * s^1.5,  s = k * log(1 + exp(vgk / k))
// a smoothed Child-Langmuir 3/2-power law (Dempwolf & Zölzer's measured 12AX7
// fit is G = 6.06e-4, exponent 1.354, knee 1/13.9 V). Amperes, volts.
struct GridLaw {
  static constexpr double kScale = 6.2e-4;  // A / V^1.5
  static constexpr double kExponent = 1.5;
  static constexpr double kKnee = 0.06;     // V
  struct Eval {
    double current, slope, curvature;
  };
  static double current(double vgk) noexcept {
    const double z = vgk / kKnee;
    const double s = kKnee * (z > 30.0 ? z : std::log1p(std::exp(z)));
    return s > 0.0 ? kScale * s * std::sqrt(s) : 0.0;
  }
  static Eval evaluate(double vgk) noexcept {
    const double z = vgk / kKnee;
    const double e = std::exp(-std::abs(z));
    const double s = kKnee * (std::max(z, 0.0) + std::log1p(e));
    const double sig = z >= 0.0 ? 1.0 / (1.0 + e) : e / (1.0 + e);
    if (!(s > 1e-300)) return {0.0, 0.0, 0.0};
    const double rs = std::sqrt(s);
    return {kScale * s * rs, 1.5 * kScale * rs * sig,
            1.5 * kScale * (0.5 * sig * sig / rs + rs * sig * (1.0 - sig) / kKnee)};
  }
};

// Smooth 12AX7-family approximation, currents in amperes, voltages in volts.
// Accelerated via separable 1D cubic Hermite shape-transform tables:
//   Ip(vgk, vpk) = A(vpk) * h(6.0 + C(vpk) * vgk)
// where A(vpk) = (vpk / 600)^1.4 / 530, C(vpk) = 600 / sqrt(300 + vpk^2),
// and h(z) = (log1p(exp(z)))^1.4.
// Eliminates exp/log1p/pow/sqrt from inner Newton iterations while matching
// the exact analytical equations to < 1e-8.
struct TubeDevice {
  struct Derivatives {
    double grid, plate;
  };
  struct GridEvaluation {
    double current, slope;
  };

  struct Tables {
    static constexpr int kNz = 4096;
    static constexpr double kZMin = -60.0, kZMax = 60.0;
    static constexpr double kZStep = (kZMax - kZMin) / (kNz - 1);
    static constexpr double kZInv = 1.0 / kZStep;

    static constexpr int kNv = 4096;
    static constexpr double kVMax = 650.0;
    static constexpr double kVStep = kVMax / (kNv - 1);
    static constexpr double kVInv = 1.0 / kVStep;

    static constexpr int kNg = 4096;
    static constexpr double kGMin = -10.0, kGMax = 20.0;
    static constexpr double kGStep = (kGMax - kGMin) / (kNg - 1);
    static constexpr double kGInv = 1.0 / kGStep;

    // Cubic Hermite node values and derivatives for h(z) = (log1p(exp(z)))^1.4
    std::array<double, kNz> h_val{};
    std::array<double, kNz> h_der{};
    std::array<double, kNz> h_d2{};

    // Precomputed 1D functions of vpk on [0, kVMax]
    std::array<double, kNv> a_val{};   // (vpk / 600)^1.4 / 530
    std::array<double, kNv> a_der{};
    std::array<double, kNv> c_val{};   // 600 / sqrt(300 + vpk^2)
    std::array<double, kNv> c_der{};   // -600 * vpk / (300 + vpk^2)^1.5

    // Cubic Hermite table for grid current Ig(vgk)
    std::array<double, kNg> g_val{};
    std::array<double, kNg> g_der{};
    std::array<double, kNg> g_d2{};

    Tables() noexcept {
      for (int i = 0; i < kNz; ++i) {
        const double z = kZMin + i * kZStep;
        const double ez = std::exp(z);
        const double soft = std::log1p(ez);
        const double sig = z >= 0.0 ? 1.0 / (1.0 + std::exp(-z)) : ez / (1.0 + ez);
        h_val[static_cast<std::size_t>(i)] = std::pow(soft, 1.4);
        h_der[static_cast<std::size_t>(i)] =
            soft > 0.0 ? 1.4 * std::pow(soft, 0.4) * sig : 0.0;
        h_d2[static_cast<std::size_t>(i)] =
            soft > 1e-12
                ? 1.4 * (0.4 * std::pow(soft, -0.6) * sig * sig +
                         std::pow(soft, 0.4) * sig * (1.0 - sig))
                : 0.0;
      }
      for (int i = 0; i < kNv; ++i) {
        const double v = i * kVStep;
        const double d2 = 300.0 + v * v;
        const double d = std::sqrt(d2);
        a_val[static_cast<std::size_t>(i)] =
            v > 0.0 ? std::pow(v / 600.0, 1.4) / 530.0 : 0.0;
        a_der[static_cast<std::size_t>(i)] =
            v > 0.0 ? (1.4 / (600.0 * 530.0)) * std::pow(v / 600.0, 0.4) : 0.0;
        c_val[static_cast<std::size_t>(i)] = 600.0 / d;
        c_der[static_cast<std::size_t>(i)] = -600.0 * v / (d2 * d);
      }
      for (int i = 0; i < kNg; ++i) {
        const auto g = GridLaw::evaluate(kGMin + i * kGStep);
        g_val[static_cast<std::size_t>(i)] = g.current;
        g_der[static_cast<std::size_t>(i)] = g.slope;
        g_d2[static_cast<std::size_t>(i)] = g.curvature;
      }
    }

    static double hermite(double t, double h, double y0, double y1, double m0,
                          double m1) noexcept {
      const double t2 = t * t, t3 = t2 * t;
      const double h00 = 2.0 * t3 - 3.0 * t2 + 1.0;
      const double h10 = t3 - 2.0 * t2 + t;
      const double h01 = -2.0 * t3 + 3.0 * t2;
      const double h11 = t3 - t2;
      return h00 * y0 + h10 * (h * m0) + h01 * y1 + h11 * (h * m1);
    }

    void evalH(double raw_z, double& out_h, double& out_dh) const noexcept {
      if (raw_z <= kZMin) {
        out_h = 0.0;
        out_dh = 0.0;
        return;
      }
      if (raw_z >= kZMax) {
        out_h = h_val[kNz - 1];
        out_dh = 0.0;
        return;
      }
      const double u = (raw_z - kZMin) * kZInv;
      const int idx = std::min(static_cast<int>(u), kNz - 2);
      const double t = u - static_cast<double>(idx);
      const auto i = static_cast<std::size_t>(idx);
      out_h = hermite(t, kZStep, h_val[i], h_val[i + 1], h_der[i], h_der[i + 1]);
      out_dh = hermite(t, kZStep, h_der[i], h_der[i + 1], h_d2[i], h_d2[i + 1]);
    }

    void evalVpk(double vpk, double& A, double& dA, double& C,
                 double& dC) const noexcept {
      if (vpk <= kVStep) {
        // Near vpk = 0, use exact power for A(vpk) as v^1.4 has infinite 2nd derivative at 0
        const double d2 = 300.0 + vpk * vpk;
        const double d = std::sqrt(d2);
        A = std::pow(vpk / 600.0, 1.4) / 530.0;
        dA = (1.4 / (600.0 * 530.0)) * std::pow(vpk / 600.0, 0.4);
        C = 600.0 / d;
        dC = -600.0 * vpk / (d2 * d);
        return;
      }
      if (vpk >= kVMax) {
        const double d2 = 300.0 + vpk * vpk;
        const double d = std::sqrt(d2);
        A = std::pow(vpk / 600.0, 1.4) / 530.0;
        dA = (1.4 / (600.0 * 530.0)) * std::pow(vpk / 600.0, 0.4);
        C = 600.0 / d;
        dC = -600.0 * vpk / (d2 * d);
        return;
      }
      const double u = vpk * kVInv;
      const int idx = std::min(static_cast<int>(u), kNv - 2);
      const double t = u - static_cast<double>(idx);
      const auto i = static_cast<std::size_t>(idx);
      A = hermite(t, kVStep, a_val[i], a_val[i + 1], a_der[i], a_der[i + 1]);
      dA = a_der[i] + t * (a_der[i + 1] - a_der[i]);
      C = hermite(t, kVStep, c_val[i], c_val[i + 1], c_der[i], c_der[i + 1]);
      dC = c_der[i] + t * (c_der[i + 1] - c_der[i]);
    }
  };

  static const Tables& tables() noexcept {
    static const Tables t{};
    return t;
  }

  static double plate(double vgk, double vpk) noexcept {
    if (vpk <= 0.0) return 0.0;
    const auto& T = tables();
    double A, dA, C, dC;
    T.evalVpk(vpk, A, dA, C, dC);
    const double raw = 6.0 + C * vgk;
    const double z = std::clamp(raw, -60.0, 60.0);
    double h, dh;
    T.evalH(z, h, dh);
    return A * h;
  }

  static double plateWithDerivatives(double vgk, double vpk,
                                     Derivatives& deriv) noexcept {
    if (vpk <= 0.0) {
      deriv = {0.0, 0.0};
      return 0.0;
    }
    const auto& T = tables();
    double A, dA, C, dC;
    T.evalVpk(vpk, A, dA, C, dC);
    const double raw = 6.0 + C * vgk;
    const double z = std::clamp(raw, -60.0, 60.0);
    double h, dh;
    T.evalH(z, h, dh);
    if (raw != z) dh = 0.0;
    deriv.grid = A * dh * C;
    deriv.plate = dA * h + A * dh * vgk * dC;
    return A * h;
  }

  static Derivatives plateDerivatives(double vgk, double vpk) noexcept {
    Derivatives d{};
    (void)plateWithDerivatives(vgk, vpk, d);
    return d;
  }

  static GridEvaluation gridWithDerivative(double vgk) noexcept {
    const auto& T = tables();
    if (vgk <= Tables::kGMin) return {0.0, 0.0};
    if (vgk >= Tables::kGMax) {
      const auto g = GridLaw::evaluate(vgk);
      return {g.current, g.slope};
    }
    const double u = (vgk - Tables::kGMin) * Tables::kGInv;
    const int idx = std::min(static_cast<int>(u), Tables::kNg - 2);
    const double t = u - static_cast<double>(idx);
    const auto i = static_cast<std::size_t>(idx);
    const double cur = Tables::hermite(t, Tables::kGStep, T.g_val[i],
                                       T.g_val[i + 1], T.g_der[i], T.g_der[i + 1]);
    const double slp = Tables::hermite(t, Tables::kGStep, T.g_der[i],
                                       T.g_der[i + 1], T.g_d2[i], T.g_d2[i + 1]);
    return {std::max(0.0, cur), std::max(0.0, slp)};
  }

  static double gridDerivative(double vgk) noexcept {
    return gridWithDerivative(vgk).slope;
  }

  static double grid(double vgk) noexcept {
    return gridWithDerivative(vgk).current;
  }
};

} // namespace rig::circuit
