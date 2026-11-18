#pragma once

#include "circuit/Calibration.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace rig::amp {

// 5-state hysteretic noise gate:
//   Idle      — closed at floorGain_; opens only when the detector reaches openThreshold_
//   Attack    — opening toward unity (≈1.5 ms one-pole)
//   Triggered — fully open while the detector stays >= closeThreshold_
//   Holding   — detector fell below closeThreshold_; unity gain for the hold time. Rising back above
//               closeThreshold_ returns to Triggered (the hold restarts on the next dip)
//   Release   — hold expired: gain falls at a constant dB/s rate (linear in dB, like an analog VCA
//               driven by a linear control voltage) until -100 dB, then snaps to floorGain_. Only the
//               OPEN threshold can re-open it, so a tail wobbling inside the hysteresis window
//               (open/close typically 6 dB apart) cannot chatter.
//
// Supports split detection/application: call detect(cleanDi) on the clean DI and multiply the
// processed signal (after a high-gain or neural amp) by the returned gain, so learned model hiss is
// muted when idle while sustaining notes keep the gate open.
class HystereticNoiseGate {
public:
    enum class State : uint8_t { Idle, Attack, Holding, Release, Triggered };

    void prepare(double sampleRate) noexcept {
        sr_ = std::isfinite(sampleRate) && sampleRate >= 8000.0 ? sampleRate : 48000.0;
        envAttack_ = coefficient(sr_, 0.001);
        envRelease_ = coefficient(sr_, 0.035);
        gainAttack_ = coefficient(sr_, 0.0015);
        updateTiming();
        updateThresholds();
        reset();
    }

    void reset() noexcept {
        state_ = State::Idle;
        envelope_ = 0.0f;
        gain_ = enabled_ ? floorGain_ : 1.0f;
        holdCounter_ = 0;
    }

    void setEnabled(bool enabled) noexcept {
        if (enabled && !enabled_) {
            // Switching on mid-note: start open and let the hold / release logic decide.
            state_ = State::Holding;
            holdCounter_ = holdSamplesMax_;
            gain_ = 1.0f;
        }
        enabled_ = enabled;
        if (!enabled_) {
            state_ = State::Triggered;
            gain_ = 1.0f;
        }
    }
    bool enabled() const noexcept { return enabled_; }

    // Open threshold in linear amplitude; close threshold = open * hysteresisRatio (default -12 dB).
    void setThresholdLinear(float openThreshold, float hysteresisRatio = 0.25118864f) noexcept {
        openThreshold_ = std::clamp(openThreshold, 1e-6f, 0.5f);
        hysteresisRatio_ = std::clamp(hysteresisRatio, 0.05f, 0.95f);
        updateThresholds();
    }

    // Control thread only (std::pow).
    void setThresholdDb(float openDb, float hysteresisDb = 12.0f) noexcept {
        openThreshold_ = std::pow(10.0f, std::clamp(openDb, -100.0f, -6.0f) * 0.05f);
        hysteresisRatio_ = std::pow(10.0f, -std::clamp(hysteresisDb, 1.0f, 30.0f) * 0.05f);
        updateThresholds();
    }

    // Hold time (s) below the close threshold before releasing, and the release slope in dB/s.
    // Control thread only (std::pow).
    void setTiming(double holdSeconds, double releaseDbPerSecond) noexcept {
        holdSeconds_ = std::clamp(holdSeconds, 0.0, 2.0);
        releaseDbPerSecond_ = std::clamp(releaseDbPerSecond, 20.0, 20000.0);
        updateTiming();
    }

    void setFloorGain(float floorGain) noexcept {
        floorGain_ = std::clamp(floorGain, 0.0f, 1.0f);
    }

    // Advance detector and state machine from a clean DI input sample.
    // Returns the instantaneous gate gain in [floorGain_, 1.0f].
    float detect(float diInput) noexcept {
        const float absIn = std::isfinite(diInput) ? std::abs(diInput) : 0.0f;
        envelope_ += (absIn - envelope_) * (absIn > envelope_ ? envAttack_ : envRelease_);

        if (!enabled_) {
            state_ = State::Triggered;
            gain_ = 1.0f;
            return 1.0f;
        }

        switch (state_) {
            case State::Idle:
                gain_ = floorGain_;
                if (envelope_ >= openThreshold_) {
                    state_ = State::Attack;
                    gain_ += (1.0f - gain_) * gainAttack_;
                }
                break;

            case State::Attack:
                gain_ += (1.0f - gain_) * gainAttack_;
                if (gain_ >= 0.999f) {
                    gain_ = 1.0f;
                    state_ = State::Triggered;
                } else if (envelope_ < closeThreshold_) {
                    state_ = State::Holding;
                    holdCounter_ = holdSamplesMax_;
                }
                break;

            case State::Triggered:
                gain_ = 1.0f;
                if (envelope_ < closeThreshold_) {
                    state_ = State::Holding;
                    holdCounter_ = holdSamplesMax_;
                }
                break;

            case State::Holding:
                // Attack may have handed over a partially open gain: keep opening while holding.
                gain_ += (1.0f - gain_) * gainAttack_;
                if (envelope_ >= closeThreshold_) {
                    state_ = gain_ >= 0.999f ? State::Triggered : State::Attack;
                } else if (holdCounter_ > 0) {
                    --holdCounter_;
                } else {
                    state_ = State::Release;
                }
                break;

            case State::Release:
                if (envelope_ >= openThreshold_) {
                    state_ = State::Attack;
                    gain_ += (1.0f - gain_) * gainAttack_;
                } else {
                    gain_ *= releaseStep_; // constant dB per sample
                    if (gain_ <= std::max(floorGain_, 1e-5f)) {
                        gain_ = floorGain_;
                        state_ = State::Idle;
                    }
                }
                break;
        }
        return gain_;
    }

