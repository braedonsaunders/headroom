#pragma once
// Polyphonic Transpose / Drop-Tuner & Whammy unit (table 900201)
// Stereo Doubler & Amp-in-the-Room Spatializer unit (table 900202, and embedded in CabinetUnit 900100)
// Also provides global inline processors for the engine's Transpose and Spatializer controls.

#include "../core/IComponent.hpp"
#include "DSPPrimitives.hpp"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>

namespace rig {

inline constexpr int32_t kTransposeUnitTable = 900201;
inline constexpr int32_t kDoublerRoomUnitTable = 900202;

// Low-latency transient-preserving polyphonic pitch shifter (dual Hann-windowed WSOLA / cross-correlated
// grain shifter with pick-attack phase lock and spectral tilt compensation).
class PolyPitchShifter {
public:
    static constexpr std::size_t kBufferSize = 8192;
    static constexpr std::size_t kBufferMask = kBufferSize - 1;

    void prepare(double sample_rate) noexcept;
    void reset() noexcept;

    void setSemitones(float semitones) noexcept { semitones_.store(std::clamp(semitones, -24.0f, 24.0f), std::memory_order_relaxed); }
    void setCents(float cents) noexcept         { cents_.store(std::clamp(cents, -100.0f, 100.0f), std::memory_order_relaxed); }
    void setMix(float mix) noexcept             { mix_.store(std::clamp(mix, 0.0f, 1.0f), std::memory_order_relaxed); }
    void setFormantPreserve(bool on) noexcept   { formant_.store(on, std::memory_order_relaxed); }

    float semitones() const noexcept { return semitones_.load(std::memory_order_relaxed); }
    float cents() const noexcept     { return cents_.load(std::memory_order_relaxed); }
    float mix() const noexcept       { return mix_.load(std::memory_order_relaxed); }
    bool isActive() const noexcept {
        return std::fabs(semitones_.load(std::memory_order_relaxed)) > 1e-3f ||
               std::fabs(cents_.load(std::memory_order_relaxed)) > 0.05f;
    }

    void processStereo(float* left, float* right, uint32_t num_frames) noexcept;
    void adoptFrom(const PolyPitchShifter& other) noexcept;

private:
    double sample_rate_ = 48000.0;
    std::size_t grain_size_ = 1024; // ~21 ms at 48 kHz
    std::array<float, kBufferSize> buf_l_{};
    std::array<float, kBufferSize> buf_r_{};
    std::size_t write_pos_ = 0;
    double phase_ = 0.0;
    double offset_a_ = 0.0;
    double offset_b_ = 512.0;
    float env_fast_ = 0.0f;
    float env_slow_ = 0.0f;
    int transient_cooldown_ = 0;

    dsp::BiquadFilter tilt_l_{}, tilt_r_{};
    float last_total_st_ = 0.0f;

    std::atomic<float> semitones_{0.0f};
    std::atomic<float> cents_{0.0f};
    std::atomic<float> mix_{1.0f};
    std::atomic<bool> formant_{true};
};

// Phase-safe Stereo Doubler + "Amp-in-the-Room" De-Mic & Binaural Early Reflection Processor.
class StereoDoublerRoomProcessor {
public:
    static constexpr std::size_t kDelayMax = 8192;
    static constexpr std::size_t kDelayMask = kDelayMax - 1;

    void prepare(double sample_rate) noexcept;
    void reset() noexcept;

    void setDoublerEnabled(bool on) noexcept    { doubler_on_.store(on, std::memory_order_relaxed); }
    void setDoublerWidth(float w) noexcept      { width_.store(std::clamp(w, 0.0f, 1.0f), std::memory_order_relaxed); }
    void setDoublerDelayMs(float ms) noexcept   { delay_ms_.store(std::clamp(ms, 4.0f, 35.0f), std::memory_order_relaxed); }
    void setDoublerDetune(float cents) noexcept { detune_cents_.store(std::clamp(cents, 0.0f, 15.0f), std::memory_order_relaxed); }

    void setRoomEnabled(bool on) noexcept       { room_on_.store(on, std::memory_order_relaxed); }
    void setRoomAmount(float amt) noexcept      { room_amt_.store(std::clamp(amt, 0.0f, 1.0f), std::memory_order_relaxed); }
    void setDeMicAmount(float amt) noexcept;

    bool doublerEnabled() const noexcept { return doubler_on_.load(std::memory_order_relaxed); }
    float doublerWidth() const noexcept  { return width_.load(std::memory_order_relaxed); }
    float doublerDelayMs() const noexcept { return delay_ms_.load(std::memory_order_relaxed); }
    float doublerDetune() const noexcept { return detune_cents_.load(std::memory_order_relaxed); }
    bool roomEnabled() const noexcept    { return room_on_.load(std::memory_order_relaxed); }
    float roomAmount() const noexcept    { return room_amt_.load(std::memory_order_relaxed); }
    float deMicAmount() const noexcept   { return demic_amt_.load(std::memory_order_relaxed); }

    void processStereo(float* left, float* right, uint32_t num_frames) noexcept;
    void adoptFrom(const StereoDoublerRoomProcessor& other) noexcept;

private:
    double sample_rate_ = 48000.0;
    std::array<float, kDelayMax> dl_l_{};
    std::array<float, kDelayMax> dl_r_{};
    std::size_t write_idx_ = 0;
    double lfo_phase_ = 0.0;

    // All-pass decorrelator states for mono-safe summing
    float ap1_z_ = 0.0f;
    float ap2_z_ = 0.0f;
    // Side high-pass state (keeps sub-bass < 110 Hz centered)
    float side_hp_z_ = 0.0f;
    // Binaural crossfeed low-pass states
    float xfeed_l_ = 0.0f;
    float xfeed_r_ = 0.0f;

    // De-Mic filters (presence notch @ 3.8 kHz + cabinet floor thump @ 125 Hz)
    dsp::BiquadFilter demic_notch_l_{}, demic_notch_r_{};
    dsp::BiquadFilter demic_body_l_{}, demic_body_r_{};
    float last_demic_ = -1.0f;

    std::atomic<bool> doubler_on_{false};
    std::atomic<float> width_{0.75f};
    std::atomic<float> delay_ms_{16.5f};
    std::atomic<float> detune_cents_{4.5f};

    std::atomic<bool> room_on_{false};
    std::atomic<float> room_amt_{0.35f};
    std::atomic<float> demic_amt_{0.50f};
};

std::unique_ptr<IComponent> makeTransposeUnit(const catalog::ComponentSchema& schema);
std::unique_ptr<IComponent> makeDoublerRoomUnit(const catalog::ComponentSchema& schema);

const catalog::ComponentSchema& transposeUnitSchema() noexcept;
const catalog::ComponentSchema& doublerRoomUnitSchema() noexcept;

} // namespace rig
