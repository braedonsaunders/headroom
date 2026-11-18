#pragma once
// Backing-track player for the Practice view: a decoded song mixed into the output *after* the rig, so the
// guitar goes through the amp and the track plays alongside it untouched.
//
//   - Decoding (src/platform/AudioDecoder: ExtAudioFile on macOS — WAV, AIFF, CAF, MP3, AAC/M4A, ALAC, FLAC …;
//     miniaudio's WAV/AIFF/FLAC/MP3 decoders elsewhere) happens off the audio thread into a
//     BackingAudio buffer at the engine rate. The control thread hands it to the audio thread through a
//     single atomic slot; buffers the audio thread lets go of come back through a small lock-free ring and are
//     freed by the control thread (collect()).
//   - Speed (0.5–1.5) keeps the pitch and transpose (±12 semitones) keeps the tempo: both run through
//     src/platform/TimeStretch (Apple's AUNewTimePitch on macOS, Signalsmith Stretch elsewhere), created and
//     initialised off the audio thread and rendered in place (it neither allocates nor locks while rendering —
//     see tests/test_backing_track.cpp). At 100 % and 0 st the stretcher is bypassed and the buffer is read directly.
//   - Loop A–B (with a short crossfade at the splice), seek (crossfaded), volume (smoothed), a count-in click
//     and an optional duck that pulls the track down while the input (your guitar) is active.
//
// Real-time contract: process() never allocates, locks, blocks or makes syscalls. Control calls may come from
// one control thread at a time (the engine's HTTP thread).

#include "../platform/TimeStretch.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace rig::audio {

// A decoded backing track: interleaved stereo at the engine rate, 16-bit (peak-scaled, so a quiet master
// keeps its resolution), plus the overview the UI draws.
struct BackingAudio {
    std::vector<int16_t> pcm;      // L R L R …
    std::size_t frames = 0;
    double sample_rate = 48000.0;
    float scale = 1.0f / 32767.0f; // int16 → float (undoes the peak scaling)
    float norm_gain = 1.0f;        // loudness normalisation (gated RMS → kTargetRms)
    float peak = 0.0f;             // source peak, linear
    float rms_db = -99.0f;         // gated RMS of the source, dBFS
    static constexpr std::size_t kBins = 4000;
    std::vector<float> bin_peak;   // kBins peak magnitudes (0..1 of the source peak), for the waveform
    std::vector<float> bin_rms;    // kBins RMS values, same scale
    double seconds() const { return sample_rate > 0 ? double(frames) / sample_rate : 0.0; }
    static constexpr float kTargetRms = 0.125f; // ≈ −18 dBFS: sits with a typical rig level at volume 100 %
};

struct BackingFileInfo {
    double seconds = 0.0;
    double sample_rate = 0.0;
    int channels = 0;
    std::string title, artist; // from the file's tags when it has them
    std::string format;        // "MP3", "AAC", "WAV" …
};

// Opens the file and reads its length / tags without decoding it. False (and `err`) when it can't be read.
bool probeBackingFile(const std::string& path, BackingFileInfo& info, std::string* err = nullptr);

// Decodes the whole file to stereo at `sample_rate` (blocking; never call on the audio thread). `cancel` is
// polled between chunks. Files longer than `max_seconds` are refused.
std::unique_ptr<BackingAudio> decodeBackingFile(const std::string& path, double sample_rate, std::string* err = nullptr,
                                                const std::atomic<bool>* cancel = nullptr, double max_seconds = 45 * 60.0);

class BackingTrackPlayer {
public:
    static constexpr uint32_t kMaxBlock = 1024;
    static constexpr int kRetireSlots = 8;
    static constexpr int kXfade = 384;          // splice crossfade (seek, loop wrap), frames

    BackingTrackPlayer();
    ~BackingTrackPlayer();
    BackingTrackPlayer(const BackingTrackPlayer&) = delete;
    BackingTrackPlayer& operator=(const BackingTrackPlayer&) = delete;

    // Not concurrent with process() (call before audio starts or while the device is stopped).
    void prepare(double sample_rate);
    double sampleRate() const { return sample_rate_; }

    // ---- control thread ----
    // Hands a decoded track to the audio thread (paused, at `start_sec`). The previous one is released.
    void setAudio(std::unique_ptr<BackingAudio> audio, double start_sec = 0.0);
    void clear();
    bool hasAudio() const { return duration_.load(std::memory_order_relaxed) > 0.0; }
    void play();                         // starts after the count-in when one is set
    void pause();
    bool playing() const { return want_play_.load(std::memory_order_relaxed); }
    void seek(double seconds);
    void setVolume(float linear);        // 0..2 (1 = unity after normalisation)
    void setLoop(bool on, double a_sec, double b_sec);
    void setSpeed(float speed);          // 0.5..1.5, pitch kept
    void setSemitones(float st);         // −12..12, tempo kept
    void setCountIn(int beats, double bpm); // 0 = off
    void setDuck(bool on, float depth_db = -12.0f);
    void setNormalize(bool on) { normalize_.store(on, std::memory_order_relaxed); }

