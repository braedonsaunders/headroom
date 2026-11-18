#pragma once
// Tube saturation compressors: 159000 "Single-Knob Harmonic Triode Compressor" (face STC-159) and 160000
// "Dual-Stage Heated Triode Saturation Comp" (face STC-160 GT). Behavioural model of a single-knob tube leveler:
//
//   in → Input Trim → [Dirt / Saturation: triode stage] → gain cell → Character EQ (GT) → (wet ⟷ dry) → Output
//                                     └→ detector HP → RMS level → soft-knee gain computer → attack/release ─┘
//
// * Compress is the one big knob: it lowers the threshold and raises the ratio together, with automatic make-up, so
//   loudness stays roughly even while the compression goes from none (0) through a few dB on a guitar (0.5) to
//   pumping (1). Input Trim sets the level into the stage (the face's ±12 dB), Output is the make-up trim (±12 dB).
// * Mix follows the hardware it is modelled on: fully left = 100 % compressed (wet), turning right blends the dry
//   signal back in (parallel compression). The catalogue default is 0 = fully compressed, and the factory presets sit
//   between 0 and 0.45.
// * 159000: Punch (switch) = slower attack, transients pass; Dirt (switch) = triode overdrive on the input;
//   Sidechain (switch) = detector high-pass, so low strings don't pump the gain.
//   160000: Detector HP Off / 120 / 400 Hz, Saturation amount + Mild / Moderate / Hot, Attack 0.5–50 ms,
//   Release 50 ms–2 s, Character amount + Fat / Warm / Bright, Stereo Mode (the detector is linked in every mode).
// * liveReadout: the gain reduction, averaged like a meter (60 ms, like the bank's RMS) so the 12.5 Hz telemetry
//   poll sees a steady value instead of single-block flicks; the face adds the VU movement on top.
#include "../core/IComponent.hpp"
#include "DSPPrimitives.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace rig::dsp {

class TubeSatCompressorComponent final : public SchemaComponentBase {
public:
    using SchemaComponentBase::SchemaComponentBase;

    static bool handles(int32_t table_id) noexcept { return table_id == 159000 || table_id == 160000; }

    bool monoPreserving() const noexcept override { return true; } // linked detector, same arithmetic on both channels

    void adoptRuntimeStateFrom(IComponent& previous) noexcept override {
        auto* o = dynamic_cast<TubeSatCompressorComponent*>(&previous);
        if (!o || tableId() != o->tableId() || host_sr_ != o->host_sr_) return;
        ms_ = o->ms_; gr_ = o->gr_; gr_meter_ = o->gr_meter_; gr_pub_ = o->gr_pub_;
        for (int c = 0; c < 2; ++c) {
            det_hp_[c].adoptRuntimeStateFrom(o->det_hp_[c]);
            eq_lo_[c].adoptRuntimeStateFrom(o->eq_lo_[c]);
            eq_hi_[c].adoptRuntimeStateFrom(o->eq_hi_[c]);
            dc_x_[c] = o->dc_x_[c]; dc_y_[c] = o->dc_y_[c];
        }
        cur_ = o->cur_; tick_ = o->tick_;
        trim_g_ = o->trim_g_; out_g_ = o->out_g_; c_cached_ = o->c_cached_; makeup_db_ = o->makeup_db_;
    }

    void liveReadout(UnitLiveReadout& r) const noexcept override { r.gain_reduction_db = std::max(0.0f, gr_pub_); }

