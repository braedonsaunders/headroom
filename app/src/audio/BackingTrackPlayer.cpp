#include "BackingTrackPlayer.hpp"
#include "../platform/AudioDecoder.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace rig::audio {

namespace {

inline float softGuard(float v) noexcept {
    const float a = std::fabs(v);
    if (a <= 0.95f) return v;
    const float x = (a - 0.95f) / 0.05f;
    return std::copysign(0.95f + 0.05f * (x / (1.0f + x)), v);
}

} // namespace

bool probeBackingFile(const std::string& path, BackingFileInfo& info, std::string* err) {
    platform::AudioFileInfo pi;
    if (!platform::probeAudioFile(path, pi, err)) return false;
    info.seconds = pi.seconds;
    info.sample_rate = pi.sample_rate;
    info.channels = pi.channels;
    info.title = pi.title;
    info.artist = pi.artist;
    info.format = pi.format;
    return true;
}

std::unique_ptr<BackingAudio> decodeBackingFile(const std::string& path, double sample_rate, std::string* err,
                                                const std::atomic<bool>* cancel, double max_seconds) {
    auto reader = platform::openAudioFile(path, sample_rate, err);
    if (!reader) return nullptr;
    const int64_t len = reader->sourceFrames();
    const double src_rate = reader->sourceRate();
    if (double(len) / src_rate > max_seconds) {
        if (err) *err = "That track is longer than " + std::to_string(int(max_seconds / 60)) + " minutes";
        return nullptr;
    }
    const uint32_t ch = static_cast<uint32_t>(reader->channels());

    auto out = std::make_unique<BackingAudio>();
    out->sample_rate = sample_rate;
    // Fixed ±2.0 range: decoders overshoot full scale a little (inter-sample peaks); 15 bits of resolution is
    // far below anything audible under a guitar.
    constexpr float kRange = 2.0f;
    out->scale = kRange / 32767.0f;
    const double est = len > 0 ? double(len) * sample_rate / src_rate : 0.0;
    const std::size_t max_frames = static_cast<std::size_t>(max_seconds * sample_rate);
    try { out->pcm.reserve(static_cast<std::size_t>(est * 1.01 + 4096) * 2); } catch (...) { if (err) *err = "Not enough memory"; return nullptr; }

    constexpr uint32_t kChunk = 16384;
    std::vector<float> buf(kChunk * ch);
    float peak = 0.0f;
    for (;;) {
        if (cancel && cancel->load(std::memory_order_relaxed)) { if (err) *err = "cancelled"; return nullptr; }
        const int64_t got = reader->read(buf.data(), kChunk, err);
        if (got < 0) return nullptr;
        const uint32_t frames = static_cast<uint32_t>(got);
        if (frames == 0) break;
        if (out->frames + frames > max_frames) {
            if (err) *err = "That track is too long";
            return nullptr;
        }
        for (uint32_t i = 0; i < frames; ++i) {
            const float l = buf[i * ch], r = ch > 1 ? buf[i * ch + 1] : l;
            peak = std::max(peak, std::max(std::fabs(l), std::fabs(r)));
            out->pcm.push_back(static_cast<int16_t>(std::lrint(std::clamp(l / kRange, -1.0f, 1.0f) * 32767.0f)));
            out->pcm.push_back(static_cast<int16_t>(std::lrint(std::clamp(r / kRange, -1.0f, 1.0f) * 32767.0f)));
        }
        out->frames += frames;
    }
    if (out->frames < static_cast<std::size_t>(0.05 * sample_rate)) {
        if (err) *err = "This file has no audio in it";
        return nullptr;
    }
    out->pcm.shrink_to_fit();
    out->peak = std::min(peak, kRange);

    // Overview bins + gated loudness (400 ms windows; −70 dBFS absolute gate, then −10 dB relative, like BS.1770
    // without the K-weighting) — enough to bring every track to a similar level under the guitar.
    const std::size_t nb = BackingAudio::kBins;
    out->bin_peak.assign(nb, 0.0f);
    out->bin_rms.assign(nb, 0.0f);
    std::vector<double> bin_ss(nb, 0.0);
    std::vector<uint32_t> bin_n(nb, 0);
    const std::size_t win = std::max<std::size_t>(1, static_cast<std::size_t>(0.4 * sample_rate));
    std::vector<double> wins;
    wins.reserve(out->frames / win + 1);
    double wss = 0.0;
    std::size_t wn = 0;
    const float s = out->scale;
    for (std::size_t i = 0; i < out->frames; ++i) {
        const float l = out->pcm[2 * i] * s, r = out->pcm[2 * i + 1] * s;
        const float m = std::max(std::fabs(l), std::fabs(r));
        const double ms = 0.5 * (double(l) * l + double(r) * r);
        const std::size_t b = std::min(nb - 1, i * nb / out->frames);
        out->bin_peak[b] = std::max(out->bin_peak[b], m);
        bin_ss[b] += ms;
        ++bin_n[b];
        wss += ms;
        if (++wn == win) { wins.push_back(wss / double(wn)); wss = 0.0; wn = 0; }
    }
    if (wn > win / 4) wins.push_back(wss / double(wn));
    for (std::size_t b = 0; b < nb; ++b) out->bin_rms[b] = bin_n[b] ? static_cast<float>(std::sqrt(bin_ss[b] / bin_n[b])) : 0.0f;
    auto gatedMean = [&](double thr) {
        double sum = 0.0;
        std::size_t k = 0;
        for (double w : wins) if (w > thr) { sum += w; ++k; }
        return k ? sum / double(k) : 0.0;
    };
    const double abs_mean = gatedMean(1e-7); // −70 dBFS
    const double rel_mean = abs_mean > 0 ? gatedMean(abs_mean * 0.1) : 0.0;
    const double rms = std::sqrt(std::max(rel_mean, 1e-12));
    out->rms_db = static_cast<float>(20.0 * std::log10(rms));
    float g = rms > 1e-5 ? static_cast<float>(BackingAudio::kTargetRms / rms) : 1.0f;
    if (out->peak > 1e-4f) g = std::min(g, 1.12f / out->peak); // never push peaks far past full scale
    out->norm_gain = std::clamp(g, 0.1f, 4.0f);
    return out;
}

