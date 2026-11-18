#pragma once
#include "DSPPrimitives.hpp"
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

namespace rig::dsp {

/**
 * Modulated fractional delay line with linear interpolation for Studio Multi-Mode Delay,
 * Dual-Pitch Shimmer Delay, Roland RE-201 Space Echo, Ping-Pong Dual Delay, and Roland Dimension D.
 */
class FractionalDelayLine {
public:
    void prepare(double sample_rate, float max_delay_seconds = 2.5f) {
        const std::size_t cap = static_cast<std::size_t>(std::ceil(sample_rate * max_delay_seconds)) + 16;
        buffer_.assign(cap, 0.0f);
        write_idx_ = 0;
    }

    void reset() noexcept {
        std::fill(buffer_.begin(), buffer_.end(), 0.0f);
        write_idx_ = 0;
    }

    void push(float x) noexcept {
        if (buffer_.empty()) return;
        buffer_[write_idx_] = x;
        write_idx_ = (write_idx_ + 1) % buffer_.size();
    }

    float read(float delay_samples) const noexcept {
        if (buffer_.empty()) return 0.0f;
        const float cap = static_cast<float>(buffer_.size());
        const float clamped = std::clamp(delay_samples, 1.0f, cap - 3.0f);
        float read_pos = static_cast<float>(write_idx_) - clamped;
        while (read_pos < 0.0f) read_pos += cap;
        const auto i0 = static_cast<std::size_t>(read_pos) % buffer_.size();
        const auto i1 = (i0 + 1) % buffer_.size();
        const float frac = read_pos - std::floor(read_pos);
        return buffer_[i0] + frac * (buffer_[i1] - buffer_[i0]);
    }

private:
    std::vector<float> buffer_;
    std::size_t write_idx_ = 0;
};

/**
 * Feedback Delay Network (FDN) + Schroeder Allpass Diffuser for Cosmic FDN Reverb (148000),
 * Classic Room Reverb (33000), Crystalline Reverb (97000), and Octave Shimmer Reverb (95000).
 */
class CosmicFDNReverb {
public:
    void adoptRuntimeStateFrom(CosmicFDNReverb& previous) noexcept {
        if (sr_ != previous.sr_) return;
        std::swap(lines_, previous.lines_);
        lfo_phase_ = previous.lfo_phase_;
        for (std::size_t i = 0; i < damp_.size(); ++i)
            damp_[i].adoptRuntimeStateFrom(previous.damp_[i]);
        if (previous.size_cur_ >= 0.0f) { // Size / Decay / Mix glide on from what was playing
            size_cur_ = previous.size_cur_;
            feedback_cur_ = previous.feedback_cur_;
            mix_cur_ = previous.mix_cur_;
        }
    }

    void prepare(double sample_rate) {
        sr_ = sample_rate > 1000.0 ? sample_rate : 48000.0;
        glide_ = 1.0f - std::exp(-1.0f / (0.030f * static_cast<float>(sr_))); // ~30 ms: a Size drag bends, never jumps
        constexpr std::array<float, 4> kTimesSec{0.0297f, 0.0371f, 0.0411f, 0.0437f};
        for (std::size_t i = 0; i < 4; ++i) {
            lines_[i].prepare(sr_, 0.25f);
            base_delays_[i] = static_cast<float>(sr_ * kTimesSec[i]);
            damp_[i].setLowPass(sr_, 4200.0f, 0.707f);
        }
        reset();
    }

    void reset() noexcept {
        for (auto& l : lines_) l.reset();
        for (auto& d : damp_) d.reset();
        lfo_phase_ = 0.0f;
    }

    void setParameters(float size_norm, float decay_norm, float damp_norm, float mod_norm) noexcept {
        size_scale_ = 0.4f + 1.4f * std::clamp(size_norm, 0.0f, 1.0f);
        feedback_ = 0.55f + 0.41f * std::clamp(decay_norm, 0.0f, 1.0f);
        mod_depth_ = 12.0f * std::clamp(mod_norm, 0.0f, 1.0f);
        const float fc = 1200.0f + 10000.0f * (1.0f - std::clamp(damp_norm, 0.0f, 1.0f));
        for (auto& d : damp_) d.setLowPass(sr_, fc, 0.707f);
    }

    void processStereo(float in_l, float in_r, float mix, float& out_l, float& out_r) noexcept {
        lfo_phase_ += static_cast<float>(0.65 / sr_);
        if (lfo_phase_ >= 1.0f) lfo_phase_ -= 1.0f;
        const float mod = std::sin(2.0f * std::numbers::pi_v<float> * lfo_phase_) * mod_depth_;
        if (size_cur_ < 0.0f) { size_cur_ = size_scale_; feedback_cur_ = feedback_; mix_cur_ = mix; }
        size_cur_ += (size_scale_ - size_cur_) * glide_;
        feedback_cur_ += (feedback_ - feedback_cur_) * glide_;
        mix_cur_ += (mix - mix_cur_) * glide_;
        const float feedback = feedback_cur_;
        mix = mix_cur_;

        std::array<float, 4> r{};
        for (std::size_t i = 0; i < 4; ++i) {
            const float d_samp = base_delays_[i] * size_cur_ + ((i & 1) ? mod : -mod);
            r[i] = damp_[i].process(lines_[i].read(d_samp));
        }

        // Hadamard 4x4 orthogonal mixing matrix * 0.5
        const float s0 = r[0] + r[1] + r[2] + r[3];
        const float s1 = r[0] - r[1] + r[2] - r[3];
        const float s2 = r[0] + r[1] - r[2] - r[3];
        const float s3 = r[0] - r[1] - r[2] + r[3];

        lines_[0].push(in_l + 0.5f * feedback * s0);
        lines_[1].push(in_r + 0.5f * feedback * s1);
        lines_[2].push(in_l + 0.5f * feedback * s2);
        lines_[3].push(in_r + 0.5f * feedback * s3);

        const float wet_l = 0.5f * (r[0] + r[2]);
        const float wet_r = 0.5f * (r[1] + r[3]);
        out_l = (1.0f - mix) * in_l + mix * wet_l;
        out_r = (1.0f - mix) * in_r + mix * wet_r;
    }

private:
    double sr_ = 48000.0;
    std::array<FractionalDelayLine, 4> lines_{};
    std::array<BiquadFilter, 4> damp_{};
    std::array<float, 4> base_delays_{1425.0f, 1780.0f, 1972.0f, 2097.0f};
    float size_scale_ = 1.0f;
    float feedback_ = 0.82f;
    float mod_depth_ = 4.0f;
    float lfo_phase_ = 0.0f;
    float glide_ = 0.0007f;
    float size_cur_ = -1.0f, feedback_cur_ = 0.82f, mix_cur_ = 0.0f; // smoothed (size_cur_ < 0 = not started)
};

} // namespace rig::dsp