    bool paramDisplay(std::string_view id, std::string& out) const override {
        char b[48];
        const bool gt = tableId() == 160000;
        auto n = [&](std::string_view pid, float d) { return std::clamp(getParamOr(pid, d), 0.0f, 1.0f); };
        auto step = [&](std::string_view pid) { return std::clamp(static_cast<int>(std::lround(getParamOr(pid, 0.5f))), 0, 2); };
        if (id == (gt ? "W11" : "W1")) {
            const float m = n(id, gt ? 0.5f : 0.0f);
            if (m < 0.005f) std::snprintf(b, sizeof b, "100%% Wet");
            else if (m > 0.995f) std::snprintf(b, sizeof b, "100%% Dry");
            else std::snprintf(b, sizeof b, "%.0f%% Dry", 100.0f * m);
        } else if (id == (gt ? "I1" : "I7") || id == (gt ? "P12" : "P6")) {
            std::snprintf(b, sizeof b, "%+.1f dB", 24.0f * (n(id, 0.5f) - 0.5f));
        } else if (!gt && (id == "P8" || id == "D9" || id == "S4")) {
            std::snprintf(b, sizeof b, "%s", n(id, 0.5f) >= 0.5f ? "On" : "Off");
        } else if (gt && id == "D2") {
            static const char* k[3] = {"Off", "120 Hz", "400 Hz"};
            std::snprintf(b, sizeof b, "%s", k[step(id)]);
        } else if (gt && id == "S5") {
            static const char* k[3] = {"Mild", "Moderate", "Hot"};
            std::snprintf(b, sizeof b, "%s", k[step(id)]);
        } else if (gt && id == "C10") {
            static const char* k[3] = {"Fat", "Warm", "Bright"};
            std::snprintf(b, sizeof b, "%s", k[step(id)]);
        } else if (gt && id == "A7") {
            std::snprintf(b, sizeof b, "%.1f ms", attackMs(n(id, 0.5f)));
        } else if (gt && id == "R8") {
            const float ms = releaseMs(n(id, 0.5f));
            if (ms >= 1000.0f) std::snprintf(b, sizeof b, "%.2f s", ms / 1000.0f);
            else std::snprintf(b, sizeof b, "%.0f ms", ms);
        } else {
            return false;
        }
        out = b;
        return true;
    }

    // ---- the law, shared with the probes/tests ----
    // Compress 0..1 → threshold (dBFS, sine-peak-equivalent detector level), ratio, knee.
    static float thresholdDb(float c) noexcept { return -15.0f - 28.0f * c; }
    static float ratio(float c) noexcept { return 1.25f + 6.75f * std::pow(c, 1.4f); }
    static constexpr float kKneeDb = 12.0f;
    static float staticGrDb(float x_db, float c) noexcept {
        const float t = thresholdDb(c), r = ratio(c), w = kKneeDb, d = x_db - t;
        if (2.0f * d <= -w) return 0.0f;
        if (2.0f * d >= w) return d * (1.0f - 1.0f / r);
        const float k = d + 0.5f * w;
        return (1.0f - 1.0f / r) * k * k / (2.0f * w);
    }
    // Automatic make-up: most of the static reduction at a typical played guitar level, so turning Compress changes
    // the squash, not the loudness.
    static constexpr float kMakeupRefDb = -17.0f;
    static float makeupDb(float c) noexcept { return 0.85f * staticGrDb(kMakeupRefDb, c); }
    static float attackMs(float a) noexcept { return 0.5f * std::pow(100.0f, a); }    // 0.5 .. 50 ms
    static float releaseMs(float r) noexcept { return 50.0f * std::pow(40.0f, r); }   // 50 ms .. 2 s

protected:
    void onPrepare(double, uint32_t) override {
        onReset();
        onProcessControlRate();
        cur_ = tgt_;
    }

    void onReset() override {
        ms_ = 0.0f; gr_ = 0.0f; gr_meter_ = 0.0f; gr_pub_ = 0.0f; gr_peak_blk_ = 0.0f; tick_ = 0;
        for (int c = 0; c < 2; ++c) { det_hp_[c].reset(); eq_lo_[c].reset(); eq_hi_[c].reset(); dc_x_[c] = dc_y_[c] = 0.0f; }
    }