// ───────────────────────────── player ─────────────────────────────

BackingTrackPlayer::BackingTrackPlayer() {
    scratch_l_.assign(kMaxBlock, 0.0f);
    scratch_r_.assign(kMaxBlock, 0.0f);
}

BackingTrackPlayer::~BackingTrackPlayer() {
    stretch_.release();
    for (auto& r : retired_) r.store(nullptr);
    incoming_.store(nullptr);
    std::lock_guard<std::mutex> lk(own_mtx_);
    owned_.clear();
}

void BackingTrackPlayer::prepare(double sample_rate) {
    sample_rate_ = sample_rate > 0 ? sample_rate : 48000.0;
    stretch_.release();
    stretching_ = false;
    if (stretch_.prepare(sample_rate_, kMaxBlock, &BackingTrackPlayer::stretchPull, this)) stretch_latency_sec_ = stretch_.latencySec();
}

void BackingTrackPlayer::setAudio(std::unique_ptr<BackingAudio> audio, double start_sec) {
    collect();
    if (!audio) { clear(); return; }
    BackingAudio* raw = audio.get();
    const double dur = audio->seconds();
    {
        std::lock_guard<std::mutex> lk(own_mtx_);
        owned_.push_back(std::move(audio));
    }
    want_play_.store(false);
    countin_req_.store(0);
    seek_req_.store(-1.0);
    ended_.store(false);
    start_sec_.store(std::clamp(start_sec, 0.0, dur));
    pos_pub_.store(std::clamp(start_sec, 0.0, dur));
    duration_.store(dur);
    clear_req_.store(false);
    if (BackingAudio* never = incoming_.exchange(raw, std::memory_order_acq_rel)) {
        // The audio thread never took the previous hand-over: it's ours to free.
        std::lock_guard<std::mutex> lk(own_mtx_);
        owned_.erase(std::remove_if(owned_.begin(), owned_.end(), [&](const auto& p) { return p.get() == never; }), owned_.end());
    }
}

void BackingTrackPlayer::clear() {
    want_play_.store(false);
    countin_req_.store(0);
    if (BackingAudio* never = incoming_.exchange(nullptr, std::memory_order_acq_rel)) {
        std::lock_guard<std::mutex> lk(own_mtx_);
        owned_.erase(std::remove_if(owned_.begin(), owned_.end(), [&](const auto& p) { return p.get() == never; }), owned_.end());
    }
    clear_req_.store(true, std::memory_order_release);
    duration_.store(0.0);
    pos_pub_.store(0.0);
    ended_.store(false);
    collect();
}

void BackingTrackPlayer::collect() {
    std::lock_guard<std::mutex> lk(own_mtx_);
    for (auto& slot : retired_) {
        if (BackingAudio* a = slot.exchange(nullptr, std::memory_order_acq_rel))
            owned_.erase(std::remove_if(owned_.begin(), owned_.end(), [&](const auto& p) { return p.get() == a; }), owned_.end());
    }
}

