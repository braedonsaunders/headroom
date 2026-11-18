#pragma once
#include "../core/AudioBlock.hpp"
#include "../core/IComponent.hpp"
#include "../core/SmoothingToolbox.hpp"
#include <array>
#include <cmath>
#include <numbers>
#include <string_view>
#include <vector>

namespace rig::dsp {

/**
 * Standard Audio EQ Cookbook / Cytomic SVF biquad filter used across ClassicDSP,
 * CircuitWaveshaper tone stacks, and amp pre/post conditioning.
 */
class BiquadFilter {
public:
    void adoptRuntimeStateFrom(const BiquadFilter& previous) noexcept {
        z1_ = previous.z1_;
        z2_ = previous.z2_;
    }

    void reset() noexcept {
        z1_ = 0.0f;
        z2_ = 0.0f;
    }

    void setLowPass(double sr, float cutoff_hz, float q = 0.7071f) noexcept {
        const float w0 = 2.0f * std::numbers::pi_v<float> * std::clamp(cutoff_hz, 10.0f, static_cast<float>(sr * 0.49));
        const float omega = w0 / static_cast<float>(sr);
        const float sn = std::sin(omega);
        const float cs = std::cos(omega);
        const float alpha = sn / (2.0f * std::max(0.1f, q));
        const float b0 = (1.0f - cs) * 0.5f;
        const float b1 = 1.0f - cs;
        const float b2 = (1.0f - cs) * 0.5f;
        const float a0 = 1.0f + alpha;
        const float a1 = -2.0f * cs;
        const float a2 = 1.0f - alpha;
        setNormalized(b0, b1, b2, a0, a1, a2);
    }

    void setHighPass(double sr, float cutoff_hz, float q = 0.7071f) noexcept {
        const float w0 = 2.0f * std::numbers::pi_v<float> * std::clamp(cutoff_hz, 10.0f, static_cast<float>(sr * 0.49));
        const float omega = w0 / static_cast<float>(sr);
        const float sn = std::sin(omega);
        const float cs = std::cos(omega);
        const float alpha = sn / (2.0f * std::max(0.1f, q));
        const float b0 = (1.0f + cs) * 0.5f;
        const float b1 = -(1.0f + cs);
        const float b2 = (1.0f + cs) * 0.5f;
        const float a0 = 1.0f + alpha;
        const float a1 = -2.0f * cs;
        const float a2 = 1.0f - alpha;
        setNormalized(b0, b1, b2, a0, a1, a2);
    }

    void setPeakingEQ(double sr, float freq_hz, float q, float gain_db) noexcept {
        const float A = std::pow(10.0f, gain_db / 40.0f);
        const float w0 = 2.0f * std::numbers::pi_v<float> * std::clamp(freq_hz, 15.0f, static_cast<float>(sr * 0.49));
        const float omega = w0 / static_cast<float>(sr);
        const float sn = std::sin(omega);
        const float cs = std::cos(omega);
        const float alpha = sn / (2.0f * std::max(0.1f, q));
        const float b0 = 1.0f + alpha * A;
        const float b1 = -2.0f * cs;
        const float b2 = 1.0f - alpha * A;
        const float a0 = 1.0f + alpha / A;
        const float a1 = -2.0f * cs;
        const float a2 = 1.0f - alpha / A;
        setNormalized(b0, b1, b2, a0, a1, a2);
    }