    void onProcessControlRate() override {
        const bool gt = tableId() == 160000;
        auto n = [&](std::string_view pid, float d) { return std::clamp(getParamOr(pid, d), 0.0f, 1.0f); };
        auto step = [&](std::string_view pid) { return std::clamp(static_cast<int>(std::lround(getParamOr(pid, 0.5f))), 0, 2); };
        const float sr = static_cast<float>(host_sr_);
        float hp_hz = 0.0f, att_ms = 6.0f, rel_ms = 240.0f;
        if (gt) {
            tgt_.compress = n("C6", 0.5f);
            tgt_.trim_db = 24.0f * (n("I1", 0.5f) - 0.5f);
            tgt_.out_db = 24.0f * (n("P12", 0.5f) - 0.5f);
            tgt_.mix_dry = n("W11", 0.5f);
            static constexpr float kSatDrive[3] = {1.6f, 3.2f, 6.5f}; // Mild / Moderate / Hot at full Saturation
            const float s = n("S4", 0.5f);
            tgt_.drive = 1.0f + (kSatDrive[step("S5")] - 1.0f) * s;
            att_ms = attackMs(n("A7", 0.5f));
            rel_ms = releaseMs(n("R8", 0.5f));
            static constexpr float kHp[3] = {0.0f, 120.0f, 400.0f};
            hp_hz = kHp[step("D2")];
            const float ch = n("C9", 0.5f);
            for (int c = 0; c < 2; ++c) {
                switch (step("C10")) {
                    case 0: eq_lo_[c].setLowShelf(host_sr_, 110.0f, 6.0f * ch); eq_hi_[c].setHighShelf(host_sr_, 6000.0f, -1.5f * ch); break; // Fat
                    case 1: eq_lo_[c].setPeakingEQ(host_sr_, 350.0f, 0.8f, 3.5f * ch); eq_hi_[c].setHighShelf(host_sr_, 4500.0f, -4.0f * ch); break; // Warm
                    default: eq_lo_[c].setLowShelf(host_sr_, 180.0f, -2.0f * ch); eq_hi_[c].setHighShelf(host_sr_, 3500.0f, 5.0f * ch); break; // Bright
                }
            }
            eq_on_ = ch > 0.002f;
        } else {
            tgt_.compress = n("M5", 0.5f);
            tgt_.trim_db = 24.0f * (n("I7", 0.5f) - 0.5f);
            tgt_.out_db = 24.0f * (n("P6", 0.5f) - 0.5f);
            tgt_.mix_dry = n("W1", 0.0f);
            tgt_.drive = n("D9", 0.5f) >= 0.5f ? 1.8f : 1.0f;
            att_ms = n("P8", 0.5f) >= 0.5f ? 20.0f : 5.0f; // Punch: a slower attack lets the pick attack through
            hp_hz = n("S4", 0.0f) >= 0.5f ? 150.0f : 0.0f;
            eq_on_ = false;
        }
        hp_on_ = hp_hz > 0.0f;
        if (hp_on_) for (auto& f : det_hp_) f.setHighPass(host_sr_, hp_hz, 0.7071f);
        a_att_ = 1.0f - std::exp(-1.0f / (0.001f * att_ms * sr));
        a_rel_ = 1.0f - std::exp(-1.0f / (0.001f * rel_ms * sr));
        a_rms_ = 1.0f - std::exp(-1.0f / (0.004f * sr));   // 4 ms mean square: the detector's own ripple filter
        a_meter_ = 1.0f - std::exp(-1.0f / (0.060f * sr)); // meter averaging
        a_smooth_ = 1.0f - std::exp(-1.0f / (0.020f * sr)); // knob smoothing
        dc_r_ = std::exp(-2.0f * 3.14159265f * 8.0f / sr);
    }