void BackingTrackPlayer::play() {
    if (!hasAudio()) return;
    const double dur = duration();
    if (ended_.load() || position() >= dur - 0.05) {
        seek(loopOn() && loopB() > loopA() ? loopA() : 0.0);
        ended_.store(false);
    }
    const int beats = countin_beats_.load();
    countin_req_.store(beats > 0 ? beats : 0);
    want_play_.store(true);
}

void BackingTrackPlayer::pause() {
    want_play_.store(false);
    countin_req_.store(0);
}

void BackingTrackPlayer::seek(double seconds) {
    const double dur = duration();
    seconds = std::clamp(seconds, 0.0, std::max(0.0, dur));
    ended_.store(false);
    seek_req_.store(seconds, std::memory_order_release);
}

void BackingTrackPlayer::setVolume(float v) { volume_.store(std::clamp(std::isfinite(v) ? v : 1.0f, 0.0f, 2.0f)); }
void BackingTrackPlayer::setSpeed(float s) { speed_.store(std::clamp(std::isfinite(s) ? s : 1.0f, 0.5f, 1.5f)); }
void BackingTrackPlayer::setSemitones(float st) { semitones_.store(std::clamp(std::isfinite(st) ? st : 0.0f, -12.0f, 12.0f)); }
void BackingTrackPlayer::setCountIn(int beats, double bpm) {
    countin_beats_.store(std::clamp(beats, 0, 16));
    if (std::isfinite(bpm) && bpm > 0) countin_bpm_.store(std::clamp(bpm, 30.0, 300.0));
}
void BackingTrackPlayer::setDuck(bool on, float depth_db) {
    duck_on_.store(on);
    if (std::isfinite(depth_db)) duck_depth_db_.store(std::clamp(depth_db, -30.0f, -1.0f));
}
void BackingTrackPlayer::setLoop(bool on, double a, double b) {
    if (!std::isfinite(a)) a = 0.0;
    if (!std::isfinite(b)) b = 0.0;
    if (b < a) std::swap(a, b);
    loop_a_.store(std::max(0.0, a));
    loop_b_.store(std::max(0.0, b));
    loop_on_.store(on && b - a >= 0.05);
}

double BackingTrackPlayer::position() const {
    const double sk = seek_req_.load(std::memory_order_acquire);
    return sk >= 0.0 ? sk : pos_pub_.load(std::memory_order_relaxed);
}

// ───────────── audio thread ─────────────

bool BackingTrackPlayer::retireSlotFree() const noexcept {
    for (const auto& s : retired_) if (!s.load(std::memory_order_relaxed)) return true;
    return false;
}

bool BackingTrackPlayer::retire(BackingAudio* a) noexcept {
    for (auto& s : retired_) {
        BackingAudio* expected = nullptr;
        if (s.compare_exchange_strong(expected, a, std::memory_order_acq_rel)) return true;
    }
    return false;
}

void BackingTrackPlayer::readFrame(int64_t f, float& l, float& r) const noexcept {
    if (!cur_ || f < 0 || f >= static_cast<int64_t>(cur_->frames)) { l = r = 0.0f; return; }
    const int16_t* p = cur_->pcm.data() + 2 * f;
    l = p[0] * cur_->scale;
    r = p[1] * cur_->scale;
}

void BackingTrackPlayer::jumpTo(int64_t target) noexcept {
    xf_from_ = pos_;
    xf_left_ = kXfade;
    pos_ = target;
}

void BackingTrackPlayer::pull(float* l, float* r, uint32_t n) noexcept {
    if (!cur_) { std::fill(l, l + n, 0.0f); std::fill(r, r + n, 0.0f); return; }
    const int64_t frames = static_cast<int64_t>(cur_->frames);
    const bool loop = loop_on_.load(std::memory_order_relaxed);
    const int64_t a = std::clamp<int64_t>(std::llround(loop_a_.load(std::memory_order_relaxed) * sample_rate_), 0, frames);
    const int64_t b = std::clamp<int64_t>(std::llround(loop_b_.load(std::memory_order_relaxed) * sample_rate_), 0, frames);
    const bool looping = loop && b - a > static_cast<int64_t>(0.05 * sample_rate_);
    for (uint32_t i = 0; i < n; ++i) {
        if (looping && pos_ >= b) jumpTo(a);
        float xl, xr;
        if (pos_ >= frames) {
            xl = xr = 0.0f;
            if (!ended_.load(std::memory_order_relaxed)) {
                ended_.store(true, std::memory_order_relaxed);
                want_play_.store(false, std::memory_order_relaxed);
            }
        } else {
            readFrame(pos_, xl, xr);
            ++pos_;
        }
        if (xf_left_ > 0) {
            // equal-power splice: the old read position fades out while the new one fades in
            const float t = 1.0f - static_cast<float>(xf_left_) / kXfade;
            const float gi = std::sin(t * 0.5f * std::numbers::pi_v<float>), go = std::cos(t * 0.5f * std::numbers::pi_v<float>);
            float ol, orr;
            readFrame(xf_from_++, ol, orr);
            xl = xl * gi + ol * go;
            xr = xr * gi + orr * go;
            --xf_left_;
        }
        l[i] = xl;
        r[i] = xr;
    }
}

