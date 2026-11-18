#pragma once
// Output level knobs that are a plain gain after everything else in the unit (the unit's output scaled, nothing
// else): the SSL channel EQ's Output (121000 O14), the Lexicon 224XL / 480L Output (154000 O22, 155000 O45) and the
// Shimmer / Multi-Mode delays' Output (157000, 158000 "output"). One law per knob, used by the DSP (OutputTrim) and
// by the level folding (core/LevelFold.hpp), so the two can never disagree.
//
// Every law is exactly 0 dB at the knob's catalogue default, so a rig that never moved the knob sounds bit-for-bit
// as it did before the knob was wired (tests/test_level_fold.cpp renders the null).
//   * SSL 4000 E channel EQ Output: -18..+18 dB, dB-linear, 0.5 = 0 dB (the range the unit's model always had;
//     the 82E242 card itself has no output control, so it is applied after the card, like the channel's trim).
//   * Lexicon 224XL / 480L Output: -12..+12 dB, dB-linear, 0.5 = 0 dB (the mainframe's output level).
//   * Shimmer Delay / Multi-Mode Delay Output: dB-linear through the catalogue default (0 dB) to +12 dB at the top
//     of the travel (157000: -36.5 dB at 0, 158000: -31.2 dB at 0). Stored presets carry the default rounded to
//     4 decimals (0.7524 / 0.7224): within 1e-4 of the default the knob reads exactly 0 dB.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace rig::dsp {

struct OutputTrimLaw {
    int32_t table_id;
    const char* param;
    double (*db)(double v); // the unit's output gain (dB) at raw (stored) value v
    float def;              // catalogue default (0 dB)
};

namespace trimlaw {
inline double ssl(double v) { return -18.0 + 36.0 * std::clamp(v, 0.0, 1.0); }
inline double lexicon(double v) { return 24.0 * (std::clamp(v, 0.0, 1.0) - 0.5); }
inline double towardTop(double v, double def) {
    const double c = std::clamp(v, 0.0, 1.0);
    if (std::abs(c - def) < 1e-4) return 0.0;
    return 12.0 * (c - def) / (1.0 - def);
}
inline double shimmer(double v) { return towardTop(v, 0.752405); }    // 157000 catalogue default
inline double multiMode(double v) { return towardTop(v, 0.722405); }  // 158000 catalogue default
} // namespace trimlaw

inline const OutputTrimLaw* outputTrimLaw(int32_t table_id) noexcept {
    static constexpr OutputTrimLaw kLaws[] = {
        {121000, "O14", trimlaw::ssl, 0.5f},
        {154000, "O22", trimlaw::lexicon, 0.5f},
        {155000, "O45", trimlaw::lexicon, 0.5f},
        {157000, "output", trimlaw::shimmer, 0.752405f},
        {158000, "output", trimlaw::multiMode, 0.722405f},
    };
    for (const auto& l : kLaws)
        if (l.table_id == table_id) return &l;
    return nullptr;
}

// The gain itself: set on the control thread (setDb), applied on the audio thread to the unit's finished output,
// gliding ~10 ms between settings. At 0 dB with nothing gliding it touches no sample.
class OutputTrim {
public:
    void prepare(double sr) noexcept { glide_ = 1.0f - std::exp(-1.0f / (0.010f * static_cast<float>(sr))); }
    void setDb(double db) noexcept { target_ = db == 0.0 ? 1.0f : static_cast<float>(std::pow(10.0, db / 20.0)); }
    void reset() noexcept { cur_ = -1.0f; } // the next block starts at the setting (no glide in from unity)
    // A prepared edit replacing a playing unit: glide on from what was playing.
    void adoptRuntimeStateFrom(const OutputTrim& o) noexcept { if (o.cur_ >= 0.0f) cur_ = o.cur_; }
    void process(float* const* out, uint32_t nch, uint32_t nf) noexcept {
        if (cur_ < 0.0f) cur_ = target_;
        if (cur_ == target_) {
            if (target_ == 1.0f) return;
            for (uint32_t c = 0; c < nch; ++c) {
                if (!out[c] || (c > 0 && out[c] == out[0])) continue;
                for (uint32_t i = 0; i < nf; ++i) out[c][i] *= target_;
            }
            return;
        }
        float g = cur_;
        for (uint32_t i = 0; i < nf; ++i) {
            g += (target_ - g) * glide_;
            if (std::abs(target_ - g) < 1e-6f) g = target_;
            for (uint32_t c = 0; c < nch; ++c)
                if (out[c] && !(c > 0 && out[c] == out[0])) out[c][i] *= g;
        }
        cur_ = g;
    }

private:
    float target_ = 1.0f, cur_ = -1.0f, glide_ = 0.002f;
};

} // namespace rig::dsp
