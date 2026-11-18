#pragma once
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <array>

namespace rig::dsp {

/**
 * First-order exponential parameter smoother (~5-10 ms time constant).
 */
class ParameterSmoother {
public:
    void prepare(double sample_rate, float time_ms = 5.0f, float initial_value = 0.0f) noexcept {
        sample_rate_ = sample_rate > 1000.0 ? sample_rate : 48000.0;
        setTimeConstantMs(time_ms);
        current_ = initial_value;
        target_ = initial_value;
    }

    void setTimeConstantMs(float time_ms) noexcept {
        const float tau = std::max(0.0001f, time_ms * 0.001f);
        coeff_ = 1.0f - std::exp(-1.0f / static_cast<float>(tau * sample_rate_));
    }

    void reset(float value) noexcept {
        current_ = value;
        target_ = value;
    }

    void setTarget(float target) noexcept {
        target_ = target;
    }

    float getTarget() const noexcept { return target_; }
    float getCurrent() const noexcept { return current_; }

    float next() noexcept {
        current_ += coeff_ * (target_ - current_);
        return current_;
    }

    bool isSmoothing(float eps = 1e-5f) const noexcept {
        return std::fabs(target_ - current_) > eps;
    }

private:
    double sample_rate_ = 48000.0;
    float coeff_ = 0.01f;
    float current_ = 0.0f;
    float target_ = 0.0f;
};

/**
 * Adaptive power-of-two internal sample rate calculator:
 *
 * Oversampling exponent k in {0, 1, 2, 3}:
 *   f_target = 44100.0 * 2^k
 *   M = 2^round(log2(f_target / host_sr))
 *   internal_sr = host_sr * M
 */
struct InternalRateConfig {
    double host_sr = 48000.0;
    double internal_sr = 48000.0;
    uint32_t oversampling_factor = 1; // 1, 2, 4, 8

    static InternalRateConfig forModernDSP(double host_sr, int32_t k) noexcept {
        InternalRateConfig cfg{};
        cfg.host_sr = host_sr > 1000.0 ? host_sr : 48000.0;
        if (k < 0) {
            cfg.oversampling_factor = 1;
            cfg.internal_sr = cfg.host_sr;
            return cfg;
        }
        const double target_sr = 44100.0 * static_cast<double>(1 << std::clamp(k, 0, 4));
        const double ratio_log2 = std::log2(target_sr / cfg.host_sr);
        const int32_t shift = std::clamp(static_cast<int32_t>(std::llround(ratio_log2)), 0, 3);
        cfg.oversampling_factor = static_cast<uint32_t>(1 << shift);
        cfg.internal_sr = cfg.host_sr * static_cast<double>(cfg.oversampling_factor);
        return cfg;
    }

    static InternalRateConfig forLegacyCircuitAmp(double host_sr) noexcept {
        InternalRateConfig cfg{};
        cfg.host_sr = host_sr > 1000.0 ? host_sr : 48000.0;
        cfg.oversampling_factor = 4;
        cfg.internal_sr = cfg.host_sr * 4.0;
        return cfg;
    }
};

/**
 * Polyphase half-band allpass cascade for 2x, 4x, or 8x oversampling.
 * Uses canonical unity-gain first-order allpass sections H(z) = (a + z^-1) / (1 + a z^-1).
 */
class HalfBandStage {
public:
    void reset() noexcept {
        a0_state_ = {0.0f, 0.0f};
        a1_state_ = {0.0f, 0.0f};
    }

    // Upsample 1 sample -> 2 samples
    void upsample(float x, float& y0, float& y1) noexcept {
        y0 = processBranch0(x);
        y1 = processBranch1(x);
    }

    // Downsample 2 samples -> 1 sample
    float downsample(float x0, float x1) noexcept {
        const float b0 = processBranch0(x1);
        const float b1 = processBranch1(x0);
        return 0.5f * (b0 + b1);
    }

private:
    // Steep 4th-order polyphase allpass coefficients
    static constexpr float kA0_0 = 0.079866426f;
    static constexpr float kA0_1 = 0.545353651f;
    static constexpr float kA1_0 = 0.283829344f;
    static constexpr float kA1_1 = 0.834411891f;

    float processBranch0(float x) noexcept {
        float y = kA0_0 * (x - a0_state_[0]) + a0_state_[0];
        a0_state_[0] = x + kA0_0 * (x - a0_state_[0]);
        float y2 = kA0_1 * (y - a0_state_[1]) + a0_state_[1];
        a0_state_[1] = y + kA0_1 * (y - a0_state_[1]);
        return y2;
    }

    float processBranch1(float x) noexcept {
        float y = kA1_0 * (x - a1_state_[0]) + a1_state_[0];
        a1_state_[0] = x + kA1_0 * (x - a1_state_[0]);
        float y2 = kA1_1 * (y - a1_state_[1]) + a1_state_[1];
        a1_state_[1] = y + kA1_1 * (y - a1_state_[1]);
        return y2;
    }

    std::array<float, 2> a0_state_{0.0f, 0.0f};
    std::array<float, 2> a1_state_{0.0f, 0.0f};
};

} // namespace rig::dsp
