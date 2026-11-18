#include "../core/IComponent.hpp"
#include "AmpUtilities.hpp"
#include "CabinetCatalog.hpp"
#include "ConvolutionEngine.hpp"
#include "OutputTrim.hpp"
#include "DSPPrimitives.hpp"
#include "ModernFXEngine.hpp"
#include "TubeAmpComponents.hpp"
#include "../midi/MidiComponents.hpp"
#include "NamComponent.hpp"
#include "../plugins/PluginHost.hpp"
#include "../core/Tempo.hpp"
#include "CabinetUnit.hpp"
#include "LooperUnit.hpp"
#include "PitchDoublerUnits.hpp"
#include "StudioEQ.hpp"
#include "TubeSatCompressor.hpp"
#include "ToneMatchAndCalibration.hpp"
#include "../circuit/CircuitUnit.hpp"
#include <cstdio>
#include <cctype>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <string_view>

namespace rig {

namespace {

bool containsCI(std::string_view hay, std::string_view needle) noexcept {
    if (needle.empty() || needle.size() > hay.size()) return false;
    for (std::size_t i = 0; i <= hay.size() - needle.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (std::tolower(static_cast<unsigned char>(hay[i + j])) !=
                std::tolower(static_cast<unsigned char>(needle[j]))) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

bool matchesAny(const catalog::ComponentSchema& s, std::initializer_list<std::string_view> kw) noexcept {
    for (auto k : kw) {
        if (containsCI(s.reference_name, k) || containsCI(s.display_name, k)) return true;
    }
    return false;
}

// A schema-only unit (the routing tables, families without a model): SchemaComponentBase's processAudio copies its input.
class PassThroughComponent final : public SchemaComponentBase {
public:
    using SchemaComponentBase::SchemaComponentBase;
    bool monoPreserving() const noexcept override { return true; } // series pipelining (RackGraph)
};

/**
 * Classic DSP — Dual/Parametric/Shelving/Console/Graphic EQ & Filter Effects
 */
class ClassicEQComponent final : public SchemaComponentBase {
public:
    void adoptRuntimeStateFrom(IComponent& previous) noexcept override {
        auto* old = dynamic_cast<ClassicEQComponent*>(&previous);
        if (!old || tableId() != old->tableId() || host_sr_ != old->host_sr_) return;
        for (int c = 0; c < 2; ++c) low_[c].adoptRuntimeStateFrom(old->low_[c]);
        for (int c = 0; c < 2; ++c) mid1_[c].adoptRuntimeStateFrom(old->mid1_[c]);
        for (int c = 0; c < 2; ++c) mid2_[c].adoptRuntimeStateFrom(old->mid2_[c]);
        for (int c = 0; c < 2; ++c) high_[c].adoptRuntimeStateFrom(old->high_[c]);

    }

    using SchemaComponentBase::SchemaComponentBase;

    // Series pipelining (RackGraph): both channels run the same filters on the same state.
    bool monoPreserving() const noexcept override { return true; }

protected:
    void onPrepare(double sr, uint32_t /*max_block_size*/) override {
        for (int c = 0; c < 2; ++c) {
            low_[c].reset();
            mid1_[c].reset();
            mid2_[c].reset();
            high_[c].reset();
        }
        updateFilters(sr);
    }

    void onReset() override {
        for (int c = 0; c < 2; ++c) {
            low_[c].reset();
            mid1_[c].reset();
            mid2_[c].reset();
            high_[c].reset();
        }
    }

    void onProcessControlRate() override {
        updateFilters(host_sr_);
    }

    void onProcessAudio(AudioBlock& block) override {
        for (uint32_t c = 0; c < block.num_channels; ++c) {
            const float* in = block.in[c];
            float* out = block.out[c];
            if (!in || !out) continue;
            auto& l = low_[c & 1];
            auto& m1 = mid1_[c & 1];
            auto& m2 = mid2_[c & 1];
            auto& h = high_[c & 1];
            for (uint32_t i = 0; i < block.num_frames; ++i) {
                float s = in[i];
                s = l.process(s);
                s = m1.process(s);
                s = m2.process(s);
                s = h.process(s);
                out[i] = s * out_gain_;
            }
        }
    }

private:
    void updateFilters(double sr) noexcept {
        // Generic stand-in for EQ-like units without their own model. The SSL (121000) has its own band layout
        // in StudioEQ.hpp; its H0..H3 keys below are no longer reached.
        const float bass_norm = getNormalizedParam({"bass", "low_gain", "lf_gain", "b1", "b2", "Band 1", "Band 2", "G1", "H0", "low"}, 0.5f);
        const float mid1_norm = getNormalizedParam({"mid", "gain_1", "lmf_gain", "b3", "b4", "Band 3", "Band 4", "G2", "H1", "low_mid"}, 0.5f);
        const float mid2_norm = getNormalizedParam({"gain_2", "hmf_gain", "b5", "b6", "Band 5", "Band 6", "G3", "H2", "high_mid"}, 0.5f);
        const float treb_norm = getNormalizedParam({"treble", "high_gain", "hf_gain", "b7", "b8", "Band 7", "Band 8", "G4", "H3", "high"}, 0.5f);
        const float f1_norm = getNormalizedParam({"F1", "freq_1", "lmf_freq", "cutoff"}, 0.35f);
        const float f2_norm = getNormalizedParam({"F2", "freq_2", "hmf_freq", "shift"}, 0.65f);
        const float vol_norm = getNormalizedParam({"Output", "Volume", "Vol", "V", "output", "volume", "level"}, 0.5f);

        const float low_db = (bass_norm - 0.5f) * 12.0f;
        const float m1_db = (mid1_norm - 0.5f) * 12.0f;
        const float m2_db = (mid2_norm - 0.5f) * 12.0f;
        const float high_db = (treb_norm - 0.5f) * 12.0f;
        const float f1_hz = 110.0f * std::pow(14.0f, std::clamp(f1_norm, 0.0f, 1.0f));
        const float f2_hz = 550.0f * std::pow(12.0f, std::clamp(f2_norm, 0.0f, 1.0f));
        out_gain_ = std::pow(10.0f, (vol_norm - 0.5f) * 8.0f / 20.0f);

        for (int c = 0; c < 2; ++c) {
            low_[c].setLowShelf(sr, 120.0f, low_db);
            mid1_[c].setPeakingEQ(sr, f1_hz, 0.9f, m1_db);
            mid2_[c].setPeakingEQ(sr, f2_hz, 0.9f, m2_db);
            high_[c].setHighShelf(sr, 4200.0f, high_db);
        }
    }

    std::array<dsp::BiquadFilter, 2> low_{};
    std::array<dsp::BiquadFilter, 2> mid1_{};
    std::array<dsp::BiquadFilter, 2> mid2_{};
    std::array<dsp::BiquadFilter, 2> high_{};
    float out_gain_ = 1.0f;
};

/**
 * 110000 Peak Filter (the DJ-style resonant peak filter). Controls, per the unit's documented behaviour:
 *   Freq  (F0) peak frequency, 40 Hz .. 18 kHz on a log scale (the face's PEAK readout and response screen)
 *   Edge  (E1) resonance: the peak's boost 0 .. +21 dB and its Q 0.7 .. 20 (log) rise together, so Edge fully left
 *              is flat and fully right is a narrow, ringing +21 dB peak
 *   Kill  (K4) inverts the curve: the same Q as a cut of the same depth (a notch at Freq)
 *   Mix   (D3) dry / filtered blend
 *   Pump  (P2) brickwall limiting after the blend: up to +12 dB of drive into a -1 dBFS ceiling (instant attack,
 *              ~80 ms release, stereo-linked), so the louder, flattened signal "pumps"; fully left the limiter is out.
 *              Its gain reduction drives the face's PUMP ladder (liveReadout).
 * The peak is a TPT state-variable bell (Simper), smoothed per sample (~8 ms), so Freq sweeps and Edge moves are
 * click-free and stay stable at any setting.
 */
class PeakFilterComponent final : public SchemaComponentBase {
public:
    using SchemaComponentBase::SchemaComponentBase;

    static double freqHz(double n) noexcept { return 40.0 * std::pow(18000.0 / 40.0, std::clamp(n, 0.0, 1.0)); }
    static double edgeQ(double e) noexcept { return 0.7 * std::pow(20.0 / 0.7, std::clamp(e, 0.0, 1.0)); }
    static double edgeDb(double e) noexcept { return 21.0 * std::clamp(e, 0.0, 1.0); }

    void adoptRuntimeStateFrom(IComponent& previous) noexcept override {
        auto* old = dynamic_cast<PeakFilterComponent*>(&previous);
        if (!old || host_sr_ != old->host_sr_) return;
        ic1_ = old->ic1_;
        ic2_ = old->ic2_;
        cur_ = old->cur_;
        lim_gain_ = old->lim_gain_;
    }
    void liveReadout(UnitLiveReadout& r) const noexcept override { r.gain_reduction_db = std::max(0.0f, gr_db_); }
    bool monoPreserving() const noexcept override { return true; } // one linked limiter, the same filter both sides

protected:
    void onPrepare(double /*sr*/, uint32_t /*max_block_size*/) override {
        onProcessControlRate();
        cur_ = tgt_;
        onReset();
    }
    void onReset() override {
        ic1_ = {};
        ic2_ = {};
        lim_gain_ = 1.0;
        gr_db_ = 0.0f;
        gr_min_ = 1.0;
    }
    void onProcessControlRate() override {
        const double sr = host_sr_;
        tgt_.logf = std::log(std::min(freqHz(getParamOr("F0", 0.5f)), 0.45 * sr));
        const double e = getParamOr("E1", 0.5f);
        tgt_.q = edgeQ(e);
        tgt_.db = edgeDb(e) * (getParamOr("K4", 0.0f) > 0.5f ? -1.0 : 1.0);
        tgt_.mix = std::clamp(static_cast<double>(getParamOr("D3", 0.5f)), 0.0, 1.0);
        tgt_.pump = std::clamp(static_cast<double>(getParamOr("P2", 0.5f)), 0.0, 1.0);
    }
    void onProcessAudio(AudioBlock& block) override {
        const double sr = host_sr_;
        const double a = 1.0 - std::exp(-1.0 / (0.008 * sr));      // parameter glide
        const double rel = std::exp(-1.0 / (0.080 * sr));         // limiter release
        constexpr double kCeil = 0.8912509381337456;              // -1 dBFS
        const uint32_t nch = std::min<uint32_t>(block.num_channels, 2);
        double min_gain = block.hostBlockStart() ? 1.0 : gr_min_;
        for (uint32_t i = 0; i < block.num_frames; ++i) {
            cur_.logf += a * (tgt_.logf - cur_.logf);
            cur_.q += a * (tgt_.q - cur_.q);
            cur_.db += a * (tgt_.db - cur_.db);
            cur_.mix += a * (tgt_.mix - cur_.mix);
            cur_.pump += a * (tgt_.pump - cur_.pump);
            const double A = std::pow(10.0, cur_.db / 40.0);
            const double g = std::tan(std::numbers::pi * std::exp(cur_.logf) / sr);
            const double k = 1.0 / (cur_.q * A);
            const double m1 = k * (A * A - 1.0);
            const double a1 = 1.0 / (1.0 + g * (g + k)), a2 = g * a1, a3 = g * a2;
            double y[2] = {0.0, 0.0};
            for (uint32_t c = 0; c < nch; ++c) {
                const double x = block.in[c] ? block.in[c][i] : 0.0;
                const double v3 = x - ic2_[c];
                const double v1 = a1 * ic1_[c] + a2 * v3;
                const double v2 = ic2_[c] + a2 * ic1_[c] + a3 * v3;
                ic1_[c] = 2.0 * v1 - ic1_[c];
                ic2_[c] = 2.0 * v2 - ic2_[c];
                const double wet = x + m1 * v1;
                y[c] = x + cur_.mix * (wet - x);
            }
            if (cur_.pump > 1e-4) { // brickwall: instant attack to the ceiling, smooth release
                const double drive = std::pow(10.0, 12.0 * cur_.pump / 20.0);
                const double peak = std::max(std::abs(y[0]), nch > 1 ? std::abs(y[1]) : 0.0) * drive;
                const double want = peak > kCeil ? kCeil / peak : 1.0;
                lim_gain_ = want < lim_gain_ ? want : rel * lim_gain_ + (1.0 - rel) * want;
                min_gain = std::min(min_gain, lim_gain_);
                for (uint32_t c = 0; c < nch; ++c) y[c] *= drive * lim_gain_;
            } else {
                lim_gain_ = 1.0;
            }
            for (uint32_t c = 0; c < nch; ++c)
                if (block.out[c]) block.out[c][i] = static_cast<float>(y[c]);
        }
        for (uint32_t c = nch; c < block.num_channels; ++c)
            if (block.out[c] && block.out[0]) std::copy(block.out[0], block.out[0] + block.num_frames, block.out[c]);
        gr_min_ = min_gain;
        if (block.hostBlockEnd()) gr_db_ = static_cast<float>(-20.0 * std::log10(std::max(1e-4, min_gain)));
    }

private:
    struct State { double logf = std::log(1000.0), q = 1.0, db = 0.0, mix = 0.5, pump = 0.0; };
    State tgt_{}, cur_{};
    std::array<double, 2> ic1_{}, ic2_{};
    double lim_gain_ = 1.0, gr_min_ = 1.0;
    float gr_db_ = 0.0f;
};

/**
 * Circuit-Waveshaper Amp & Pedal Engine (4x oversampling for multi-stage tube amps,
 * 512-float interpolated asymmetric-triode waveshaper tables, passive tone stack, sag & presence).
 */
class CircuitWaveshaperComponent final : public SchemaComponentBase {
public:
    int subBlockSafety() const noexcept override { return 2; } // channel 1 reads the smoothers' end-of-call values
    void adoptRuntimeStateFrom(IComponent& previous) noexcept override {
        auto* old = dynamic_cast<CircuitWaveshaperComponent*>(&previous);
        if (!old || tableId() != old->tableId() || host_sr_ != old->host_sr_) return;
        if (rate_cfg_.oversampling_factor != old->rate_cfg_.oversampling_factor) return;
        up1_ = old->up1_; up2_ = old->up2_;
        down1_ = old->down1_; down2_ = old->down2_;
        const float drive_target = drive_smoother_.getTarget(), master_target = master_smoother_.getTarget();
        drive_smoother_ = old->drive_smoother_; master_smoother_ = old->master_smoother_;
        drive_smoother_.setTarget(drive_target); master_smoother_.setTarget(master_target);
        for (int c = 0; c < 2; ++c) in_hp_[c].adoptRuntimeStateFrom(old->in_hp_[c]);
        for (int c = 0; c < 2; ++c) tone_low_[c].adoptRuntimeStateFrom(old->tone_low_[c]);
        for (int c = 0; c < 2; ++c) tone_mid_[c].adoptRuntimeStateFrom(old->tone_mid_[c]);
        for (int c = 0; c < 2; ++c) tone_high_[c].adoptRuntimeStateFrom(old->tone_high_[c]);
        for (int c = 0; c < 2; ++c) pres_shelf_[c].adoptRuntimeStateFrom(old->pres_shelf_[c]);
        for (int c = 0; c < 2; ++c) post_lp_[c].adoptRuntimeStateFrom(old->post_lp_[c]);

    }

    explicit CircuitWaveshaperComponent(const catalog::ComponentSchema& schema)
        : SchemaComponentBase(schema) {
        const std::size_t n_stages = schema_.fixed_4x_oversampling ? 3 : 2;
        stages_.resize(n_stages);
        const float id_seed = static_cast<float>((schema_.table_id / 1000) % 7);
        for (std::size_t i = 0; i < n_stages; ++i) {
            const float bias = 0.05f + 0.025f * static_cast<float>((i + static_cast<std::size_t>(id_seed)) % 4);
            const float hardness = 1.05f + 0.22f * static_cast<float>(i) + 0.06f * id_seed;
            stages_[i].initAsymmetricTriode(bias, hardness);
        }
    }

protected:
    void onPrepare(double sr, uint32_t /*max_block_size*/) override {
        rate_cfg_ = schema_.fixed_4x_oversampling
                        ? dsp::InternalRateConfig::forLegacyCircuitAmp(sr)
                        : dsp::InternalRateConfig::forModernDSP(sr, schema_.target_sample_rate_k);
        drive_smoother_.prepare(rate_cfg_.internal_sr, 5.0f, 1.0f);
        master_smoother_.prepare(rate_cfg_.internal_sr, 5.0f, 0.5f);
        onReset();
        onProcessControlRate();
    }

    void onReset() override {
        for (int c = 0; c < 2; ++c) {
            up1_[c].reset();
            up2_[c].reset();
            down1_[c].reset();
            down2_[c].reset();
            in_hp_[c].reset();
            tone_low_[c].reset();
            tone_mid_[c].reset();
            tone_high_[c].reset();
            pres_shelf_[c].reset();
            post_lp_[c].reset();
        }
    }

    void onProcessControlRate() override {
        const float gain_norm = getNormalizedParam({
            "Preamp", "Gain", "Overdrive", "Drive", "Distortion", "Fuzz", "Sustain",
            "Volume I", "Br", "P1", "P0", "V1", "Dr", "G", "g", "gain", "drive", "overdrive", "boost"
        }, 0.55f);
        const float bass_norm = getNormalizedParam({"Bass", "Low", "Bottom", "Bs", "B3", "B", "b", "bass", "low"}, 0.5f);
        const float mid_norm = getNormalizedParam({"Mid", "Middle", "Body", "Contour", "M4", "M", "m", "mid"}, 0.5f);
        const float treb_norm = getNormalizedParam({"Treble", "Tone", "High", "Tb", "T5", "T", "Tn", "t", "treble", "tone"}, 0.5f);
        const float pres_norm = getNormalizedParam({"Presence", "Bright", "Edge", "Cut", "Pr", "P7", "Tc", "presence", "bright"}, 0.5f);
        const float master_norm = getNormalizedParam({"Master", "Volume", "Output", "Level", "M6", "V2", "Vol", "V", "v", "master", "volume"}, 0.55f);

        const bool is_amp = schema_.fixed_4x_oversampling;
        const float max_drive = is_amp ? 1.95f : 1.45f;
        const float base_drive = is_amp ? 0.78f : 0.65f;
        drive_smoother_.setTarget(base_drive + std::pow(gain_norm, 1.30f) * max_drive);

        const float target_master = is_amp
                                        ? (0.48f + 0.45f * std::pow(std::max(0.20f, master_norm), 0.70f))
                                        : (0.65f + 0.55f * master_norm);
        master_smoother_.setTarget(target_master);

        const double isr = rate_cfg_.internal_sr;
        for (int c = 0; c < 2; ++c) {
            in_hp_[c].setHighPass(isr, is_amp ? 75.0f : 85.0f, 0.707f);
            tone_low_[c].setLowShelf(isr, 135.0f, (bass_norm - 0.5f) * 11.0f);
            tone_mid_[c].setPeakingEQ(isr, 680.0f, 0.85f, (mid_norm - 0.5f) * 10.0f);
            tone_high_[c].setHighShelf(isr, 3100.0f, (treb_norm - 0.5f) * 11.0f);
            pres_shelf_[c].setHighShelf(isr, 4600.0f, (pres_norm - 0.5f) * 8.0f);
            post_lp_[c].setLowPass(isr, 6800.0f, 0.707f);
        }
    }

    void onProcessAudio(AudioBlock& block) override {
        for (uint32_t c = 0; c < block.num_channels; ++c) {
            const float* in = block.in[c];
            float* out = block.out[c];
            if (!in || !out) continue;
            const int ch = c & 1;
            for (uint32_t i = 0; i < block.num_frames; ++i) {
                const float x = in[i];
                if (rate_cfg_.oversampling_factor == 4) {
                    float u0, u1, u00, u01, u10, u11;
                    up1_[ch].upsample(x, u0, u1);
                    up2_[ch].upsample(u0, u00, u01);
                    up2_[ch].upsample(u1, u10, u11);
                    const float y00 = processNonlinearSample(ch, u00);
                    const float y01 = processNonlinearSample(ch, u01);
                    const float y10 = processNonlinearSample(ch, u10);
                    const float y11 = processNonlinearSample(ch, u11);
                    const float d0 = down2_[ch].downsample(y00, y01);
                    const float d1 = down2_[ch].downsample(y10, y11);
                    out[i] = down1_[ch].downsample(d0, d1);
                } else {
                    out[i] = processNonlinearSample(ch, x);
                }
            }
        }
    }

private:
    float processNonlinearSample(int ch, float x) noexcept {
        const float drive = (ch == 0) ? drive_smoother_.next() : drive_smoother_.getCurrent();
        const float master = (ch == 0) ? master_smoother_.next() : master_smoother_.getCurrent();

        float s = in_hp_[ch].process(x) * drive;
        const std::size_t active_stages = std::min<std::size_t>(stages_.size(), 2);
        for (std::size_t st = 0; st < active_stages; ++st) {
            s = stages_[st].process(s * (st == 0 ? 1.0f : 1.05f));
        }
        s = tone_low_[ch].process(s);
        s = tone_mid_[ch].process(s);
        s = tone_high_[ch].process(s);
        s = pres_shelf_[ch].process(s);
        s = post_lp_[ch].process(s);
        return std::tanh(s) * master;
    }

    dsp::InternalRateConfig rate_cfg_{};
    dsp::ParameterSmoother drive_smoother_{};
    dsp::ParameterSmoother master_smoother_{};
    std::vector<dsp::WaveshaperLUT512> stages_{};
    std::array<dsp::HalfBandStage, 2> up1_{}, up2_{}, down1_{}, down2_{};
    std::array<dsp::BiquadFilter, 2> in_hp_{}, tone_low_{}, tone_mid_{}, tone_high_{}, pres_shelf_{}, post_lp_{};
};

/**
 * Speaker cabinets & mic rooms: Classic Amp-Matched Cab (88000), Amp-Matched Cab (156000),
 * Studio Mic Room (100000) and the 8-channel Studio Cab Room (119000). The selected cabinet/mic/position
 * impulses are mixed into one stereo IR and convolved with a zero-latency partitioned convolver.
 */
class ConvolutionCabinetComponent final : public SchemaComponentBase {
public:
    SpeakerImpedanceProfile speakerImpedanceProfile() const noexcept override {
        return (is_reverb_ || isBypassed()) ? SpeakerImpedanceProfile{} : spk_profile_;
    }

    // Series pipelining (RackGraph): a cabinet whose IR is the same on both sides (the Amp-Matched Cabs always; the
    // loaders and mic rooms with Stereo off) convolves both channels alike. The reverb mode is a stereo FDN.
    bool monoPreserving() const noexcept override { return !is_reverb_ && ir_mono_; }

    void adoptRuntimeStateFrom(IComponent& previous) noexcept override {
        auto* old = dynamic_cast<ConvolutionCabinetComponent*>(&previous);
        if (!old || tableId() != old->tableId() || host_sr_ != old->host_sr_) return;
        // The convolver's input history is IR-independent: keep it, and re-aim the pending tail when
        // the cabinet / mic / position changed, so the tail continues instead of restarting.
        std::swap(conv_, old->conv_);
        if (ir_ && ir_ != old->ir_) {
            conv_[0].retarget(ir_->left);
            conv_[1].retarget(ir_->right);
        }
        fdn_.adoptRuntimeStateFrom(old->fdn_);
        for (int c = 0; c < 2; ++c) low_[c].adoptRuntimeStateFrom(old->low_[c]);
        for (int c = 0; c < 2; ++c) high_[c].adoptRuntimeStateFrom(old->high_[c]);
        if (old->vol_cur_ >= 0.0f) vol_cur_ = old->vol_cur_; // the Volume knob glides on from what was playing

    }

    explicit ConvolutionCabinetComponent(const catalog::ComponentSchema& schema) : SchemaComponentBase(schema) {
        // Amp-Matched Cab (156000): the schema's generic 0.5 X-Fade would mean 50 % room (see buildIR); a fresh unit
        // starts on the close cabinet capture (X-Fade 0-0.2).
        if (schema_.table_id == 156000) params_["c"].num_val = 0.0f;
    }

    std::vector<ExtraParam> extraParams() const override {
        if (schema_.table_id != 168000) return {};
        std::vector<std::string> labels{"Empty"};
        for (const auto& f : dsp::CabinetIRLibrary::listFiles("Cabinet IR Loader", ".wav")) labels.push_back(dsp::CabinetIRLibrary::prettyIRName(f));
        std::vector<ExtraParam> out;
        for (int i = 1; i <= 4; ++i) {
            const std::string n = std::to_string(i);
            out.push_back({"IR" + n, "IR " + n, labels, i == 1 ? defaultLoaderIR() : 0.0f});
        }
        return out;
    }

protected:
    void onPrepare(double sr, uint32_t /*max_block_size*/) override {
        is_reverb_ = matchesAny(schema_, {"Studio IR Reverb", "Verb", "Reverb", "Plate", "Spring"});
        for (const auto& ep : extraParams())
            if (params_.find(ep.id) == params_.end()) params_[ep.id].num_val = ep.def;
        fdn_.prepare(sr);
        ir_key_.clear();
        vol_glide_ = 1.0f - std::exp(-1.0f / (0.010f * static_cast<float>(sr)));
        onProcessControlRate();
        onReset();
    }

    void onReset() override {
        fdn_.reset();
        for (auto& c : conv_) c.reset();
        for (int c = 0; c < 2; ++c) {
            low_[c].reset();
            high_[c].reset();
        }
    }

    void onProcessControlRate() override {
        if (is_reverb_) {
            const float mix = getNormalizedParam({"W0", "M2", "Mix", "Dry/Wet", "mix", "reverb"}, 0.25f);
            const float sz = getNormalizedParam({"S1", "Size", "size"}, 0.55f);
            reverb_mix_ = std::clamp(mix, 0.0f, 0.75f);
            fdn_.setParameters(sz, 0.65f, 0.35f, 0.35f);
            spk_profile_ = {};
            return;
        }
        {
            cabinet::Equivalent eq;
            if (cabinet::equivalentOf(*this, eq)) spk_profile_ = dsp::cab::speakerProfileForCab(eq.cab);
            else if (schema_.table_id == 100000) {
                const int cab = std::clamp(static_cast<int>(std::lround(getParamOr("S", 3.0f))), 0, 28);
                spk_profile_ = dsp::cab::speakerProfileForCab(std::string("studio:") + dsp::CabinetIRLibrary::studioId(cab));
            } else {
                spk_profile_ = {}; // unidentified IR: leave the amp's own voiced speaker load
            }
        }
        vol_target_ = volumeParam();
        const std::string key = irKey();
        if (key != ir_key_ || !ir_) {
            ir_key_ = key;
            ir_ = dsp::CabinetIRCache::getOrBuild(std::to_string(schema_.table_id) + "|" + key, host_sr_,
                                                  [&]() -> std::shared_ptr<const dsp::CabinetIR> { return buildIR(); });
            ir_mono_ = !ir_ || ir_->left.sameAs(ir_->right);
        }
        float lo = 0.5f, hi = 0.5f;
        if (schema_.table_id == 119000 || schema_.table_id == 100000) {
            lo = getParamOr("b", 0.5f);
            hi = getParamOr("t", 0.5f);
        }
        for (int c = 0; c < 2; ++c) {
            low_[c].setLowShelf(host_sr_, 120.0f, (lo - 0.5f) * 16.0f);
            high_[c].setHighShelf(host_sr_, 3500.0f, (hi - 0.5f) * 16.0f);
        }
        eq_active_ = std::abs(lo - 0.5f) > 0.01f || std::abs(hi - 0.5f) > 0.01f;
    }

    void onProcessAudio(AudioBlock& block) override {
        if (is_reverb_) {
            if (block.num_channels < 2) {
                block.copyInToOut();
                return;
            }
            for (uint32_t i = 0; i < block.num_frames; ++i) {
                fdn_.processStereo(block.in[0][i], block.in[1][i], reverb_mix_, block.out[0][i], block.out[1][i]);
            }
            return;
        }
        const std::shared_ptr<const dsp::CabinetIR> ir = ir_;
        if (!ir) {
            block.copyInToOut();
            return;
        }
        // A mono guitar signal feeding a stereo IR: convolve the same input with both IR channels.
        const float* inL = block.in[0];
        const float* inR = (block.num_channels > 1 && block.in[1]) ? block.in[1] : inL;
        if (vol_cur_ < 0.0f) vol_cur_ = vol_target_;
        for (uint32_t i = 0; i < block.num_frames; ++i) {
            float l = conv_[0].process(inL[i], ir->left);
            float r = conv_[1].process(inR[i], ir->right);
            if (eq_active_) {
                l = high_[0].process(low_[0].process(l));
                r = high_[1].process(low_[1].process(r));
            }
            vol_cur_ += (vol_target_ - vol_cur_) * vol_glide_; // Volume: smoothed output gain (~10 ms), not in the IR
            l *= vol_cur_;
            r *= vol_cur_;
            block.out[0][i] = l;
            if (block.num_channels > 1 && block.out[1]) block.out[1][i] = r;
        }
    }

private:
    using Lib = dsp::CabinetIRLibrary;

    std::string irKey() const {
        std::string k;
        auto add = [&](std::string_view id) {
            k += std::to_string(static_cast<int>(std::lround(getParamOr(id, -1.0f) * 1000.0f)));
            k += ',';
        };
        switch (schema_.table_id) {
            case 156000: for (auto id : {"Cab", "c", "V", "st"}) add(id); break; // Volume: volumeParam(), not the IR
            case 168000:
                for (auto id : {"a", "st"}) add(id);
                for (int i = 1; i <= 4; ++i) for (auto p : {"IR", "g", "p", "m", "s", "pi", "ph"}) add(std::string(p) + std::to_string(i));
                break;
            case 88000: for (auto id : {"Cab", "c", "a"}) add(id); break;
            case 100000:
                for (auto id : {"S", "a", "st"}) add(id);
                for (int i = 1; i <= 8; ++i) for (auto p : {"g", "p", "m"}) add(std::string(p) + std::to_string(i));
                break;
            default:
                for (auto id : {"a", "st"}) add(id);
                for (int i = 1; i <= 8; ++i)
                    for (auto p : {"Cab", "Mic", "MPos", "g", "p", "m", "s", "pi", "ph", "r", "rm"}) add(std::string(p) + std::to_string(i));
                break;
        }
        return k;
    }

    std::shared_ptr<const dsp::CabinetIR> buildIR() const {
        const double sr = host_sr_;
        dsp::IRMixer mix(sr);
        auto addFile = [&](const std::string& rel, float gain, float pan, float delay_ms = 0.0f) {
            if (std::abs(gain) < 1e-4f) return;
            if (auto ir = Lib::load(rel, sr)) mix.add(*ir, gain, pan, delay_ms);
        };
        bool stereo = false;
        switch (schema_.table_id) {
            case 156000: { // Amp-Matched Cab ("%.0f%% Cabinet / %.0f%% Room"
                // readout and its <cab>_<A|B> / <cab>_room_<A|B> IR set): "Cab A/B" (V) picks the A or B impulse pair,
                // the X-Fade (c) blends the close cabinet IR with its recorded room, and Volume (MV) is the output level.
                // (A and B are alternative captures, not two mics to mix: several B files are polarity-inverted or
                // 1.6 ms late against A, so summing them cancelled up to 7 dB and combed the tone.)
                // The room is energy-matched to the close capture and the two are blended at constant power, so
                // the slider moves the mic back without changing the level the offline levelling and staging see.
                const int cab = static_cast<int>(std::lround(getParamOr("Cab", 9.0f)));
                const char ab = getParamOr("V", 0.0f) > 0.5f ? 'B' : 'A';
                const float room = std::clamp(getParamOr("c", 0.0f), 0.0f, 1.0f);
                const float g_close = std::cos(room * 1.5707963f), g_room = std::sin(room * 1.5707963f);
                const auto close = Lib::load(Lib::matched(cab, ab, false), sr);
                if (close && g_close > 1e-4f) mix.add(*close, g_close, 0.5f, 0.0f);
                if (g_room > 1e-4f) { // stereo room capture: left / right channels
                    const std::string rel = Lib::matched(cab, ab, true);
                    const auto l = Lib::load(rel, sr, 0), r = Lib::load(rel, sr, 1);
                    // Energies as this unit outputs them (mono: L and R hard-panned, x sqrt2 in the mixer, then summed),
                    // weighted to a guitar amp's band (80 Hz-4 kHz) so a room with a different tilt still matches by ear.
                    std::vector<float> room_mono(std::max(l ? l->size() : 0, r ? r->size() : 0), 0.0f);
                    for (std::size_t i = 0; i < room_mono.size(); ++i)
                        room_mono[i] = static_cast<float>(std::numbers::sqrt2 * 0.5 * (((l && i < l->size()) ? (*l)[i] : 0.0f) + ((r && i < r->size()) ? (*r)[i] : 0.0f)));
                    auto bandEnergy = [sr](const std::vector<float>& v) {
                        dsp::BiquadFilter hp, lp;
                        hp.setHighPass(sr, 80.0f);
                        lp.setLowPass(sr, 4000.0f);
                        double e = 0.0;
                        for (float x : v) { const double y = lp.process(hp.process(x)); e += y * y; }
                        for (int i = 0; i < 4096; ++i) { const double y = lp.process(hp.process(0.0f)); e += y * y; } // tail
                        return e;
                    };
                    const double e_close = close ? bandEnergy(*close) : 0.0, e_room = bandEnergy(room_mono);
                    const float k = (e_close > 1e-12 && e_room > 1e-12) ? static_cast<float>(std::sqrt(e_close / e_room)) : 1.0f;
                    if (l) mix.add(*l, g_room * k, 0.0f, 0.0f);
                    if (r) mix.add(*r, g_room * k, 1.0f, 0.0f);
                }
                break;
            }
            case 88000: { // Classic Amp-Matched Cab (legacy list: generic cabs, then matched models)
                const int cab = std::clamp(static_cast<int>(std::lround(getParamOr("Cab", 24.0f))), 0, 28);
                static constexpr int kGeneric[11] = {17, 3, 8, 6, 5, 25, 26, 24, 23, 15, 13};
                static constexpr int kMatchedFrom11[18] = {1, 6, 16, 4, 10, 11, 2, 5, 14, 0, 13, 15, 8, 9, 3, 12, 17, 7};
                const float x = std::clamp(getParamOr("c", 0.3f), 0.0f, 1.0f);
                const float air = std::clamp(getParamOr("a", 0.0f), 0.0f, 1.0f);
                if (cab < 11) {
                    const int rc = kGeneric[cab];
                    addFile(Lib::roomCabMic(rc, 0, 1), std::cos(x * 1.5707963f), 0.5f);
                    addFile(Lib::roomCabMic(rc, 2, 0), std::sin(x * 1.5707963f), 0.5f);
                    addFile(Lib::roomCabRoom(rc), 0.6f * air, 0.5f);
                } else {
                    const int m = kMatchedFrom11[cab - 11];
                    addFile(Lib::matched(m, 'A', false), std::cos(x * 1.5707963f), 0.5f);
                    addFile(Lib::matched(m, 'B', false), std::sin(x * 1.5707963f), 0.5f);
                    addFile(Lib::matched(m, 'A', true), 0.6f * air, 0.3f);
                    addFile(Lib::matched(m, 'B', true), 0.6f * air, 0.7f);
                }
                break;
            }
            case 168000: { // Cabinet IR Loader: four slots of the bundled cabinet IRs.
                const auto& files = Lib::listFiles("Cabinet IR Loader", ".wav");
                bool any_solo = false;
                for (int i = 1; i <= 4; ++i) any_solo |= getParamOr("s" + std::to_string(i), 0.0f) >= 0.5f;
                for (int i = 1; i <= 4; ++i) {
                    const std::string n = std::to_string(i);
                    const int sel = static_cast<int>(std::lround(getParamOr("IR" + n, i == 1 ? defaultLoaderIR() : 0.0f)));
                    if (sel <= 0 || sel > static_cast<int>(files.size())) continue;
                    if (getParamOr("m" + n, 0.0f) >= 0.5f || (any_solo && getParamOr("s" + n, 0.0f) < 0.5f)) continue;
                    const float fader = 2.0f * std::clamp(getParamOr("g" + n, 0.5f), 0.0f, 1.0f);
                    const float pol = getParamOr("pi" + n, 0.0f) >= 0.5f ? -1.0f : 1.0f;
                    const std::string rel = "Cabinet IR Loader/" + files[static_cast<std::size_t>(sel - 1)];
                    if (auto irl = Lib::load(rel, sr, 0)) {
                        mix.add(*irl, fader * pol, getParamOr("p" + n, 0.5f), 0.5f * std::clamp(getParamOr("ph" + n, 0.0f), 0.0f, 1.0f));
                    }
                }
                stereo = getParamOr("st", 0.0f) >= 0.5f;
                break;
            }
            case 100000: { // Studio Mic Room: one cabinet, eight fixed mic positions.
                const int cab = std::clamp(static_cast<int>(std::lround(getParamOr("S", 3.0f))), 0, 28);
                static constexpr int kMic[8][2] = {{0, 0}, {0, 1}, {0, 2}, {2, 0}, {2, 1}, {3, 0}, {1, 0}, {4, 1}};
                for (int i = 0; i < 8; ++i) {
                    const std::string n = std::to_string(i + 1);
                    if (getParamOr("m" + n, 0.0f) >= 0.5f) continue;
                    const float g = getParamOr("g" + n, 0.0f);
                    addFile(Lib::roomCabMic(cab, kMic[i][0], kMic[i][1]), g * g * 2.0f, getParamOr("p" + n, 0.5f));
                }
                addFile(Lib::roomCabRoom(cab), 0.5f * getParamOr("a", 0.0f), 0.5f);
                stereo = getParamOr("st", 0.0f) >= 0.5f;
                break;
            }
            default: { // Studio Cab Room (119000): 8-channel mic mixer.
                bool any_solo = false;
                for (int i = 1; i <= 8; ++i) any_solo |= getParamOr("s" + std::to_string(i), 0.0f) >= 0.5f;
                const float air = std::clamp(getParamOr("a", 0.0f), 0.0f, 1.0f);
                for (int i = 1; i <= 8; ++i) {
                    const std::string n = std::to_string(i);
                    const int cab = static_cast<int>(std::lround(getParamOr("Cab" + n, i <= 2 ? 10.0f : 1.0f)));
                    if (cab == 1) continue; // "No Cabinet"
                    if (getParamOr("m" + n, 0.0f) >= 0.5f) continue;
                    if (any_solo && getParamOr("s" + n, 0.0f) < 0.5f) continue;
                    const float fader = 2.0f * std::clamp(getParamOr("g" + n, 0.5f), 0.0f, 1.0f);
                    const float pol = getParamOr("pi" + n, 0.0f) >= 0.5f ? -1.0f : 1.0f;
                    const float pan = getParamOr("p" + n, 0.5f);
                    const float delay = 0.25f * std::clamp(getParamOr("ph" + n, 0.0f), 0.0f, 1.0f);
                    if (cab == 0) { // DI box
                        mix.addImpulse(0.35f * fader * pol, pan);
                        continue;
                    }
                    const int rc = cab - 2;
                    const int mic = static_cast<int>(std::lround(getParamOr("Mic" + n, 0.0f)));
                    const int pos = static_cast<int>(std::lround(getParamOr("MPos" + n, 0.0f)));
                    addFile(Lib::roomCabMic(rc, mic, pos), fader * pol, pan, delay);
                    if (getParamOr("rm" + n, 0.0f) < 0.5f) {
                        const float room = std::clamp(getParamOr("r" + n, 0.0f), 0.0f, 1.0f) + air * 0.5f;
                        addFile(Lib::roomCabRoom(rc), 0.5f * room * fader, pan);
                    }
                }
                stereo = getParamOr("st", 0.0f) >= 0.5f;
                break;
            }
        }
        return mix.finish(stereo, 1.0f); // the Volume knob is a smoothed output gain (volumeParam)
    }

    // Output level of the Volume knob ("MV" / "v"), applied after the convolver with a glide so a drag never steps it
    // (it used to be baked into the IR: every tick rebuilt the IR and jumped the level).
    float volumeParam() const {
        switch (schema_.table_id) {
            case 156000: return dsp::ampMatchedCabVolume(getParamOr("MV", 0.5f));
            case 88000: return 0.7f + 0.9f * getParamOr("MV", 0.5f);
            default: return 0.6f + 0.8f * getParamOr("v", 0.5f);
        }
    }

    static float defaultLoaderIR() {
        const auto& files = dsp::CabinetIRLibrary::listFiles("Cabinet IR Loader", ".wav");
        for (std::size_t i = 0; i < files.size(); ++i)
            if (files[i].find("MARSH 1960A") != std::string::npos) return static_cast<float>(i + 1);
        return files.empty() ? 0.0f : 1.0f;
    }

    bool is_reverb_ = false;
    float reverb_mix_ = 0.25f;
    dsp::CosmicFDNReverb fdn_{};
    std::array<dsp::PartitionedConvolver, 2> conv_{};
    std::shared_ptr<const dsp::CabinetIR> ir_{};
    std::string ir_key_{};
    std::array<dsp::BiquadFilter, 2> low_{}, high_{};
    bool eq_active_ = false;
    bool ir_mono_ = true; // ir_ is the same impulse on both sides
    SpeakerImpedanceProfile spk_profile_{};
    float vol_target_ = 1.0f, vol_cur_ = -1.0f, vol_glide_ = 0.002f; // Volume knob (vol_cur_ < 0: not started)
};

/**
 * Convolution reverbs using the bundled impulse responses: Vintage Plate & Spring Reverb,
 * Studio IR Reverb and Compact IR Reverb. Long IRs run through the partitioned convolver.
 */
class IRReverbComponent final : public SchemaComponentBase {
public:
    void adoptRuntimeStateFrom(IComponent& previous) noexcept override {
        auto* old = dynamic_cast<IRReverbComponent*>(&previous);
        if (!old || tableId() != old->tableId() || host_sr_ != old->host_sr_) return;
        if (key_ == old->key_) std::swap(conv_, old->conv_);
        for (int c = 0; c < 2; ++c) low_cut_[c].adoptRuntimeStateFrom(old->low_cut_[c]);
        if (old->wet_cur_ >= 0.0f) { wet_cur_ = old->wet_cur_; dry_cur_ = old->dry_cur_; } // Mix glides on

    }

    using SchemaComponentBase::SchemaComponentBase;

    // Series pipelining (RackGraph): a mono impulse (the same on both sides) convolves both channels alike.
    bool monoPreserving() const noexcept override { return ir_mono_; }

    std::vector<ExtraParam> extraParams() const override {
        if (schema_.table_id != 118000) return {};
        std::vector<std::string> labels;
        for (const auto& f : dsp::CabinetIRLibrary::listFiles("Studio IR Reverb", ".ir")) labels.push_back(dsp::CabinetIRLibrary::prettyIRName(f, true));
        return {{"IR", "Space", labels, defaultStudioIR()}};
    }

protected:
    void onPrepare(double sr, uint32_t /*max_block_size*/) override {
        for (const auto& ep : extraParams())
            if (params_.find(ep.id) == params_.end()) params_[ep.id].num_val = ep.def;
        max_taps_ = static_cast<std::size_t>(sr * 3.2);
        const int parts = static_cast<int>(max_taps_ / dsp::kConvPart) + 1;
        for (auto& c : conv_) c.setCapacity(parts);
        key_.clear();
        onProcessControlRate();
        onReset();
    }

    void onReset() override {
        for (auto& c : conv_) c.reset();
        for (auto& f : low_cut_) f.reset();
    }

    void onProcessControlRate() override {
        std::string file;
        float decay = 1.0f, size = 1.0f, low_cut = 0.0f;
        switch (schema_.table_id) {
            case 127000: {
                static constexpr const char* kModes[8] = {"Gold Plate Short", "Gold Plate Long", "Silver Plate Short", "Silver Plate Long",
                                                          "Studio Spring", "Dual Spring", "Amp Spring", "Dub Spring"};
                const int m = std::clamp(static_cast<int>(std::lround(getParamOr("S1", 0.0f))), 0, 7);
                file = dsp::CabinetIRLibrary::plateSpring(kModes[m]);
                wet_ = std::clamp(getParamOr("M2", 0.2f), 0.0f, 1.0f) * 1.6f;
                dry_ = 1.0f;
                muted_ = getParamOr("M3", 0.0f) > 0.5f; // switch: untouched schema default 0.5 = off
                break;
            }
            case 126000: {
                static constexpr const char* kIrs[8] = {"ShortA", "ShortB", "MediumA", "MediumB", "LongA", "LongB", "XXLA", "XXLB"};
                const int m = std::clamp(static_cast<int>(std::lround(getParamOr("I7", 0.3f) * 7.0f)), 0, 7);
                file = std::string("Compact IR Reverb/") + kIrs[m] + ".ir";
                const float mix = std::clamp(getParamOr("W1", 0.3f), 0.0f, 1.0f);
                wet_ = mix * 1.4f;
                dry_ = 1.0f - 0.5f * mix;
                decay = 0.25f + 0.75f * getParamOr("D3", 1.0f);
                low_cut = getParamOr("L4", 0.0f);
                muted_ = getParamOr("M6", 0.0f) > 0.5f;
                break;
            }
            default: { // Studio IR Reverb: any of the bundled halls, rooms, plates, springs and special spaces
                const auto& files = dsp::CabinetIRLibrary::listFiles("Studio IR Reverb", ".ir");
                const int sel = static_cast<int>(std::lround(getParamOr("IR", defaultStudioIR())));
                file = files.empty() ? std::string("Studio IR Reverb/13 Short Plates/0.8s Short Vocal Plate - Processed.ir")
                                     : "Studio IR Reverb/" + files[static_cast<std::size_t>(std::clamp(sel, 0, static_cast<int>(files.size()) - 1))];
                wet_ = std::clamp(getParamOr("W0", 0.3f), 0.0f, 1.0f) * 1.2f;
                dry_ = std::clamp(getParamOr("D7", 1.0f), 0.0f, 1.0f);
                size = 0.25f + 0.75f * getParamOr("S1", 1.0f);
                decay = 0.2f + 0.8f * getParamOr("D8", 1.0f);
                muted_ = getParamOr("M12", 0.0f) > 0.5f;
                break;
            }
        }
        const std::string key = file + "|" + std::to_string(static_cast<int>(decay * 100.0f)) + "|" + std::to_string(static_cast<int>(size * 100.0f));
        if (key != key_) {
            key_ = key;
            ir_ = buildIR(file, decay, size);
            ir_mono_ = !ir_ || ir_->left.sameAs(ir_->right);
        }
        use_low_cut_ = low_cut > 0.01f;
        for (auto& f : low_cut_) f.setHighPass(host_sr_, 60.0f + 500.0f * low_cut, 0.7f);
    }

    void onProcessAudio(AudioBlock& block) override {
        const std::shared_ptr<const dsp::CabinetIR> ir = ir_;
        if (!ir || block.num_channels < 2 || !block.out[1]) {
            block.copyInToOut();
            return;
        }
        const float* inL = block.in[0];
        const float* inR = block.in[1] ? block.in[1] : inL;
        if (wet_cur_ < 0.0f) { wet_cur_ = wet_; dry_cur_ = dry_; }
        const float glide = 1.0f - std::exp(-1.0f / (0.010f * static_cast<float>(host_sr_)));
        for (uint32_t i = 0; i < block.num_frames; ++i) {
            wet_cur_ += (wet_ - wet_cur_) * glide; // Mix / Dry knobs: ~10 ms glide
            dry_cur_ += (dry_ - dry_cur_) * glide;
            float sl = muted_ ? 0.0f : inL[i];
            float sr = muted_ ? 0.0f : inR[i];
            if (use_low_cut_) {
                sl = low_cut_[0].process(sl);
                sr = low_cut_[1].process(sr);
            }
            const float wl = conv_[0].process(sl, ir->left);
            const float wr = conv_[1].process(sr, ir->right);
            block.out[0][i] = dry_cur_ * inL[i] + wet_cur_ * wl;
            block.out[1][i] = dry_cur_ * inR[i] + wet_cur_ * wr;
        }
    }

private:
    std::shared_ptr<const dsp::CabinetIR> buildIR(const std::string& file, float decay, float size) const {
        auto l = dsp::CabinetIRLibrary::load(file, host_sr_, 0, max_taps_);
        auto r = dsp::CabinetIRLibrary::load(file, host_sr_, 1, max_taps_);
        if (!l) return nullptr;
        std::vector<float> L = *l, R = r ? *r : *l;
        const std::size_t n = std::min(L.size(), R.size());
        L.resize(n);
        R.resize(n);
        // Size shortens the response, decay steepens it (exponential envelope).
        const std::size_t keep = std::max<std::size_t>(256, static_cast<std::size_t>(static_cast<float>(n) * std::clamp(size, 0.05f, 1.0f)));
        L.resize(std::min(keep, n));
        R.resize(std::min(keep, n));
        const float tau = static_cast<float>(host_sr_) * (0.15f + 2.5f * decay * decay);
        double e = 0.0;
        for (std::size_t i = 0; i < L.size(); ++i) {
            float w = (decay < 0.999f) ? std::exp(-static_cast<float>(i) / tau) : 1.0f;
            const std::size_t fade = std::min<std::size_t>(L.size() / 8, static_cast<std::size_t>(host_sr_ * 0.05));
            if (fade > 0 && i + fade >= L.size()) w *= static_cast<float>(L.size() - 1 - i) / static_cast<float>(fade);
            L[i] *= w;
            R[i] *= w;
            e += 0.5 * (static_cast<double>(L[i]) * L[i] + static_cast<double>(R[i]) * R[i]);
        }
        const float g = (e > 1e-12) ? static_cast<float>(0.6 / std::sqrt(e)) : 1.0f;
        for (auto& v : L) v *= g;
        for (auto& v : R) v *= g;
        auto out = std::make_shared<dsp::CabinetIR>();
        out->left.build(L, max_taps_);
        out->right.build(R, max_taps_);
        return out;
    }

    static float defaultStudioIR() {
        const auto& files = dsp::CabinetIRLibrary::listFiles("Studio IR Reverb", ".ir");
        for (std::size_t i = 0; i < files.size(); ++i)
            if (files[i].find("Short Vocal Plate") != std::string::npos) return static_cast<float>(i);
        return 0.0f;
    }

    std::size_t max_taps_ = 150000;
    std::array<dsp::PartitionedConvolver, 2> conv_{dsp::PartitionedConvolver(1), dsp::PartitionedConvolver(1)};
    std::shared_ptr<const dsp::CabinetIR> ir_{};
    bool ir_mono_ = true; // ir_ is the same impulse on both sides
    std::string key_{};
    std::array<dsp::BiquadFilter, 2> low_cut_{};
    bool use_low_cut_ = false;
    bool muted_ = false;
    float wet_ = 0.2f, dry_ = 1.0f;
    float wet_cur_ = -1.0f, dry_cur_ = 1.0f; // audio: smoothed wet / dry (wet_cur_ < 0 = not started)
};

/**
 * Cosmic / FDN Diffusion Reverb Component
 */
class ReverbFDNComponent final : public SchemaComponentBase {
public:
    void adoptRuntimeStateFrom(IComponent& previous) noexcept override {
        auto* old = dynamic_cast<ReverbFDNComponent*>(&previous);
        if (!old || tableId() != old->tableId() || host_sr_ != old->host_sr_) return;
        fdn_.adoptRuntimeStateFrom(old->fdn_);
        std::swap(pre_, old->pre_);
        pre_cur_ = old->pre_cur_;
        pre_primed_ = old->pre_primed_;
        trim_.adoptRuntimeStateFrom(old->trim_);
    }

    using SchemaComponentBase::SchemaComponentBase;

    bool paramDisplay(std::string_view id, std::string& out) const override {
        if (!pre_sync_ || id != "preD") return false;
        out = pre_label_;
        return true;
    }

protected:
    void onPrepare(double sr, uint32_t /*max_block_size*/) override {
        fdn_.prepare(sr);
        if (hasPredelayLine())
            for (auto& p : pre_) p.prepare(sr, 2.5f);
        trim_.prepare(sr);
        onProcessControlRate();
    }

    void onReset() override {
        fdn_.reset();
        for (auto& p : pre_) p.reset();
        pre_primed_ = false;
        trim_.reset();
    }

    void onProcessControlRate() override {
        float sz = getNormalizedParam({"size", "room_size", "predelay", "preD"}, 0.55f);
        float dec = getNormalizedParam({"revTime", "decay", "feedback", "time", "reverb_time"}, 0.60f);
        float dmp = getNormalizedParam({"damp", "highCut", "high_cut", "treble", "color"}, 0.40f);
        float mod = getNormalizedParam({"mod", "modulation", "diffusion", "detune"}, 0.35f);
        mix_ = std::clamp(getNormalizedParam({"mix", "dry_wet", "W0", "M2"}, 0.22f), 0.0f, 0.65f);
        free_pre_s_ = 0.0f;
        // Explicit control maps where the keyword lookup above routes the wrong control (docs/UNIT_AUDIT.md).
        auto n01 = [&](std::string_view id, float def) { return getNormalizedParam({id}, def); };
        switch (schema_.table_id) {
            case 154000: // Lexicon 224XL: the lookup took Predelay as the size and left decay / hi cut fixed.
                // Mid = reverb time of the main (mid/high) band, Bass blends in the low-band time, Hi Cut fader
                // up = brighter tail, Algorithm room / small hall / large hall and Depth (listener position) set
                // the apparent size, Intensity = modulation. Predelay 0-152 ms.
                sz = std::clamp(0.15f + 0.55f * n01("T8", 0.0f) + 0.3f * n01("D1", 0.5f), 0.0f, 1.0f);
                dec = 0.2f + 0.8f * (0.8f * n01("M5", 0.5f) + 0.2f * n01("B4", 0.5f));
                dmp = 1.0f - n01("H7", 0.5f);
                mod = n01("I2", 0.5f);
                free_pre_s_ = 0.152f * n01("P0", 0.0f);
                break;
            case 155000: // Lexicon 480L: decay (Mid / Bass), Hi Cut, Size and Predelay were unmapped.
                sz = n01("S2", 0.5f);
                dec = 0.2f + 0.8f * (0.8f * n01("M4", 0.5f) + 0.2f * n01("B3", 0.5f));
                dmp = 1.0f - n01("H5", 0.5f);
                mod = 0.5f * (n01("S30", 0.5f) + n01("W31", 0.5f));
                free_pre_s_ = 0.2f * n01("P0", 0.0f);
                break;
            case 27000: // Spring tank: "damp" matched Low Damp first, so Low Damp drove the HF damping.
                dmp = n01("L", 0.5f);                          // High Damp
                dec = 0.35f + 0.5f * n01("SpringLength", 0.5f); // = the old fixed 0.6 at the default length
                break;
            default: break;
        }
        fdn_.setParameters(sz, dec, dmp, mod);
        // Lexicon 224XL / 480L Output: the mainframe's output level, after the mix (dsp/OutputTrim.hpp)
        if (const auto* law = dsp::outputTrimLaw(schema_.table_id)) trim_.setDb(law->db(getParamOr(law->param, law->def)));

        // Cosmic Reverb (148000) synced predelay: Numerator / Denominator (1/64 … 1/4)
        // × Sync Mode (straight, dotted, triplet).
        pre_sync_ = false;
        if (schema_.table_id == 148000 && getParamOr("preDSync", 0.0f) > 0.5f) {
            static constexpr int kDen[5] = {64, 32, 16, 8, 4};
            const int num = std::clamp(static_cast<int>(std::lround(getParamOr("preDNumerator", 1.0f))), 0, 16);
            const int den = kDen[std::clamp(static_cast<int>(std::lround(getParamOr("preDDenominator", 4.0f))), 0, 4)];
            const int mode = std::clamp(static_cast<int>(std::lround(getParamOr("preDMode", 0.0f))), 0, 3);
            const double mult = mode == 1 ? 1.5 : mode == 2 ? 2.0 / 3.0 : 1.0;
            pre_beats_ = 4.0 * num / den * mult;
            pre_sync_ = num > 0;
            pre_label_ = std::to_string(num) + "/" + std::to_string(den) + (mode == 1 ? " D" : mode == 2 ? " T" : "");
        }
    }

    void onProcessAudio(AudioBlock& block) override {
        render(block);
        trim_.process(block.out, block.num_channels, block.num_frames);
    }

private:
    void render(AudioBlock& block) noexcept {
        if (block.num_channels < 2) {
            block.copyInToOut();
            return;
        }
        const float* in_l = block.in[0];
        const float* in_r = block.in[1];
        float* out_l = block.out[0];
        float* out_r = block.out[1];
        if (pre_sync_ || free_pre_s_ > 0.0f) {
            const float target = pre_sync_ ? static_cast<float>(syncedSeconds(pre_beats_, block.bpm, 2.4) * host_sr_)
                                           : static_cast<float>(free_pre_s_ * host_sr_);
            if (!pre_primed_) { pre_cur_ = target; pre_primed_ = true; }
            const float glide = 1.0f - std::exp(-1.0f / (0.06f * static_cast<float>(host_sr_)));
            for (uint32_t i = 0; i < block.num_frames; ++i) {
                pre_cur_ += glide * (target - pre_cur_);
                pre_[0].push(in_l[i]);
                pre_[1].push(in_r[i]);
                const float d = std::max(1.0f, pre_cur_);
                float wl = 0.0f, wr = 0.0f;
                fdn_.processStereo(pre_[0].read(d), pre_[1].read(d), 1.0f, wl, wr);
                out_l[i] = (1.0f - mix_) * in_l[i] + mix_ * wl;
                out_r[i] = (1.0f - mix_) * in_r[i] + mix_ * wr;
            }
            return;
        }
        for (uint32_t i = 0; i < block.num_frames; ++i) {
            fdn_.processStereo(in_l[i], in_r[i], mix_, out_l[i], out_r[i]);
        }
    }

    // Units with a predelay line: Cosmic (synced predelay) and the Lexicons (free predelay).
    bool hasPredelayLine() const noexcept {
        const auto t = schema_.table_id;
        return t == 148000 || t == 154000 || t == 155000;
    }

    dsp::CosmicFDNReverb fdn_{};
    float free_pre_s_ = 0.0f; // free-running predelay (s); only set on tables with hasPredelayLine()
    float mix_ = 0.22f;
    std::array<dsp::FractionalDelayLine, 2> pre_{};
    bool pre_sync_ = false, pre_primed_ = false;
    double pre_beats_ = 1.0;
    float pre_cur_ = 0.0f;
    std::string pre_label_;
    dsp::OutputTrim trim_{}; // Lexicon Output (inactive, 0 dB, on the other tables)
};

/**
 * Stereo Modulated Delay / Tape Delay / Multi-Mode Delay Component
 *
 * Tempo sync (rig tempo via AudioBlock::bpm): Ping-Pong Dual Delay (98000) "Sync" quantizes Time L/R to note
 * values and "Sync Pre" adds a synced predelay; Performance Delay (107000) is synced unless "Free";
 * Multi-Mode Delay (158000) Time Mode 1/2/3 = note / dotted / triplet; Shimmer Delay (157000)
 * gets an engine-side "Sync" switch. Synced times glide (~60 ms) when the tempo changes.
 */
class StereoDelayComponent final : public SchemaComponentBase {
public:
    void adoptRuntimeStateFrom(IComponent& previous) noexcept override {
        auto* old = dynamic_cast<StereoDelayComponent*>(&previous);
        if (!old || tableId() != old->tableId() || host_sr_ != old->host_sr_) return;
        std::swap(delay_, old->delay_);
        std::swap(pre_, old->pre_);
        lfo_phase_ = old->lfo_phase_;
        cur_l_ = old->cur_l_;
        cur_r_ = old->cur_r_;
        cur_pre_l_ = old->cur_pre_l_;
        cur_pre_r_ = old->cur_pre_r_;
        primed_ = old->primed_;
        for (int c = 0; c < 2; ++c) lp_[c].adoptRuntimeStateFrom(old->lp_[c]);
        for (int c = 0; c < 2; ++c) hp_[c].adoptRuntimeStateFrom(old->hp_[c]);
        trim_.adoptRuntimeStateFrom(old->trim_);

    }

    using SchemaComponentBase::SchemaComponentBase;

    std::vector<ExtraParam> extraParams() const override {
        if (schema_.table_id != 157000) return {};
        return {{"sync", "Sync", {"Free", "Tempo"}, 0.0f, {}}};
    }

    bool paramDisplay(std::string_view id, std::string& out) const override {
        const auto t = schema_.table_id;
        if (sync_on_ && ((t == 98000 && (id == "tl" || id == "tr")) || (t == 107000 && id == "R0") || ((t == 158000 || t == 157000) && id == "time"))) {
            out = divisionText(id == "tr" ? idx_r_ : idx_l_);
            return true;
        }
        if (t == 98000 && pre_sync_ && (id == "pl" || id == "pr")) {
            out = kStraightDivisions[id == "pr" ? pre_idx_r_ : pre_idx_l_].label;
            return true;
        }
        return false;
    }

protected:
    void onPrepare(double sr, uint32_t /*max_block_size*/) override {
        for (const auto& ep : extraParams())
            if (params_.find(ep.id) == params_.end()) params_[ep.id].num_val = ep.def;
        for (int c = 0; c < 2; ++c) {
            delay_[c].prepare(sr, 2.0f);
            if (schema_.table_id == 98000) pre_[c].prepare(sr, 2.0f);
            lp_[c].setLowPass(sr, 4800.0f, 0.707f);
            hp_[c].setHighPass(sr, 110.0f, 0.707f);
        }
        trim_.prepare(sr);
        onProcessControlRate();
        primed_ = false;
    }

    void onReset() override {
        for (int c = 0; c < 2; ++c) {
            delay_[c].reset();
            pre_[c].reset();
            lp_[c].reset();
            hp_[c].reset();
        }
        lfo_phase_ = 0.0f;
        trim_.reset();
    }

    void onProcessControlRate() override {
        const float t_norm = getNormalizedParam({"time", "delay", "delay_time", "T", "tl", "rate", "speed"}, 0.38f);
        const float fb_norm = getNormalizedParam({"feedback", "regen", "repeats", "F", "decay"}, 0.35f);
        const float mod_norm = getNormalizedParam({"depth", "mod", "flutter", "chorus", "shimmer"}, 0.20f);
        const float hc_norm = getNormalizedParam({"hicut", "high_cut", "treble", "tone", "damp"}, 0.65f);
        mix_ = std::clamp(getNormalizedParam({"mix", "dry_wet", "D", "level"}, 0.24f), 0.0f, 0.50f);

        delay_samples_l_ = static_cast<float>(host_sr_ * (0.08f + 0.62f * t_norm));
        delay_samples_r_ = delay_samples_l_ * 1.125f;
        feedback_ = std::clamp(fb_norm * 0.78f, 0.0f, 0.82f);
        mod_depth_ = 12.0f * mod_norm;

        const float fc = 1400.0f + 7500.0f * hc_norm;
        for (int c = 0; c < 2; ++c) {
            lp_[c].setLowPass(host_sr_, fc, 0.707f);
        }
        // Shimmer / Multi-Mode Delay Output: a gain on the finished output (dry and repeats), dsp/OutputTrim.hpp
        if (const auto* law = dsp::outputTrimLaw(schema_.table_id)) trim_.setDb(law->db(getParamOr(law->param, law->def)));

        // Tempo sync configuration (the tempo itself arrives per block).
        auto raw01 = [&](const char* id, float def) { return std::clamp(getParamOr(id, def), 0.0f, 1.0f); };
        auto idx = [&](const char* id, float def) { return divisionIndex(kStraightDivisions, raw01(id, def)); };
        sync_on_ = pre_sync_ = false;
        mult_ = 1.0;
        switch (schema_.table_id) {
            case 98000:
                sync_on_ = getParamOr("s", 0.0f) > 0.5f; // schema default 0.5 = unset → off
                idx_l_ = idx("tl", 0.5f);
                idx_r_ = idx("tr", 0.5f);
                pre_sync_ = getParamOr("p", 0.0f) > 0.5f;
                pre_idx_l_ = idx("pl", 0.5f);
                pre_idx_r_ = idx("pr", 0.5f);
                break;
            case 107000:
                sync_on_ = getParamOr("F4", 0.0f) <= 0.5f;
                idx_l_ = idx_r_ = idx("R0", 0.5f);
                break;
            case 158000: {
                const int mode = static_cast<int>(std::lround(getParamOr("timemode", 0.0f)));
                sync_on_ = mode >= 1;
                mult_ = mode == 2 ? 1.5 : mode == 3 ? 2.0 / 3.0 : 1.0;
                idx_l_ = idx_r_ = idx("time", 0.5f);
                break;
            }
            case 157000:
                sync_on_ = getParamOr("sync", 0.0f) > 0.5f;
                idx_l_ = idx_r_ = idx("time", 0.5f);
                break;
            default: break;
        }
    }

    void onProcessAudio(AudioBlock& block) override {
        render(block);
        trim_.process(block.out, block.num_channels, block.num_frames);
    }

    void render(AudioBlock& block) noexcept {
        if (block.num_channels < 2) {
            block.copyInToOut();
            return;
        }
        const float* in_l = block.in[0];
        const float* in_r = block.in[1];
        float* out_l = block.out[0];
        float* out_r = block.out[1];

        // Synced targets for this block (one tempo read per block, no allocation).
        float tgt_l = delay_samples_l_, tgt_r = delay_samples_r_, tgt_pre_l = 0.0f, tgt_pre_r = 0.0f;
        const double sr = host_sr_;
        if (sync_on_) {
            tgt_l = static_cast<float>(syncedSeconds(kStraightDivisions[idx_l_].beats * mult_, block.bpm, 1.95) * sr);
            tgt_r = static_cast<float>(syncedSeconds(kStraightDivisions[idx_r_].beats * mult_, block.bpm, 1.95) * sr);
        }
        const bool pre = pre_sync_ && schema_.table_id == 98000;
        if (pre) {
            tgt_pre_l = static_cast<float>(syncedSeconds(kStraightDivisions[pre_idx_l_].beats, block.bpm, 1.95) * sr);
            tgt_pre_r = static_cast<float>(syncedSeconds(kStraightDivisions[pre_idx_r_].beats, block.bpm, 1.95) * sr);
        }
        const float glide = 1.0f - std::exp(-1.0f / (0.06f * static_cast<float>(sr)));
        if (!primed_) { // a freshly built rig starts on the grid instead of gliding onto it
            cur_l_ = tgt_l; cur_r_ = tgt_r; cur_pre_l_ = tgt_pre_l; cur_pre_r_ = tgt_pre_r;
            primed_ = true;
        }

        for (uint32_t i = 0; i < block.num_frames; ++i) {
            lfo_phase_ += static_cast<float>(0.85 / host_sr_);
            if (lfo_phase_ >= 1.0f) lfo_phase_ -= 1.0f;
            const float mod = std::sin(2.0f * std::numbers::pi_v<float> * lfo_phase_) * mod_depth_;
            // Synced and free times both glide (~60 ms): dragging Time bends the repeats like tape instead of
            // jumping the read position (a click per knob tick).
            cur_l_ += glide * (tgt_l - cur_l_);
            cur_r_ += glide * (tgt_r - cur_r_);

            const float dl = hp_[0].process(lp_[0].process(delay_[0].read(cur_l_ + mod)));
            const float dr = hp_[1].process(lp_[1].process(delay_[1].read(cur_r_ - mod)));

            float xl = in_l[i], xr = in_r[i];
            if (pre) {
                cur_pre_l_ += glide * (tgt_pre_l - cur_pre_l_);
                cur_pre_r_ += glide * (tgt_pre_r - cur_pre_r_);
                pre_[0].push(xl);
                pre_[1].push(xr);
                xl = pre_[0].read(std::max(1.0f, cur_pre_l_));
                xr = pre_[1].read(std::max(1.0f, cur_pre_r_));
            }
            delay_[0].push(xl + dr * feedback_);
            delay_[1].push(xr + dl * feedback_);

            out_l[i] = (1.0f - 0.45f * mix_) * in_l[i] + mix_ * dl;
            out_r[i] = (1.0f - 0.45f * mix_) * in_r[i] + mix_ * dr;
        }
    }

private:
    std::string divisionText(std::size_t i) const {
        std::string s = kStraightDivisions[i].label;
        if (mult_ > 1.01) s += " D";
        else if (mult_ < 0.99) s += " T";
        return s;
    }

    std::array<dsp::FractionalDelayLine, 2> delay_{};
    std::array<dsp::FractionalDelayLine, 2> pre_{}; // Dual Delay (98000) synced predelay only
    std::array<dsp::BiquadFilter, 2> lp_{}, hp_{};
    float delay_samples_l_ = 16000.0f;
    float delay_samples_r_ = 18000.0f;
    float cur_l_ = 16000.0f, cur_r_ = 18000.0f, cur_pre_l_ = 0.0f, cur_pre_r_ = 0.0f;
    float feedback_ = 0.35f;
    float mod_depth_ = 4.0f;
    float mix_ = 0.24f;
    float lfo_phase_ = 0.0f;
    bool sync_on_ = false, pre_sync_ = false, primed_ = false;
    double mult_ = 1.0;
    std::size_t idx_l_ = 4, idx_r_ = 4, pre_idx_l_ = 4, pre_idx_r_ = 4;
    dsp::OutputTrim trim_{}; // Shimmer / Multi-Mode Delay Output (inactive, 0 dB, on the other tables)
};

/**
 * Stereo Chorus / Flanger / Phaser / Tape Warble / Rotary Modulation Component
 *
 * Tempo sync: Tremolo (13000 "Ts"), Small Stone Phaser (20000 "Sy"), Electric Mistress Flanger (28000 "Sy")
 * and Dimension Chorus (29000 "TS") turn the Rate knob into an LFO period from 4 bars to 1/32 at the rig tempo.
 * With a playing host transport the LFO phase locks to the song position.
 */
class StereoModulationComponent final : public SchemaComponentBase {
public:
    void adoptRuntimeStateFrom(IComponent& previous) noexcept override {
        auto* old = dynamic_cast<StereoModulationComponent*>(&previous);
        if (!old || tableId() != old->tableId() || host_sr_ != old->host_sr_) return;
        std::swap(line_, old->line_);
        phase_ = old->phase_;
    }

    using SchemaComponentBase::SchemaComponentBase;

    void liveReadout(UnitLiveReadout& r) const noexcept override { r.lfo_phase = phase_; }

    bool paramDisplay(std::string_view id, std::string& out) const override {
        if (!sync_on_ || id != rate_id_) return false;
        out = kLfoDivisions[sync_idx_].label;
        return true;
    }

protected:
    void onPrepare(double sr, uint32_t /*max_block_size*/) override {
        for (int c = 0; c < 2; ++c) {
            line_[c].prepare(sr, 0.1f);
        }
        onProcessControlRate();
    }

    void onReset() override {
        for (int c = 0; c < 2; ++c) line_[c].reset();
        phase_ = 0.0f;
    }

    void onProcessControlRate() override {
        const float r_norm = getNormalizedParam({"rate", "speed", "freq", "flutter", "Rt"}, 0.35f);
        const float d_norm = getNormalizedParam({"depth", "intensity", "width", "amount", "wow", "CI"}, 0.45f);
        const float fb_norm = getNormalizedParam({"feedback", "resonance", "color"}, 0.20f);
        mix_ = std::clamp(getNormalizedParam({"mix", "dry_wet", "blend", "DW"}, 0.35f), 0.0f, 0.65f);

        rate_hz_ = 0.15f + 4.0f * r_norm * r_norm;
        depth_ms_ = 1.0f + 5.0f * d_norm;
        feedback_ = (fb_norm - 0.5f) * 0.7f;

        const char* sync_id = nullptr;
        rate_id_ = "Rt";
        switch (schema_.table_id) {
            case 13000: sync_id = "Ts"; break;
            case 20000: case 28000: sync_id = "Sy"; break;
            case 29000: sync_id = "TS"; rate_id_ = "VR"; break;
            default: break;
        }
        sync_on_ = sync_id && getParamOr(sync_id, 0.0f) > 0.5f; // 0.5 schema default = unset
        sync_idx_ = divisionIndex(kLfoDivisions, std::clamp(getParamOr(rate_id_, 0.5f), 0.0f, 1.0f));
    }

    void onProcessAudio(AudioBlock& block) override {
        if (block.num_channels < 2) {
            block.copyInToOut();
            return;
        }
        const float* in_l = block.in[0];
        const float* in_r = block.in[1];
        float* out_l = block.out[0];
        float* out_r = block.out[1];

        const float sr = static_cast<float>(host_sr_);
        float rate = rate_hz_;
        if (sync_on_) {
            const double beats = kLfoDivisions[sync_idx_].beats;
            rate = static_cast<float>(clampTempo(block.bpm) / 60.0 / beats);
            if (block.host_tempo && block.transport_playing) { // lock to the host's bar grid
                const double ph = block.ppq_position / beats;
                phase_ = static_cast<float>(ph - std::floor(ph));
            }
        }
        for (uint32_t i = 0; i < block.num_frames; ++i) {
            phase_ += rate / sr;
            if (phase_ >= 1.0f) phase_ -= 1.0f;

            const float lfo_l = 0.5f + 0.5f * std::sin(2.0f * std::numbers::pi_v<float> * phase_);
            const float lfo_r = 0.5f + 0.5f * std::cos(2.0f * std::numbers::pi_v<float> * phase_);

            const float d_l = (3.5f + depth_ms_ * lfo_l) * 0.001f * sr;
            const float d_r = (3.5f + depth_ms_ * lfo_r) * 0.001f * sr;

            const float wl = line_[0].read(d_l);
            const float wr = line_[1].read(d_r);

            line_[0].push(in_l[i] + feedback_ * wl);
            line_[1].push(in_r[i] + feedback_ * wr);

            out_l[i] = (1.0f - mix_) * in_l[i] + mix_ * wl;
            out_r[i] = (1.0f - mix_) * in_r[i] + mix_ * wr;
        }
    }

private:
    std::array<dsp::FractionalDelayLine, 2> line_{};
    float phase_ = 0.0f;
    float rate_hz_ = 0.8f;
    float depth_ms_ = 3.5f;
    float feedback_ = 0.15f;
    float mix_ = 0.35f;
    bool sync_on_ = false;
    std::size_t sync_idx_ = 7;
    const char* rate_id_ = "Rt";
};

/**
 * Trance Gate (114000): a tempo-synced trance gate. Rate picks the step (1/2 … 1/32) at the rig tempo
 * ("Free" makes it run in Hz), Gate sets the open part of each step, Shape the edge softness,
 * Stutter repeats the previous step's audio on alternate (or most) steps, Noise adds a gated noise
 * burst, Mix blends with the dry signal. (Earlier builds ran this unit as a noise gate.)
 */
class TranceGateComponent final : public SchemaComponentBase {
public:
    using SchemaComponentBase::SchemaComponentBase;

    void adoptRuntimeStateFrom(IComponent& previous) noexcept override {
        auto* old = dynamic_cast<TranceGateComponent*>(&previous);
        if (!old || tableId() != old->tableId() || host_sr_ != old->host_sr_) return;
        phase_ = old->phase_;
        env_ = old->env_;
        step_ = old->step_;
        rec_pos_ = old->rec_pos_;
        play_pos_ = old->play_pos_;
        rec_len_ = old->rec_len_;
        noise_ = old->noise_;
        std::swap(buf_, old->buf_);
    }

    void liveReadout(UnitLiveReadout& r) const noexcept override {
        r.lfo_phase = static_cast<float>(phase_);
        // the gate's own attenuation at the end of the block (dry/wet mix included), for the face's hold/close lamps
        r.gain_reduction_db = -20.0f * std::log10(std::max(1e-3f, (1.0f - mix_) + mix_ * env_));
    }

    // Series pipelining (RackGraph): one gate envelope, one noise stream and one stutter position drive both channels.
    bool monoPreserving() const noexcept override { return true; }

    bool paramDisplay(std::string_view id, std::string& out) const override {
        if (id != "R0") return false;
        if (free_) { char b[32]; std::snprintf(b, sizeof(b), "%.1f Hz", free_hz_); out = b; }
        else out = kGateDivisions[idx_].label;
        return true;
    }

protected:
    void onPrepare(double sr, uint32_t) override {
        const std::size_t n = static_cast<std::size_t>(sr * 2.0);
        for (auto& b : buf_) b.assign(n, 0.0f);
        onProcessControlRate();
        onReset();
    }
    void onReset() override {
        phase_ = 0.0;
        env_ = 0.0f;
        step_ = 0;
        rec_pos_ = play_pos_ = rec_len_ = 0;
        for (auto& b : buf_) std::fill(b.begin(), b.end(), 0.0f);
    }
    void onProcessControlRate() override {
        auto raw = [&](const char* id, float d) { return std::clamp(getParamOr(id, d), 0.0f, 1.0f); };
        idx_ = divisionIndex(kGateDivisions, raw("R0", 0.5f));
        free_ = getParamOr("F7", 0.0f) > 0.5f;
        free_hz_ = 1.0f + 19.0f * raw("R0", 0.5f) * raw("R0", 0.5f);
        duty_ = 0.15f + 0.8f * raw("O6", 0.5f);
        edge_ms_ = 0.5f + 30.0f * raw("S1", 0.5f);
        noise_amt_ = 0.06f * raw("N2", 0.0f);
        mix_ = raw("D3", 0.5f);
        stutter_ = raw("S4", 0.0f);
    }
    void onProcessAudio(AudioBlock& block) override {
        const float* inL = block.in[0];
        const float* inR = (block.num_channels > 1 && block.in[1]) ? block.in[1] : inL;
        float* outL = block.out[0];
        float* outR = (block.num_channels > 1 && block.out[1]) ? block.out[1] : nullptr;
        const double sr = host_sr_;
        const double beats = kGateDivisions[idx_].beats;
        const double steps_per_s = free_ ? free_hz_ : clampTempo(block.bpm) / 60.0 / beats;
        const double inc = steps_per_s / sr;
        if (!free_ && block.host_tempo && block.transport_playing) {
            const double ph = block.ppq_position / beats;
            phase_ = ph - std::floor(ph);
        }
        const float k = 1.0f - std::exp(-1.0f / (edge_ms_ * 0.001f * static_cast<float>(sr)));
        const std::size_t cap = buf_[0].size();
        for (uint32_t i = 0; i < block.num_frames; ++i) {
            phase_ += inc;
            if (phase_ >= 1.0) {
                phase_ -= std::floor(phase_);
                ++step_;
                rec_len_ = std::min(rec_pos_, cap);
                rec_pos_ = 0;
                play_pos_ = 0;
            }
            const bool stutter = cap > 0 && rec_len_ > 0 &&
                                 ((stutter_ >= 0.5f && (step_ & 1u)) || (stutter_ >= 0.85f && (step_ & 3u) != 0));
            float l = inL[i], r = inR[i];
            if (cap > 0 && rec_pos_ < cap) {
                buf_[0][rec_pos_] = l;
                buf_[1][rec_pos_] = r;
                ++rec_pos_;
            }
            if (stutter && cap > 0) {
                const std::size_t p = play_pos_ % std::max<std::size_t>(1, rec_len_);
                l = buf_[0][p];
                r = buf_[1][p];
                ++play_pos_;
            }
            const float target = phase_ < duty_ ? 1.0f : 0.0f;
            env_ += k * (target - env_);
            noise_ = noise_ * 1664525u + 1013904223u;
            const float nz = noise_amt_ * (static_cast<float>(noise_ >> 9) * (2.0f / 8388608.0f) - 1.0f);
            const float wl = (l + nz) * env_, wr = (r + nz) * env_;
            outL[i] = (1.0f - mix_) * inL[i] + mix_ * wl;
            if (outR) outR[i] = (1.0f - mix_) * inR[i] + mix_ * wr;
        }
    }

private:
    std::array<std::vector<float>, 2> buf_{};
    double phase_ = 0.0;
    float env_ = 0.0f;
    uint32_t step_ = 0, noise_ = 22222u;
    std::size_t rec_pos_ = 0, play_pos_ = 0, rec_len_ = 0;
    std::size_t idx_ = 3;
    bool free_ = false;
    float free_hz_ = 5.0f, duty_ = 0.55f, edge_ms_ = 15.0f, noise_amt_ = 0.0f, mix_ = 0.5f, stutter_ = 0.0f;
};

/**
 * Dynamics Compressor / Limiter / Maximizer / Noise Gate Component
 */
class DynamicsComponent final : public SchemaComponentBase {
public:
    void adoptRuntimeStateFrom(IComponent& previous) noexcept override {
        auto* old = dynamic_cast<DynamicsComponent*>(&previous);
        if (!old || tableId() != old->tableId() || host_sr_ != old->host_sr_) return;
        env_ = old->env_;
        gate_.adoptRuntimeStateFrom(old->gate_);
    }

    using SchemaComponentBase::SchemaComponentBase;

    // Series pipelining (RackGraph): one linked detector, one gain for both channels.
    bool monoPreserving() const noexcept override { return true; }

protected:
    void onPrepare(double sr, uint32_t /*max_block_size*/) override {
        env_ = 0.0f;
        gate_.prepare(sr);
        onProcessControlRate();
    }

    void onReset() override {
        env_ = 0.0f;
        gate_.reset();
    }

    void onProcessControlRate() override {
        is_gate_ = matchesAny(schema_, {"Gate", "Noise"});
        thresh_norm_ = getNormalizedParam({"threshold", "thresh", "sustain", "sensitivity", "input", "I0", "Th", "Lm", "comp"}, is_gate_ ? 0.25f : 0.50f);
        makeup_norm_ = getNormalizedParam({"output", "volume", "level", "makeup", "O4", "O1", "Out", "V", "gain"}, 0.55f);
        mix_ = getNormalizedParam({"mix", "dry_wet"}, 1.0f);
        gate_.setEnabled(is_gate_);
        if (is_gate_) {
            const float thr_db = -72.0f + 42.0f * thresh_norm_;
            gate_.setThresholdDb(thr_db, 12.0f);
        }
    }

    void onProcessAudio(AudioBlock& block) override {
        const float sr = static_cast<float>(host_sr_);
        const float att_coeff = std::exp(-1.0f / (0.004f * sr));
        const float rel_coeff = std::exp(-1.0f / (0.090f * sr));
        const float makeup = 0.85f + 0.55f * makeup_norm_;
        float min_gain = block.hostBlockStart() ? 1.0f : gr_min_; // the face's GR: the deepest over the host block

        for (uint32_t i = 0; i < block.num_frames; ++i) {
            float l = block.in[0] ? block.in[0][i] : 0.0f;
            float r = (block.num_channels > 1 && block.in[1]) ? block.in[1][i] : l;
            const float peak = std::max(std::abs(l), std::abs(r));

            env_ = (peak > env_) ? (att_coeff * env_ + (1.0f - att_coeff) * peak)
                                 : (rel_coeff * env_ + (1.0f - rel_coeff) * peak);

            float gain = 1.0f;
            if (is_gate_) {
                gain = gate_.detect(peak);
            } else {
                const float comp_thr = 0.16f + 0.40f * (1.0f - thresh_norm_);
                if (env_ > comp_thr) {
                    gain = std::pow(comp_thr / env_, 0.45f);
                }
                min_gain = std::min(min_gain, gain);
                gain *= makeup;
            }
            if (is_gate_) min_gain = std::min(min_gain, gain);

            if (block.out[0]) block.out[0][i] = (1.0f - mix_) * l + mix_ * (l * gain);
            if (block.num_channels > 1 && block.out[1]) block.out[1][i] = (1.0f - mix_) * r + mix_ * (r * gain);
        }
        gr_min_ = min_gain;
        if (block.hostBlockEnd()) gr_db_ = -20.0f * std::log10(std::max(1e-4f, min_gain));
    }

public:
    void liveReadout(UnitLiveReadout& r) const noexcept override { r.gain_reduction_db = std::max(0.0f, gr_db_); }

private:
    float gr_db_ = 0.0f;
    float gr_min_ = 1.0f; // running minimum gain across a host block's sub-blocks
    bool is_gate_ = false;
    float env_ = 0.0f;
    amp::HystereticNoiseGate gate_{};
    float thresh_norm_ = 0.5f;
    float makeup_norm_ = 0.55f;
    float mix_ = 1.0f;
};

} // namespace

const char* implementationKind(int32_t table_id) {
    const auto* schema = catalog::findByTableId(table_id);
    if (!schema) return "none";
    if (plugins::isPluginTable(table_id)) return "plugin";
    if (table_id == 900001 || table_id == 900002 || table_id == 900006) return "midi";
    if (table_id == 900003 || table_id == 900004) return "instrument";
    if (table_id == 900005) return "nam";
    if (table_id == kCabinetUnitTable) return "cab";
    if (table_id == kLooperUnitTable || table_id == kLegacyLoopStationTable) return "mod";
    if (table_id == kTransposeUnitTable || table_id == kDoublerRoomUnitTable) return "mod";
    if (table_id == kToneMatchUnitTable) return "eq";
    if (table_id == 114000) return "mod";
    if (isTubeAmpTable(table_id)) return "amp";
    if (isDrivePedalTable(table_id)) return "drive";
    if (table_id == 127000 || table_id == 118000 || table_id == 126000) return "ir_reverb";
    switch (schema->family) {
        case catalog::EngineFamily::CircuitWaveshaper: return "drive";
        case catalog::EngineFamily::Neural: return "drive";
        case catalog::EngineFamily::Convolution: return matchesAny(*schema, {"Studio IR Reverb", "Verb", "Reverb", "Plate", "Spring"}) ? "fdn_reverb" : "cab";
        case catalog::EngineFamily::ModernDSP:
        case catalog::EngineFamily::ClassicDSP:
        case catalog::EngineFamily::StudioHardware:
        case catalog::EngineFamily::MasteringLimiter:
            if (table_id == 100 || table_id == 7000 || table_id == 8000 || table_id == 9000 || table_id == 39000 || table_id == 135000) return "routing";
            if (table_id == 100000) return "cab";
            if (matchesAny(*schema, {"Reverb", "Cosmic", "Verb", "Studio IR Reverb", "Spring", "Lexicon", "EMT 140"})) return "fdn_reverb";
            if (matchesAny(*schema, {"Delay", "Echo", "Space Echo", "Memory Man"})) return "delay";
            if (matchesAny(*schema, {"Chorus", "Dimension", "Flanger", "Electric Mistress", "Phaser", "Small Stone", "Phase 90", "Uni-Vibe", "Ensemble", "Rotary", "Leslie", "Tremolo", "Vibrato", "Warble", "Pitch", "Whammy", "Octaver", "OC-2", "Octave"})) return "mod";
            if (matchesAny(*schema, {"Comp", "1176", "LA-2A", "160VU", "Distressor", "Fairchild", "Limiter", "Limiting", "Maximizer", "Gate", "Noise", "SPL", "Transient", "Dynamics", "Dynapoint"})) return "dynamics";
            if (matchesAny(*schema, {"Tube Screamer", "TS808", "TS9", "Overdrive", "HM-2", "Surveyor", "Distortion", "Fuzz", "Big Muff", "RAT", "Bit Crusher", "Wavefolder", "Crush", "Driver", "Tube Driver", "SansAmp", "Metal Zone", "Jackhammer", "Rangemaster", "Micro Amp", "Dime"})) return "drive";
            if (matchesAny(*schema, {"EQ", "Filter", "Equalizer", "Wah", "Cry Baby", "Talk Box", "Envelope Filter", "Ladder Filter"})) return "eq";
            return "fallback";
        default: return "none";
    }
}

std::unique_ptr<IComponent> createComponentByTableId(int32_t table_id) {
    const auto* schema = catalog::findByTableId(table_id);
    if (!schema) {
        return nullptr;
    }
    if (plugins::isPluginTable(table_id)) {
        return plugins::createComponent(table_id); // hosted Audio Unit (or a pass-through placeholder)
    }
    if (table_id == 900005) {
        return makeNamComponent(*schema);
    }
    // Circuit engine (docs/CIRCUIT_ENGINE.md §8): every validated bound unit runs on it (clean cut, no flag).
    if (auto circuit = ce::makeCircuitUnit(*schema)) {
        return circuit;
    }
    if (dsp::TubeSatCompressorComponent::handles(table_id)) { // STC-159 / STC-160 GT single-knob tube compressors
        return std::make_unique<dsp::TubeSatCompressorComponent>(*schema);
    }
    if (isMidiComponentTable(table_id)) {
        return makeMidiComponent(*schema);
    }
    if (isTubeAmpTable(table_id)) {
        return makeTubeAmp(*schema);
    }
    if (table_id == 127000 || table_id == 118000 || table_id == 126000) {
        return std::make_unique<IRReverbComponent>(*schema);
    }
    if (table_id == kCabinetUnitTable) {
        return makeCabinetUnit(*schema);
    }
    if (table_id == kLooperUnitTable || table_id == kLegacyLoopStationTable) {
        return makeLooperUnit(*schema);
    }
    if (table_id == kTransposeUnitTable) {
        return makeTransposeUnit(*schema);
    }
    if (table_id == kDoublerRoomUnitTable) {
        return makeDoublerRoomUnit(*schema);
    }
    if (table_id == kToneMatchUnitTable) {
        return makeToneMatchUnit(*schema);
    }
    if (table_id == 114000) {
        return std::make_unique<TranceGateComponent>(*schema);
    }
    if (dsp::studio_eq::isStudioEqTable(table_id)) { // SSL 4000 E/G channel EQ
        return std::make_unique<dsp::studio_eq::StudioEQComponent>(*schema);
    }
    if (isDrivePedalTable(table_id)) {
        return makeDrivePedal(*schema);
    }
    if (table_id == 110000) { // Peak Filter: resonant peak / notch + pump limiter (was the generic EQ stand-in, a pass-through)
        return std::make_unique<PeakFilterComponent>(*schema);
    }

    switch (schema->family) {
        case catalog::EngineFamily::CircuitWaveshaper:
        case catalog::EngineFamily::Neural:
            return std::make_unique<CircuitWaveshaperComponent>(*schema);
        case catalog::EngineFamily::Convolution:
            return std::make_unique<ConvolutionCabinetComponent>(*schema);
        case catalog::EngineFamily::ModernDSP:
        case catalog::EngineFamily::ClassicDSP:
        case catalog::EngineFamily::StudioHardware:
        case catalog::EngineFamily::MasteringLimiter: {
            if (table_id == 100 || table_id == 7000 || table_id == 8000 || table_id == 9000 ||
                table_id == 39000 || table_id == 100000 || table_id == 135000) {
                if (table_id == 100000) {
                    return std::make_unique<ConvolutionCabinetComponent>(*schema);
                }
                return std::make_unique<PassThroughComponent>(*schema);
            }
            if (matchesAny(*schema, {"Reverb", "Cosmic", "Verb", "Studio IR Reverb", "Spring", "Lexicon", "EMT 140"})) {
                return std::make_unique<ReverbFDNComponent>(*schema);
            }
            if (matchesAny(*schema, {"Delay", "Echo", "Space Echo", "Memory Man"})) {
                return std::make_unique<StereoDelayComponent>(*schema);
            }
            if (matchesAny(*schema, {"Chorus", "Dimension", "Flanger", "Electric Mistress", "Phaser", "Small Stone", "Phase 90", "Uni-Vibe", "Ensemble", "Rotary", "Leslie", "Tremolo", "Vibrato", "Warble", "Pitch", "Whammy", "Octaver", "OC-2", "Octave"})) {
                return std::make_unique<StereoModulationComponent>(*schema);
            }
            if (matchesAny(*schema, {"Comp", "1176", "LA-2A", "160VU", "Distressor", "Fairchild", "Limiter", "Limiting", "Maximizer", "Gate", "Noise", "SPL", "Transient", "Dynamics", "Dynapoint"})) {
                return std::make_unique<DynamicsComponent>(*schema);
            }
            if (matchesAny(*schema, {"Tube Screamer", "TS808", "TS9", "Overdrive", "HM-2", "Surveyor", "Distortion", "Fuzz", "Big Muff", "RAT", "Bit Crusher", "Wavefolder", "Crush", "Driver", "Tube Driver", "SansAmp", "Metal Zone", "Jackhammer", "Rangemaster", "Micro Amp", "Dime"})) {
                return std::make_unique<CircuitWaveshaperComponent>(*schema);
            }
            return std::make_unique<ClassicEQComponent>(*schema);
        }
        default:
            return std::make_unique<PassThroughComponent>(*schema);
    }
}

// The behavioural models that test their two-position switches with `>= 0.5` (so an untouched catalogue switch,
// raw 0.5, is on); every other behavioural model tests `> 0.5`. Circuit units answer for themselves (CircuitUnit::
// switchState, from the netlist control each parameter drives). tests/test_unit_param_mapping.cpp renders every
// catalogue switch at 0 / 0.5 / 1 and checks this answer against what the unit actually does.
bool switchReadsMidpointAsOn(int32_t table_id, std::string_view id) noexcept {
    // TubeAmpComponent / DrivePedalComponent: on(id) = P(id) >= 0.5 (amp_core, input_gate and level_matching too)
    if (isTubeAmpTable(table_id) || isDrivePedalTable(table_id)) return true;
    // ConvolutionCabinetComponent's rooms and loader: mic mute mN, solo sN, polarity piN, room mute rmN and Stereo
    if (table_id == 100000 || table_id == 119000 || table_id == 168000) {
        auto numbered = [&](std::string_view pre) {
            if (id.size() <= pre.size() || id.substr(0, pre.size()) != pre) return false;
            for (char c : id.substr(pre.size())) if (c < '0' || c > '9') return false;
            return true;
        };
        return id == "st" || numbered("m") || numbered("s") || numbered("pi") || numbered("rm");
    }
    // Headroom's own units (Transpose, Doubler Room, Looper, Cabinet): their ParamKind::Bool switches test >= 0.5
    if (const auto* s = catalog::findByTableId(table_id))
        for (const auto& p : s->params)
            if (p.id == id) return p.kind == catalog::ParamKind::Bool;
    return false;
}

int switchStateFor(int32_t table_id, std::string_view id, float raw) {
    if (raw != 0.5f) return raw > 0.5f ? 1 : 0; // every unit agrees away from the midpoint
    const auto* schema = catalog::findByTableId(table_id);
    if (schema && ce::circuitEngineEnabledFor(table_id)) {
        // A circuit unit reads the midpoint by the netlist control behind the parameter (a switch: off; a two-way
        // selector: its second position). Ask an unprepared instance (it parses the netlist, nothing more), kept.
        static std::mutex m;
        static std::map<int32_t, std::unique_ptr<IComponent>> units;
        std::lock_guard<std::mutex> lock(m);
        auto& u = units[table_id];
        if (!u) u = ce::makeCircuitUnit(*schema);
        if (u) {
            u->setParameter(id, raw);
            const int st = u->switchState(id);
            if (st >= 0) return st;
        }
    }
    return switchReadsMidpointAsOn(table_id, id) ? 1 : 0;
}

} // namespace rig