    void setLowShelf(double sr, float freq_hz, float gain_db) noexcept {
        const float A = std::pow(10.0f, gain_db / 40.0f);
        const float w0 = 2.0f * std::numbers::pi_v<float> * std::clamp(freq_hz, 15.0f, static_cast<float>(sr * 0.49));
        const float omega = w0 / static_cast<float>(sr);
        const float sn = std::sin(omega);
        const float cs = std::cos(omega);
        const float alpha = sn * 0.5f * std::sqrt(2.0f);
        const float two_sqrtA_alpha = 2.0f * std::sqrt(A) * alpha;
        const float b0 = A * ((A + 1.0f) - (A - 1.0f) * cs + two_sqrtA_alpha);
        const float b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cs);
        const float b2 = A * ((A + 1.0f) - (A - 1.0f) * cs - two_sqrtA_alpha);
        const float a0 = (A + 1.0f) + (A - 1.0f) * cs + two_sqrtA_alpha;
        const float a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cs);
        const float a2 = (A + 1.0f) + (A - 1.0f) * cs - two_sqrtA_alpha;
        setNormalized(b0, b1, b2, a0, a1, a2);
    }

    void setHighShelf(double sr, float freq_hz, float gain_db) noexcept {
        const float A = std::pow(10.0f, gain_db / 40.0f);
        const float w0 = 2.0f * std::numbers::pi_v<float> * std::clamp(freq_hz, 15.0f, static_cast<float>(sr * 0.49));
        const float omega = w0 / static_cast<float>(sr);
        const float sn = std::sin(omega);
        const float cs = std::cos(omega);
        const float alpha = sn * 0.5f * std::sqrt(2.0f);
        const float two_sqrtA_alpha = 2.0f * std::sqrt(A) * alpha;
        const float b0 = A * ((A + 1.0f) + (A - 1.0f) * cs + two_sqrtA_alpha);
        const float b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cs);
        const float b2 = A * ((A + 1.0f) + (A - 1.0f) * cs - two_sqrtA_alpha);
        const float a0 = (A + 1.0f) - (A - 1.0f) * cs + two_sqrtA_alpha;
        const float a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cs);
        const float a2 = (A + 1.0f) - (A - 1.0f) * cs - two_sqrtA_alpha;
        setNormalized(b0, b1, b2, a0, a1, a2);
    }

    float process(float x) noexcept {
        const float y = b0_ * x + z1_;
        z1_ = b1_ * x - a1_ * y + z2_;
        z2_ = b2_ * x - a2_ * y;
        return y;
    }

private:
    void setNormalized(float b0, float b1, float b2, float a0, float a1, float a2) noexcept {
        const float inv_a0 = 1.0f / a0;
        b0_ = b0 * inv_a0;
        b1_ = b1 * inv_a0;
        b2_ = b2 * inv_a0;
        a1_ = a1 * inv_a0;
        a2_ = a2 * inv_a0;
    }

    float b0_ = 1.0f, b1_ = 0.0f, b2_ = 0.0f;
    float a1_ = 0.0f, a2_ = 0.0f;
    float z1_ = 0.0f, z2_ = 0.0f;
};

/**
 * 512-entry uniformly-spaced interpolated asymmetric-triode lookup table.
 */
class WaveshaperLUT512 {
public:
    WaveshaperLUT512() noexcept {
        initAsymmetricTriode(0.18f, 1.4f);
    }

    void initAsymmetricTriode(float bias, float hardness) noexcept {
        input_scale_ = 1.0f;
        for (std::size_t i = 0; i < 512; ++i) {
            const float x = -4.0f + 8.0f * (static_cast<float>(i) / 511.0f);
            const float xb = x + bias;
            if (xb >= 0.0f) {
                table_[i] = std::tanh(hardness * xb);
            } else {
                table_[i] = 0.85f * std::tanh(0.8f * hardness * xb);
            }
        }
        const float dc = evaluateRaw(0.0f);
        for (float& v : table_) v -= dc;
    }

    float process(float x) const noexcept {
        return evaluateRaw(x * input_scale_);
    }

private:
    float evaluateRaw(float x) const noexcept {
        const float norm = std::clamp((x + 4.0f) * (511.0f / 8.0f), 0.0f, 510.999f);
        const auto idx = static_cast<std::size_t>(norm);
        const float frac = norm - static_cast<float>(idx);
        return table_[idx] + frac * (table_[idx + 1] - table_[idx]);
    }

    std::array<float, 512> table_{};
    float input_scale_ = 1.0f;
};

} // namespace rig::dsp
