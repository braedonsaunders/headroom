#include "PitchDoublerUnits.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace rig {

namespace {

using catalog::ParamKind;
using catalog::ParamSchema;

constexpr ParamSchema kTransposeParams[] = {
    {"semitones", "Transpose",  ParamKind::Float, 0.5f, 0.0f, 1.0f, "", ""}, // 0..1 -> -12..+12 st (0.5 = 0 st)
    {"cents",     "Fine Tune",  ParamKind::Float, 0.5f, 0.0f, 1.0f, "", ""}, // 0..1 -> -50..+50 ct (0.5 = 0 ct)
    {"pedal",     "Whammy",     ParamKind::Float, 0.0f, 0.0f, 1.0f, "", ""}, // 0..1 pedal sweep
    {"range",     "Pedal Mode", ParamKind::Float, 5.0f, 0.0f, 7.0f, "", ""}, // 0=-24, 1=-12, 2=-7, 3=-2, 4=+2, 5=+7, 6=+12, 7=+24
    {"formant",   "Tightness",  ParamKind::Bool,  1.0f, 0.0f, 1.0f, "", ""},
    {"mix",       "Mix",        ParamKind::Float, 1.0f, 0.0f, 1.0f, "", ""},
    {"Pwr",       "On/Off",     ParamKind::Float, 1.0f, 0.0f, 1.0f, "", ""},
};

constexpr ParamSchema kDoublerRoomParams[] = {
    {"doubler",   "Doubler",    ParamKind::Bool,  1.0f, 0.0f, 1.0f, "", ""},
    {"width",     "Width",      ParamKind::Float, 0.78f, 0.0f, 1.0f, "", ""},
    {"delay",     "Tightness",  ParamKind::Float, 0.45f, 0.0f, 1.0f, "", ""}, // maps to 6..28 ms
    {"detune",    "Detune",     ParamKind::Float, 0.35f, 0.0f, 1.0f, "", ""}, // maps to 0..12 cents
    {"room_on",   "Amp In Room",ParamKind::Bool,  1.0f, 0.0f, 1.0f, "", ""},
    {"demic",     "De-Mic Air", ParamKind::Float, 0.55f, 0.0f, 1.0f, "", ""},
    {"room",      "Room Space", ParamKind::Float, 0.35f, 0.0f, 1.0f, "", ""},
    {"Pwr",       "On/Off",     ParamKind::Float, 1.0f, 0.0f, 1.0f, "", ""},
};

inline float readCubic(const std::array<float, PolyPitchShifter::kBufferSize>& buf, double read_idx) noexcept {
    const double wrapped = std::fmod(read_idx, static_cast<double>(PolyPitchShifter::kBufferSize));
    const double pos = wrapped < 0.0 ? wrapped + static_cast<double>(PolyPitchShifter::kBufferSize) : wrapped;
    const std::size_t i1 = static_cast<std::size_t>(pos) & PolyPitchShifter::kBufferMask;
    const std::size_t i0 = (i1 + PolyPitchShifter::kBufferSize - 1) & PolyPitchShifter::kBufferMask;
    const std::size_t i2 = (i1 + 1) & PolyPitchShifter::kBufferMask;
    const std::size_t i3 = (i1 + 2) & PolyPitchShifter::kBufferMask;
    const float f = static_cast<float>(pos - std::floor(pos));

    const float y0 = buf[i0], y1 = buf[i1], y2 = buf[i2], y3 = buf[i3];
    const float c0 = y1;
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * f + c2) * f + c1) * f + c0;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// PolyPitchShifter
// ─────────────────────────────────────────────────────────────────────────────
void PolyPitchShifter::prepare(double sample_rate) noexcept {
    sample_rate_ = sample_rate > 8000.0 ? sample_rate : 48000.0;
    // Use ~22 ms grain window for low-latency polyphonic guitar chord stability
    grain_size_ = sample_rate_ > 64000.0 ? 2048 : 1024;
    reset();
}

