#pragma once
#include <algorithm>
#include <cmath>
namespace rig::circuit {
// Smooth reduced 6L6-like beam tetrode; currents in amperes, voltages in volts.
struct PowerDevices {
  static double plateFactor(double vg, double vs) noexcept {
    const double e = std::max(0.0, vg + vs / 8.0);
    return 0.00037 * e * std::sqrt(e) * (2.0 / 3.141592653589793);
  }
  static double plate(double vg, double vp, double vs) noexcept {
    return plateFactor(vg, vs) * std::atan(std::max(0.0, vp) / 55.0);
  }
  static double screen(double vg, double vp, double vs) noexcept {
    return plate(vg, vp, vs) *
           (0.06 + 0.16 * std::exp(-std::max(0.0, vp) / 60.0));
  }
  static double grid(double vg) noexcept {
    return vg > 0 ? vg * vg / (1500.0 * (vg + 0.5)) : 0.0;
  }
  static double gridDerivative(double vg) noexcept {
    return vg > 0 ? vg * (vg + 1.0) / (1500.0 * (vg + 0.5) * (vg + 0.5)) : 0.0;
  }
};
} // namespace rig::circuit
