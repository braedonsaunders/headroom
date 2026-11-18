#pragma once
// LoopStation — Headroom's global multitrack looper (the Looper tab; GET /api/looper, see docs/ENGINE_API.md).
//
// Modelled on hardware loop stations rather than a DAW clip launcher:
//   - kTracks tracks, each a stack of layers (the first recording + one layer per overdub pass), so undo /
//     redo step through passes like a Boss RC or Ditto (several levels deep, not just the last one);
//   - three sync modes: Free (independent tracks), Loop (the first loop is the master; other tracks snap to
//     ¼, ½, 1, 2, 3 … master lengths, Pigtronix Infinity / RC "loop sync") and Tempo (bars of the rig tempo:
//     recording starts on the next bar line or after a one-bar count-in, length = N bars or the nearest bar);
//   - stop modes Instant / End of loop / Fade, an All Start/Stop, per-track level, pan, mute, reverse and
//     ½× / 1× / 2× speed (octave down / up), feedback that ages older layers each overdub pass (Blooper /
//     Ditto "decay"), input monitoring, a metronome click (never recorded) and WAV export of the mix or stems.
//
// Real-time contract: process*() never allocates, locks or makes system calls. Layer buffers come from a
// fixed pool reserved in prepare() with calloc, so untouched pages cost no RAM (the first write to a page is
// a kernel zero-fill fault, not a syscall). Freed layers are re-zeroed incrementally on the audio thread.
// Commands from the HTTP / MIDI threads go through a lock-free bounded MPSC queue; settings are atomics.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace rig {

enum class LoopTrackState : int { Empty = 0, Recording = 1, Playing = 2, Overdubbing = 3, Stopped = 4, Armed = 5 };
enum class LoopSync : int { Free = 0, Loop = 1, Tempo = 2 };
enum class LoopStopMode : int { Instant = 0, LoopEnd = 1, Fade = 2 };

class LoopStation {
public:
    static constexpr int kTracks = 3;
    static constexpr int kPool = 16;          // layer buffers shared by all tracks
    static constexpr int kMaxLayers = 12;     // per track: the first recording + overdub passes (incl. redo)
    static constexpr double kMaxSeconds = 120.0;
    static constexpr int kQueue = 64;

    LoopStation();
    ~LoopStation();
    LoopStation(const LoopStation&) = delete;
    LoopStation& operator=(const LoopStation&) = delete;

    // Control thread, audio stopped. Reserves the pool for this rate and clears every track.
    void prepare(double sample_rate);

    // ── Audio thread ──
    // In the app: processPre() on the DI before the rig, processPost() on the rig output. The loops run at
    // whichever point the Pre-rig / Post-rig setting picks (latched once per block); the click is always
    // added post-rig, so it is never recorded and never goes through the amp.
    void processPre(float* l, float* r, uint32_t n, double bpm) noexcept;
    void processPost(float* l, float* r, uint32_t n, double bpm) noexcept;
    // Both in one call (tests, offline use): loops + click on the same buffer.
    void processStereo(float* l, float* r, uint32_t n, double bpm) noexcept;

    // ── Any thread ──
    enum Cmd : int {
        kPress = 1, kRecord, kOverdub, kPlay, kStop, kStopAll, kPlayAll, kAllStartStop, kClear, kClearAll,
        kUndo, kRedo, kUndoRedo, kStopInstantAll
    };
    bool post(int cmd, int track = -1, float value = 0.0f) noexcept;   // track -1 = selected track

    void select(int t) noexcept { if (t >= 0 && t < kTracks) selected_.store(t, std::memory_order_relaxed); }
    int selected() const noexcept { return selected_.load(std::memory_order_relaxed); }
    void setPreRig(bool v) noexcept { pre_rig_.store(v, std::memory_order_relaxed); }
    bool preRig() const noexcept { return pre_rig_.load(std::memory_order_relaxed); }
    void setMonitor(bool v) noexcept { monitor_.store(v, std::memory_order_relaxed); }
    void setSync(LoopSync s) noexcept { sync_.store(static_cast<int>(s), std::memory_order_relaxed); }
    void setBars(int b) noexcept;
    void setBeatsPerBar(int b) noexcept;
    void setCountIn(bool v) noexcept { count_in_.store(v, std::memory_order_relaxed); }
    void setClick(bool v) noexcept { click_.store(v, std::memory_order_relaxed); }
    void setClickLevel(float v) noexcept;
    void setStopMode(LoopStopMode m) noexcept { stop_mode_.store(static_cast<int>(m), std::memory_order_relaxed); }
    void setFadeSeconds(float s) noexcept;
    void setAfterRecDub(bool dub) noexcept { after_rec_dub_.store(dub, std::memory_order_relaxed); }
    void setFeedback(float fb) noexcept;
    void setLevel(float lvl) noexcept;
    void setTrackLevel(int t, float v) noexcept;
    void setTrackPan(int t, float v) noexcept;
    void setTrackMute(int t, bool v) noexcept;
    void setTrackReverse(int t, bool v) noexcept;
    void setTrackSpeed(int t, float v) noexcept;   // 0.5, 1 or 2

