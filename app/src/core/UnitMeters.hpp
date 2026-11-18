#pragma once
// Per-unit live meters for the illustrated hardware faces (VU needles, LED ladders, GR meters, spectrum
// screens, tuner readouts). See app/docs/ENGINE_API.md → "Per-unit meters".
//
// Audio thread (RackGraph::processNode, only while armed): one pass over each unit's input and output to get
// the block's peak and mean square, folded into single-writer envelopes and published as relaxed atomics, plus
// the unit's own readout (gain reduction, LFO phase). Units that the UI asks for a spectrum, waveform or pitch
// also copy their mono output into one of a few preallocated rings. No locks, no allocation.
//
// HTTP thread (/api/telemetry?units=…): arms metering for ~1.5 s per poll (so nothing runs while no UI polls),
// reads the atomics, and does the analysis (FFT bands, pitch, waveform) on its own time.
#include "IComponent.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <numbers>
#include <string>
#include <vector>

namespace rig {

class UnitMeterBank {
public:
    static constexpr int kSlots = 128;
    static constexpr int kRings = 4;
    static constexpr uint32_t kRingSize = 8192; // power of two
    static constexpr int kBands = 28;

    // ---------- HTTP thread ----------
    void arm(double seconds = 1.5) noexcept {
        const double sr = std::max(8000.0, static_cast<double>(sample_rate_.load(std::memory_order_relaxed)));
        arm_until_.store(frames_.load(std::memory_order_relaxed) + static_cast<uint64_t>(seconds * sr), std::memory_order_relaxed);
    }

    // ---------- audio thread ----------
    // Once per rendered block. Returns whether units should be metered this block.
    bool beginBlock(uint32_t num_frames, double sample_rate) noexcept {
        const uint64_t f = frames_.load(std::memory_order_relaxed) + num_frames;
        frames_.store(f, std::memory_order_relaxed);
        if (sample_rate_.load(std::memory_order_relaxed) != static_cast<float>(sample_rate))
            sample_rate_.store(static_cast<float>(sample_rate), std::memory_order_relaxed);
        return f < arm_until_.load(std::memory_order_relaxed);
    }