void BackingTrackPlayer::stretchPull(void* ref, float* l, float* r, uint32_t n) noexcept {
    static_cast<BackingTrackPlayer*>(ref)->pull(l, r, n);
}

void BackingTrackPlayer::process(float* out_l, float* out_r, uint32_t n, const float* side_l, const float* side_r) noexcept {
    for (uint32_t off = 0; off < n; off += kMaxBlock) {
        const uint32_t m = std::min(kMaxBlock, n - off);
        processChunk(out_l + off, out_r + off, m, side_l ? side_l + off : nullptr, side_r ? side_r + off : nullptr);
    }
}

void BackingTrackPlayer::processChunk(float* out_l, float* out_r, uint32_t n, const float* side_l, const float* side_r) noexcept {
    const double sr = sample_rate_;
    // 1. buffer hand-over
    if (clear_req_.load(std::memory_order_relaxed) && clear_req_.exchange(false, std::memory_order_acq_rel)) {
        if (cur_ && !retire(cur_)) clear_req_.store(true, std::memory_order_relaxed); // ring full: try next block
        else { cur_ = nullptr; running_ = false; run_ = 0.0f; countin_left_ = 0; }
    }
    if (incoming_.load(std::memory_order_relaxed) && (!cur_ || retireSlotFree())) {
        if (BackingAudio* a = incoming_.exchange(nullptr, std::memory_order_acq_rel)) {
            if (cur_) retire(cur_);
            cur_ = a;
            pos_ = std::llround(start_sec_.load(std::memory_order_relaxed) * a->sample_rate);
            xf_left_ = 0;
            run_ = 0.0f;
            running_ = false;
            countin_left_ = 0;
            stretching_ = false;
        }
    }
    if (!cur_) {
        countin_left_beats_.store(0, std::memory_order_relaxed);
        level_.store(0.0f, std::memory_order_relaxed);
        return;
    }

    // 2. seek
    if (seek_req_.load(std::memory_order_relaxed) >= 0.0) {
        const double sk = seek_req_.exchange(-1.0, std::memory_order_acq_rel);
        if (sk >= 0.0) {
            const int64_t t = std::clamp<int64_t>(std::llround(sk * sr), 0, static_cast<int64_t>(cur_->frames));
            if (running_ && run_ > 0.0f) jumpTo(t);
            else {
                pos_ = t;
                xf_left_ = 0;
                if (stretching_ && stretch_.ok()) stretch_.reset(); // drop audio buffered before the jump
            }
            pos_pub_.store(double(t) / sr, std::memory_order_relaxed);
        }
    }

    // 3. play state + count-in
    const bool want = want_play_.load(std::memory_order_relaxed);
    if (countin_req_.load(std::memory_order_relaxed)) {
        const int beats = countin_req_.exchange(0, std::memory_order_acq_rel);
        if (beats > 0 && want) {
            const double bpm = std::clamp(countin_bpm_.load(std::memory_order_relaxed), 30.0, 300.0);
            beat_len_ = std::max<int64_t>(1, std::llround(60.0 / bpm * sr));
            countin_total_ = countin_left_ = beats * beat_len_;
            click_phase_ = 0.0;
        }
    }
    if (!want) countin_left_ = 0;
    uint32_t start = 0;
    if (countin_left_ > 0 && !running_) {
        for (; start < n && countin_left_ > 0; ++start, --countin_left_) {
            const int64_t el = countin_total_ - countin_left_;
            const int64_t in_beat = el % beat_len_;
            const double t = double(in_beat) / sr;
            if (t < 0.05) {
                const double f = el < beat_len_ ? 1760.0 : 1320.0; // accent the first beat
                const float env = static_cast<float>(std::exp(-t * 85.0) * (1.0 - std::exp(-t * 4000.0)));
                const float s = 0.3f * env * static_cast<float>(std::sin(2.0 * std::numbers::pi * f * t));
                out_l[start] = softGuard(out_l[start] + s);
                out_r[start] = softGuard(out_r[start] + s);
            }
        }
        countin_left_beats_.store(countin_left_ > 0 ? static_cast<int>((countin_left_ + beat_len_ - 1) / beat_len_) : 0, std::memory_order_relaxed);
        if (start >= n) return;
    } else {
        countin_left_beats_.store(0, std::memory_order_relaxed);
    }

    // 4. the track
    const bool target = want && countin_left_ == 0;
    if (!running_ && !target) {
        level_.store(level_.load(std::memory_order_relaxed) * 0.8f, std::memory_order_relaxed);
        return;
    }
    running_ = true;
    float* out_lp = out_l + start;
    float* out_rp = out_r + start;
    const uint32_t m = n - start;
    const float speed = speed_.load(std::memory_order_relaxed);
    const float semis = semitones_.load(std::memory_order_relaxed);
    const bool stretch = stretch_.ok() && (std::fabs(speed - 1.0f) > 0.002f || std::fabs(semis) > 0.01f);
    if (stretch && !stretching_) {
        stretch_.reset();
        stretching_ = true;
    } else if (!stretch && stretching_) {
        // Back to the direct read: rewind by what the unit was still holding so nothing is skipped.
        pos_ = std::max<int64_t>(0, pos_ - std::llround(stretch_latency_sec_ * sr * last_speed_));
        stretching_ = false;
    }
    float* xl = scratch_l_.data();
    float* xr = scratch_r_.data();
    if (stretching_) {
        stretch_.setParams(speed, semis);
        last_speed_ = speed;
        stretch_.render(xl, xr, m);
    } else {
        pull(xl, xr, m);
    }

    // gains: play/pause fade, volume × normalisation × duck (all smoothed per sample)
    const float run_step = static_cast<float>(1.0 / (0.012 * sr));
    const float g_smooth = static_cast<float>(1.0 - std::exp(-1.0 / (0.010 * sr)));
    const bool duck = duck_on_.load(std::memory_order_relaxed);
    const float depth = std::pow(10.0f, duck_depth_db_.load(std::memory_order_relaxed) / 20.0f);
    const float env_rel = static_cast<float>(std::exp(-1.0 / (0.060 * sr)));
    const float duck_att = static_cast<float>(1.0 - std::exp(-1.0 / (0.025 * sr)));
    const float duck_rel = static_cast<float>(1.0 - std::exp(-1.0 / (0.350 * sr)));
    const int hold = static_cast<int>(0.30 * sr);
    constexpr float kDuckThr = 0.012f; // ≈ −38 dBFS at the input: a played note, not the interface's hiss
    const float base = volume_.load(std::memory_order_relaxed) * (normalize_.load(std::memory_order_relaxed) ? cur_->norm_gain : 1.0f);
    double ss = 0.0;
    for (uint32_t i = 0; i < m; ++i) {
        run_ = target ? std::min(1.0f, run_ + run_step) : std::max(0.0f, run_ - run_step);
        float dtarget = 1.0f;
        if (duck) {
            const uint32_t k = start + i;
            const float x = std::max(side_l ? std::fabs(side_l[k]) : 0.0f, side_r ? std::fabs(side_r[k]) : 0.0f);
            duck_env_ = std::max(x, duck_env_ * env_rel);
            if (duck_env_ > kDuckThr) duck_hold_ = hold;
            else if (duck_hold_ > 0) --duck_hold_;
            dtarget = duck_hold_ > 0 ? depth : 1.0f;
        }
        duck_gain_ += (dtarget - duck_gain_) * (dtarget < duck_gain_ ? duck_att : duck_rel);
        gain_ += (base * duck_gain_ - gain_) * g_smooth;
        const float g = gain_ * run_;
        const float yl = xl[i] * g, yr = xr[i] * g;
        ss += 0.5 * (double(yl) * yl + double(yr) * yr);
        out_lp[i] = softGuard(out_lp[i] + yl);
        out_rp[i] = softGuard(out_rp[i] + yr);
    }
    if (!target && run_ <= 0.0f) running_ = false;
    const float lv = m ? static_cast<float>(std::sqrt(ss / m)) : 0.0f;
    level_.store(0.7f * level_.load(std::memory_order_relaxed) + 0.3f * lv, std::memory_order_relaxed);
    duck_now_.store(depth < 1.0f ? std::clamp((1.0f - duck_gain_) / (1.0f - depth), 0.0f, 1.0f) : 0.0f, std::memory_order_relaxed);
    const double lag = stretching_ ? stretch_latency_sec_ * last_speed_ : 0.0;
    pos_pub_.store(std::clamp(double(pos_) / sr - lag, 0.0, cur_->seconds()), std::memory_order_relaxed);
}

} // namespace rig::audio
