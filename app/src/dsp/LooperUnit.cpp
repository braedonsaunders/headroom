#include "LooperUnit.hpp"
#include "../audio/WavIO.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

namespace rig {

namespace {

using catalog::ParamKind;
using catalog::ParamSchema;

constexpr ParamSchema kLooperParams[] = {
    {"mode",     "State",      ParamKind::Float, 0.0f, 0.0f, 4.0f, "", ""}, // 0=Empty/Clear, 1=Rec, 2=Play, 3=Overdub, 4=Stop
    {"level",    "Loop Level", ParamKind::Float, 0.85f, 0.0f, 1.0f, "", ""},
    {"feedback", "Decay",      ParamKind::Float, 1.0f, 0.0f, 1.0f, "", ""},
    {"reverse",  "Reverse",    ParamKind::Bool,  0.0f, 0.0f, 1.0f, "", ""},
    {"half",     "Half Speed", ParamKind::Bool,  0.0f, 0.0f, 1.0f, "", ""},
    {"quantize", "Bar Sync",   ParamKind::Bool,  0.0f, 0.0f, 1.0f, "", ""},
    {"undo",     "Undo/Redo",  ParamKind::Bool,  0.0f, 0.0f, 1.0f, "", ""},
    {"Pwr",      "On/Off",     ParamKind::Float, 1.0f, 0.0f, 1.0f, "", ""},
};

} // namespace

PhraseLooperEngine::PhraseLooperEngine() {
    prepare(48000.0);
}

void PhraseLooperEngine::prepare(double sample_rate) {
    sample_rate_ = sample_rate > 8000.0 ? sample_rate : 48000.0;
    const std::size_t target = static_cast<std::size_t>(sample_rate_ * kMaxLoopSeconds);
    if (base_l_.size() < target) {
        base_l_.assign(target, 0.0f);
        base_r_.assign(target, 0.0f);
        undo_l_.assign(target, 0.0f);
        undo_r_.assign(target, 0.0f);
    }
    max_frames_ = base_l_.size();
}

void PhraseLooperEngine::reset() noexcept {
    state_.store(static_cast<int>(LooperState::Empty), std::memory_order_relaxed);
    loop_len_.store(0, std::memory_order_relaxed);
    playhead_.store(0.0, std::memory_order_relaxed);
    overdub_count_.store(0, std::memory_order_relaxed);
    has_undo_.store(false, std::memory_order_relaxed);
    undo_active_.store(false, std::memory_order_relaxed);
}

void PhraseLooperEngine::adoptFrom(PhraseLooperEngine& other) noexcept {
    if (other.max_frames_ == 0) return;
    sample_rate_ = other.sample_rate_;
    max_frames_ = other.max_frames_;
    base_l_.swap(other.base_l_);
    base_r_.swap(other.base_r_);
    undo_l_.swap(other.undo_l_);
    undo_r_.swap(other.undo_r_);
    state_.store(other.state_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    loop_len_.store(other.loop_len_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    playhead_.store(other.playhead_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    overdub_count_.store(other.overdub_count_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    has_undo_.store(other.has_undo_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    undo_active_.store(other.undo_active_.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

void PhraseLooperEngine::snapshotForUndo() noexcept {
    const std::size_t len = std::min(loop_len_.load(std::memory_order_relaxed), max_frames_);
    if (len > 0 && undo_l_.size() >= len && undo_r_.size() >= len) {
        std::memcpy(undo_l_.data(), base_l_.data(), len * sizeof(float));
        std::memcpy(undo_r_.data(), base_r_.data(), len * sizeof(float));
        has_undo_.store(true, std::memory_order_relaxed);
        undo_active_.store(false, std::memory_order_relaxed);
    }
}

void PhraseLooperEngine::handleCommand(int cmd, double bpm) noexcept {
    auto cur = static_cast<LooperState>(state_.load(std::memory_order_relaxed));
    std::size_t len = loop_len_.load(std::memory_order_relaxed);

    auto finalizeRecordLength = [&]() noexcept {
        std::size_t recorded = static_cast<std::size_t>(playhead_.load(std::memory_order_relaxed));
        if (recorded < 256) {
            state_.store(static_cast<int>(LooperState::Empty), std::memory_order_relaxed);
            loop_len_.store(0, std::memory_order_relaxed);
            playhead_.store(0.0, std::memory_order_relaxed);
            return false;
        }
        if (quantize_.load(std::memory_order_relaxed) && bpm >= 20.0 && bpm <= 300.0) {
            const double samples_per_beat = (60.0 / bpm) * sample_rate_;
            const double beats = std::max(1.0, std::round(static_cast<double>(recorded) / samples_per_beat));
            const std::size_t q_len = std::min(max_frames_, static_cast<std::size_t>( std::llround(beats * samples_per_beat)));
            if (q_len > recorded && q_len <= max_frames_) {
                std::memset(base_l_.data() + recorded, 0, (q_len - recorded) * sizeof(float));
                std::memset(base_r_.data() + recorded, 0, (q_len - recorded) * sizeof(float));
            }
            recorded = std::max<std::size_t>(256, q_len);
        }
        // Apply seamless boundary crossfade over kCrossfadeSamples
        const std::size_t xf = std::min<std::size_t>(kCrossfadeSamples, recorded / 4);
        for (std::size_t i = 0; i < xf; ++i) {
            const float g = static_cast<float>(i) / static_cast<float>(xf);
            base_l_[i] *= g;
            base_r_[i] *= g;
            base_l_[recorded - 1 - i] *= g;
            base_r_[recorded - 1 - i] *= g;
        }
        loop_len_.store(recorded, std::memory_order_relaxed);
        playhead_.store(0.0, std::memory_order_relaxed);
        snapshotForUndo();
        return true;
    };

    if (cmd == 1) { // Record -> Play -> Overdub -> Play toggle (classic 1-button looper pedal behavior)
        if (cur == LooperState::Empty) {
            cmd = 2; // Record
        } else if (cur == LooperState::Recording) {
            cmd = 4; // Play
        } else if (cur == LooperState::Playing) {
            cmd = 3; // Overdub
        } else if (cur == LooperState::Overdubbing) {
            cmd = 4; // Play
        } else if (cur == LooperState::Stopped) {
            cmd = 4; // Play
        }
    }

    switch (cmd) {
        case 2: // Record fresh
            loop_len_.store(0, std::memory_order_relaxed);
            playhead_.store(0.0, std::memory_order_relaxed);
            overdub_count_.store(0, std::memory_order_relaxed);
            has_undo_.store(false, std::memory_order_relaxed);
            undo_active_.store(false, std::memory_order_relaxed);
            state_.store(static_cast<int>(LooperState::Recording), std::memory_order_relaxed);
            break;
        case 3: // Overdub
            if (cur == LooperState::Recording) {
                if (!finalizeRecordLength()) break;
            }
            if (loop_len_.load(std::memory_order_relaxed) > 0) {
                snapshotForUndo();
                overdub_count_.fetch_add(1, std::memory_order_relaxed);
                state_.store(static_cast<int>(LooperState::Overdubbing), std::memory_order_relaxed);
            } else {
                playhead_.store(0.0, std::memory_order_relaxed);
                state_.store(static_cast<int>(LooperState::Recording), std::memory_order_relaxed);
            }
            break;
        case 4: // Play
            if (cur == LooperState::Recording) {
                if (!finalizeRecordLength()) break;
            }
            if (loop_len_.load(std::memory_order_relaxed) > 0) {
                if (cur == LooperState::Stopped) playhead_.store(0.0, std::memory_order_relaxed);
                state_.store(static_cast<int>(LooperState::Playing), std::memory_order_relaxed);
            }
            break;
        case 5: // Stop
            if (cur == LooperState::Recording) {
                finalizeRecordLength();
            }
            if (loop_len_.load(std::memory_order_relaxed) > 0) {
                state_.store(static_cast<int>(LooperState::Stopped), std::memory_order_relaxed);
            } else {
                state_.store(static_cast<int>(LooperState::Empty), std::memory_order_relaxed);
            }
            break;
        case 6: // Clear
            reset();
            break;
        case 7: // Undo / Redo swap
            if (has_undo_.load(std::memory_order_relaxed) && len > 0 && len <= max_frames_) {
                for (std::size_t i = 0; i < len; ++i) {
                    std::swap(base_l_[i], undo_l_[i]);
                    std::swap(base_r_[i], undo_r_[i]);
                }
                const bool now_undone = !undo_active_.load(std::memory_order_relaxed);
                undo_active_.store(now_undone, std::memory_order_relaxed);
            }
            break;
        default:
            break;
    }
}

void PhraseLooperEngine::processStereo(float* left, float* right, uint32_t num_frames, double bpm) noexcept {
    if (!left || !right || max_frames_ == 0) return;

    const int pending = cmd_trigger_.exchange(0, std::memory_order_acq_rel);
    if (pending != 0) {
        handleCommand(pending, bpm);
    }

    float blk_peak = 0.0f;
    for (uint32_t i = 0; i < num_frames; ++i) {
        blk_peak = std::max(blk_peak, std::max(std::fabs(left[i]), std::fabs(right[i])));
    }
    const float prev_peak = input_peak_.load(std::memory_order_relaxed);
    input_peak_.store(blk_peak > prev_peak ? blk_peak : prev_peak * 0.92f, std::memory_order_relaxed);

    const auto st = static_cast<LooperState>(state_.load(std::memory_order_relaxed));
    if (st == LooperState::Empty || st == LooperState::Stopped) return;

    const float lvl = level_.load(std::memory_order_relaxed);
    const float fb = feedback_.load(std::memory_order_relaxed);
    const bool rev = reverse_.load(std::memory_order_relaxed);
    const bool half = half_speed_.load(std::memory_order_relaxed);
    const double step = half ? 0.5 : 1.0;

    double pos = playhead_.load(std::memory_order_relaxed);
    std::size_t len = loop_len_.load(std::memory_order_relaxed);

    if (st == LooperState::Recording) {
        for (uint32_t i = 0; i < num_frames; ++i) {
            const std::size_t idx = static_cast<std::size_t>(pos);
            if (idx < max_frames_) {
                base_l_[idx] = left[i];
                base_r_[idx] = right[i];
                pos += 1.0;
            } else {
                // Reached max 60s buffer: automatically switch to Play
                loop_len_.store(max_frames_, std::memory_order_relaxed);
                playhead_.store(0.0, std::memory_order_relaxed);
                snapshotForUndo();
                state_.store(static_cast<int>(LooperState::Playing), std::memory_order_relaxed);
                return;
            }
        }
        playhead_.store(pos, std::memory_order_relaxed);
        loop_len_.store(static_cast<std::size_t>(pos), std::memory_order_relaxed);
        return;
    }

    if (len < 64) return;
    if (len > max_frames_) len = max_frames_;

    for (uint32_t i = 0; i < num_frames; ++i) {
        if (pos >= static_cast<double>(len)) pos -= static_cast<double>(len);
        if (pos < 0.0) pos += static_cast<double>(len);

        const double read_pos = rev ? (static_cast<double>(len - 1) - pos) : pos;
        const std::size_t idx0 = static_cast<std::size_t>(read_pos) % len;
        const std::size_t idx1 = (idx0 + 1) % len;
        const float frac = static_cast<float>(read_pos - std::floor(read_pos));

        const float loop_l = base_l_[idx0] + frac * (base_l_[idx1] - base_l_[idx0]);
        const float loop_r = base_r_[idx0] + frac * (base_r_[idx1] - base_r_[idx0]);

        const float in_l = left[i];
        const float in_r = right[i];

        if (st == LooperState::Overdubbing) {
            base_l_[idx0] = std::clamp(base_l_[idx0] * fb + in_l, -4.0f, 4.0f);
            base_r_[idx0] = std::clamp(base_r_[idx0] * fb + in_r, -4.0f, 4.0f);
        }

        left[i] = in_l + loop_l * lvl;
        right[i] = in_r + loop_r * lvl;

        pos += step;
    }

    if (pos >= static_cast<double>(len)) pos = std::fmod(pos, static_cast<double>(len));
    playhead_.store(pos, std::memory_order_relaxed);
}

LooperTelemetry PhraseLooperEngine::telemetry() const noexcept {
    LooperTelemetry t;
    t.state = static_cast<LooperState>(state_.load(std::memory_order_relaxed));
    t.pre_rig = pre_rig_.load(std::memory_order_relaxed);
    t.reverse = reverse_.load(std::memory_order_relaxed);
    t.half_speed = half_speed_.load(std::memory_order_relaxed);
    t.quantize = quantize_.load(std::memory_order_relaxed);
    const bool has_u = has_undo_.load(std::memory_order_relaxed);
    const bool u_act = undo_active_.load(std::memory_order_relaxed);
    t.can_undo = has_u && !u_act;
    t.can_redo = has_u && u_act;
    t.overdub_layers = overdub_count_.load(std::memory_order_relaxed);
    const std::size_t len = loop_len_.load(std::memory_order_relaxed);
    const double pos = playhead_.load(std::memory_order_relaxed);
    t.length_seconds = sample_rate_ > 0.0 ? static_cast<double>(len) / sample_rate_ : 0.0;
    t.position_norm = len > 0 ? std::clamp(pos / static_cast<double>(len), 0.0, 1.0) : 0.0;
    t.level = level_.load(std::memory_order_relaxed);
    t.input_peak = input_peak_.load(std::memory_order_relaxed);
    return t;
}

std::string PhraseLooperEngine::toJson() const {
    const auto t = telemetry();
    const char* sname = "empty";
    switch (t.state) {
        case LooperState::Empty:       sname = "empty"; break;
        case LooperState::Recording:   sname = "recording"; break;
        case LooperState::Playing:     sname = "playing"; break;
        case LooperState::Overdubbing: sname = "overdubbing"; break;
        case LooperState::Stopped:     sname = "stopped"; break;
    }
    const std::size_t len = std::min(loop_len_.load(std::memory_order_relaxed), max_frames_);
    std::ostringstream oss;
    oss << "{\"state\":\"" << sname << "\""
        << ",\"state_id\":" << static_cast<int>(t.state)
        << ",\"pre_rig\":" << (t.pre_rig ? "true" : "false")
        << ",\"reverse\":" << (t.reverse ? "true" : "false")
        << ",\"half_speed\":" << (t.half_speed ? "true" : "false")
        << ",\"quantize\":" << (t.quantize ? "true" : "false")
        << ",\"can_undo\":" << (t.can_undo ? "true" : "false")
        << ",\"can_redo\":" << (t.can_redo ? "true" : "false")
        << ",\"layers\":" << t.overdub_layers
        << ",\"length_s\":" << t.length_seconds
        << ",\"pos\":" << t.position_norm
        << ",\"level\":" << t.level
        << ",\"feedback\":" << feedback_.load(std::memory_order_relaxed)
        << ",\"peak\":" << t.input_peak
        << ",\"peaks\":[";
    constexpr int kBins = 32;
    for (int b = 0; b < kBins; ++b) {
        float pk = 0.0f;
        if (len >= static_cast<std::size_t>(kBins) && base_l_.size() >= len) {
            const std::size_t s0 = (static_cast<std::size_t>(b) * len) / kBins;
            const std::size_t s1 = (static_cast<std::size_t>(b + 1) * len) / kBins;
            const std::size_t stride = std::max<std::size_t>(1, (s1 - s0) / 16);
            for (std::size_t i = s0; i < s1; i += stride) {
                pk = std::max(pk, std::max(std::fabs(base_l_[i]), std::fabs(base_r_[i])));
            }
        }
        oss << (b ? "," : "") << std::clamp(pk, 0.0f, 1.0f);
    }
    oss << "]}";
    return oss.str();
}

bool PhraseLooperEngine::exportWav(const std::string& path, std::string* err) const {
    const std::size_t len = std::min(loop_len_.load(std::memory_order_relaxed), max_frames_);
    if (len == 0) {
        if (err) *err = "Looper buffer is empty";
        return false;
    }
    const uint32_t sr = static_cast<uint32_t>(std::lround(sample_rate_));
    const std::vector<float> l_vec(base_l_.begin(), base_l_.begin() + static_cast<std::ptrdiff_t>(len));
    const std::vector<float> r_vec(base_r_.begin(), base_r_.begin() + static_cast<std::ptrdiff_t>(len));
    if (!audio::writeWavFile16(path, sr, l_vec, r_vec)) {
        if (err) *err = "Failed to write WAV file: " + path;
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Rack Component wrapper (table 900200 & legacy 53000)
// ─────────────────────────────────────────────────────────────────────────────
class LooperComponent final : public SchemaComponentBase {
public:
    explicit LooperComponent(const catalog::ComponentSchema& schema)
        : SchemaComponentBase(schema) {}

    std::vector<ExtraParam> extraParams() const override {
        return {
            {"mode", "Transport", {"Clear / Empty", "Record", "Play", "Overdub", "Stop"}, 0.0f, {}}
        };
    }

    void adoptRuntimeStateFrom(IComponent& previous) noexcept override {
        if (auto* old = dynamic_cast<LooperComponent*>(&previous)) {
            engine_.adoptFrom(old->engine_);
        }
    }

    const PhraseLooperEngine& engine() const noexcept { return engine_; }
    PhraseLooperEngine& engine() noexcept { return engine_; }

    // Series pipelining (RackGraph): empty, stopped or recording, the output is the input. A playing loop may hold a
    // stereo take, so then the answer is no.
    bool monoPreserving() const noexcept override {
        const LooperState st = engine_.state();
        return st == LooperState::Empty || st == LooperState::Stopped || st == LooperState::Recording;
    }

protected:
    void onPrepare(double sample_rate, uint32_t /*max_block_size*/) override {
        engine_.prepare(sample_rate);
    }

    void onReset() override {
        // Keep recorded loop on DSP reset unless explicitly cleared via mode=0
    }

    void onParameterChanged(std::string_view param_id, float value) override {
        if (param_id == "level" || param_id == "LL") {
            engine_.setLevel(value * 1.2f);
        } else if (param_id == "feedback") {
            engine_.setFeedback(value);
        } else if (param_id == "reverse") {
            engine_.setReverse(value >= 0.5f);
        } else if (param_id == "half") {
            engine_.setHalfSpeed(value >= 0.5f);
        } else if (param_id == "quantize") {
            engine_.setQuantize(value >= 0.5f);
        } else if (param_id == "undo") {
            engine_.triggerUndoRedo();
        } else if (param_id == "mode") {
            const int m = std::clamp(static_cast<int>(std::lround(value)), 0, 4);
            switch (m) {
                case 0: engine_.triggerClear(); break;
                case 1: engine_.triggerRecord(); break;
                case 2: engine_.triggerPlay(); break;
                case 3: engine_.triggerOverdub(); break;
                case 4: engine_.triggerStop(); break;
            }
        }
    }

    void onProcessAudio(AudioBlock& block) override {
        block.copyInToOut();
        if (!block.out || !block.out[0] || block.num_frames == 0) return;
        float* l = block.out[0];
        float* r = (block.num_channels > 1 && block.out[1]) ? block.out[1] : block.out[0];
        engine_.processStereo(l, r, block.num_frames, block.bpm);
    }

private:
    PhraseLooperEngine engine_;
};

const catalog::ComponentSchema& looperUnitSchema() noexcept {
    static const catalog::ComponentSchema s{
        kLooperUnitTable,
        "Studio Looper",
        "Stereo Phrase & Re-Amp Looper",
        "HeadroomLooper",
        catalog::EngineFamily::ModernDSP,
        -1,
        false,
        std::span<const ParamSchema>(kLooperParams)
    };
    return s;
}

std::unique_ptr<IComponent> makeLooperUnit(const catalog::ComponentSchema& schema) {
    return std::make_unique<LooperComponent>(schema);
}

bool isLooperUnit(const IComponent& c) noexcept {
    return dynamic_cast<const LooperComponent*>(&c) != nullptr;
}

std::string looperSlotJson(const IComponent& unit) {
    const auto* u = dynamic_cast<const LooperComponent*>(&unit);
    if (!u) return {};
    return "\"looper\":" + u->engine().toJson();
}

} // namespace rig
