#pragma once
#include "CircuitDefinition.hpp"
#include <cstdint>

namespace rig::circuit {
// Composes the implemented subcircuits in physical volts. No input/output
// gain normalization, saturation multiplier or artificial sag envelope.
// Supply coupling is explicit partitioned time stepping: the preamp consumes
// the previous accepted supply voltage; its solved current loads this frame's
// supply solve. This is not a simultaneous full-amplifier MNA solve.
class CircuitAmp {
public:
  struct RuntimeInfo {
    double supplyVolts, screenVolts, preampSupplyVolts, preampCurrentAmps;
    double preampOutputVolts, phaseInputVolts, speakerOutputVolts;
    std::uint64_t preampFailures, powerFailures, inputClamps;
  };
  CircuitAmp() = default;
  // Configuration and prepare/reset are control-thread operations. Build a
  // replacement off the audio thread, then adoptAudioStateFrom at a block
  // boundary. Rejected definitions leave all processing state/config intact.
  bool configure(const CircuitDefinition &definition, std::string &error) {
    if (!definition.valid(error))
      return false;
    CircuitAmp next;
    next.rate_ = rate_;
    next.definition_ = definition;
    const auto &c = definition.applied();
    next.preamp_.setConfig(c.preamp);
    next.power_.setConfig(c.power);
    next.preamp_.setControls(c.bass, c.mid, c.treble);
    next.power_.setControls(c.feedback, c.presence, c.depth);
    next.power_.setLoad(c.loadR, c.loadL);
    next.preamp_.prepare(rate_);
    next.power_.prepare(rate_);
    next.reset();
    if (!next.ready_) {
      error =
          "Circuit definition has no supported converged DC operating point";
      return false;
    }
    *this = std::move(next);
    error.clear();
    return true;
  }
  bool prepare(double sampleRate, std::string &error) {
    if (!std::isfinite(sampleRate) || sampleRate < 8000 ||
        sampleRate > 384000) {
      error =
          "Circuit sample rate must be finite and between 8000 and 384000 Hz";
      return false;
    }
    CircuitAmp next;
    next.rate_ = sampleRate;
    if (!next.configure(definition_, error))
      return false;
    *this = std::move(next);
    return true;
  }
  void prepare(double sampleRate) {
    std::string error;
    prepare(sampleRate, error);
  }
  void reset() noexcept {
    // Alternate DC operating-point estimates without advancing the preamp's
    // reactive state. The power circuit reset obtains its own supply
    // equilibrium for the measured triode load. Final preamp reset initializes
    // capacitor histories at this shared operating point, not at zero node
    // voltage.
    power_.setPreampLoadAmps(0.002);
    power_.reset();
    ready_ = false;
    interfaceInputClamps_ = 0;
    for (int i = 0; i < 64; ++i) {
      const double previousSupply = power_.preampSupplyVolts();
      preamp_.setSupplyVolts(power_.preampSupplyVolts());
      preamp_.reset();
      power_.setPreampLoadAmps(preamp_.supplyCurrentAmps());
      power_.reset();
      if (std::abs(power_.preampSupplyVolts() - previousSupply) < 1e-6) {
        ready_ = true;
        break;
      }
    }
    preamp_.setSupplyVolts(power_.preampSupplyVolts());
    preamp_.reset();
    preampCurrent_ = preamp_.supplyCurrentAmps();
    preampOutput_ = preamp_.diagnostics().output;
    // Blocking capacitor holds the interstage source DC; receiver is at 0 V.
    couplingVoltage_ = preampOutput_ * definition_.applied().master;
    phaseInput_ = speakerOutput_ = 0;
    ready_ = ready_ && power_.failureCount() == 0 &&
             preamp_.diagnostics().failures == 0 &&
             power_.preampSupplyVolts() >= 50 &&
             power_.preampSupplyVolts() <= 600 &&
             std::isfinite(preampCurrent_) && preampCurrent_ >= 0 &&
             preampCurrent_ <= 0.02;
  }
  double process(double inputVolts) noexcept {
    return ready_ ? processPower(processPreamp(inputVolts)) : 0;
  }
  bool ready() const noexcept { return ready_; }
  // Split interface supports inspection/insertion. Exactly one preamp call and
  // one power call per sample is required to retain shared-supply timing.
  double processPreamp(double inputVolts) noexcept {
    if (!ready_)
      return 0;
    preamp_.setSupplyVolts(power_.preampSupplyVolts());
    preampOutput_ = preamp_.process(inputVolts);
    preampCurrent_ = preamp_.supplyCurrentAmps();
    power_.setPreampLoadAmps(preampCurrent_);
    return preampOutput_;
  }
  double processPower(double preampOutputVolts) noexcept {
    if (!ready_)
      return 0;
    const auto &c = definition_.applied();
    if (!std::isfinite(preampOutputVolts)) {
      preampOutputVolts = 0;
      ++interfaceInputClamps_;
    } else if (std::abs(preampOutputVolts) > 10000) {
      // Numerical guard for callers of the split voltage port. Ordinary
      // preamp output is supply-bounded, far below this safety threshold.
      preampOutputVolts = std::clamp(preampOutputVolts, -10000., 10000.);
      ++interfaceInputClamps_;
    }
    const double source = c.master * preampOutputVolts;
    // Backward Euler companion of series C into a grid-leak resistor.
    // Master is an unloaded wiper in this reduced voltage-port connection;
    // total pot resistance loads the actual solved preamp output node.
    const double g = rate_ * c.interfaceC;
    phaseInput_ = g * (source - couplingVoltage_) / (g + 1 / c.interfaceR);
    couplingVoltage_ = source - phaseInput_;
    speakerOutput_ = power_.process(phaseInput_);
    return speakerOutput_;
  }
  // Safe for a serialized audio-thread knob update: no allocation or DC reset.
  bool setPerformanceControls(double master, double bass, double mid,
                              double treble, double feedback, double presence,
                              double depth) noexcept {
    if (!definition_.setPerformanceControls(master, bass, mid, treble, feedback,
                                            presence, depth))
      return false;
    preamp_.setControls(bass, mid, treble);
    power_.setControls(feedback, presence, depth);
    return true;
  }
  // Dynamic state only. New scalar values remain installed. A topology/rate
  // change requires the prepared DC state, so adoption returns false then.
  bool adoptAudioStateFrom(const CircuitAmp &old) noexcept {
    if (!ready_ || !old.ready_ ||
        definition_.topology() != old.definition_.topology() ||
        rate_ != old.rate_)
      return false;
    preamp_.adoptAudioStateFrom(old.preamp_);
    power_.adoptAudioStateFrom(old.power_);
    interfaceInputClamps_ = old.interfaceInputClamps_;
    couplingVoltage_ = old.couplingVoltage_;
    preampOutput_ = old.preampOutput_;
    preampCurrent_ = old.preampCurrent_;
    phaseInput_ = old.phaseInput_;
    speakerOutput_ = old.speakerOutput_;
    power_.setPreampLoadAmps(preampCurrent_);
    return true;
  }
  const CircuitDefinition &definition() const noexcept { return definition_; }
  const PreampCircuit &preamp() const noexcept { return preamp_; }
  const PowerCircuit &power() const noexcept { return power_; }
  RuntimeInfo runtimeInfo() const noexcept {
    return {power_.supplyVolts(),
            power_.screenVolts(),
            power_.preampSupplyVolts(),
            preampCurrent_,
            preampOutput_,
            phaseInput_,
            speakerOutput_,
            preamp_.diagnostics().failures,
            power_.failureCount(),
            power_.inputClampCount() + interfaceInputClamps_};
  }

private:
  CircuitDefinition definition_{};
  PreampCircuit preamp_{};
  PowerCircuit power_{};
  bool ready_ = false;
  std::uint64_t interfaceInputClamps_ = 0;
  double rate_ = 48000, couplingVoltage_ = 0, preampCurrent_ = 0,
         preampOutput_ = 0, phaseInput_ = 0, speakerOutput_ = 0;
};
} // namespace rig::circuit
