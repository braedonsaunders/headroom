#pragma once
#include <cmath>

namespace rig::circuit {
// Software mapping only. The assumed default is a convenient 1 V/FS scale,
// not a statement about an interface, pickup, amplifier or measured hardware.
// Calibrated provenance may only be supplied by a caller with actual measured
// full-scale voltage/gain data; this utility performs no hardware calibration.
struct VoltageCalibration {
  enum class Provenance { Assumed, Calibrated };
  double voltsPerDigitalFullScale = 1.0;
  Provenance provenance = Provenance::Assumed;

  bool valid() const noexcept {
    return std::isfinite(voltsPerDigitalFullScale) && voltsPerDigitalFullScale > 0 &&
           (provenance == Provenance::Assumed || provenance == Provenance::Calibrated);
  }
  // Invalid configuration or input is silenced; callers should reject invalid
  // configuration using valid() before installing it in a processing chain.
  double toVolts(double digital) const noexcept {
    if (!valid() || !std::isfinite(digital)) return 0;
    const double value = digital * voltsPerDigitalFullScale;
    return std::isfinite(value) ? value : 0;
  }
  double toDigital(double volts) const noexcept {
    if (!valid() || !std::isfinite(volts)) return 0;
    const double value = volts / voltsPerDigitalFullScale;
    return std::isfinite(value) ? value : 0;
  }
  // RMS level, in dBu (0 dBu = 0.7746 V RMS), of a sine whose peak hits digital
  // full scale — the convention NAM's input_level_dbu / output_level_dbu use.
  // The default 1 V/FS gives -0.79 dBu.
  static constexpr double kDbuReferenceVolts = 0.77459666924148;
  double fullScaleSineDbu() const noexcept {
    if (!valid()) return 0;
    return 20.0 * std::log10(voltsPerDigitalFullScale / std::sqrt(2.0) / kDbuReferenceVolts);
  }
  static VoltageCalibration assumed(double scale) noexcept {
    return {scale, Provenance::Assumed};
  }
  static VoltageCalibration calibrated(double measuredScale) noexcept {
    return {measuredScale, Provenance::Calibrated};
  }
};
} // namespace rig::circuit