void PolyPitchShifter::reset() noexcept {
    buf_l_.fill(0.0f);
    buf_r_.fill(0.0f);
    write_pos_ = 0;
    phase_ = 0.0;
    offset_a_ = static_cast<double>(grain_size_) * 0.5;
    offset_b_ = static_cast<double>(grain_size_);
    env_fast_ = 0.0f;
    env_slow_ = 0.0f;
    transient_cooldown_ = 0;
    tilt_l_.reset();
    tilt_r_.reset();
    last_total_st_ = -999.0f;
}

void PolyPitchShifter::adoptFrom(const PolyPitchShifter& other) noexcept {
    if (other.sample_rate_ != sample_rate_) return;
    buf_l_ = other.buf_l_;
    buf_r_ = other.buf_r_;
    write_pos_ = other.write_pos_;
    phase_ = other.phase_;
    offset_a_ = other.offset_a_;
    offset_b_ = other.offset_b_;
    env_fast_ = other.env_fast_;
    env_slow_ = other.env_slow_;
    transient_cooldown_ = other.transient_cooldown_;
    tilt_l_.adoptRuntimeStateFrom(other.tilt_l_);
    tilt_r_.adoptRuntimeStateFrom(other.tilt_r_);
}

void PolyPitchShifter::processStereo(float* left, float* right, uint32_t num_frames) noexcept {
    if (!left || !right || num_frames == 0) return;

    const float st = semitones_.load(std::memory_order_relaxed);
    const float ct = cents_.load(std::memory_order_relaxed);
    const float total_st = st + 0.01f * ct;
    const float wet_mix = mix_.load(std::memory_order_relaxed);

    // True zero-latency bypass when transpose is 0.0 and mix is 100%
    if (std::fabs(total_st) < 1e-3f && wet_mix >= 0.999f) {
        for (uint32_t i = 0; i < num_frames; ++i) {
            buf_l_[write_pos_] = left[i];
            buf_r_[write_pos_] = right[i];
            write_pos_ = (write_pos_ + 1) & kBufferMask;
        }
        return;
    }

    if ( std::fabs(total_st - last_total_st_) > 0.05f ) {
        last_total_st_ = total_st;
        // Compensate spectral brightness when drop-tuning or pitch-lifting so pick attack stays natural
        const float tilt_db = std::clamp(-total_st * 0.28f, -4.5f, 4.5f);
        tilt_l_.setHighShelf(sample_rate_, 2200.0, tilt_db);
        tilt_r_.setHighShelf(sample_rate_, 2200.0, tilt_db);
    }

    const double pitch_ratio = std::pow(2.0, static_cast<double>(total_st) / 12.0);
    const double delay_rate = 1.0 - pitch_ratio;
    const double gsize = static_cast<double>(grain_size_);
    const double phase_inc = 1.0 / gsize;
    const bool do_formant = formant_.load(std::memory_order_relaxed);

    // Search cross-correlation window (in samples) when splicing grains to lock fundamental phase
    auto findBestSpliceOffset = [&](double target_delay) noexcept -> double {
        const std::size_t ref_idx = (write_pos_ + kBufferSize - static_cast<std::size_t>(std::clamp(offset_a_, 16.0, gsize * 1.8))) & kBufferMask;
        const float ref0 = buf_l_[ref_idx];
        const float ref_prev = buf_l_[(ref_idx + kBufferSize - 4) & kBufferMask];
        const float ref_slope = ref0 - ref_prev;

        double best_d = target_delay;
        float best_err = 1e9f;
        for (int delta = -48; delta <= 48; delta += 4) {
            const double cand = std::clamp(target_delay + static_cast<double>(delta), 24.0, gsize * 1.9);
            const std::size_t c_idx = (write_pos_ + kBufferSize - static_cast<std::size_t>(cand)) & kBufferMask;
            const float c0 = buf_l_[c_idx];
            const float c_prev = buf_l_[(c_idx + kBufferSize - 4) & kBufferMask];
            const float err = std::fabs(c0 - ref0) + 1.5f * std::fabs((c0 - c_prev) - ref_slope);
            if (err < best_err) {
                best_err = err;
                best_d = cand;
            }
        }
        return best_d;
    };

    for (uint32_t i = 0; i < num_frames; ++i) {
        const float in_l = left[i];
        const float in_r = right[i];
        buf_l_[write_pos_] = in_l;
        buf_r_[write_pos_] = in_r;

        // Pick-attack transient detector: on sharp transient, re-anchor the inactive grain close to write_pos_
        const float abs_in = 0.5f * (std::fabs(in_l) + std::fabs(in_r));
        env_fast_ = 0.96f * env_fast_ + 0.04f * abs_in;
        env_slow_ = 0.9992f * env_slow_ + 0.0008f * abs_in;
        if (transient_cooldown_ > 0) --transient_cooldown_;

        const double prev_phase = phase_;
        phase_ += phase_inc;
        if (phase_ >= 1.0) {
            phase_ -= 1.0;
            // Grain A just hit zero weight -> reset Grain A delay & align phase with Grain B
            const double base_d = delay_rate >= 0.0 ? 32.0 : gsize * 1.25;
            std::swap(offset_a_, offset_b_); // keep roles consistent
            offset_a_ = findBestSpliceOffset(base_d);
        } else if (prev_phase < 0.5 && phase_ >= 0.5) {
            // Grain B just hit zero weight -> reset Grain B delay & align phase with Grain A
            const double base_d = delay_rate >= 0.0 ? 32.0 : gsize * 1.25;
            offset_b_ = findBestSpliceOffset(base_d);
        } else if (transient_cooldown_ == 0 && env_fast_ > 3.2f * env_slow_ + 0.008f) {
            // Sharp pick transient: snap whichever grain has lower weight toward minimum latency
            transient_cooldown_ = static_cast<int>(grain_size_ / 2);
            const double w_a = 0.5 * (1.0 - std::cos(2.0 * std::numbers::pi * phase_));
            if (w_a < 0.25) {
                offset_a_ = delay_rate >= 0.0 ? 24.0 : gsize * 0.85;
            } else if (w_a > 0.75) {
                offset_b_ = delay_rate >= 0.0 ? 24.0 : gsize * 0.85;
            }
        }

        offset_a_ = std::clamp(offset_a_ + delay_rate, 16.0, gsize * 2.5);
        offset_b_ = std::clamp(offset_b_ + delay_rate, 16.0, gsize * 2.5);

        // Complementary Hann weights (w_a + w_b == 1.0)
        const float w_a = static_cast<float>(0.5 * (1.0 - std::cos(2.0 * std::numbers::pi * phase_)));
        const float w_b = 1.0f - w_a;

        const double read_a = static_cast<double>(write_pos_) - offset_a_;
        const double read_b = static_cast<double>(write_pos_) - offset_b_;

        float wet_l = w_a * readCubic(buf_l_, read_a) + w_b * readCubic(buf_l_, read_b);
        float wet_r = w_a * readCubic(buf_r_, read_a) + w_b * readCubic(buf_r_, read_b);

        if (do_formant) {
            wet_l = tilt_l_.process(wet_l);
            wet_r = tilt_r_.process(wet_r);
        }

        left[i]  = (1.0f - wet_mix) * in_l + wet_mix * wet_l;
        right[i] = (1.0f - wet_mix) * in_r + wet_mix * wet_r;

        write_pos_ = (write_pos_ + 1) & kBufferMask;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// StereoDoublerRoomProcessor
// ─────────────────────────────────────────────────────────────────────────────
void StereoDoublerRoomProcessor::prepare(double sample_rate) noexcept {
    sample_rate_ = sample_rate > 8000.0 ? sample_rate : 48000.0;
    reset();
    const float cl = demic_amt_.load(std::memory_order_relaxed);
    last_demic_ = cl;
    demic_notch_l_.setPeakingEQ(sample_rate_, 3850.0f, 1.4f, -4.2f * cl);
    demic_notch_r_.setPeakingEQ(sample_rate_, 3850.0f, 1.4f, -4.2f * cl);
    demic_body_l_.setPeakingEQ(sample_rate_, 132.0f, 0.9f, 2.8f * cl);
    demic_body_r_.setPeakingEQ(sample_rate_, 132.0f, 0.9f, 2.8f * cl);
}

void StereoDoublerRoomProcessor::setDeMicAmount(float amt) noexcept {
    const float cl = std::clamp(amt, 0.0f, 1.0f);
    demic_amt_.store(cl, std::memory_order_relaxed);
    if (std::fabs(cl - last_demic_) > 1e-4f) {
        last_demic_ = cl;
        demic_notch_l_.setPeakingEQ(sample_rate_, 3850.0f, 1.4f, -4.2f * cl);
        demic_notch_r_.setPeakingEQ(sample_rate_, 3850.0f, 1.4f, -4.2f * cl);
        demic_body_l_.setPeakingEQ(sample_rate_, 132.0f, 0.9f, 2.8f * cl);
        demic_body_r_.setPeakingEQ(sample_rate_, 132.0f, 0.9f, 2.8f * cl);
    }
}

void StereoDoublerRoomProcessor::reset() noexcept {
    dl_l_.fill(0.0f);
    dl_r_.fill(0.0f);
    write_idx_ = 0;
    lfo_phase_ = 0.0;
    ap1_z_ = 0.0f;
    ap2_z_ = 0.0f;
    side_hp_z_ = 0.0f;
    xfeed_l_ = 0.0f;
    xfeed_r_ = 0.0f;
    demic_notch_l_.reset();
    demic_notch_r_.reset();
    demic_body_l_.reset();
    demic_body_r_.reset();
    last_demic_ = -1.0f;
}

void StereoDoublerRoomProcessor::adoptFrom(const StereoDoublerRoomProcessor& other) noexcept {
    if (other.sample_rate_ != sample_rate_) return;
    dl_l_ = other.dl_l_;
    dl_r_ = other.dl_r_;
    write_idx_ = other.write_idx_;
    lfo_phase_ = other.lfo_phase_;
    ap1_z_ = other.ap1_z_;
    ap2_z_ = other.ap2_z_;
    side_hp_z_ = other.side_hp_z_;
    xfeed_l_ = other.xfeed_l_;
    xfeed_r_ = other.xfeed_r_;
    demic_notch_l_.adoptRuntimeStateFrom(other.demic_notch_l_);
    demic_notch_r_.adoptRuntimeStateFrom(other.demic_notch_r_);
    demic_body_l_.adoptRuntimeStateFrom(other.demic_body_l_);
    demic_body_r_.adoptRuntimeStateFrom(other.demic_body_r_);
}

void StereoDoublerRoomProcessor::processStereo(float* left, float* right, uint32_t num_frames) noexcept {
    if (!left || !right || num_frames == 0) return;

    const bool dbl_on = doubler_on_.load(std::memory_order_relaxed);
    const bool rm_on = room_on_.load(std::memory_order_relaxed);
    if (!dbl_on && !rm_on) return;

    const float width = width_.load(std::memory_order_relaxed);
    const float delay_ms = delay_ms_.load(std::memory_order_relaxed);
    const float detune = detune_cents_.load(std::memory_order_relaxed);
    const float room_amt = room_amt_.load(std::memory_order_relaxed);
    const float demic = demic_amt_.load(std::memory_order_relaxed);

    if (rm_on && std::fabs(demic - last_demic_) > 0.01f) {
        last_demic_ = demic;
        // Soften close-mic SM57 3.8 kHz presence spike and restore 130 Hz live cabinet floor coupling
        demic_notch_l_.setPeakingEQ(sample_rate_, 3850.0f, 1.4f, -4.2f * demic);
        demic_notch_r_.setPeakingEQ(sample_rate_, 3850.0f, 1.4f, -4.2f * demic);
        demic_body_l_.setPeakingEQ(sample_rate_, 132.0f, 0.9f, 2.8f * demic);
        demic_body_r_.setPeakingEQ(sample_rate_, 132.0f, 0.9f, 2.8f * demic);
    }

    const double sr = sample_rate_;
    const double base_delay_samps = std::clamp((delay_ms * 0.001) * sr, 64.0, static_cast<double>(kDelayMax - 512));
    const double mod_depth_samps = (detune * 0.18) * (sr / 48000.0);
    const double lfo_inc = (0.85 * 2.0 * std::numbers::pi) / sr;
    const float hp_coeff = static_cast<float>(std::exp(-2.0 * std::numbers::pi * 115.0 / sr));
    const float xfeed_lp = static_cast<float>(1.0 - std::exp(-2.0 * std::numbers::pi * 680.0 / sr));

    // Early reflection tap offsets (in ms) for a 4x12 cab in a live wood/ plaster studio room
    const std::size_t er_l1 = static_cast<std::size_t>(0.0043 * sr) & kDelayMask;
    const std::size_t er_r1 = static_cast<std::size_t>(0.0061 * sr) & kDelayMask;
    const std::size_t er_l2 = static_cast<std::size_t>(0.0117 * sr) & kDelayMask;
    const std::size_t er_r2 = static_cast<std::size_t>(0.0149 * sr) & kDelayMask;
    const std::size_t er_l3 = static_cast<std::size_t>(0.0223 * sr) & kDelayMask;
    const std::size_t er_r3 = static_cast<std::size_t>(0.0271 * sr) & kDelayMask;

    auto readFrac = [&](const std::array<float, kDelayMax>& b, double d_samps) noexcept -> float {
        const double rpos = static_cast<double>(write_idx_) - d_samps;
        const double wrapped = rpos < 0.0 ? rpos + static_cast<double>(kDelayMax) : rpos;
        const std::size_t i0 = static_cast<std::size_t>(wrapped) & kDelayMask;
        const std::size_t i1 = (i0 + 1) & kDelayMask;
        const float frac = static_cast<float>(wrapped - std::floor(wrapped));
        return b[i0] + frac * (b[i1] - b[i0]);
    };

    for (uint32_t i = 0; i < num_frames; ++i) {
        float l = left[i];
        float r = right[i];

        if (rm_on && demic > 0.001f) {
            l = demic_body_l_.process(demic_notch_l_.process(l));
            r = demic_body_r_.process(demic_notch_r_.process(r));
        }

        dl_l_[write_idx_] = l;
        dl_r_[write_idx_] = r;

        if (dbl_on && width > 0.001f) {
            lfo_phase_ += lfo_inc;
            if (lfo_phase_ > 2.0 * std::numbers::pi) lfo_phase_ -= 2.0 * std::numbers::pi;

            const double d_mod_l = 12.0 + mod_depth_samps * (1.0 + std::sin(lfo_phase_));
            const double d_mod_r = base_delay_samps + mod_depth_samps * (1.0 + std::cos(lfo_phase_));

            const float tight_l = readFrac(dl_l_, d_mod_l);
            float dbl_r = readFrac(dl_r_, d_mod_r);

            // 2-stage all-pass transient decorrelator on the double track
            const float ap_g = 0.45f;
            const float ap1_out = -ap_g * dbl_r + ap1_z_;
            ap1_z_ = dbl_r + ap_g * ap1_out;
            const float ap2_out = -ap_g * ap1_out + ap2_z_;
            ap2_z_ = ap1_out + ap_g * ap2_out;
            dbl_r = ap2_out;

            // Keep sub-bass centered by high-passing the stereo difference signal
            const float raw_side = 0.5f * (tight_l - dbl_r);
            side_hp_z_ = hp_coeff * side_hp_z_ + (1.0f - hp_coeff) * raw_side;
            const float hp_side = raw_side - side_hp_z_;
            const float mid = 0.5f * (l + r);

            l = (1.0f - width) * l + width * (mid + hp_side * 1.25f);
            r = (1.0f - width) * r + width * (mid - hp_side * 1.25f);
        }

        if (rm_on && room_amt > 0.001f) {
            const float early_l = 0.48f * dl_r_[(write_idx_ + kDelayMax - er_l1) & kDelayMask]
                                - 0.31f * dl_l_[(write_idx_ + kDelayMax - er_l2) & kDelayMask]
                                + 0.19f * dl_r_[(write_idx_ + kDelayMax - er_l3) & kDelayMask];
            const float early_r = 0.48f * dl_l_[(write_idx_ + kDelayMax - er_r1) & kDelayMask]
                                - 0.31f * dl_r_[(write_idx_ + kDelayMax - er_r2) & kDelayMask]
                                + 0.19f * dl_l_[(write_idx_ + kDelayMax - er_r3) & kDelayMask];

            // Binaural low-frequency speaker crossfeed (< 680 Hz)
            xfeed_l_ += xfeed_lp * (l - xfeed_l_);
            xfeed_r_ += xfeed_lp * (r - xfeed_r_);

            const float rm_scale = room_amt * 0.42f;
            l = l + rm_scale * (early_l + 0.25f * xfeed_r_);
            r = r + rm_scale * (early_r + 0.25f * xfeed_l_);
        }

        left[i] = l;
        right[i] = r;
        write_idx_ = (write_idx_ + 1) & kDelayMask;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Rack Component wrappers (900201 & 900202)
// ─────────────────────────────────────────────────────────────────────────────
class TransposeComponent final : public SchemaComponentBase {
public:
    // Grain offsets vary over time, so there is no fixed latency to report.
    explicit TransposeComponent(const catalog::ComponentSchema& schema)
        : SchemaComponentBase(schema) {}

    // Series pipelining (RackGraph): one grain clock and splice search (on the left buffer) drive both channels alike.
    bool monoPreserving() const noexcept override { return true; }

    std::vector<ExtraParam> extraParams() const override {
        return {
            {"range", "Whammy Interval", {
                "-24 st (Dive Bomb)", "-12 st (Octave Down)", "-7 st (Fifth Down)", "-2 st (Whole Step Down)",
                "+2 st (Whole Step Up)", "+7 st (Fifth Up)", "+12 st (Octave Up)", "+24 st (2 Octaves Up)"
            }, 5.0f, {}}
        };
    }

    bool paramDisplay(std::string_view param_id, std::string& out) const override {
        if (param_id == "semitones") {
            const int st = static_cast<int>(std::lround((getParamOr("semitones", 0.5f) - 0.5f) * 24.0f));
            out = (st > 0 ? "+" : "") + std::to_string(st) + " st";
            return true;
        }
        if (param_id == "cents") {
            const int ct = static_cast<int>(std::lround((getParamOr("cents", 0.5f) - 0.5f) * 100.0f));
            out = (ct > 0 ? "+" : "") + std::to_string(ct) + " ct";
            return true;
        }
        return false;
    }

    void adoptRuntimeStateFrom(IComponent& previous) noexcept override {
        if (auto* old = dynamic_cast<TransposeComponent*>(&previous)) {
            shifter_.adoptFrom(old->shifter_);
        }
    }

protected:
    void onPrepare(double sample_rate, uint32_t /*max_block_size*/) override {
        shifter_.prepare(sample_rate);
        updatePitch();
    }

    void onReset() override {
        shifter_.reset();
    }

    void onParameterChanged(std::string_view /*param_id*/, float /*value*/) override {
        updatePitch();
    }

    void onProcessControlRate() override {
        updatePitch();
    }

    void onProcessAudio(AudioBlock& block) override {
        block.copyInToOut();
        if (!block.out || !block.out[0] || block.num_frames == 0) return;
        float* l = block.out[0];
        float* r = (block.num_channels > 1 && block.out[1]) ? block.out[1] : block.out[0];
        shifter_.processStereo(l, r, block.num_frames);
    }

private:
    void updatePitch() noexcept {
        const float base_st = std::round((getParamOr("semitones", 0.5f) - 0.5f) * 24.0f);
        const float cents = (getParamOr("cents", 0.5f) - 0.5f) * 100.0f;
        const float pedal = std::clamp(getParamOr("pedal", 0.0f), 0.0f, 1.0f);
        const int range_idx = std::clamp(static_cast<int>(std::lround(getParamOr("range", 5.0f))), 0, 7);
        static constexpr float kIntervals[8] = {-24.0f, -12.0f, -7.0f, -2.0f, 2.0f, 7.0f, 12.0f, 24.0f};
        const float whammy_st = pedal * kIntervals[range_idx];

        shifter_.setSemitones(base_st + whammy_st);
        shifter_.setCents(cents);
        shifter_.setFormantPreserve(getParamOr("formant", 1.0f) >= 0.5f);
        shifter_.setMix(std::clamp(getParamOr("mix", 1.0f), 0.0f, 1.0f));
    }

    PolyPitchShifter shifter_;
};

class DoublerRoomComponent final : public SchemaComponentBase {
public:
    explicit DoublerRoomComponent(const catalog::ComponentSchema& schema)
        : SchemaComponentBase(schema) {}

    void adoptRuntimeStateFrom(IComponent& previous) noexcept override {
        if (auto* old = dynamic_cast<DoublerRoomComponent*>(&previous)) {
            proc_.adoptFrom(old->proc_);
        }
    }

protected:
    void onPrepare(double sample_rate, uint32_t /*max_block_size*/) override {
        proc_.prepare(sample_rate);
        syncParams();
    }

    void onReset() override {
        proc_.reset();
    }

    void onParameterChanged(std::string_view /*param_id*/, float /*value*/) override {
        syncParams();
    }

    void onProcessControlRate() override {
        syncParams();
    }

    void onProcessAudio(AudioBlock& block) override {
        block.copyInToOut();
        if (!block.out || !block.out[0] || block.num_frames == 0) return;
        float* l = block.out[0];
        float* r = (block.num_channels > 1 && block.out[1]) ? block.out[1] : block.out[0];
        proc_.processStereo(l, r, block.num_frames);
    }

private:
    void syncParams() noexcept {
        proc_.setDoublerEnabled(getParamOr("doubler", 1.0f) >= 0.5f);
        proc_.setDoublerWidth(getParamOr("width", 0.78f));
        proc_.setDoublerDelayMs(6.0f + 24.0f * std::clamp(getParamOr("delay", 0.45f), 0.0f, 1.0f));
        proc_.setDoublerDetune(12.0f * std::clamp(getParamOr("detune", 0.35f), 0.0f, 1.0f));
        proc_.setRoomEnabled(getParamOr("room_on", 1.0f) >= 0.5f);
        proc_.setDeMicAmount(getParamOr("demic", 0.55f));
        proc_.setRoomAmount(getParamOr("room", 0.35f));
    }

    StereoDoublerRoomProcessor proc_;
};

const catalog::ComponentSchema& transposeUnitSchema() noexcept {
    static const catalog::ComponentSchema s{
        kTransposeUnitTable,
        "Poly Transpose & Whammy",
        "Polyphonic Drop-Tuner & Whammy Pedal",
        "HeadroomTranspose",
        catalog::EngineFamily::ModernDSP,
        -1,
        false,
        std::span<const ParamSchema>(kTransposeParams)
    };
    return s;
}

const catalog::ComponentSchema& doublerRoomUnitSchema() noexcept {
    static const catalog::ComponentSchema s{
        kDoublerRoomUnitTable,
        "Stereo Doubler & Room",
        "Phase-Safe Double Tracker & Amp-In-Room",
        "HeadroomDoublerRoom",
        catalog::EngineFamily::ModernDSP,
        -1,
        false,
        std::span<const ParamSchema>(kDoublerRoomParams)
    };
    return s;
}

std::unique_ptr<IComponent> makeTransposeUnit(const catalog::ComponentSchema& schema) {
    return std::make_unique<TransposeComponent>(schema);
}

std::unique_ptr<IComponent> makeDoublerRoomUnit(const catalog::ComponentSchema& schema) {
    return std::make_unique<DoublerRoomComponent>(schema);
}

} // namespace rig