    float apply(float sample) const noexcept {
        return enabled_ ? sample * gain_ : sample;
    }

    float process(float sample) noexcept {
        return sample * detect(sample);
    }

    float currentGain() const noexcept { return enabled_ ? gain_ : 1.0f; }
    float envelope() const noexcept { return envelope_; }
    State state() const noexcept { return state_; }
    float openThreshold() const noexcept { return openThreshold_; }
    float closeThreshold() const noexcept { return closeThreshold_; }

    void adoptRuntimeStateFrom(const HystereticNoiseGate& previous) noexcept {
        envelope_ = previous.envelope_;
        gain_ = previous.gain_;
        state_ = previous.state_;
        holdCounter_ = std::min(previous.holdCounter_, holdSamplesMax_);
        if (!enabled_) { state_ = State::Triggered; gain_ = 1.0f; }
    }

private:
    static float coefficient(double sampleRate, double seconds) noexcept {
        return static_cast<float>(-std::expm1(-1.0 / (sampleRate * seconds)));
    }
    void updateThresholds() noexcept {
        closeThreshold_ = openThreshold_ * hysteresisRatio_;
    }
    void updateTiming() noexcept {
        holdSamplesMax_ = static_cast<uint32_t>(std::max(1.0, sr_ * holdSeconds_));
        releaseStep_ = static_cast<float>(std::pow(10.0, -releaseDbPerSecond_ / (20.0 * sr_)));
    }

    double sr_ = 48000.0;
    bool enabled_ = false;
    State state_ = State::Idle;
    float openThreshold_ = 0.0009f;
    float closeThreshold_ = 0.0009f * 0.25118864f; // -12 dB hysteresis
    float hysteresisRatio_ = 0.25118864f;
    float floorGain_ = 0.0f;
    float envelope_ = 0.0f;
    float gain_ = 1.0f;
    float envAttack_ = 1.0f, envRelease_ = 1.0f;
    float gainAttack_ = 1.0f;
    double holdSeconds_ = 0.035, releaseDbPerSecond_ = 400.0; // -60 dB in 150 ms
    float releaseStep_ = 0.9999f;
    uint32_t holdSamplesMax_ = 1680;
    uint32_t holdCounter_ = 0;
};

// Utilities around the amplifier, not part of its electrical circuit. Defaults
// deliberately leave a decaying guitar signal untouched. The voltage scale is
// an explicitly assumed software reference unless the caller supplies measured
// interface data; it must not be described as automatic calibration.
class AmpInputUtilities {
public:
    void prepare(double sampleRate) noexcept {
        sampleRate = std::isfinite(sampleRate) && sampleRate >= 8000 ? sampleRate : 48000;
        gate_.prepare(sampleRate);
        gate_.setThresholdLinear(0.0009f, 0.25118864f); // -12 dB hysteresis window
        reset();
    }

    void reset() noexcept { gate_.reset(); }
    void setGateEnabled(bool enabled) noexcept {
        gateEnabled_ = enabled;
        gate_.setEnabled(enabled);
    }
    bool gateEnabled() const noexcept { return gateEnabled_; }

    bool setVoltageScale(circuit::VoltageCalibration scale) noexcept {
        if (!scale.valid() || scale.voltsPerDigitalFullScale > 100.0) return false;
        scale_ = scale;
        return true;
    }
    const circuit::VoltageCalibration& voltageScale() const noexcept { return scale_; }

    // The gate detector is in digital-input units, before conversion to volts.
    // Disabled means exact unity immediately; turning it off cannot truncate a
    // note while a hidden release envelope slowly opens.
    float processDigital(float input) noexcept {
        if (!std::isfinite(input)) return 0;
        const float g = gate_.detect(input);
        if (!gateEnabled_) return input;
        return input * g;
    }

    double processVolts(float input) noexcept { return scale_.toVolts(processDigital(input)); }

    float currentGateGain() const noexcept { return gate_.currentGain(); }

    // Called at an audio-thread handoff; retain detector history but preserve
    // the new configuration, including the explicit gate/voltage selection.
    void adoptRuntimeStateFrom(const AmpInputUtilities& previous) noexcept {
        gate_.adoptRuntimeStateFrom(previous.gate_);
    }

private:
    circuit::VoltageCalibration scale_{};
    bool gateEnabled_ = false;
    HystereticNoiseGate gate_{};
};

} // namespace rig::amp
