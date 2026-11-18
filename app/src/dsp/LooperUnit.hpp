#pragma once
// Studio Looper unit (table 900200) & Global Pre/Post Looper engine.
// Supports Record, Overdub, Play, Stop, Clear, Undo/Redo last overdub layer,
// Half-Speed (0.5x), Reverse playback, Tempo Bar-Quantize, and Pre-Rig (Re-Amp DI)
// vs. Post-Rig (Output Layering) operation.
//
// Real-time contract: all loop buffers are pre-allocated off the audio callback;
// processAudio() / processInline() never allocate, lock, or block.

#include "../core/IComponent.hpp"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rig {

inline constexpr int32_t kLooperUnitTable = 900200;
inline constexpr int32_t kLegacyLoopStationTable = 53000;

enum class LooperState : int {
    Empty = 0,
    Recording = 1,
    Playing = 2,
    Overdubbing = 3,
    Stopped = 4
};

struct LooperTelemetry {
    LooperState state = LooperState::Empty;
    bool pre_rig = false;       // true = loops raw DI into the rig (re-amp mode); false = loops post-rig output
    bool reverse = false;
    bool half_speed = false;
    bool quantize = false;
    bool can_undo = false;
    bool can_redo = false;
    int overdub_layers = 0;
    double length_seconds = 0.0;
    double position_norm = 0.0; // 0..1 playhead position
    float level = 0.85f;
    float input_peak = 0.0f;
};

// Lock-free real-time stereo phrase looper engine (used both by the rack LooperUnit 900200
// and by the global Pre/Post Rig Looper in RealtimeAudioServer).
class PhraseLooperEngine {
public:
    static constexpr std::size_t kMaxLoopSeconds = 60;
    static constexpr std::size_t kDefaultSampleRate = 48000;
    static constexpr std::size_t kCrossfadeSamples = 128;

    PhraseLooperEngine();

    void prepare(double sample_rate);
    void reset() noexcept;

    // Control-thread commands (lock-free atomic triggers consumed on the audio thread)
    void triggerRecordOrPlay() noexcept { cmd_trigger_.store(1, std::memory_order_release); }
    void triggerRecord() noexcept       { cmd_trigger_.store(2, std::memory_order_release); }
    void triggerOverdub() noexcept      { cmd_trigger_.store(3, std::memory_order_release); }
    void triggerPlay() noexcept         { cmd_trigger_.store(4, std::memory_order_release); }
    void triggerStop() noexcept         { cmd_trigger_.store(5, std::memory_order_release); }
    void triggerClear() noexcept        { cmd_trigger_.store(6, std::memory_order_release); }
    void triggerUndoRedo() noexcept     { cmd_trigger_.store(7, std::memory_order_release); }

    void setLevel(float level_linear) noexcept { level_.store(std::clamp(level_linear, 0.0f, 2.0f), std::memory_order_relaxed); }
    void setFeedback(float fb) noexcept        { feedback_.store(std::clamp(fb, 0.0f, 1.0f), std::memory_order_relaxed); }
    void setReverse(bool rev) noexcept         { reverse_.store(rev, std::memory_order_relaxed); }
    void setHalfSpeed(bool half) noexcept      { half_speed_.store(half, std::memory_order_relaxed); }
    void setQuantize(bool q) noexcept          { quantize_.store(q, std::memory_order_relaxed); }
    void setPreRig(bool pre) noexcept          { pre_rig_.store(pre, std::memory_order_relaxed); }

    bool isPreRig() const noexcept { return pre_rig_.load(std::memory_order_relaxed); }
    LooperState state() const noexcept { return static_cast<LooperState>(state_.load(std::memory_order_relaxed)); }

    // Audio-thread processing: reads/writes stereo buffers in-place.
    void processStereo(float* left, float* right, uint32_t num_frames, double bpm) noexcept;

    // Telemetry snapshot (safe to call from HTTP/UI thread)
    LooperTelemetry telemetry() const noexcept;
    std::string toJson() const;

    // Export current loop buffer to a 32-bit float stereo WAV file (control thread).
    bool exportWav(const std::string& path, std::string* err = nullptr) const;

    // Transfer state across rack graph rebuilds without allocation.
    void adoptFrom(PhraseLooperEngine& other) noexcept;

private:
    void handleCommand(int cmd, double bpm) noexcept;
    void snapshotForUndo() noexcept;

    double sample_rate_ = 48000.0;
    std::size_t max_frames_ = kMaxLoopSeconds * kDefaultSampleRate;

    // Primary loop buffer + backup layer for instant undo/redo
    std::vector<float> base_l_;
    std::vector<float> base_r_;
    std::vector<float> undo_l_;
    std::vector<float> undo_r_;

    std::atomic<int> state_{static_cast<int>(LooperState::Empty)};
    std::atomic<int> cmd_trigger_{0};

    std::atomic<std::size_t> loop_len_{0};
    std::atomic<double> playhead_{0.0};
    std::atomic<int> overdub_count_{0};
    std::atomic<bool> has_undo_{false};
    std::atomic<bool> undo_active_{false};

    std::atomic<float> level_{0.85f};
    std::atomic<float> feedback_{1.0f};
    std::atomic<bool> reverse_{false};
    std::atomic<bool> half_speed_{false};
    std::atomic<bool> quantize_{false};
    std::atomic<bool> pre_rig_{false};
    std::atomic<float> input_peak_{0.0f};
};

std::unique_ptr<IComponent> makeLooperUnit(const catalog::ComponentSchema& schema);
const catalog::ComponentSchema& looperUnitSchema() noexcept;
bool isLooperUnit(const IComponent& c) noexcept;
std::string looperSlotJson(const IComponent& unit);

} // namespace rig