    // True while anything is recorded, armed, counting in or clicking (keeps the app's output open).
    bool active() const noexcept { return active_.load(std::memory_order_relaxed); }

    // ── HTTP thread ──
    std::string toJson() const;
    std::string waveJson(int track, int bins) const;
    // The whole /api/looper surface (see ENGINE_API.md). `q(key)` returns a query value or "".
    std::string handleApi(const std::string& path, const std::function<std::string(const char*)>& q, const std::string& export_dir);
    // Renders the mix (track = -1) or one track over one full cycle of the song, phase-aligned. 24-bit WAV.
    bool exportWav(const std::string& path, int track, std::string* err = nullptr) const;

    double sampleRate() const noexcept { return sr_; }
    int64_t maxFrames() const noexcept { return max_frames_; }

    // Published per-track state (any thread; as of the last processed block). Used by tests and JSON.
    LoopTrackState trackState(int t) const noexcept { return static_cast<LoopTrackState>(pub_[t].state.load(std::memory_order_relaxed)); }
    int64_t trackLength(int t) const noexcept { return pub_[t].len.load(std::memory_order_relaxed); }
    int trackLayers(int t) const noexcept { return pub_[t].n.load(std::memory_order_relaxed); }
    int trackRedo(int t) const noexcept { return pub_[t].total.load(std::memory_order_relaxed) - pub_[t].n.load(std::memory_order_relaxed); }
    double trackPhase(int t) const noexcept { return pub_[t].phase.load(std::memory_order_relaxed); }   // loop frames, 0..len
    float layerGain(int t, int k) const noexcept { return pub_[t].gain[static_cast<std::size_t>(k)].load(std::memory_order_relaxed); }
    int64_t clockFrames() const noexcept { return clock_frames_pub_.load(std::memory_order_relaxed); }

private:
    struct Track {
        LoopTrackState state = LoopTrackState::Empty;
        int64_t len = 0;              // loop length (frames)
        double pos = 0.0;             // loop phase 0..len (advances by |speed|)
        int64_t rec = 0;              // frames recorded while Recording
        int64_t close_at = 0;         // Recording: auto-close when rec reaches this (0 = open)
        int64_t start_at = -1;        // Armed: clock frame to start at
        bool armed_rec = false;       // Armed for a recording (else for playback)
        int64_t origin = 0;           // clock frame at which pos was 0
        int ids[kMaxLayers] = {};     // pool ids, bottom (first recording) .. top
        float gain[kMaxLayers] = {};
        float gs[kMaxLayers] = {};    // smoothed gains
        float before[kMaxLayers][kMaxLayers] = {};   // gains of layers below k before pass k
        float after[kMaxLayers][kMaxLayers] = {};    // gains of layers 0..k when pass k ended
        int n = 0;                    // audible layers
        int total = 0;                // n + layers kept for redo
        // overdub pass
        int dub = -1;                 // stack index being written, -1 none
        bool dub_new = false;         // pass owns a new layer (undoable)
        bool dub_on = false;          // still punching in (false = ramping out)
        float wg = 0.0f;              // punch in/out write gain
        double dub_cycle = 0.0;       // loop frames dubbed since the last decay step
        bool dub_decayed = false;
        int64_t last_w = -1;          // last written index (gap fill / revisit)
        int64_t env_cell = -1;
        // playback
        float fade = 0.0f, fade_target = 0.0f, fade_step = 0.0f;
        int stop_pending = 0;         // 0 none, 1 at loop end, 2 fading
        float mute_g = 1.0f, lvl_g = 1.0f;
        bool rev = false;
        float speed = 1.0f;
        int seam = 0;                 // 1 while the progressive seam cross-fade is still running
        int seam_len = 0;
        bool stop_on_close = false;   // Stop pressed while a quantized recording waits for its bar line
        bool full = false;            // ran out of layers: the last pass merged into the top layer
        float peak = 0.0f;
    };
    struct TrackPub {
        std::atomic<int> state{0}, n{0}, total{0}, stop_pending{0};
        std::atomic<int64_t> len{0}, rec{0}, close_at{0}, wait{0};
        std::atomic<double> pos{0.0}, phase{0.0};   // read position 0..1 (mirrors when reversed), loop phase in frames
        std::atomic<int64_t> origin{0};
        std::atomic<float> speed{1.0f};
        std::atomic<bool> rev{false};
        std::atomic<float> peak{0.0f}, fade{0.0f};
        std::atomic<bool> full{false}, armed_rec{false};
        std::atomic<uint32_t> env_rev{0};
        std::array<std::atomic<int>, kMaxLayers> ids{};
        std::array<std::atomic<float>, kMaxLayers> gain{};
    };
    struct TrackParams {
        std::atomic<float> level{1.0f}, pan{0.0f}, speed{1.0f};
        std::atomic<bool> mute{false}, rev{false};
    };
    struct QSlot { std::atomic<uint32_t> seq{0}; int cmd = 0; int track = 0; float value = 0.0f; };