    // Sub-block form of record() (series pipelining): accumulate() each sub-block in order, then commit() once for the
    // whole block. The sums run in the same sample order as record()'s, so the result is bit for bit the same.
    struct Acc { float ie = 0.0f, oe = 0.0f, ip = 0.0f, op = 0.0f; };
    void accumulate(std::size_t slot, Acc& a, const float* in_l, const float* in_r, const float* out_l, const float* out_r, uint32_t n) noexcept {
        if (slot >= static_cast<std::size_t>(kSlots) || n == 0) return;
        for (uint32_t i = 0; i < n; ++i) {
            const float x = in_l[i], b = in_r[i], c = out_l[i], d = out_r[i];
            a.ie += x * x + b * b;
            a.oe += c * c + d * d;
            a.ip = std::max(a.ip, std::max(std::abs(x), std::abs(b)));
            a.op = std::max(a.op, std::max(std::abs(c), std::abs(d)));
        }
        for (auto& r : rings_) {
            if (r.slot.load(std::memory_order_relaxed) != static_cast<int>(slot)) continue;
            uint32_t w = r.w.load(std::memory_order_relaxed);
            for (uint32_t i = 0; i < n; ++i) r.buf[(w + i) & (kRingSize - 1)] = 0.5f * (out_l[i] + out_r[i]);
            r.w.store(w + n, std::memory_order_release);
        }
    }
    void commit(std::size_t slot, const Acc& a, uint32_t n, const UnitLiveReadout& ro) noexcept {
        if (slot >= static_cast<std::size_t>(kSlots) || n == 0) return;
        Slot& s = slots_[slot];
        const float sr = std::max(8000.0f, sample_rate_.load(std::memory_order_relaxed));
        const float k_rms = 1.0f - std::exp(-static_cast<float>(n) / (0.060f * sr));
        const float k_pk = std::exp(-static_cast<float>(n) / (0.300f * sr));
        const float inv = 0.5f / static_cast<float>(n);
        s.in_ms += k_rms * (a.ie * inv - s.in_ms);
        s.out_ms += k_rms * (a.oe * inv - s.out_ms);
        s.in_pk = std::max(a.ip, s.in_pk * k_pk);
        s.out_pk = std::max(a.op, s.out_pk * k_pk);
        s.in_rms_a.store(std::sqrt(std::max(0.0f, s.in_ms)), std::memory_order_relaxed);
        s.out_rms_a.store(std::sqrt(std::max(0.0f, s.out_ms)), std::memory_order_relaxed);
        s.in_pk_a.store(s.in_pk, std::memory_order_relaxed);
        s.out_pk_a.store(s.out_pk, std::memory_order_relaxed);
        s.gr_a.store(ro.gain_reduction_db, std::memory_order_relaxed);
        s.lfo_a.store(ro.lfo_phase, std::memory_order_relaxed);
        s.lfo2_a.store(ro.lfo2_phase, std::memory_order_relaxed);
        s.stamp.store(frames_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }

    void record(std::size_t slot, const float* in_l, const float* in_r, const float* out_l, const float* out_r,
                uint32_t n, const UnitLiveReadout& ro) noexcept {
        if (slot >= static_cast<std::size_t>(kSlots) || n == 0) return;
        Slot& s = slots_[slot];
        float ie = 0.0f, oe = 0.0f, ip = 0.0f, op = 0.0f;
        for (uint32_t i = 0; i < n; ++i) {
            const float a = in_l[i], b = in_r[i], c = out_l[i], d = out_r[i];
            ie += a * a + b * b;
            oe += c * c + d * d;
            ip = std::max(ip, std::max(std::abs(a), std::abs(b)));
            op = std::max(op, std::max(std::abs(c), std::abs(d)));
        }
        const float sr = std::max(8000.0f, sample_rate_.load(std::memory_order_relaxed));
        const float k_rms = 1.0f - std::exp(-static_cast<float>(n) / (0.060f * sr)); // ~60 ms mean square
        const float k_pk = std::exp(-static_cast<float>(n) / (0.300f * sr));         // peak: instant rise, 300 ms fall
        const float inv = 0.5f / static_cast<float>(n);
        s.in_ms += k_rms * (ie * inv - s.in_ms);
        s.out_ms += k_rms * (oe * inv - s.out_ms);
        s.in_pk = std::max(ip, s.in_pk * k_pk);
        s.out_pk = std::max(op, s.out_pk * k_pk);
        s.in_rms_a.store(std::sqrt(std::max(0.0f, s.in_ms)), std::memory_order_relaxed);
        s.out_rms_a.store(std::sqrt(std::max(0.0f, s.out_ms)), std::memory_order_relaxed);
        s.in_pk_a.store(s.in_pk, std::memory_order_relaxed);
        s.out_pk_a.store(s.out_pk, std::memory_order_relaxed);
        s.gr_a.store(ro.gain_reduction_db, std::memory_order_relaxed);
        s.lfo_a.store(ro.lfo_phase, std::memory_order_relaxed);
        s.lfo2_a.store(ro.lfo2_phase, std::memory_order_relaxed);
        s.stamp.store(frames_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        for (auto& r : rings_) {
            if (r.slot.load(std::memory_order_relaxed) != static_cast<int>(slot)) continue;
            uint32_t w = r.w.load(std::memory_order_relaxed);
            for (uint32_t i = 0; i < n; ++i) r.buf[(w + i) & (kRingSize - 1)] = 0.5f * (out_l[i] + out_r[i]);
            r.w.store(w + n, std::memory_order_release);
        }
    }

    // A bypassed (or silent) unit: its meters fall to rest.
    void recordIdle(std::size_t slot) noexcept {
        if (slot >= static_cast<std::size_t>(kSlots)) return;
        Slot& s = slots_[slot];
        s.in_ms = s.out_ms = s.in_pk = s.out_pk = 0.0f;
        s.in_rms_a.store(0.0f, std::memory_order_relaxed);
        s.out_rms_a.store(0.0f, std::memory_order_relaxed);
        s.in_pk_a.store(0.0f, std::memory_order_relaxed);
        s.out_pk_a.store(0.0f, std::memory_order_relaxed);
        s.gr_a.store(-1.0f, std::memory_order_relaxed);
        s.stamp.store(frames_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }

    // ---------- HTTP thread: JSON ----------
    // query: units=0,3,7 (slots to report) & spec=3 (28-band output spectrum) & tune=5 (pitch) & wave=2 (64-point
    // output waveform). At most kRings units can have spec/tune/wave at once.
    std::string json(const std::vector<int>& units, const std::vector<int>& spec, const std::vector<int>& tune,
                     const std::vector<int>& wave) {
        arm();
        assignRings(spec, tune, wave);
        const float sr = std::max(8000.0f, sample_rate_.load(std::memory_order_relaxed));
        const uint64_t now = frames_.load(std::memory_order_relaxed);
        const uint64_t stale = static_cast<uint64_t>(0.5 * sr);
        std::string o = "[";
        char b[96];
        auto db = [](float v) { return v > 1e-6f ? std::max(-120.0f, 20.0f * std::log10(v)) : -120.0f; };
        auto has = [](const std::vector<int>& v, int x) { return std::find(v.begin(), v.end(), x) != v.end(); };
        bool first = true;
        for (int u : units) {
            if (u < 0 || u >= kSlots) continue;
            const Slot& s = slots_[u];
            const uint64_t st = s.stamp.load(std::memory_order_relaxed);
            const bool live = st != 0 && now - st < stale;
            if (!first) o += ',';
            first = false;
            std::snprintf(b, sizeof(b), "{\"s\":%d", u);
            o += b;
            if (!live) { o += ",\"idle\":1}"; continue; }
            std::snprintf(b, sizeof(b), ",\"ir\":%.1f,\"ip\":%.1f,\"or\":%.1f,\"op\":%.1f",
                          db(s.in_rms_a.load(std::memory_order_relaxed)), db(s.in_pk_a.load(std::memory_order_relaxed)),
                          db(s.out_rms_a.load(std::memory_order_relaxed)), db(s.out_pk_a.load(std::memory_order_relaxed)));
            o += b;
            const float gr = s.gr_a.load(std::memory_order_relaxed);
            if (gr >= 0.0f) { std::snprintf(b, sizeof(b), ",\"gr\":%.1f", std::min(60.0f, gr)); o += b; }
            const float lfo = s.lfo_a.load(std::memory_order_relaxed);
            if (lfo >= 0.0f) { std::snprintf(b, sizeof(b), ",\"lfo\":%.3f", lfo); o += b; }
            else if (lfo < -1.5f) o += ",\"lfo\":-1"; // the unit's oscillator is not running (its lamp rests)
            const float lfo2 = s.lfo2_a.load(std::memory_order_relaxed);
            if (lfo2 >= 0.0f) { std::snprintf(b, sizeof(b), ",\"lfo2\":%.3f", lfo2); o += b; }
            else if (lfo2 < -1.5f) o += ",\"lfo2\":-1";
            const bool sp = has(spec, u), tn = has(tune, u), wv = has(wave, u);
            if (sp || tn || wv) {
                std::vector<float> x(kAnalysis);
                if (readRing(u, x.data(), kAnalysis)) {
                    if (sp) o += ",\"sp\":" + spectrumJson(x, sr);
                    if (tn) { std::snprintf(b, sizeof(b), ",\"hz\":%.2f", pitchHz(std::vector<float>(x.end() - 2048, x.end()), sr)); o += b; }
                    if (wv) o += ",\"wv\":" + waveJson(x);
                }
            }
            o += '}';
        }
        o += ']';
        return o;
    }

    static std::vector<int> parseList(const std::string& v) {
        std::vector<int> out;
        int cur = -1;
        for (char c : v) {
            if (c >= '0' && c <= '9') cur = (cur < 0 ? 0 : cur * 10) + (c - '0');
            else { if (cur >= 0 && out.size() < kSlots) out.push_back(cur); cur = -1; }
        }
        if (cur >= 0 && out.size() < kSlots) out.push_back(cur);
        return out;
    }

    // Band centres used by "sp" (Hz), for documentation and the UI.
    static double bandHz(int i) noexcept { return 40.0 * std::pow(16000.0 / 40.0, (i + 0.5) / kBands); }

private:
    static constexpr uint32_t kAnalysis = 4096; // 11.7 Hz bins at 48 kHz

    struct Slot {
        std::atomic<float> in_rms_a{0.0f}, in_pk_a{0.0f}, out_rms_a{0.0f}, out_pk_a{0.0f}, gr_a{-1.0f}, lfo_a{-1.0f}, lfo2_a{-1.0f};
        std::atomic<uint64_t> stamp{0};
        float in_ms = 0.0f, out_ms = 0.0f, in_pk = 0.0f, out_pk = 0.0f; // audio thread only
    };
    struct Ring {
        std::atomic<int> slot{-1};
        std::atomic<uint32_t> w{0};
        std::atomic<uint64_t> since{0};
        std::array<float, kRingSize> buf{};
    };

    void assignRings(const std::vector<int>& a, const std::vector<int>& b, const std::vector<int>& c) {
        std::vector<int> want;
        for (const auto* v : {&a, &b, &c})
            for (int x : *v)
                if (x >= 0 && x < kSlots && std::find(want.begin(), want.end(), x) == want.end() && want.size() < kRings) want.push_back(x);
        for (auto& r : rings_) {
            const int s = r.slot.load(std::memory_order_relaxed);
            if (s >= 0 && std::find(want.begin(), want.end(), s) == want.end()) r.slot.store(-1, std::memory_order_relaxed);
        }
        for (int x : want) {
            bool have = false;
            for (auto& r : rings_) have = have || r.slot.load(std::memory_order_relaxed) == x;
            if (have) continue;
            for (auto& r : rings_) {
                if (r.slot.load(std::memory_order_relaxed) != -1) continue;
                r.buf.fill(0.0f);
                r.since.store(r.w.load(std::memory_order_relaxed), std::memory_order_relaxed);
                r.slot.store(x, std::memory_order_release);
                break;
            }
        }
    }

    bool readRing(int slot, float* dst, uint32_t n) const {
        for (const auto& r : rings_) {
            if (r.slot.load(std::memory_order_acquire) != slot) continue;
            const uint32_t w = r.w.load(std::memory_order_acquire);
            if (w - static_cast<uint32_t>(r.since.load(std::memory_order_relaxed)) < 256) return false; // nothing yet
            for (uint32_t i = 0; i < n; ++i) dst[i] = r.buf[(w - n + i) & (kRingSize - 1)];
            return true;
        }
        return false;
    }

    static void fft(std::vector<std::complex<float>>& a) {
        const std::size_t n = a.size();
        for (std::size_t i = 1, j = 0; i < n; ++i) {
            std::size_t bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(a[i], a[j]);
        }
        for (std::size_t len = 2; len <= n; len <<= 1) {
            const float ang = -2.0f * std::numbers::pi_v<float> / static_cast<float>(len);
            const std::complex<float> wl(std::cos(ang), std::sin(ang));
            for (std::size_t i = 0; i < n; i += len) {
                std::complex<float> w(1.0f, 0.0f);
                for (std::size_t j = 0; j < len / 2; ++j) {
                    const auto u = a[i + j], v = a[i + j + len / 2] * w;
                    a[i + j] = u + v;
                    a[i + j + len / 2] = u - v;
                    w *= wl;
                }
            }
        }
    }

    // 28 log-spaced bands, 40 Hz – 16 kHz, dBFS (a full-scale sine reads 0 dB in its band).
    static std::string spectrumJson(const std::vector<float>& x, float sr) {
        const std::size_t n = x.size();
        std::vector<std::complex<float>> a(n);
        for (std::size_t i = 0; i < n; ++i) {
            const float w = 0.5f - 0.5f * std::cos(2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(n - 1));
            a[i] = {x[i] * w, 0.0f};
        }
        fft(a);
        const float norm = 4.0f / static_cast<float>(n);
        std::string o = "[";
        char b[16];
        for (int k = 0; k < kBands; ++k) {
            const double lo = 40.0 * std::pow(400.0, static_cast<double>(k) / kBands), hi = 40.0 * std::pow(400.0, (k + 1.0) / kBands);
            std::size_t b0 = static_cast<std::size_t>(std::floor(lo * n / sr)), b1 = static_cast<std::size_t>(std::ceil(hi * n / sr));
            b0 = std::clamp<std::size_t>(b0, 1, n / 2 - 1);
            b1 = std::clamp<std::size_t>(b1, b0 + 1, n / 2);
            double e = 0.0;
            for (std::size_t i = b0; i < b1; ++i) e += std::norm(a[i] * norm);
            const double d = e > 1e-14 ? std::max(-120.0, 10.0 * std::log10(e)) : -120.0;
            std::snprintf(b, sizeof(b), "%s%d", k ? "," : "", static_cast<int>(std::lround(d)));
            o += b;
        }
        return o + "]";
    }

    // McLeod pitch method over the last 2048 samples (the caller passes them) (≈47 Hz – 1 kHz). 0 = no clear pitch.
    static double pitchHz(const std::vector<float>& x, float sr) {
        const std::size_t n = x.size();
        double energy = 0.0;
        for (float v : x) energy += static_cast<double>(v) * v;
        if (energy / static_cast<double>(n) < 1e-7) return 0.0;
        const std::size_t min_lag = static_cast<std::size_t>(sr / 1000.0f), max_lag = std::min<std::size_t>(n / 2, static_cast<std::size_t>(sr / 45.0f));
        std::vector<double> nsdf(max_lag + 2, 0.0);
        for (std::size_t tau = min_lag; tau <= max_lag + 1 && tau < n; ++tau) {
            double r = 0.0, m = 0.0;
            for (std::size_t i = 0; i + tau < n; ++i) {
                r += static_cast<double>(x[i]) * x[i + tau];
                m += static_cast<double>(x[i]) * x[i] + static_cast<double>(x[i + tau]) * x[i + tau];
            }
            nsdf[tau] = m > 0.0 ? 2.0 * r / m : 0.0;
        }
        double best = 0.0;
        for (std::size_t t = min_lag; t <= max_lag; ++t) best = std::max(best, nsdf[t]);
        if (best < 0.6) return 0.0;
        for (std::size_t t = min_lag + 1; t <= max_lag; ++t) {
            if (nsdf[t] >= 0.9 * best && nsdf[t] >= nsdf[t - 1] && nsdf[t] >= nsdf[t + 1]) {
                const double a = nsdf[t - 1], b = nsdf[t], c = nsdf[t + 1];
                const double den = a - 2.0 * b + c;
                const double shift = std::abs(den) > 1e-12 ? 0.5 * (a - c) / den : 0.0;
                return sr / (static_cast<double>(t) + std::clamp(shift, -0.5, 0.5));
            }
        }
        return 0.0;
    }

    // 64 points of the output, triggered on a rising zero crossing so the trace stands still.
    static std::string waveJson(const std::vector<float>& x) {
        const std::size_t n = x.size(), span = 512;
        std::size_t start = n - span;
        for (std::size_t i = n - span; i > n / 2; --i)
            if (x[i - 1] < 0.0f && x[i] >= 0.0f) { start = i; break; }
        start = std::min(start, n - span);
        std::string o = "[";
        char b[16];
        for (int k = 0; k < 64; ++k) {
            std::snprintf(b, sizeof(b), "%s%.3f", k ? "," : "", std::clamp(x[start + k * 8], -1.0f, 1.0f));
            o += b;
        }
        return o + "]";
    }

    std::array<Slot, kSlots> slots_{};
    std::array<Ring, kRings> rings_{};
    std::atomic<uint64_t> frames_{0};
    std::atomic<uint64_t> arm_until_{0};
    std::atomic<float> sample_rate_{48000.0f};
};

} // namespace rig