    void onProcessAudio(AudioBlock& block) override {
        const uint32_t n = block.num_frames;
        const bool stereo = block.num_channels > 1 && block.in[1] && block.out[1];
        if (block.hostBlockStart()) gr_peak_blk_ = 0.0f;
        for (uint32_t i = 0; i < n; ++i) {
            // Knob smoothing (no zipper on drags or automation).
            cur_.compress += a_smooth_ * (tgt_.compress - cur_.compress);
            cur_.trim_db += a_smooth_ * (tgt_.trim_db - cur_.trim_db);
            cur_.out_db += a_smooth_ * (tgt_.out_db - cur_.out_db);
            cur_.mix_dry += a_smooth_ * (tgt_.mix_dry - cur_.mix_dry);
            cur_.drive += a_smooth_ * (tgt_.drive - cur_.drive);
            if ((tick_++ & 15u) == 0) { // the derived gains move slowly: every 16 samples (a running count, so sub-blocks match)
                trim_g_ = dbToGain(cur_.trim_db);
                out_g_ = dbToGain(cur_.out_db);
                c_cached_ = cur_.compress;
                makeup_db_ = makeupDb(c_cached_);
            }
            float x[2];
            x[0] = block.in[0] ? block.in[0][i] : 0.0f;
            x[1] = stereo ? block.in[1][i] : x[0];
            float s[2];
            for (int c = 0; c < 2; ++c) {
                s[c] = triode(c, x[c] * trim_g_);
            }
            // Detector: linked, optionally high-passed, RMS over ~4 ms, read as a sine-peak-equivalent level.
            float d0 = s[0], d1 = s[1];
            if (hp_on_) { d0 = det_hp_[0].process(d0); d1 = det_hp_[1].process(d1); }
            const float e = std::max(d0 * d0, d1 * d1);
            ms_ += a_rms_ * (e - ms_);
            const float lvl_db = 10.0f * std::log10(2.0f * ms_ + 1e-12f);
            const float target = staticGrDb(lvl_db, c_cached_);
            gr_ += (target > gr_ ? a_att_ : a_rel_) * (target - gr_);
            gr_meter_ += a_meter_ * (gr_ - gr_meter_);
            const float g = dbToGain(makeup_db_ - gr_);
            for (int c = 0; c < 2; ++c) {
                float w = s[c] * g;
                if (eq_on_) w = eq_hi_[c].process(eq_lo_[c].process(w));
                const float y = (1.0f - cur_.mix_dry) * w + cur_.mix_dry * (x[c] * trim_g_);
                s[c] = y * out_g_;
            }
            block.out[0][i] = s[0];
            if (stereo) block.out[1][i] = s[1];
        }
        if (block.num_channels > 1 && block.out[1] && !stereo) std::copy(block.out[0], block.out[0] + n, block.out[1]);
        gr_peak_blk_ = std::max(gr_peak_blk_, gr_meter_);
        if (block.hostBlockEnd()) gr_pub_ = gr_peak_blk_;
    }

private:
    static float dbToGain(float db) noexcept { return std::exp(db * 0.11512925f); }

    // Triode stage: asymmetric (grid-current side clips first → even harmonics), unity small-signal gain, DC blocked.
    float triode(int c, float v) noexcept {
        const float d = cur_.drive;
        if (d <= 1.001f) return v;
        constexpr float kBias = 0.22f;
        const float tb = std::tanh(kBias);
        const float slope = d * (1.0f - tb * tb);
        const float y = (std::tanh(d * v + kBias) - tb) / slope;
        // DC blocker (asymmetric clipping shifts the mean)
        const float o = y - dc_x_[c] + dc_r_ * dc_y_[c];
        dc_x_[c] = y; dc_y_[c] = o;
        return o;
    }

    struct Knobs { float compress = 0.5f, trim_db = 0.0f, out_db = 0.0f, mix_dry = 0.0f, drive = 1.0f; };
    Knobs tgt_{}, cur_{};
    float trim_g_ = 1.0f, out_g_ = 1.0f, c_cached_ = 0.5f, makeup_db_ = 0.0f;
    float a_att_ = 0.0f, a_rel_ = 0.0f, a_rms_ = 0.0f, a_meter_ = 0.0f, a_smooth_ = 1.0f, dc_r_ = 0.999f;
    float ms_ = 0.0f, gr_ = 0.0f, gr_meter_ = 0.0f, gr_pub_ = 0.0f, gr_peak_blk_ = 0.0f;
    float dc_x_[2] = {0.0f, 0.0f}, dc_y_[2] = {0.0f, 0.0f};
    uint32_t tick_ = 0;
    bool hp_on_ = false, eq_on_ = false;
    BiquadFilter det_hp_[2], eq_lo_[2], eq_hi_[2];
};

} // namespace rig::dsp