    void core(float* l, float* r, uint32_t n, double bpm) noexcept;
    void mixClick(float* l, float* r, uint32_t n) noexcept;
    void handle(int cmd, int t, float value) noexcept;
    void zeroWork() noexcept;
    int allocLayer() noexcept;
    void releaseLayer(int id) noexcept;
    void clearTrack(int t) noexcept;
    void startRecord(int t) noexcept;
    void requestClose(int t) noexcept;
    void finishRecord(int t, int64_t len, bool from_close) noexcept;
    void beginDub(int t) noexcept;
    void endDub(int t) noexcept;
    void finalizeDub(int t) noexcept;
    void decayOlder(int t, int upto) noexcept;
    void startPlay(int t, bool quantized) noexcept;
    void beginPlayback(int t, double phase) noexcept;
    void armOrRecord(int t) noexcept;
    void startClock(int t_for_alignment) noexcept;
    void latchTempo() noexcept;
    void resetIfAllEmpty() noexcept;
    void stopTrack(int t, int mode) noexcept;
    void undo(int t) noexcept;
    void redo(int t) noexcept;
    void pressTrack(int t) noexcept;
    void allStartStop() noexcept;
    bool clockNeeded() const noexcept;
    double barFrames() const noexcept;
    double beatFrames() const noexcept;
    int64_t gridFrames() const noexcept;
    int64_t nextBoundary(int64_t origin) const noexcept;
    int masterTrack() const noexcept;
    void publish() noexcept;
    float* buf(int id) const noexcept { return pool_[id]; }
    void envWrite(int id, int64_t idx, float a, int64_t& cell) noexcept;
    void playFrame(int t, float in_l, float in_r, float& out_l, float& out_r) noexcept;
    void writeDub(Track& T, int64_t idx, float in_l, float in_r, float w, float fb, bool fresh) noexcept;
    bool stemCycle(int track, int64_t& frames) const;

    double sr_ = 48000.0;
    int64_t max_frames_ = 0;          // max loop length
    int64_t buf_frames_ = 0;          // + seam head-room
    int env_frames_ = 480;            // frames per envelope cell
    int64_t env_cells_ = 0;
    float* pool_[kPool] = {};
    std::atomic<float>* env_ = nullptr;   // [kPool][env_cells_]
    int layer_state_[kPool] = {};         // 0 free (zeroed), 1 used, 2 dirty (needs zeroing)
    int64_t layer_hw_[kPool] = {};        // frames written (zeroing bound)
    int zero_id_ = -1;
    int64_t zero_pos_ = 0;

    Track tr_[kTracks];
    TrackPub pub_[kTracks];
    TrackParams par_[kTracks];

    // clock / grid
    bool clock_on_ = false;
    int64_t clock_ = 0;
    int64_t anchor_ = 0;              // tempo: clock frame of bar 1
    double loop_bpm_ = 0.0;           // tempo latched by the first loop (0 = follow the rig)
    int64_t loop_bar_ = 0;            // latched bar length in frames (integer, so the grid never drifts)
    int loop_bpb_ = 4;
    double live_bpm_ = 120.0;
    int master_ = -1;
    int64_t count_in_end_ = -1;
    int64_t last_beat_ = INT64_MIN;
    // click synth
    float click_phase_ = 0.0f, click_env_ = 0.0f, click_inc_ = 0.0f;
    static constexpr uint32_t kClickBuf = 4096;
    float click_buf_[kClickBuf] = {};
    uint32_t click_n_ = 0;
    float in_peak_ = 0.0f;
    float mon_g_ = 1.0f;
    bool block_pre_ = false;

    // settings
    std::atomic<int> selected_{0};
    std::atomic<bool> pre_rig_{false}, monitor_{true}, count_in_{true}, click_{false}, after_rec_dub_{false};
    std::atomic<int> sync_{static_cast<int>(LoopSync::Free)}, bars_{0}, bpb_{4}, stop_mode_{static_cast<int>(LoopStopMode::Instant)};
    std::atomic<float> click_level_{0.5f}, fade_s_{4.0f}, feedback_{1.0f}, level_{1.0f};

    // published globals
    std::atomic<bool> active_{false}, clock_pub_{false};
    std::atomic<int64_t> clock_frames_pub_{0}, anchor_pub_{0}, count_in_pub_{-1};
    std::atomic<double> loop_bpm_pub_{0.0}, live_bpm_pub_{120.0};
    std::atomic<int> master_pub_{-1}, free_layers_pub_{kPool}, bpb_pub_{4};
    std::atomic<float> in_peak_pub_{0.0f};

    // command queue (bounded MPSC, Vyukov)
    std::array<QSlot, kQueue> q_{};
    std::atomic<uint32_t> q_head_{0};
    uint32_t q_tail_ = 0;
};

} // namespace rig
