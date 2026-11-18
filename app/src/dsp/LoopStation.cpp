#include "LoopStation.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <vector>

namespace rig {

namespace {

constexpr double kSeamSec = 0.008;      // first-recording seam cross-fade
constexpr double kPunchSec = 0.006;     // overdub punch in / out ramp
constexpr double kQuickFadeSec = 0.006; // instant stop / start de-click
constexpr double kGainSec = 0.012;      // layer gain smoothing (undo / redo / decay)
constexpr double kMinRecSec = 0.05;     // a shorter take is discarded
constexpr uint32_t kZeroFloatsPerBlock = 1u << 15;

constexpr const char* kStateNames[] = {"empty", "recording", "playing", "overdubbing", "stopped", "armed"};
constexpr const char* kSyncNames[] = {"free", "loop", "tempo"};
constexpr const char* kStopNames[] = {"instant", "loop", "fade"};

inline bool running(LoopTrackState s) noexcept {
    return s == LoopTrackState::Playing || s == LoopTrackState::Overdubbing;
}
inline int64_t posMod(int64_t a, int64_t m) noexcept { const int64_t r = a % m; return r < 0 ? r + m : r; }

} // namespace

LoopStation::LoopStation() {
    for (uint32_t i = 0; i < kQueue; ++i) q_[i].seq.store(i, std::memory_order_relaxed);
    for (auto& p : pub_)
        for (auto& id : p.ids) id.store(-1, std::memory_order_relaxed);
    prepare(48000.0);
}

LoopStation::~LoopStation() {
    for (auto*& p : pool_) { std::free(p); p = nullptr; }
    delete[] env_;
}

void LoopStation::prepare(double sample_rate) {
    sr_ = sample_rate > 8000.0 ? sample_rate : 48000.0;
    const int64_t mf = static_cast<int64_t>(sr_ * kMaxSeconds);
    const int64_t bf = mf + static_cast<int64_t>(sr_ * 0.25) + 4096;   // room past the end for seam / close
    if (bf != buf_frames_ || !pool_[0]) {
        for (auto*& p : pool_) { std::free(p); p = nullptr; }
        delete[] env_;
        env_ = nullptr;
        // calloc of a large block maps fresh zero pages lazily: the reservation costs address space, and RAM
        // only for what a loop actually touches.
        bool ok = true;
        for (auto*& p : pool_) {
            p = static_cast<float*>(std::calloc(static_cast<std::size_t>(bf) * 2, sizeof(float)));
            ok = ok && p;
        }
        env_frames_ = std::max(64, static_cast<int>(std::lround(sr_ / 100.0)));
        env_cells_ = bf / env_frames_ + 2;
        env_ = new std::atomic<float>[static_cast<std::size_t>(kPool * env_cells_)]();
        if (!ok) {
            for (auto*& p : pool_) { std::free(p); p = nullptr; }
            max_frames_ = buf_frames_ = 0;
            return;
        }
        buf_frames_ = bf;
        for (int i = 0; i < kPool; ++i) { layer_state_[i] = 0; layer_hw_[i] = 0; }
    } else {
        for (int i = 0; i < kPool; ++i) {
            if (layer_state_[i] != 0 && layer_hw_[i] > 0) {
                std::memset(pool_[i], 0, static_cast<std::size_t>(std::min(layer_hw_[i], buf_frames_)) * 2 * sizeof(float));
                for (int64_t c = 0; c < env_cells_; ++c) env_[i * env_cells_ + c].store(0.0f, std::memory_order_relaxed);
            }
            layer_state_[i] = 0;
            layer_hw_[i] = 0;
        }
    }
    max_frames_ = mf;
    zero_id_ = -1;
    zero_pos_ = 0;
    for (int t = 0; t < kTracks; ++t) {
        tr_[t] = Track{};
        tr_[t].rev = par_[t].rev.load(std::memory_order_relaxed);
        tr_[t].speed = par_[t].speed.load(std::memory_order_relaxed);
    }
    clock_on_ = false;
    clock_ = anchor_ = 0;
    loop_bpm_ = 0.0;
    loop_bar_ = 0;
    master_ = -1;
    count_in_end_ = -1;
    last_beat_ = INT64_MIN;
    click_env_ = 0.0f;
    publish();
}

// ───────────────────────────── settings ─────────────────────────────
void LoopStation::setBars(int b) noexcept { bars_.store(std::clamp(b, 0, 64), std::memory_order_relaxed); }
void LoopStation::setBeatsPerBar(int b) noexcept { bpb_.store(std::clamp(b, 1, 16), std::memory_order_relaxed); }
void LoopStation::setClickLevel(float v) noexcept { click_level_.store(std::clamp(v, 0.0f, 1.0f), std::memory_order_relaxed); }
void LoopStation::setFadeSeconds(float s) noexcept { fade_s_.store(std::clamp(s, 0.2f, 30.0f), std::memory_order_relaxed); }
void LoopStation::setFeedback(float fb) noexcept { feedback_.store(std::clamp(fb, 0.0f, 1.0f), std::memory_order_relaxed); }
void LoopStation::setLevel(float lvl) noexcept { level_.store(std::clamp(lvl, 0.0f, 2.0f), std::memory_order_relaxed); }
void LoopStation::setTrackLevel(int t, float v) noexcept { if (t >= 0 && t < kTracks) par_[t].level.store(std::clamp(v, 0.0f, 2.0f), std::memory_order_relaxed); }
void LoopStation::setTrackPan(int t, float v) noexcept { if (t >= 0 && t < kTracks) par_[t].pan.store(std::clamp(v, -1.0f, 1.0f), std::memory_order_relaxed); }
void LoopStation::setTrackMute(int t, bool v) noexcept { if (t >= 0 && t < kTracks) par_[t].mute.store(v, std::memory_order_relaxed); }
void LoopStation::setTrackReverse(int t, bool v) noexcept { if (t >= 0 && t < kTracks) par_[t].rev.store(v, std::memory_order_relaxed); }
void LoopStation::setTrackSpeed(int t, float v) noexcept {
    if (t < 0 || t >= kTracks) return;
    const float s = v < 0.75f ? 0.5f : (v > 1.5f ? 2.0f : 1.0f);
    par_[t].speed.store(s, std::memory_order_relaxed);
}

// ───────────────────────────── command queue ─────────────────────────────
bool LoopStation::post(int cmd, int track, float value) noexcept {
    if (track < 0 || track >= kTracks) track = selected();
    uint32_t pos = q_head_.load(std::memory_order_relaxed);
    for (;;) {
        QSlot& s = q_[pos % kQueue];
        const uint32_t seq = s.seq.load(std::memory_order_acquire);
        const int32_t d = static_cast<int32_t>(seq - pos);
        if (d == 0) {
            if (q_head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                s.cmd = cmd;
                s.track = track;
                s.value = value;
                s.seq.store(pos + 1, std::memory_order_release);
                return true;
            }
        } else if (d < 0) {
            return false;   // full (audio not running)
        } else {
            pos = q_head_.load(std::memory_order_relaxed);
        }
    }
}

// ───────────────────────────── layer pool ─────────────────────────────
int LoopStation::allocLayer() noexcept {
    for (int i = 0; i < kPool; ++i) {
        if (layer_state_[i] == 0) { layer_state_[i] = 1; layer_hw_[i] = 0; return i; }
    }
    return -1;
}
void LoopStation::releaseLayer(int id) noexcept {
    if (id >= 0 && id < kPool && layer_state_[id] == 1) layer_state_[id] = 2;
}
void LoopStation::zeroWork() noexcept {
    if (zero_id_ < 0) {
        for (int i = 0; i < kPool; ++i)
            if (layer_state_[i] == 2) { zero_id_ = i; zero_pos_ = 0; break; }
        if (zero_id_ < 0) return;
    }
    const int id = zero_id_;
    const int64_t total = std::min(layer_hw_[id], buf_frames_) * 2;
    const int64_t chunk = std::min<int64_t>(total - zero_pos_, kZeroFloatsPerBlock);
    if (chunk > 0) std::memset(pool_[id] + zero_pos_, 0, static_cast<std::size_t>(chunk) * sizeof(float));
    zero_pos_ += std::max<int64_t>(chunk, 0);
    if (zero_pos_ >= total) {
        const int64_t cells = std::min<int64_t>(env_cells_, layer_hw_[id] / env_frames_ + 2);
        for (int64_t c = 0; c < cells; ++c) env_[id * env_cells_ + c].store(0.0f, std::memory_order_relaxed);
        layer_hw_[id] = 0;
        layer_state_[id] = 0;
        zero_id_ = -1;
    }
}
void LoopStation::envWrite(int id, int64_t idx, float a, int64_t& cell) noexcept {
    const int64_t c = idx / env_frames_;
    if (c < 0 || c >= env_cells_) return;
    std::atomic<float>& e = env_[id * env_cells_ + c];
    if (c != cell) { e.store(a, std::memory_order_relaxed); cell = c; }
    else if (a > e.load(std::memory_order_relaxed)) e.store(a, std::memory_order_relaxed);
}

// ───────────────────────────── grid ─────────────────────────────
double LoopStation::barFrames() const noexcept {
    if (loop_bar_ > 0) return static_cast<double>(loop_bar_);
    return 60.0 / std::clamp(live_bpm_, 20.0, 400.0) * sr_ * bpb_.load(std::memory_order_relaxed);
}
double LoopStation::beatFrames() const noexcept {
    const int bpb = loop_bar_ > 0 ? loop_bpb_ : bpb_.load(std::memory_order_relaxed);
    return barFrames() / std::max(1, bpb);
}
int LoopStation::masterTrack() const noexcept {
    if (master_ >= 0 && tr_[master_].len > 0) return master_;
    return -1;
}
int64_t LoopStation::gridFrames() const noexcept {
    const auto sync = static_cast<LoopSync>(sync_.load(std::memory_order_relaxed));
    if (sync == LoopSync::Tempo) return std::max<int64_t>(1, std::llround(barFrames()));
    if (sync == LoopSync::Loop) { const int m = masterTrack(); return m >= 0 ? tr_[m].len : 0; }
    return 0;
}
// First clock frame >= now that sits on `origin`'s grid (bar lines in Tempo, master cycles in Loop).
int64_t LoopStation::nextBoundary(int64_t origin) const noexcept {
    const int64_t g = gridFrames();
    if (g <= 0) return clock_;
    const int64_t d = clock_ - origin;
    const int64_t k = d >= 0 ? (d + g - 1) / g : -((-d) / g);
    return origin + k * g;
}
void LoopStation::latchTempo() noexcept {
    if (loop_bar_ > 0) return;
    loop_bpb_ = bpb_.load(std::memory_order_relaxed);
    loop_bpm_ = std::clamp(live_bpm_, 20.0, 400.0);
    loop_bar_ = std::max<int64_t>(1, std::llround(60.0 / loop_bpm_ * sr_ * loop_bpb_));
}
bool LoopStation::clockNeeded() const noexcept {
    for (const auto& T : tr_)
        if (T.state == LoopTrackState::Armed || T.state == LoopTrackState::Recording || running(T.state)) return true;
    return false;
}
// The clock pauses while nothing runs; restarting it jumps to a grid line so every stored origin stays valid.
void LoopStation::startClock(int t) noexcept {
    if (clock_on_) return;
    clock_on_ = true;
    last_beat_ = INT64_MIN;
    const auto sync = static_cast<LoopSync>(sync_.load(std::memory_order_relaxed));
    bool content = false;
    for (const auto& T : tr_) content = content || T.len > 0;
    if (!content) { clock_ = 0; anchor_ = 0; return; }
    if (sync == LoopSync::Tempo && loop_bar_ > 0) clock_ = nextBoundary(anchor_);
    else if (sync == LoopSync::Loop && t >= 0 && tr_[t].len > 0 && masterTrack() >= 0) clock_ = nextBoundary(tr_[t].origin);
}
void LoopStation::resetIfAllEmpty() noexcept {
    for (const auto& T : tr_) if (T.state != LoopTrackState::Empty) return;
    clock_on_ = false;
    clock_ = anchor_ = 0;
    loop_bpm_ = 0.0;
    loop_bar_ = 0;
    master_ = -1;
    count_in_end_ = -1;
}

// ───────────────────────────── transport ─────────────────────────────
void LoopStation::clearTrack(int t) noexcept {
    Track& T = tr_[t];
    for (int k = 0; k < T.total; ++k) releaseLayer(T.ids[k]);
    const bool rev = T.rev;
    const float speed = T.speed;
    T = Track{};
    T.rev = rev;
    T.speed = speed;
    ++pub_[t].env_rev;
    if (master_ == t) {
        master_ = -1;
        int64_t best = 0;
        for (int i = 0; i < kTracks; ++i)
            if (tr_[i].len > 0 && (best == 0 || tr_[i].len < best)) { best = tr_[i].len; master_ = i; }
    }
    resetIfAllEmpty();
}

void LoopStation::armOrRecord(int t) noexcept {
    Track& T = tr_[t];
    if (T.state != LoopTrackState::Empty || max_frames_ <= 0) return;
    const auto sync = static_cast<LoopSync>(sync_.load(std::memory_order_relaxed));
    if (sync == LoopSync::Tempo) {
        const bool was_on = clock_on_;
        latchTempo();
        if (!was_on) {
            bool content = false;
            for (const auto& o : tr_) content = content || o.len > 0;
            if (!content) { clock_on_ = true; clock_ = 0; anchor_ = 0; last_beat_ = INT64_MIN; }
            else { startClock(t); }
        }
        if (!was_on && count_in_.load(std::memory_order_relaxed)) {
            T.state = LoopTrackState::Armed;
            T.armed_rec = true;
            T.start_at = nextBoundary(anchor_) + loop_bar_;
            count_in_end_ = T.start_at;
            return;
        }
        const int64_t b = nextBoundary(anchor_);
        if (b <= clock_) { startRecord(t); return; }
        T.state = LoopTrackState::Armed;
        T.armed_rec = true;
        T.start_at = b;
        return;
    }
    startClock(t);
    startRecord(t);
}

void LoopStation::startRecord(int t) noexcept {
    Track& T = tr_[t];
    const int id = allocLayer();
    if (id < 0) { T.state = LoopTrackState::Empty; T.full = true; resetIfAllEmpty(); return; }
    T = Track{};
    T.rev = par_[t].rev.load(std::memory_order_relaxed);
    T.speed = par_[t].speed.load(std::memory_order_relaxed);
    T.ids[0] = id;
    T.gain[0] = T.gs[0] = 1.0f;
    T.n = T.total = 1;
    T.state = LoopTrackState::Recording;
    T.origin = clock_;
    T.fade = T.fade_target = 1.0f;
    if (static_cast<LoopSync>(sync_.load(std::memory_order_relaxed)) == LoopSync::Tempo) {
        latchTempo();
        const int bars = bars_.load(std::memory_order_relaxed);
        if (bars > 0) T.close_at = std::min<int64_t>(max_frames_, bars * loop_bar_);
    }
    if (!clock_on_) startClock(t);
    ++pub_[t].env_rev;
}

void LoopStation::requestClose(int t) noexcept {
    Track& T = tr_[t];
    if (T.state != LoopTrackState::Recording) return;
    const int64_t rec = T.rec;
    if (rec < static_cast<int64_t>(sr_ * kMinRecSec)) { clearTrack(t); return; }
    const auto sync = static_cast<LoopSync>(sync_.load(std::memory_order_relaxed));
    int64_t target = rec;
    if (sync == LoopSync::Tempo && loop_bar_ > 0) {
        target = std::max<int64_t>(1, std::llround(static_cast<double>(rec) / static_cast<double>(loop_bar_))) * loop_bar_;
        while (target > max_frames_ && target > loop_bar_) target -= loop_bar_;
    } else if (sync == LoopSync::Loop) {
        const int m = masterTrack();
        if (m >= 0 && m != t) {
            const int64_t g = tr_[m].len;
            int64_t best = g;
            const int64_t cands[] = {g / 4, g / 2};
            for (int64_t c : cands) if (c > 0 && std::llabs(c - rec) < std::llabs(best - rec)) best = c;
            for (int64_t k = 1; k * g <= max_frames_; ++k) if (std::llabs(k * g - rec) < std::llabs(best - rec)) best = k * g;
            target = best;
        }
    }
    target = std::min(target, max_frames_);
    if (target > rec) { T.close_at = target; return; }   // keep recording to the grid line
    finishRecord(t, target, false);
}

void LoopStation::finishRecord(int t, int64_t len, bool /*from_close*/) noexcept {
    Track& T = tr_[t];
    if (len <= 0) { clearTrack(t); return; }
    float* b = buf(T.ids[0]);
    const int64_t extra = std::max<int64_t>(0, T.rec - len);
    const int x = static_cast<int>(std::min<int64_t>(static_cast<int64_t>(sr_ * kSeamSec), len / 4));
    const bool dub_next = after_rec_dub_.load(std::memory_order_relaxed) && !T.stop_on_close;
    // Seam: what was played just after the loop point is cross-faded into the head, so the wrap is continuous.
    const int64_t k0 = std::min<int64_t>(extra, x);
    for (int64_t k = 0; k < k0; ++k) {
        const float w = static_cast<float>(k) / static_cast<float>(x);
        b[2 * k] = b[2 * k] * w + b[2 * (len + k)] * (1.0f - w);
        b[2 * k + 1] = b[2 * k + 1] * w + b[2 * (len + k) + 1] * (1.0f - w);
    }
    T.seam_len = x;
    T.seam = 0;
    if (extra < x) {
        if (dub_next) {
            // the overdub layer carries what follows; just fade the head in
            for (int64_t k = extra; k < x; ++k) {
                const float w = static_cast<float>(k) / static_cast<float>(x);
                b[2 * k] *= w;
                b[2 * k + 1] *= w;
            }
        } else {
            T.seam = 1;   // finish it live as the playhead passes (playFrame)
        }
    }
    T.len = len;
    T.pos = static_cast<double>(extra % len);
    T.origin = clock_ - extra;
    T.close_at = 0;
    T.rec = 0;
    T.fade = T.fade_target = 1.0f;
    T.stop_pending = 0;
    T.state = LoopTrackState::Playing;
    if (masterTrack() < 0) master_ = t;
    ++pub_[t].env_rev;
    if (T.stop_on_close) {
        T.stop_on_close = false;
        T.state = LoopTrackState::Stopped;
        T.pos = 0.0;
        T.fade = 0.0f;
        return;
    }
    if (dub_next) {
        beginDub(t);
        T.wg = 1.0f;   // the recording carries straight on into the first overdub
    }
}

void LoopStation::beginDub(int t) noexcept {
    Track& T = tr_[t];
    if (T.len <= 0 || T.n <= 0) return;
    if (T.dub >= 0) { T.dub_on = true; T.state = LoopTrackState::Overdubbing; return; }
    for (int k = T.n; k < T.total; ++k) releaseLayer(T.ids[k]);   // a new pass drops the redo layers
    T.total = T.n;
    const int id = T.n < kMaxLayers ? allocLayer() : -1;
    if (id >= 0) {
        const int k = T.n;
        T.ids[k] = id;
        for (int j = 0; j < k; ++j) T.before[k][j] = T.gain[j];
        T.gain[k] = 1.0f;
        T.gs[k] = 1.0f;
        T.n = T.total = k + 1;
        T.dub = k;
        T.dub_new = true;
        T.full = false;
    } else {
        T.dub = T.n - 1;   // out of layers: this pass merges into the top layer (no separate undo step)
        T.dub_new = false;
        T.full = true;
    }
    T.dub_on = true;
    T.wg = 0.0f;
    T.dub_cycle = 0.0;
    T.dub_decayed = false;
    T.last_w = -1;
    T.env_cell = -1;
    T.state = LoopTrackState::Overdubbing;
    ++pub_[t].env_rev;
}

void LoopStation::endDub(int t) noexcept {
    Track& T = tr_[t];
    if (T.dub < 0) return;
    T.dub_on = false;   // keeps writing for the punch-out ramp, then finalizeDub()
    if (T.state == LoopTrackState::Overdubbing) T.state = LoopTrackState::Playing;
}

void LoopStation::decayOlder(int t, int upto) noexcept {
    Track& T = tr_[t];
    const float fb = feedback_.load(std::memory_order_relaxed);
    if (fb >= 0.9999f) return;
    for (int j = 0; j < upto && j < T.n; ++j) T.gain[j] *= fb;
}

void LoopStation::finalizeDub(int t) noexcept {
    Track& T = tr_[t];
    if (T.dub < 0) return;
    // Every pass ages the layers under it at least once (Blooper / Ditto "decay"); long passes age them
    // once per full cycle as they go (playFrame).
    if (!T.dub_decayed) decayOlder(t, T.dub);
    if (T.dub_new)
        for (int j = 0; j <= T.dub; ++j) T.after[T.dub][j] = T.gain[j];
    T.dub = -1;
    T.dub_on = false;
    T.wg = 0.0f;
    T.last_w = -1;
    if (T.state == LoopTrackState::Overdubbing) T.state = LoopTrackState::Playing;
    ++pub_[t].env_rev;
}

void LoopStation::beginPlayback(int t, double phase) noexcept {
    Track& T = tr_[t];
    T.pos = std::clamp(phase, 0.0, static_cast<double>(std::max<int64_t>(1, T.len) - 1));
    T.origin = clock_ - static_cast<int64_t>(phase);
    T.state = LoopTrackState::Playing;
    T.stop_pending = 0;
    T.fade = 0.0f;
    T.fade_target = 1.0f;
    T.fade_step = static_cast<float>(1.0 / (kQuickFadeSec * sr_));
    T.start_at = -1;
}

void LoopStation::startPlay(int t, bool quantized) noexcept {
    Track& T = tr_[t];
    if (T.len <= 0 || (T.state != LoopTrackState::Stopped && T.state != LoopTrackState::Armed)) return;
    const auto sync = static_cast<LoopSync>(sync_.load(std::memory_order_relaxed));
    bool others = false;
    for (int i = 0; i < kTracks; ++i)
        if (i != t && (running(tr_[i].state) || tr_[i].state == LoopTrackState::Recording)) others = true;
    if (sync != LoopSync::Free && quantized && clock_on_ && others) {
        const int64_t b = sync == LoopSync::Tempo ? nextBoundary(anchor_) : nextBoundary(T.origin);
        if (b > clock_) {
            T.state = LoopTrackState::Armed;
            T.armed_rec = false;
            T.start_at = b;
            return;
        }
    }
    startClock(t);
    beginPlayback(t, 0.0);
}

void LoopStation::stopTrack(int t, int mode) noexcept {
    Track& T = tr_[t];
    switch (T.state) {
        case LoopTrackState::Armed:
            T.state = T.armed_rec ? LoopTrackState::Empty : LoopTrackState::Stopped;
            T.start_at = -1;
            if (T.armed_rec) { count_in_end_ = -1; resetIfAllEmpty(); }
            return;
        case LoopTrackState::Recording:
            T.stop_on_close = true;
            requestClose(t);
            return;
        case LoopTrackState::Overdubbing:
            endDub(t);
            [[fallthrough]];
        case LoopTrackState::Playing:
            if (T.stop_pending != 0 || mode == static_cast<int>(LoopStopMode::Instant)) {
                T.stop_pending = 3;   // de-click fade, then Stopped
                T.fade_target = 0.0f;
                T.fade_step = static_cast<float>(1.0 / (kQuickFadeSec * sr_));
            } else if (mode == static_cast<int>(LoopStopMode::LoopEnd)) {
                T.stop_pending = 1;
            } else {
                T.stop_pending = 2;
                T.fade_target = 0.0f;
                T.fade_step = static_cast<float>(1.0 / (fade_s_.load(std::memory_order_relaxed) * sr_));
            }
            return;
        default:
            return;
    }
}

void LoopStation::undo(int t) noexcept {
    Track& T = tr_[t];
    if (T.state == LoopTrackState::Recording || (T.state == LoopTrackState::Armed && T.armed_rec)) { clearTrack(t); return; }
    if (T.dub >= 0) finalizeDub(t);
    if (T.n <= 1) return;
    const int k = T.n - 1;
    for (int j = 0; j < k; ++j) T.gain[j] = T.before[k][j];
    T.n = k;
    ++pub_[t].env_rev;
}

void LoopStation::redo(int t) noexcept {
    Track& T = tr_[t];
    if (T.dub >= 0 || T.n >= T.total) return;
    const int k = T.n;
    for (int j = 0; j <= k; ++j) T.gain[j] = T.after[k][j];
    T.gs[k] = 0.0f;
    T.n = k + 1;
    ++pub_[t].env_rev;
}

void LoopStation::pressTrack(int t) noexcept {
    selected_.store(t, std::memory_order_relaxed);
    Track& T = tr_[t];
    switch (T.state) {
        case LoopTrackState::Empty: armOrRecord(t); break;
        case LoopTrackState::Armed:
            T.state = T.armed_rec ? LoopTrackState::Empty : LoopTrackState::Stopped;
            if (T.armed_rec) { count_in_end_ = -1; resetIfAllEmpty(); }
            break;
        case LoopTrackState::Recording: requestClose(t); break;
        case LoopTrackState::Playing:
            if (T.stop_pending != 0) {   // pressed during a pending stop: keep playing
                T.stop_pending = 0;
                T.fade_target = 1.0f;
                T.fade_step = static_cast<float>(1.0 / (kQuickFadeSec * sr_));
            } else {
                beginDub(t);
            }
            break;
        case LoopTrackState::Overdubbing: endDub(t); break;
        case LoopTrackState::Stopped: startPlay(t, true); break;
    }
}

// All Start / Stop: stops everything that runs; with nothing running, starts every recorded track together,
// phase-aligned (the longest loop from its top, the others where their stored origins put them).
void LoopStation::allStartStop() noexcept {
    bool any_running = false, any_content = false;
    for (const auto& T : tr_) {
        any_running = any_running || running(T.state) || T.state == LoopTrackState::Recording || T.state == LoopTrackState::Armed;
        any_content = any_content || T.len > 0;
    }
    const int mode = stop_mode_.load(std::memory_order_relaxed);
    if (any_running) { for (int t = 0; t < kTracks; ++t) stopTrack(t, mode); return; }
    if (!any_content) return;
    const auto sync = static_cast<LoopSync>(sync_.load(std::memory_order_relaxed));
    if (sync == LoopSync::Free) {
        startClock(-1);
        for (int t = 0; t < kTracks; ++t) if (tr_[t].state == LoopTrackState::Stopped) beginPlayback(t, 0.0);
        return;
    }
    int ref = -1;
    for (int t = 0; t < kTracks; ++t)
        if (tr_[t].state == LoopTrackState::Stopped && (ref < 0 || tr_[t].len > tr_[ref].len)) ref = t;
    if (ref < 0) return;
    clock_on_ = true;
    last_beat_ = INT64_MIN;
    {
        const int64_t L = tr_[ref].len;
        const int64_t d = clock_ - tr_[ref].origin;
        const int64_t k = d >= 0 ? (d + L - 1) / L : -((-d) / L);
        clock_ = tr_[ref].origin + k * L;
    }
    for (int t = 0; t < kTracks; ++t) {
        Track& T = tr_[t];
        if (T.state != LoopTrackState::Stopped) continue;
        const int64_t ph = posMod(clock_ - T.origin, T.len);
        beginPlayback(t, static_cast<double>(ph));
    }
}

void LoopStation::handle(int cmd, int t, float value) noexcept {
    if (t < 0 || t >= kTracks) t = selected();
    const int mode = stop_mode_.load(std::memory_order_relaxed);
    switch (cmd) {
        case kPress: pressTrack(t); break;
        case kRecord:
            selected_.store(t, std::memory_order_relaxed);
            if (tr_[t].state != LoopTrackState::Empty) clearTrack(t);
            armOrRecord(t);
            break;
        case kOverdub:
            if (tr_[t].state == LoopTrackState::Recording) {
                const bool d = after_rec_dub_.exchange(true);
                requestClose(t);
                after_rec_dub_.store(d);
            } else if (tr_[t].state == LoopTrackState::Playing) beginDub(t);
            break;
        case kPlay:
            if (tr_[t].state == LoopTrackState::Recording) {
                const bool d = after_rec_dub_.exchange(false);
                requestClose(t);
                after_rec_dub_.store(d);
            } else if (tr_[t].state == LoopTrackState::Overdubbing) endDub(t);
            else if (tr_[t].state == LoopTrackState::Stopped) startPlay(t, true);
            break;
        case kStop: stopTrack(t, value > 0.5f ? static_cast<int>(value) - 1 : mode); break;
        case kStopAll: for (int i = 0; i < kTracks; ++i) stopTrack(i, mode); break;
        case kStopInstantAll: for (int i = 0; i < kTracks; ++i) stopTrack(i, static_cast<int>(LoopStopMode::Instant)); break;
        case kPlayAll: {
            bool any_running = false;
            for (const auto& T : tr_) any_running = any_running || running(T.state);
            if (!any_running) allStartStop();
            else for (int i = 0; i < kTracks; ++i) if (tr_[i].state == LoopTrackState::Stopped) startPlay(i, true);
            break;
        }
        case kAllStartStop: allStartStop(); break;
        case kClear: clearTrack(t); break;
        case kClearAll: for (int i = 0; i < kTracks; ++i) clearTrack(i); break;
        case kUndo: undo(t); break;
        case kRedo: redo(t); break;
        case kUndoRedo:
            if (tr_[t].n > 1 || tr_[t].dub >= 0 || tr_[t].state == LoopTrackState::Recording || (tr_[t].state == LoopTrackState::Armed && tr_[t].armed_rec)) undo(t);
            else redo(t);
            break;
        default: break;
    }
}

// ───────────────────────────── audio ─────────────────────────────
void LoopStation::writeDub(Track& T, int64_t j, float in_l, float in_r, float w, float fb, bool fresh) noexcept {
    const int id = T.ids[T.dub];
    float* b = buf(id);
    float& L = b[2 * j];
    float& R = b[2 * j + 1];
    if (fresh) { L = L * fb + in_l * w; R = R * fb + in_r * w; }
    else { L += in_l * w; R += in_r * w; }
    L = std::clamp(L, -4.0f, 4.0f);
    R = std::clamp(R, -4.0f, 4.0f);
    if (j + 1 > layer_hw_[id]) layer_hw_[id] = j + 1;
    envWrite(id, j, std::max(std::fabs(L), std::fabs(R)), T.env_cell);
}

void LoopStation::playFrame(int t, float in_l, float in_r, float& out_l, float& out_r) noexcept {
    Track& T = tr_[t];
    const int64_t len = T.len;
    const double lenD = static_cast<double>(len);
    double rp = T.rev ? lenD - T.pos : T.pos;
    if (rp >= lenD) rp -= lenD;
    if (rp < 0.0) rp = 0.0;
    int64_t i0 = static_cast<int64_t>(rp);
    if (i0 >= len) i0 = len - 1;

    // progressive first-recording seam: blend the live continuation into the head as it plays
    if (T.seam) {
        const int64_t k = static_cast<int64_t>(T.pos);
        if (!T.rev && T.speed == 1.0f && k < T.seam_len) {
            float* b = buf(T.ids[0]);
            const float w = static_cast<float>(k) / static_cast<float>(T.seam_len);
            b[2 * k] = b[2 * k] * w + in_l * (1.0f - w);
            b[2 * k + 1] = b[2 * k + 1] * w + in_r * (1.0f - w);
        } else {
            T.seam = 0;
        }
    }

    // read: sum of layers (smoothed gains), linear interpolation for ½× / reverse fractions
    const float frac = static_cast<float>(rp - static_cast<double>(i0));
    const int64_t i1 = i0 + 1 >= len ? 0 : i0 + 1;
    const float ga = static_cast<float>(1.0 / (kGainSec * sr_));
    float sl = 0.0f, sr = 0.0f;
    for (int k = 0; k < T.total; ++k) {
        const float target = k < T.n ? T.gain[k] : 0.0f;
        T.gs[k] += (target - T.gs[k]) * ga;
        const float g = T.gs[k];
        if (g < 1e-6f && target == 0.0f) { T.gs[k] = 0.0f; continue; }
        const float* b = buf(T.ids[k]);
        const float l0 = b[2 * i0], r0 = b[2 * i0 + 1];
        sl += g * (l0 + frac * (b[2 * i1] - l0));
        sr += g * (r0 + frac * (b[2 * i1 + 1] - r0));
    }

    // overdub write (after the read, so the pass isn't heard twice)
    if (T.dub >= 0) {
        const float punch = static_cast<float>(1.0 / (kPunchSec * sr_));
        T.wg = T.dub_on ? std::min(1.0f, T.wg + punch) : std::max(0.0f, T.wg - punch);
        const float w = T.wg * std::min(1.0f, T.speed);
        const float fb = feedback_.load(std::memory_order_relaxed);
        if (w > 0.0f || T.dub_on) {
            if (T.last_w < 0) {
                writeDub(T, i0, in_l, in_r, w, fb, true);
            } else {
                const int64_t delta = T.rev ? posMod(T.last_w - i0, len) : posMod(i0 - T.last_w, len);
                if (delta == 0) {
                    writeDub(T, i0, in_l, in_r, w, fb, false);
                } else if (delta <= 3) {
                    for (int64_t s = 1; s <= delta; ++s) {
                        const int64_t j = T.rev ? posMod(T.last_w - s, len) : posMod(T.last_w + s, len);
                        writeDub(T, j, in_l, in_r, w, fb, true);
                    }
                } else {
                    writeDub(T, i0, in_l, in_r, w, fb, true);
                }
            }
            T.last_w = i0;
        }
        if (T.dub_on) {
            T.dub_cycle += T.speed;
            if (T.dub_cycle >= lenD) { T.dub_cycle -= lenD; decayOlder(t, T.dub); T.dub_decayed = true; }
        } else if (T.wg <= 0.0f) {
            finalizeDub(t);
        }
    }

    // gains: start / stop fades, mute, level, pan (balance)
    if (T.fade != T.fade_target) {
        T.fade = T.fade < T.fade_target ? std::min(T.fade_target, T.fade + T.fade_step) : std::max(T.fade_target, T.fade - T.fade_step);
    }
    const float qa = static_cast<float>(1.0 / (kQuickFadeSec * sr_));
    const float mute_t = par_[t].mute.load(std::memory_order_relaxed) ? 0.0f : 1.0f;
    T.mute_g += std::clamp(mute_t - T.mute_g, -qa, qa);
    const float lvl_t = par_[t].level.load(std::memory_order_relaxed);
    T.lvl_g += (lvl_t - T.lvl_g) * ga;
    const float pan = par_[t].pan.load(std::memory_order_relaxed);
    const float g = T.fade * T.fade * T.mute_g * T.lvl_g;   // squared: fades fall away evenly to the ear
    const float ol = sl * g * (pan > 0.0f ? 1.0f - pan : 1.0f);
    const float orr = sr * g * (pan < 0.0f ? 1.0f + pan : 1.0f);
    out_l += ol;
    out_r += orr;
    T.peak = std::max(T.peak, std::max(std::fabs(ol), std::fabs(orr)));

    // advance
    T.pos += T.speed;
    if (T.pos >= lenD) {
        T.pos -= lenD;
        if (T.stop_pending == 1) {
            T.stop_pending = 3;
            T.fade_target = 0.0f;
            T.fade_step = static_cast<float>(1.0 / (kQuickFadeSec * sr_));
        }
    }
    if (T.stop_pending >= 2 && T.fade <= 0.0f) {
        if (T.dub >= 0) finalizeDub(t);
        T.state = LoopTrackState::Stopped;
        T.stop_pending = 0;
        T.pos = 0.0;
        T.fade = 0.0f;
    }
}

void LoopStation::core(float* l, float* r, uint32_t n, double bpm) noexcept {
    if (bpm >= 20.0 && bpm <= 400.0) live_bpm_ = bpm;
    // commands
    for (int guard = 0; guard < kQueue; ++guard) {
        QSlot& s = q_[q_tail_ % kQueue];
        const uint32_t seq = s.seq.load(std::memory_order_acquire);
        if (static_cast<int32_t>(seq - (q_tail_ + 1)) < 0) break;
        const int c = s.cmd, t = s.track;
        const float v = s.value;
        s.seq.store(q_tail_ + kQueue, std::memory_order_release);
        ++q_tail_;
        handle(c, t, v);
    }
    zeroWork();
    // parameters that need a transition on the audio thread
    for (int t = 0; t < kTracks; ++t) {
        Track& T = tr_[t];
        const bool rev = par_[t].rev.load(std::memory_order_relaxed);
        if (rev != T.rev) {
            if (T.len > 0) { T.pos = static_cast<double>(T.len) - T.pos; if (T.pos >= static_cast<double>(T.len)) T.pos = 0.0; }
            T.rev = rev;
            T.last_w = -1;
        }
        const float sp = par_[t].speed.load(std::memory_order_relaxed);
        if (sp != T.speed) { T.speed = sp; T.last_w = -1; }
    }

    const bool monitor = monitor_.load(std::memory_order_relaxed);
    const float mon_t = monitor ? 1.0f : 0.0f;
    const float qa = static_cast<float>(1.0 / (kQuickFadeSec * sr_));
    const float master = level_.load(std::memory_order_relaxed);
    const bool tempo = static_cast<LoopSync>(sync_.load(std::memory_order_relaxed)) == LoopSync::Tempo;
    const bool click_on = click_.load(std::memory_order_relaxed);
    const float click_lvl = click_level_.load(std::memory_order_relaxed);
    const double beatF = beatFrames();
    const int bpb = loop_bar_ > 0 ? loop_bpb_ : bpb_.load(std::memory_order_relaxed);
    const float decay = std::exp(static_cast<float>(-1.0 / (0.018 * sr_)));
    float pk = in_peak_ * 0.94f;
    bool wrote[kTracks] = {};
    for (int t = 0; t < kTracks; ++t) tr_[t].peak *= 0.9f;
    const uint32_t nc = std::min(n, kClickBuf);

    for (uint32_t i = 0; i < n; ++i) {
        const float in_l = l[i], in_r = r[i];
        pk = std::max(pk, std::max(std::fabs(in_l), std::fabs(in_r)));
        // grid events
        if (clock_on_) {
            for (int t = 0; t < kTracks; ++t) {
                Track& T = tr_[t];
                if (T.state == LoopTrackState::Armed && clock_ >= T.start_at) {
                    if (T.armed_rec) { T.state = LoopTrackState::Empty; count_in_end_ = -1; startRecord(t); }
                    else beginPlayback(t, 0.0);
                }
            }
            const bool counting = count_in_end_ > clock_;
            if (tempo && (click_on || counting)) {
                const double rel = static_cast<double>(clock_ - anchor_);
                const int64_t b = static_cast<int64_t>(std::floor(rel / beatF));
                if (b != last_beat_) {
                    last_beat_ = b;
                    if (rel - static_cast<double>(b) * beatF < 1.0) {
                        const bool down = posMod(b, bpb) == 0;
                        click_env_ = 1.0f;
                        click_phase_ = 0.0f;
                        click_inc_ = static_cast<float>(2.0 * M_PI * (down ? 1760.0 : 1175.0) / sr_);
                        if (counting) click_env_ = 1.15f;
                    }
                }
            }
        }
        float cl = 0.0f;
        if (click_env_ > 1e-4f) {
            cl = std::sin(click_phase_) * click_env_ * click_lvl * 0.45f;
            click_phase_ += click_inc_;
            if (click_phase_ > 6.2831853f) click_phase_ -= 6.2831853f;
            click_env_ *= decay;
        }
        if (i < nc) click_buf_[i] = cl;

        float ol = 0.0f, orr = 0.0f;
        for (int t = 0; t < kTracks; ++t) {
            Track& T = tr_[t];
            if (T.state == LoopTrackState::Recording) {
                if (T.rec >= max_frames_) { finishRecord(t, max_frames_, true); }
                else {
                    float* b = buf(T.ids[0]);
                    b[2 * T.rec] = in_l;
                    b[2 * T.rec + 1] = in_r;
                    envWrite(T.ids[0], T.rec, std::max(std::fabs(in_l), std::fabs(in_r)), T.env_cell);
                    ++T.rec;
                    if (T.rec > layer_hw_[T.ids[0]]) layer_hw_[T.ids[0]] = T.rec;
                    wrote[t] = true;
                    if (T.close_at > 0 && T.rec >= T.close_at) finishRecord(t, T.close_at, true);
                    continue;
                }
            }
            if ((running(T.state)) && T.len > 0) {
                playFrame(t, in_l, in_r, ol, orr);
                if (T.dub >= 0) wrote[t] = true;
            }
        }
        mon_g_ += std::clamp(mon_t - mon_g_, -qa, qa);
        l[i] = in_l * mon_g_ + ol * master;
        r[i] = in_r * mon_g_ + orr * master;
        if (clock_on_) ++clock_;
    }
    click_n_ = nc;
    in_peak_ = pk;
    if (clock_on_ && !clockNeeded()) clock_on_ = false;
    for (int t = 0; t < kTracks; ++t) if (wrote[t]) ++pub_[t].env_rev;
    publish();
}

void LoopStation::mixClick(float* l, float* r, uint32_t n) noexcept {
    const uint32_t m = std::min(n, click_n_);
    for (uint32_t i = 0; i < m; ++i) { l[i] += click_buf_[i]; r[i] += click_buf_[i]; }
    click_n_ = 0;
}

void LoopStation::processPre(float* l, float* r, uint32_t n, double bpm) noexcept {
    block_pre_ = pre_rig_.load(std::memory_order_relaxed);
    if (!l || !r || max_frames_ <= 0) return;
    if (block_pre_) core(l, r, n, bpm);
}
void LoopStation::processPost(float* l, float* r, uint32_t n, double bpm) noexcept {
    if (!l || !r || max_frames_ <= 0) return;
    if (!block_pre_) core(l, r, n, bpm);
    mixClick(l, r, n);
}
void LoopStation::processStereo(float* l, float* r, uint32_t n, double bpm) noexcept {
    if (!l || !r || max_frames_ <= 0) return;
    uint32_t off = 0;
    while (off < n) {
        const uint32_t c = std::min(n - off, kClickBuf);
        core(l + off, r + off, c, bpm);
        mixClick(l + off, r + off, c);
        off += c;
    }
}

void LoopStation::publish() noexcept {
    bool any = clock_on_;
    int free_layers = 0;
    for (int i = 0; i < kPool; ++i) free_layers += layer_state_[i] != 1;
    for (int t = 0; t < kTracks; ++t) {
        const Track& T = tr_[t];
        TrackPub& P = pub_[t];
        any = any || T.state != LoopTrackState::Empty;
        P.state.store(static_cast<int>(T.state), std::memory_order_relaxed);
        P.n.store(T.n, std::memory_order_relaxed);
        P.total.store(T.total, std::memory_order_relaxed);
        P.stop_pending.store(T.stop_pending, std::memory_order_relaxed);
        P.len.store(T.len, std::memory_order_relaxed);
        P.rec.store(T.rec, std::memory_order_relaxed);
        P.close_at.store(T.close_at, std::memory_order_relaxed);
        P.wait.store(T.state == LoopTrackState::Armed ? std::max<int64_t>(0, T.start_at - clock_) : 0, std::memory_order_relaxed);
        double rp = 0.0;
        if (T.len > 0) { rp = T.rev ? static_cast<double>(T.len) - T.pos : T.pos; if (rp >= static_cast<double>(T.len)) rp = 0.0; rp /= static_cast<double>(T.len); }
        P.pos.store(rp, std::memory_order_relaxed);
        P.phase.store(T.pos, std::memory_order_relaxed);
        P.origin.store(T.origin, std::memory_order_relaxed);
        P.speed.store(T.speed, std::memory_order_relaxed);
        P.rev.store(T.rev, std::memory_order_relaxed);
        P.peak.store(T.peak, std::memory_order_relaxed);
        P.fade.store(T.fade, std::memory_order_relaxed);
        P.full.store(T.full, std::memory_order_relaxed);
        P.armed_rec.store(T.armed_rec, std::memory_order_relaxed);
        for (int k = 0; k < kMaxLayers; ++k) {
            P.ids[static_cast<std::size_t>(k)].store(k < T.total ? T.ids[k] : -1, std::memory_order_relaxed);
            P.gain[static_cast<std::size_t>(k)].store(k < T.total ? (k < T.n ? T.gain[k] : T.after[k][k]) : 0.0f, std::memory_order_relaxed);
        }
    }
    active_.store(any || click_env_ > 1e-4f, std::memory_order_relaxed);
    clock_pub_.store(clock_on_, std::memory_order_relaxed);
    clock_frames_pub_.store(clock_, std::memory_order_relaxed);
    anchor_pub_.store(anchor_, std::memory_order_relaxed);
    count_in_pub_.store(count_in_end_ > clock_ ? count_in_end_ - clock_ : -1, std::memory_order_relaxed);
    loop_bpm_pub_.store(loop_bpm_, std::memory_order_relaxed);
    live_bpm_pub_.store(live_bpm_, std::memory_order_relaxed);
    master_pub_.store(masterTrack(), std::memory_order_relaxed);
    free_layers_pub_.store(free_layers, std::memory_order_relaxed);
    in_peak_pub_.store(in_peak_, std::memory_order_relaxed);
    bpb_pub_.store(loop_bar_ > 0 ? loop_bpb_ : bpb_.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

// ───────────────────────────── JSON / API (HTTP thread) ─────────────────────────────
namespace {
void jnum(std::ostringstream& o, const char* k, double v, int prec = 4) {
    char b[64];
    std::snprintf(b, sizeof(b), "\"%s\":%.*f", k, prec, std::isfinite(v) ? v : 0.0);
    o << b;
}
const char* jb(bool v) { return v ? "true" : "false"; }
} // namespace

std::string LoopStation::toJson() const {
    std::ostringstream o;
    const int sel = selected();
    const auto sync = static_cast<LoopSync>(sync_.load(std::memory_order_relaxed));
    const double sr = sr_;
    const double loop_bpm = loop_bpm_pub_.load(std::memory_order_relaxed);
    const double live_bpm = live_bpm_pub_.load(std::memory_order_relaxed);
    const double bpm = loop_bpm > 0.0 ? loop_bpm : live_bpm;
    const int bpb = std::max(1, bpb_pub_.load(std::memory_order_relaxed));
    const double beatF = 60.0 / std::clamp(bpm, 20.0, 400.0) * sr;
    const double barF = beatF * bpb;
    const int64_t clk = clock_frames_pub_.load(std::memory_order_relaxed);
    const int64_t anc = anchor_pub_.load(std::memory_order_relaxed);
    const int64_t cin = count_in_pub_.load(std::memory_order_relaxed);
    const float fb = feedback_.load(std::memory_order_relaxed);

    auto trackJson = [&](int t) {
        const TrackPub& P = pub_[t];
        const int st = P.state.load(std::memory_order_relaxed);
        const int64_t len = P.len.load(std::memory_order_relaxed);
        const int n = P.n.load(std::memory_order_relaxed);
        const int total = P.total.load(std::memory_order_relaxed);
        const int sp = P.stop_pending.load(std::memory_order_relaxed);
        std::ostringstream q;
        q << "{\"idx\":" << t << ",\"state\":\"" << kStateNames[std::clamp(st, 0, 5)] << "\",";
        jnum(q, "length_s", len / sr); q << ",";
        jnum(q, "pos", P.pos.load(std::memory_order_relaxed), 5); q << ",";
        jnum(q, "rec_s", P.rec.load(std::memory_order_relaxed) / sr); q << ",";
        jnum(q, "close_s", P.close_at.load(std::memory_order_relaxed) / sr); q << ",";
        jnum(q, "wait_s", P.wait.load(std::memory_order_relaxed) / sr); q << ",";
        q << "\"armed_rec\":" << jb(P.armed_rec.load(std::memory_order_relaxed)) << ",";
        q << "\"layers\":" << n << ",\"redo\":" << std::max(0, total - n) << ",\"full\":" << jb(P.full.load(std::memory_order_relaxed)) << ",";
        q << "\"can_undo\":" << jb(n > 1 || st == static_cast<int>(LoopTrackState::Overdubbing)) << ",\"can_redo\":" << jb(total > n) << ",";
        jnum(q, "bars", sync == LoopSync::Tempo && barF > 0 ? len / barF : 0.0, 3); q << ",";
        jnum(q, "level", par_[t].level.load(std::memory_order_relaxed), 3); q << ",";
        jnum(q, "pan", par_[t].pan.load(std::memory_order_relaxed), 3); q << ",";
        q << "\"mute\":" << jb(par_[t].mute.load(std::memory_order_relaxed)) << ",";
        q << "\"reverse\":" << jb(par_[t].rev.load(std::memory_order_relaxed)) << ",";
        jnum(q, "speed", par_[t].speed.load(std::memory_order_relaxed), 2); q << ",";
        q << "\"stopping\":\"" << (sp == 1 ? "loop" : sp == 2 ? "fade" : sp == 3 ? "instant" : "none") << "\",";
        jnum(q, "fade", P.fade.load(std::memory_order_relaxed), 3); q << ",";
        jnum(q, "peak", P.peak.load(std::memory_order_relaxed), 4); q << ",";
        q << "\"env_rev\":" << P.env_rev.load(std::memory_order_relaxed) << ",\"gains\":[";
        for (int k = 0; k < total && k < kMaxLayers; ++k) {
            char b[24];
            std::snprintf(b, sizeof(b), "%s%.3f", k ? "," : "", P.gain[static_cast<std::size_t>(k)].load(std::memory_order_relaxed));
            q << b;
        }
        q << "]}";
        return q.str();
    };

    const TrackPub& S = pub_[sel];
    const int sst = S.state.load(std::memory_order_relaxed);
    const int sn = S.n.load(std::memory_order_relaxed), stotal = S.total.load(std::memory_order_relaxed);
    const int64_t slen = S.len.load(std::memory_order_relaxed);
    o << "{\"version\":2,";
    // ── legacy single-loop fields (the selected track) — kept for older callers ──
    o << "\"state\":\"" << kStateNames[std::clamp(sst, 0, 5)] << "\",\"state_id\":" << sst << ",";
    o << "\"pre_rig\":" << jb(pre_rig_.load(std::memory_order_relaxed)) << ",";
    o << "\"reverse\":" << jb(par_[sel].rev.load(std::memory_order_relaxed)) << ",";
    o << "\"half_speed\":" << jb(par_[sel].speed.load(std::memory_order_relaxed) < 0.75f) << ",";
    o << "\"quantize\":" << jb(sync == LoopSync::Tempo) << ",";
    o << "\"can_undo\":" << jb(sn > 1 || sst == static_cast<int>(LoopTrackState::Overdubbing)) << ",\"can_redo\":" << jb(stotal > sn) << ",";
    o << "\"layers\":" << std::max(0, sn - 1) << ",";
    jnum(o, "length_s", slen / sr); o << ",";
    jnum(o, "pos", S.pos.load(std::memory_order_relaxed), 5); o << ",";
    jnum(o, "speed", par_[sel].speed.load(std::memory_order_relaxed), 2); o << ",";
    jnum(o, "level", level_.load(std::memory_order_relaxed), 3); o << ",";
    jnum(o, "feedback", fb, 3); o << ",";
    jnum(o, "peak", in_peak_pub_.load(std::memory_order_relaxed), 4); o << ",";
    // ── multitrack ──
    o << "\"selected\":" << sel << ",\"tracks_max\":" << kTracks << ",";
    o << "\"sync\":\"" << kSyncNames[static_cast<int>(sync)] << "\",\"bars_setting\":" << bars_.load(std::memory_order_relaxed) << ",";
    o << "\"beats\":" << bpb << ",\"count_in\":" << jb(count_in_.load(std::memory_order_relaxed)) << ",";
    o << "\"click\":" << jb(click_.load(std::memory_order_relaxed)) << ",";
    jnum(o, "click_level", click_level_.load(std::memory_order_relaxed), 3); o << ",";
    o << "\"stop_mode\":\"" << kStopNames[std::clamp(stop_mode_.load(std::memory_order_relaxed), 0, 2)] << "\",";
    jnum(o, "fade_s", fade_s_.load(std::memory_order_relaxed), 2); o << ",";
    o << "\"after_rec\":\"" << (after_rec_dub_.load(std::memory_order_relaxed) ? "dub" : "play") << "\",";
    o << "\"monitor\":" << jb(monitor_.load(std::memory_order_relaxed)) << ",";
    jnum(o, "bpm", bpm, 2); o << ",";
    jnum(o, "rig_bpm", live_bpm, 2); o << ",";
    o << "\"tempo_locked\":" << jb(loop_bpm > 0.0) << ",";
    {
        const double rel = static_cast<double>(clk - anc);
        const double beats = rel / beatF;
        const int64_t bi = static_cast<int64_t>(std::floor(beats));
        o << "\"clock\":{\"running\":" << jb(clock_pub_.load(std::memory_order_relaxed)) << ",";
        jnum(o, "beat_pos", beats, 3);
        o << ",\"bar\":" << (bi >= 0 ? bi / bpb + 1 : 0) << ",\"beat\":" << posMod(bi, bpb) + 1;
        o << ",\"count_in\":" << (cin > 0 ? static_cast<int>(std::ceil(cin / beatF - 1e-6)) : 0) << "},";
    }
    o << "\"master\":" << master_pub_.load(std::memory_order_relaxed) << ",\"free_layers\":" << free_layers_pub_.load(std::memory_order_relaxed)
      << ",\"max_layers\":" << kMaxLayers << ",";
    jnum(o, "max_s", kMaxSeconds, 0); o << ",";
    o << "\"active\":" << jb(active()) << ",";
    // legacy 32-point envelope of the selected track's audible mix
    o << "\"peaks\":[";
    {
        constexpr int kB = 32;
        for (int b = 0; b < kB; ++b) {
            float v = 0.0f;
            if (slen > 0 && env_) {
                const int64_t c0 = (static_cast<int64_t>(b) * slen / kB) / env_frames_;
                const int64_t c1 = std::max(c0 + 1, (static_cast<int64_t>(b + 1) * slen / kB) / env_frames_);
                for (int k = 0; k < sn && k < kMaxLayers; ++k) {
                    const int id = S.ids[static_cast<std::size_t>(k)].load(std::memory_order_relaxed);
                    if (id < 0) continue;
                    float m = 0.0f;
                    for (int64_t c = c0; c < c1 && c < env_cells_; ++c) m = std::max(m, env_[id * env_cells_ + c].load(std::memory_order_relaxed));
                    v += m * S.gain[static_cast<std::size_t>(k)].load(std::memory_order_relaxed);
                }
            }
            char buf2[16];
            std::snprintf(buf2, sizeof(buf2), "%s%.3f", b ? "," : "", std::min(1.0f, v));
            o << buf2;
        }
    }
    o << "],\"tracks\":[";
    for (int t = 0; t < kTracks; ++t) o << (t ? "," : "") << trackJson(t);
    o << "]}";
    return o.str();
}

std::string LoopStation::waveJson(int t, int bins) const {
    if (t < 0 || t >= kTracks) t = selected();
    bins = std::clamp(bins, 8, 2048);
    const TrackPub& P = pub_[t];
    const int st = P.state.load(std::memory_order_relaxed);
    const int64_t frames = st == static_cast<int>(LoopTrackState::Recording) ? P.rec.load(std::memory_order_relaxed) : P.len.load(std::memory_order_relaxed);
    const int n = P.n.load(std::memory_order_relaxed), total = P.total.load(std::memory_order_relaxed);
    std::ostringstream o;
    o << "{\"track\":" << t << ",\"rev\":" << P.env_rev.load(std::memory_order_relaxed) << ",\"frames\":" << frames << ",";
    jnum(o, "length_s", frames / sr_);
    o << ",\"bins\":" << bins << ",\"layers\":[";
    for (int k = 0; k < total && k < kMaxLayers; ++k) {
        const int id = P.ids[static_cast<std::size_t>(k)].load(std::memory_order_relaxed);
        o << (k ? "," : "") << "{\"gain\":";
        char g[24];
        std::snprintf(g, sizeof(g), "%.3f", P.gain[static_cast<std::size_t>(k)].load(std::memory_order_relaxed));
        o << g << ",\"redo\":" << jb(k >= n) << ",\"peaks\":[";
        for (int b = 0; b < bins; ++b) {
            float v = 0.0f;
            if (frames > 0 && id >= 0 && env_) {
                const int64_t c0 = (static_cast<int64_t>(b) * frames / bins) / env_frames_;
                const int64_t c1 = std::max(c0 + 1, (static_cast<int64_t>(b + 1) * frames / bins) / env_frames_);
                for (int64_t c = c0; c < c1 && c < env_cells_; ++c) v = std::max(v, env_[id * env_cells_ + c].load(std::memory_order_relaxed));
            }
            char buf2[16];
            std::snprintf(buf2, sizeof(buf2), "%s%.3f", b ? "," : "", std::min(2.0f, v));
            o << buf2;
        }
        o << "]}";
    }
    o << "]}";
    return o.str();
}

// One song cycle: the longest audible loop, extended to a common multiple of the others (≤ 8×, ≤ 10 min).
bool LoopStation::stemCycle(int track, int64_t& frames) const {
    double longest = 0.0;
    double cyc[kTracks] = {};
    for (int t = 0; t < kTracks; ++t) {
        const int64_t len = pub_[t].len.load(std::memory_order_relaxed);
        if (len <= 0 || (track >= 0 && t != track)) continue;
        cyc[t] = static_cast<double>(len) / std::max(0.25f, pub_[t].speed.load(std::memory_order_relaxed));
        longest = std::max(longest, cyc[t]);
    }
    if (longest <= 0.0) return false;
    double best = longest;
    for (int m = 1; m <= 8; ++m) {
        const double c = longest * m;
        if (c > sr_ * 600.0) break;
        bool ok = true;
        for (double e : cyc) if (e > 0.0) { const double r = std::fmod(c, e); ok = ok && (r < 2.0 || e - r < 2.0); }
        if (ok) { best = c; break; }
    }
    frames = static_cast<int64_t>(std::llround(best));
    return frames > 0;
}

bool LoopStation::exportWav(const std::string& path, int track, std::string* err) const {
    int64_t frames = 0;
    if (!stemCycle(-1, frames)) { if (err) *err = "Nothing recorded yet"; return false; }
    if (track >= 0 && pub_[track].len.load(std::memory_order_relaxed) <= 0) { if (err) *err = "That track is empty"; return false; }
    // What plays is what you get: with anything running, only running tracks, from their live phase;
    // with everything stopped, every recorded track lined up as All Start would start it.
    bool any_running = false;
    for (int t = 0; t < kTracks; ++t) any_running = any_running || running(static_cast<LoopTrackState>(pub_[t].state.load(std::memory_order_relaxed)));
    int ref = -1;
    for (int t = 0; t < kTracks; ++t) {
        const int64_t len = pub_[t].len.load(std::memory_order_relaxed);
        if (len <= 0) continue;
        if (any_running && !running(static_cast<LoopTrackState>(pub_[t].state.load(std::memory_order_relaxed)))) continue;
        if (ref < 0 || len > pub_[ref].len.load(std::memory_order_relaxed)) ref = t;
    }
    if (ref < 0) { if (err) *err = "Nothing recorded yet"; return false; }
    const auto sync = static_cast<LoopSync>(sync_.load(std::memory_order_relaxed));
    double start[kTracks] = {};
    bool use[kTracks] = {};
    {
        const double refLen = static_cast<double>(pub_[ref].len.load(std::memory_order_relaxed));
        const double refSpeed = pub_[ref].speed.load(std::memory_order_relaxed);
        const double refPhase = pub_[ref].phase.load(std::memory_order_relaxed);
        const double dt = any_running ? (refLen - refPhase) / refSpeed : 0.0;   // frames until the reference wraps
        const int64_t refOrigin = pub_[ref].origin.load(std::memory_order_relaxed);
        for (int t = 0; t < kTracks; ++t) {
            const int64_t len = pub_[t].len.load(std::memory_order_relaxed);
            if (len <= 0) continue;
            const auto st = static_cast<LoopTrackState>(pub_[t].state.load(std::memory_order_relaxed));
            if (any_running && !running(st)) continue;
            if (track < 0 && par_[t].mute.load(std::memory_order_relaxed)) continue;
            if (track >= 0 && t != track) continue;
            use[t] = true;
            if (any_running) start[t] = std::fmod(pub_[t].phase.load(std::memory_order_relaxed) + dt * pub_[t].speed.load(std::memory_order_relaxed), static_cast<double>(len));
            else if (sync != LoopSync::Free) start[t] = static_cast<double>(posMod(refOrigin - pub_[t].origin.load(std::memory_order_relaxed), len));
        }
    }
    std::ofstream f(path, std::ios::binary);
    if (!f) { if (err) *err = "Couldn't write " + path; return false; }
    const uint32_t sr = static_cast<uint32_t>(std::lround(sr_));
    const uint16_t ch = 2, bits = 24, fmt = 1;
    const uint16_t align = ch * 3;
    const uint32_t byte_rate = sr * align;
    const uint64_t data64 = static_cast<uint64_t>(frames) * align;
    if (data64 > 0xFFFFFF00ull) { if (err) *err = "Loop too long to export"; return false; }
    const uint32_t data = static_cast<uint32_t>(data64), riff = 36 + data, fmt_size = 16;
    f.write("RIFF", 4); f.write(reinterpret_cast<const char*>(&riff), 4); f.write("WAVE", 4);
    f.write("fmt ", 4); f.write(reinterpret_cast<const char*>(&fmt_size), 4); f.write(reinterpret_cast<const char*>(&fmt), 2);
    f.write(reinterpret_cast<const char*>(&ch), 2); f.write(reinterpret_cast<const char*>(&sr), 4); f.write(reinterpret_cast<const char*>(&byte_rate), 4);
    f.write(reinterpret_cast<const char*>(&align), 2); f.write(reinterpret_cast<const char*>(&bits), 2);
    f.write("data", 4); f.write(reinterpret_cast<const char*>(&data), 4);

    const float master = track < 0 ? level_.load(std::memory_order_relaxed) : 1.0f;
    std::vector<char> out;
    out.reserve(4096 * align);
    for (int64_t s = 0; s < frames; ++s) {
        float L = 0.0f, R = 0.0f;
        for (int t = 0; t < kTracks; ++t) {
            if (!use[t]) continue;
            const TrackPub& P = pub_[t];
            const int64_t len = P.len.load(std::memory_order_relaxed);
            const double sp = P.speed.load(std::memory_order_relaxed);
            double ph = std::fmod(start[t] + static_cast<double>(s) * sp, static_cast<double>(len));
            double rp = P.rev.load(std::memory_order_relaxed) ? static_cast<double>(len) - ph : ph;
            if (rp >= static_cast<double>(len)) rp -= static_cast<double>(len);
            const int64_t i0 = std::min<int64_t>(len - 1, static_cast<int64_t>(rp));
            const int64_t i1 = i0 + 1 >= len ? 0 : i0 + 1;
            const float fr = static_cast<float>(rp - static_cast<double>(i0));
            const int n = P.n.load(std::memory_order_relaxed);
            float tl = 0.0f, trr = 0.0f;
            for (int k = 0; k < n && k < kMaxLayers; ++k) {
                const int id = P.ids[static_cast<std::size_t>(k)].load(std::memory_order_relaxed);
                if (id < 0) continue;
                const float g = P.gain[static_cast<std::size_t>(k)].load(std::memory_order_relaxed);
                const float* b = pool_[id];
                tl += g * (b[2 * i0] + fr * (b[2 * i1] - b[2 * i0]));
                trr += g * (b[2 * i0 + 1] + fr * (b[2 * i1 + 1] - b[2 * i0 + 1]));
            }
            const float lv = par_[t].level.load(std::memory_order_relaxed);
            const float pan = par_[t].pan.load(std::memory_order_relaxed);
            L += tl * lv * (pan > 0.0f ? 1.0f - pan : 1.0f);
            R += trr * lv * (pan < 0.0f ? 1.0f + pan : 1.0f);
        }
        for (float v : {L * master, R * master}) {
            const int32_t q = static_cast<int32_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 8388607.0f));
            out.push_back(static_cast<char>(q & 0xFF));
            out.push_back(static_cast<char>((q >> 8) & 0xFF));
            out.push_back(static_cast<char>((q >> 16) & 0xFF));
        }
        if (out.size() >= 4096u * align) { f.write(out.data(), static_cast<std::streamsize>(out.size())); out.clear(); }
    }
    if (!out.empty()) f.write(out.data(), static_cast<std::streamsize>(out.size()));
    if (!f) { if (err) *err = "Couldn't write " + path; return false; }
    return true;
}

std::string LoopStation::handleApi(const std::string& path, const std::function<std::string(const char*)>& q, const std::string& export_dir) {
    auto has = [&](const char* k) { return !q(k).empty(); };
    auto num = [&](const char* k, double d) { const std::string v = q(k); if (v.empty()) return d; char* e = nullptr; const double x = std::strtod(v.c_str(), &e); return (e && e != v.c_str() && std::isfinite(x)) ? x : d; };
    auto on = [&](const char* k) { const std::string v = q(k); return v == "1" || v == "true" || v == "on"; };
    const std::string tq = q("track");
    const bool all = tq == "all";
    int t = tq.empty() || all ? selected() : std::clamp(static_cast<int>(num("track", selected())), 0, kTracks - 1);

    if (path == "/api/looper/wave") return waveJson(t, static_cast<int>(num("bins", 256)));

    if (has("select")) select(std::clamp(static_cast<int>(num("select", 0)), 0, kTracks - 1));
    // settings
    if (has("pre_rig")) setPreRig(on("pre_rig"));
    if (has("monitor")) setMonitor(on("monitor"));
    if (has("sync")) { const std::string s = q("sync"); setSync(s == "tempo" ? LoopSync::Tempo : s == "loop" ? LoopSync::Loop : LoopSync::Free); }
    if (has("quantize")) setSync(on("quantize") ? LoopSync::Tempo : LoopSync::Free);   // legacy
    if (has("bars")) setBars(static_cast<int>(num("bars", 0)));
    if (has("beats")) setBeatsPerBar(static_cast<int>(num("beats", 4)));
    if (has("count_in")) setCountIn(on("count_in"));
    if (has("click")) setClick(on("click"));
    if (has("click_level")) setClickLevel(static_cast<float>(num("click_level", 0.5)));
    if (has("stop_mode")) { const std::string s = q("stop_mode"); setStopMode(s == "fade" ? LoopStopMode::Fade : s == "loop" ? LoopStopMode::LoopEnd : LoopStopMode::Instant); }
    if (has("fade_s")) setFadeSeconds(static_cast<float>(num("fade_s", 4)));
    if (has("after_rec")) setAfterRecDub(q("after_rec") == "dub");
    if (has("feedback")) setFeedback(static_cast<float>(num("feedback", 1)));
    if (has("level") && tq.empty()) setLevel(static_cast<float>(num("level", 1)));
    // per-track (track=N, default the selected track)
    if (has("level") && !tq.empty() && !all) setTrackLevel(t, static_cast<float>(num("level", 1)));
    if (has("track_level")) setTrackLevel(t, static_cast<float>(num("track_level", 1)));
    if (has("pan")) setTrackPan(t, static_cast<float>(num("pan", 0)));
    if (has("mute")) setTrackMute(t, on("mute"));
    if (has("reverse")) setTrackReverse(t, on("reverse"));
    if (has("half_speed")) setTrackSpeed(t, on("half_speed") ? 0.5f : 1.0f);   // legacy
    if (has("speed")) setTrackSpeed(t, static_cast<float>(num("speed", 1)));

    const std::string a = q("action");
    bool queued = true;
    if (a.empty()) {
    } else if (a == "toggle" || a == "press") queued = post(kPress, t);
    else if (a == "record") queued = post(kRecord, t);
    else if (a == "overdub") queued = post(kOverdub, t);
    else if (a == "play") queued = all ? post(kPlayAll) : post(kPlay, t);
    else if (a == "play_all") queued = post(kPlayAll);
    else if (a == "all") queued = post(kAllStartStop);
    else if (a == "stop") {
        const std::string m = q("mode");
        const float mv = m == "instant" ? 1.0f : m == "loop" ? 2.0f : m == "fade" ? 3.0f : 0.0f;
        queued = tq.empty() || all ? (m == "instant" ? post(kStopInstantAll) : post(kStopAll)) : post(kStop, t, mv);
    }
    else if (a == "stop_all") queued = post(kStopAll);
    else if (a == "clear") queued = tq.empty() || all ? post(kClearAll) : post(kClear, t);
    else if (a == "undo") queued = post(kUndoRedo, t);
    else if (a == "redo") queued = post(kRedo, t);
    else if (a == "export") {
        const std::string what = q("what");
        { std::error_code ec; std::filesystem::create_directory(export_dir, ec); } // like mkdir: one level, errors ignored
        char stamp[64];
        const std::time_t now = std::time(nullptr);
        std::tm tmv{};
#ifdef _WIN32
        localtime_s(&tmv, &now);
#else
        localtime_r(&now, &tmv);
#endif
        std::strftime(stamp, sizeof(stamp), "Loop %Y-%m-%d %H.%M.%S", &tmv);
        std::vector<std::string> files;
        std::string err;
        bool ok = true;
        if (what == "stems") {
            const std::string dir = export_dir + "/" + stamp;
            { std::error_code ec; std::filesystem::create_directory(dir, ec); }
            ok = exportWav(dir + "/Mix.wav", -1, &err);
            if (ok) files.push_back(dir + "/Mix.wav");
            for (int i = 0; ok && i < kTracks; ++i) {
                if (pub_[i].len.load(std::memory_order_relaxed) <= 0) continue;
                const std::string p = dir + "/Track " + std::to_string(i + 1) + ".wav";
                if (exportWav(p, i, &err)) files.push_back(p);
            }
        } else if (what == "track") {
            const std::string p = export_dir + "/" + stamp + " - Track " + std::to_string(t + 1) + ".wav";
            ok = exportWav(p, t, &err);
            if (ok) files.push_back(p);
        } else {
            const std::string p = export_dir + "/" + stamp + ".wav";
            ok = exportWav(p, -1, &err);
            if (ok) files.push_back(p);
        }
        auto esc = [](const std::string& s) {
            std::string r;
            for (char c : s) { if (c == '"' || c == '\\') r += '\\'; r += c; }
            return r;
        };
        std::ostringstream o;
        o << "{\"ok\":" << jb(ok && !files.empty());
        if (!files.empty()) o << ",\"path\":\"" << esc(files.front()) << "\"";
        o << ",\"folder\":\"" << esc(export_dir) << "\",\"files\":[";
        for (std::size_t i = 0; i < files.size(); ++i) o << (i ? "," : "") << "\"" << esc(files[i]) << "\"";
        o << "]";
        if (!ok || files.empty()) o << ",\"error\":\"" << esc(err.empty() ? "Nothing recorded yet" : err) << "\"";
        o << "}";
        return o.str();
    }
    std::string j = toJson();
    if (!queued) j.insert(1, "\"queued\":false,");
    return j;
}

} // namespace rig