    float volume() const { return volume_.load(std::memory_order_relaxed); }
    float speed() const { return speed_.load(std::memory_order_relaxed); }
    float semitones() const { return semitones_.load(std::memory_order_relaxed); }
    bool loopOn() const { return loop_on_.load(std::memory_order_relaxed); }
    double loopA() const { return loop_a_.load(std::memory_order_relaxed); }
    double loopB() const { return loop_b_.load(std::memory_order_relaxed); }
    int countInBeats() const { return countin_beats_.load(std::memory_order_relaxed); }
    double countInBpm() const { return countin_bpm_.load(std::memory_order_relaxed); }
    bool duckOn() const { return duck_on_.load(std::memory_order_relaxed); }
    float duckDepthDb() const { return duck_depth_db_.load(std::memory_order_relaxed); }
    bool normalize() const { return normalize_.load(std::memory_order_relaxed); }

    double position() const;             // seconds into the track (what you hear)
    double duration() const { return duration_.load(std::memory_order_relaxed); }
    int countInLeft() const { return countin_left_beats_.load(std::memory_order_relaxed); } // beats still to click
    float duckAmount() const { return duck_now_.load(std::memory_order_relaxed); }         // 0 = none, 1 = full depth
    float level() const { return level_.load(std::memory_order_relaxed); }                 // output RMS (smoothed)
    bool ended() const { return ended_.load(std::memory_order_relaxed); }
    double stretchLatency() const { return stretch_latency_sec_; }

    // Frees buffers the audio thread has released. Cheap; call from any control path (telemetry does).
    void collect();

    // ---- audio thread ----
    // Adds the track into out_l/out_r (n ≤ kMaxBlock per call; larger blocks are split). `side_l/r` is the
    // pre-rig input used by the duck (may be null). The sum is soft-limited above −0.4 dBFS.
    void process(float* out_l, float* out_r, uint32_t n, const float* side_l, const float* side_r) noexcept;

private:
    void processChunk(float* out_l, float* out_r, uint32_t n, const float* side_l, const float* side_r) noexcept;
    void pull(float* l, float* r, uint32_t n) noexcept; // source reader: loop, splice crossfade, end of track
    void readFrame(int64_t f, float& l, float& r) const noexcept;
    void jumpTo(int64_t target) noexcept;
    bool retire(BackingAudio* a) noexcept;
    bool retireSlotFree() const noexcept;
    static void stretchPull(void* ref, float* l, float* r, uint32_t n) noexcept;

    double sample_rate_ = 48000.0;

    // control-side ownership: every buffer ever handed over that hasn't been freed yet
    std::mutex own_mtx_;
    std::vector<std::unique_ptr<BackingAudio>> owned_;
    std::atomic<BackingAudio*> incoming_{nullptr};
    std::atomic<bool> clear_req_{false};
    std::array<std::atomic<BackingAudio*>, kRetireSlots> retired_{};
    std::atomic<double> start_sec_{0.0};

    // control → audio
    std::atomic<bool> want_play_{false};
    std::atomic<int> countin_req_{0};
    std::atomic<double> seek_req_{-1.0};
    std::atomic<float> volume_{1.0f}, speed_{1.0f}, semitones_{0.0f};
    std::atomic<bool> loop_on_{false};
    std::atomic<double> loop_a_{0.0}, loop_b_{0.0};
    std::atomic<int> countin_beats_{0};
    std::atomic<double> countin_bpm_{120.0};
    std::atomic<bool> duck_on_{false};
    std::atomic<float> duck_depth_db_{-12.0f};
    std::atomic<bool> normalize_{true};

    // audio → control
    std::atomic<double> duration_{0.0};      // control-side copy of the loaded track length
    std::atomic<double> pos_pub_{0.0};
    std::atomic<int> countin_left_beats_{0};
    std::atomic<float> duck_now_{0.0f}, level_{0.0f};
    std::atomic<bool> ended_{false};

    // audio-thread state
    BackingAudio* cur_ = nullptr;
    int64_t pos_ = 0;                        // next source frame to read
    int64_t xf_from_ = 0;                    // splice crossfade: old read position
    int xf_left_ = 0;
    float run_ = 0.0f;                       // play/pause fade 0..1
    bool running_ = false;
    int64_t countin_left_ = 0, countin_total_ = 0, beat_len_ = 1;
    double click_phase_ = 0.0;
    float gain_ = 0.0f;                      // smoothed volume × normalisation × duck
    float duck_env_ = 0.0f, duck_gain_ = 1.0f;
    int duck_hold_ = 0;
    bool stretching_ = false;
    float last_speed_ = 1.0f;

    // time-pitch (created in prepare)
    platform::TimeStretch stretch_;
    double stretch_latency_sec_ = 0.0;
    std::vector<float> scratch_l_, scratch_r_;
};

} // namespace rig::audio
