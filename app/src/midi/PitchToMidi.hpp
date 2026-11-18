#pragma once
// Guitar → MIDI note detection (independent implementation).
//
//  * Mono mode: YIN (de Cheveigné & Kawahara 2002) on a 16 kHz analysis signal, 4 ms hop, with
//    continuous pitch-bend tracking and legato note switching for bends beyond the bend range.
//  * Poly mode: multi-resolution FFT harmonic salience with iterative estimation & cancellation and
//    spectral-smoothness partial sharing (after Klapuri 2006), spectral-flux onsets, per-note
//    hysteresis tracking, guitar polyphony limit (6).
//
// Both modes emit sample-accurate note on/off and pitch-bend events at the host rate.
#include "MidiEvent.hpp"
#include "../platform/VDsp.hpp" // Accelerate on macOS, portable vDSP subset elsewhere
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <functional>
#include <vector>

namespace rig::midi {


struct DetectorSettings {
    bool poly = true;
    float sensitivity = 0.5f; // 0..1 (higher = picks up quieter notes)
    int bend_range = 2;       // semitones
    int low_note = 40;        // E2
    int high_note = 88;       // E6
    float velocity_curve = 0.5f;
};

class PitchToMidi {
public:
    void prepare(double host_sr) {
        host_sr_ = host_sr;
        decim_ = std::max(1, static_cast<int>(std::lround(host_sr / 16000.0)));
        fa_ = host_sr / decim_;
        // 4th-order Butterworth anti-alias at 0.42 * fa/2... (two biquads)
        designLowpass(std::min(6500.0, 0.42 * fa_));
        hist_.assign(kHist, 0.0f);
        hpos_ = 0;
        dec_phase_ = 0;
        hop_count_ = 0;
        // FFT setup
        fft_ = vDSP_create_fftsetup(kLog2Pad, kFFTRadix2);
        win_long_.resize(kLong);
        win_short_.resize(kShort);
        for (int i = 0; i < kLong; ++i) win_long_[static_cast<std::size_t>(i)] = 0.5f - 0.5f * std::cos(2.0f * static_cast<float>(M_PI) * (i + 0.5f) / kLong);
        for (int i = 0; i < kShort; ++i) win_short_[static_cast<std::size_t>(i)] = 0.5f - 0.5f * std::cos(2.0f * static_cast<float>(M_PI) * (i + 0.5f) / kShort);
        spec_long_.assign(kPad / 2, 0.0f);
        spec_short_.assign(kPad / 2, 0.0f);
        prev_logspec_.assign(kPad / 2, 0.0f);
        for (auto& n : notes_) n = NoteState{};
        smooth_.fill(0.0f);
        mono_note_ = -1;
        level_ = 0.0f;
        flux_avg_ = 0.0f;
        flux_prev_ = 0.0f;
        flux_prev2_ = 0.0f;
        yin_buf_.assign(kYinW, 0.0f);
    }

    void setSettings(const DetectorSettings& s) { set_ = s; }
    const DetectorSettings& settings() const { return set_; }

    // Feeds `n` host-rate samples; appends events (offsets relative to this call) to `out`.
    void process(const float* x, uint32_t n, std::vector<MidiEvent>& out) {
        for (uint32_t i = 0; i < n; ++i) {
            float v = x[i];
            for (auto& bq : aa_) v = bq.process(v);
            if (++dec_phase_ < decim_) continue;
            dec_phase_ = 0;
            hist_[hpos_] = v;
            hpos_ = (hpos_ + 1) % kHist;
            const float a = std::abs(v);
            level_ += (a - level_) * (a > level_ ? 0.05f : 0.0015f);
            if (++hop_count_ >= kHop) {
                hop_count_ = 0;
                analyse(i, out);
            }
        }
    }

    // For UI telemetry.
    float pitchHz() const { return pitch_hz_; }
    float confidence() const { return confidence_; }
    float level() const { return level_; }
    void activeNotes(std::vector<std::pair<int, float>>& out) const {
        out.clear();
        if (set_.poly) {
            for (int k = 0; k < 128; ++k)
                if (notes_[static_cast<std::size_t>(k)].on) out.emplace_back(k, notes_[static_cast<std::size_t>(k)].vel);
        } else if (mono_note_ >= 0) {
            out.emplace_back(mono_note_, mono_vel_);
        }
    }

    // Releases everything (e.g. when switching modes).
    void allNotesOff(std::vector<MidiEvent>& out) {
        for (int k = 0; k < 128; ++k) {
            if (notes_[static_cast<std::size_t>(k)].on) {
                out.push_back({MidiEvent::NoteOff, k, 0.0f, 0.0f, 0});
                notes_[static_cast<std::size_t>(k)].on = false;
            }
        }
        if (mono_note_ >= 0) {
            out.push_back({MidiEvent::NoteOff, mono_note_, 0.0f, 0.0f, 0});
            mono_note_ = -1;
        }
    }

private:
    static constexpr int kHist = 4096;   // analysis-rate history (≈ 256 ms)
    static constexpr int kHop = 64;      // 4 ms at 16 kHz
    static constexpr int kLong = 736;    // 46 ms window (low register)
    static constexpr int kShort = 368;   // 23 ms window (mid/high register, onsets)
    static constexpr int kLog2Pad = 13;
    static constexpr int kPad = 8192;    // zero-padded FFT (≈ 2 Hz bins at 16 kHz)
    static constexpr int kYinW = 840;    // YIN buffer (max); the window used follows the Lowest Note setting

    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        float process(float xf) {
            const double x = xf, y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return static_cast<float>(y);
        }
    };
    struct NoteState {
        bool on = false;
        float vel = 0.0f;
        float peak = 0.0f;
        int below = 0;
        int above = 0;
        int age = 0;
        int refractory = 0;
        float slow = 0.0f; // slow activation average (re-pluck reference)
    };

    void designLowpass(double fc) {
        const double qs[2] = {0.5411961, 1.3065630};
        for (int k = 0; k < 2; ++k) {
            const double w0 = 2.0 * M_PI * fc / host_sr_, cs = std::cos(w0), al = std::sin(w0) / (2.0 * qs[k]);
            const double a0 = 1.0 + al;
            aa_[k].b0 = (1.0 - cs) * 0.5 / a0;
            aa_[k].b1 = (1.0 - cs) / a0;
            aa_[k].b2 = (1.0 - cs) * 0.5 / a0;
            aa_[k].a1 = -2.0 * cs / a0;
            aa_[k].a2 = (1.0 - al) / a0;
        }
    }

    float sample(int back) const { // back = 0 → newest
        return hist_[static_cast<std::size_t>((hpos_ - 1 - back + kHist * 4) % kHist)];
    }

    void spectrum(int win_len, const std::vector<float>& win, std::vector<float>& mag) {
        std::array<float, kPad> buf{};
        for (int i = 0; i < win_len; ++i) buf[static_cast<std::size_t>(i)] = sample(win_len - 1 - i) * win[static_cast<std::size_t>(i)];
        alignas(vdsp::kFftAlign) std::array<float, kPad / 2> re{}, im{}; // else Accelerate's result depends on the address
        DSPSplitComplex sc{re.data(), im.data()};
        vDSP_ctoz(reinterpret_cast<const DSPComplex*>(buf.data()), 2, &sc, 1, kPad / 2);
        vDSP_fft_zrip(fft_, &sc, 1, kLog2Pad, kFFTDirection_Forward);
        const float norm = 2.0f / static_cast<float>(win_len);
        for (int k = 1; k < kPad / 2; ++k)
            mag[static_cast<std::size_t>(k)] = std::sqrt(re[static_cast<std::size_t>(k)] * re[static_cast<std::size_t>(k)] + im[static_cast<std::size_t>(k)] * im[static_cast<std::size_t>(k)]) * norm * 0.5f;
        mag[0] = 0.0f;
    }

    void analyse(uint32_t host_index, std::vector<MidiEvent>& out) {
        const uint32_t off = host_index;
        const float gate = 0.0006f + 0.006f * (1.0f - set_.sensitivity) * (1.0f - set_.sensitivity);
        if (!set_.poly) {
            analyseMono(off, gate, out);
            return;
        }
        analysePoly(off, gate, out);
    }

    // ───────────── Mono (YIN) ─────────────
    void analyseMono(uint32_t off, float gate, std::vector<MidiEvent>& out) {
        // Integration window = longest lag searched, sized from the lowest note we need to resolve: standard
        // tuning keeps the short (low-latency) window; drop tunings / bass ranges get a longer one.
        const double low_hz = std::max(30.0, 440.0 * std::pow(2.0, (set_.low_note - 69) / 12.0));
        const int W = std::clamp(static_cast<int>(fa_ / (low_hz * 0.92)), 210, kYinW / 2 - 2);
        for (int i = 0; i < 2 * W; ++i) yin_buf_[static_cast<std::size_t>(i)] = sample(2 * W - 1 - i);
        const int tau_min = static_cast<int>(fa_ / 1400.0), tau_max = W - 2;
        std::array<float, kYinW / 2> d{};
        float cum = 0.0f;
        int best = -1;
        d[0] = 1.0f;
        for (int tau = 1; tau <= tau_max; ++tau) {
            float acc = 0.0f;
            vDSP_distancesq(yin_buf_.data(), 1, yin_buf_.data() + tau, 1, &acc, static_cast<vDSP_Length>(W));
            cum += acc;
            d[static_cast<std::size_t>(tau)] = (cum > 1e-12f) ? acc * static_cast<float>(tau) / cum : 1.0f;
        }
        constexpr float kThr = 0.15f;
        for (int tau = tau_min; tau < tau_max; ++tau) {
            if (d[static_cast<std::size_t>(tau)] < kThr) {
                while (tau + 1 < tau_max && d[static_cast<std::size_t>(tau) + 1] < d[static_cast<std::size_t>(tau)]) ++tau;
                best = tau;
                break;
            }
        }
        float conf = 0.0f, f0 = 0.0f;
        if (best > 0) {
            const float a = d[static_cast<std::size_t>(best) - 1], b = d[static_cast<std::size_t>(best)], c = d[static_cast<std::size_t>(best) + 1];
            const float den = a - 2.0f * b + c;
            const float shift = (std::abs(den) > 1e-9f) ? 0.5f * (a - c) / den : 0.0f;
            f0 = static_cast<float>(fa_) / (static_cast<float>(best) + std::clamp(shift, -0.5f, 0.5f));
            conf = std::clamp(1.0f - b, 0.0f, 1.0f);
        }
        const bool voiced = conf > 0.80f && level_ > gate && f0 > std::min(60.0f, static_cast<float>(low_hz) * 0.9f);
        confidence_ = conf;
        pitch_hz_ = voiced ? f0 : 0.0f;
        if (voiced) {
            const float semis = 69.0f + 12.0f * std::log2(f0 / 440.0f);
            if (semis < set_.low_note - 0.5f || semis > set_.high_note + 0.5f) return;
            if (mono_note_ < 0) {
                if (++mono_stable_ >= 2) {
                    mono_note_ = static_cast<int>(std::lround(semis));
                    mono_vel_ = velocityFromLevel();
                    out.push_back({MidiEvent::NoteOn, mono_note_, mono_vel_, 0.0f, off});
                    mono_off_count_ = 0;
                }
            } else {
                const float bend = semis - static_cast<float>(mono_note_);
                const bool new_attack = onsetMono();
                if (std::abs(bend) > static_cast<float>(set_.bend_range) - 0.25f || (new_attack && std::abs(bend) > 0.6f)) {
                    out.push_back({MidiEvent::NoteOff, mono_note_, 0.0f, 0.0f, off});
                    mono_note_ = static_cast<int>(std::lround(semis));
                    out.push_back({MidiEvent::PitchBend, mono_note_, 0.0f, 0.0f, off});
                    if (new_attack) mono_vel_ = velocityFromLevel();
                    out.push_back({MidiEvent::NoteOn, mono_note_, mono_vel_, 0.0f, off});
                } else {
                    out.push_back({MidiEvent::PitchBend, mono_note_, 0.0f, bend, off});
                }
                mono_off_count_ = 0;
            }
        } else {
            mono_stable_ = 0;
            if (mono_note_ >= 0 && ++mono_off_count_ > (level_ > gate ? 12 : 3)) {
                out.push_back({MidiEvent::NoteOff, mono_note_, 0.0f, 0.0f, off});
                out.push_back({MidiEvent::PitchBend, mono_note_, 0.0f, 0.0f, off});
                mono_note_ = -1;
            }
        }
    }

    bool onsetMono() {
        const bool on = level_ > 1.8f * mono_level_slow_ && level_ > 0.002f;
        mono_level_slow_ += (level_ - mono_level_slow_) * 0.05f;
        return on;
    }

    float velocityFromLevel() const {
        const float db = 20.0f * std::log10(std::max(1e-5f, level_));
        float v = std::clamp((db + 54.0f) / 42.0f, 0.0f, 1.0f);
        v = std::pow(v, 0.5f + set_.velocity_curve);
        return std::clamp(0.15f + 0.85f * v, 0.05f, 1.0f);
    }

    // ───────────── Poly (harmonic salience) ─────────────
    float binOf(float hz) const { return hz * static_cast<float>(kPad) / static_cast<float>(fa_); }

    float peakNear(const std::vector<float>& s, float bin, float width) const {
        const int lo = std::max(1, static_cast<int>(bin - width)), hi = std::min(kPad / 2 - 1, static_cast<int>(bin + width + 1));
        float m = 0.0f;
        int at = lo;
        for (int b = lo; b <= hi; ++b)
            if (s[static_cast<std::size_t>(b)] > m) { m = s[static_cast<std::size_t>(b)]; at = b; }
        last_peak_bin_ = at;
        return m;
    }

    static float tune(const char* k, float d) {
        const char* v = std::getenv(k);
        return v ? static_cast<float>(std::atof(v)) : d;
    }

    void analysePoly(uint32_t off, float gate, std::vector<MidiEvent>& out) {
        static const bool kUseNNLS = tune("P2M_NNLS", 1.0f) > 0.5f;
        static const float kOnRel = tune("P2M_ONREL", 0.30f);
        static const int kHarmHold = static_cast<int>(tune("P2M_HH", 9.0f));
        static const int kAttackHold = static_cast<int>(tune("P2M_AH", 3.0f));
        static const int kHighWait = static_cast<int>(tune("P2M_HW", 5.0f));
        static const int kOffFrames = static_cast<int>(tune("P2M_OFF", 10.0f));
        static const float kRepluck = tune("P2M_REPLUCK", 1.8f), kGhostOn = tune("P2M_GHOSTON", 0.6f);
        static const float kOffAbs = tune("P2M_OFFABS", 0.4f), kOffRel = tune("P2M_OFFREL", 0.08f);
        static const float kRel = tune("P2M_REL", 0.15f), kAbs = tune("P2M_ABS", 1.0f), kOct = tune("P2M_OCT", 0.55f), kOdd = tune("P2M_ODD", 0.35f);
        spectrum(kLong, win_long_, spec_long_);
        spectrum(kShort, win_short_, spec_short_);
        spec_long_raw_ = spec_long_;
        spec_short_raw_ = spec_short_;

        // Onset: positive log-spectral flux of the short spectrum (100 Hz – 5 kHz).
        float flux = 0.0f;
        const int b0 = static_cast<int>(binOf(100.0f)), b1 = static_cast<int>(binOf(5000.0f));
        for (int b = b0; b < b1; b += 4) {
            const float l = std::log(1e-4f + 40.0f * spec_short_[static_cast<std::size_t>(b)]);
            const float dlt = l - prev_logspec_[static_cast<std::size_t>(b)];
            if (dlt > 0.0f) flux += dlt;
            prev_logspec_[static_cast<std::size_t>(b)] = l;
        }
        flux /= static_cast<float>((b1 - b0) / 4);
        const bool onset = flux_prev_ > flux_prev2_ && flux_prev_ >= flux && flux_prev_ > std::max(0.08f, 2.2f * flux_avg_) && level_ > gate;
        flux_avg_ += (flux - flux_avg_) * 0.04f;
        flux_prev2_ = flux_prev_;
        flux_prev_ = flux;

        std::array<float, 128> sal{};
        float abs_thr = (0.010f + 0.05f * (1.0f - set_.sensitivity)) * kAbs;
        if (kUseNNLS) {
            abs_thr = nnlsSalience(sal);
        } else {
            // Compressed, whitened spectra.
            whiten(spec_long_);
            whiten(spec_short_);
            peaksOnly(spec_long_);
            peaksOnly(spec_short_);

            // Iterative salience estimation + cancellation.
            std::array<bool, 128> picked{};
            std::vector<float> resL = spec_long_, resS = spec_short_;
            float first = 0.0f;
            for (int iter = 0; iter < 6; ++iter) {
                int best = -1;
                float best_s = 0.0f;
                for (int n = set_.low_note; n <= set_.high_note; ++n) {
                    if (picked[static_cast<std::size_t>(n)]) continue;
                    const float s = salience(n, n < 52 ? resL : resS);
                    if (s > best_s) { best_s = s; best = n; }
                }
                if (best < 0 || best_s < abs_thr || (iter > 0 && best_s < kRel * first)) break;
                // Octave-error guard: if the octave (or octave+fifth) below explains the same partials and has
                // its own odd partials, the lower note is the real one.
                for (int down : {12}) {
                    const int lo = best - down;
                    if (lo < set_.low_note || picked[static_cast<std::size_t>(lo)]) continue;
                    const auto& sp = lo < 52 ? resL : resS;
                    // The lower note must show its own fundamental, not just shared partials.
                    const float flo = 440.0f * std::pow(2.0f, (lo - 69) / 12.0f), fhi = 440.0f * std::pow(2.0f, (best - 69) / 12.0f);
                    const float a_lo = peakNear(sp, binOf(flo), std::max(2.0f, binOf(flo) * 0.022f));
                    const float a_hi = peakNear(best < 52 ? resL : resS, binOf(fhi), std::max(2.0f, binOf(fhi) * 0.022f));
                    if (a_lo > 0.5f * a_hi && salience(lo, sp) > kOct * best_s && oddSupport(lo, sp) > kOdd) { best = lo; best_s = salience(lo, sp); break; }
                }
                if (iter == 0) first = best_s;
                picked[static_cast<std::size_t>(best)] = true;
                sal[static_cast<std::size_t>(best)] = best_s;
                cancel(best, resL, kLong);
                cancel(best, resS, kShort);
            }

        }

        // Guitar playability: every note needs its own string (standard tuning, frets 0–22) and the fretted
        // notes must fit one hand span. Candidates are placed strongest-first; unplayable ones are dropped.
        {
            static constexpr int kOpen[6] = {40, 45, 50, 55, 59, 64};
            std::array<int, 6> used{};
            used.fill(-1);
            std::vector<int> order;
            for (int n = 0; n < 128; ++n) if (sal[static_cast<std::size_t>(n)] > 0.0f) order.push_back(n);
            // Already-sounding notes keep priority so they aren't evicted by transient candidates.
            for (int n = 0; n < 128; ++n) if (notes_[static_cast<std::size_t>(n)].on && sal[static_cast<std::size_t>(n)] <= 0.0f) {}
            std::sort(order.begin(), order.end(), [&](int a, int b) {
                const bool ao = notes_[static_cast<std::size_t>(a)].on, bo = notes_[static_cast<std::size_t>(b)].on;
                if (ao != bo) return ao;
                return sal[static_cast<std::size_t>(a)] > sal[static_cast<std::size_t>(b)];
            });
            int fmin = 99, fmax = -1;
            for (int n : order) {
                int best_s = -1;
                for (int st = 5; st >= 0; --st) {
                    const int fret = n - kOpen[st];
                    if (fret < 0 || fret > 22 || used[static_cast<std::size_t>(st)] >= 0) continue;
                    const int nmin = fret > 0 ? std::min(fmin, fret) : fmin, nmax = fret > 0 ? std::max(fmax, fret) : fmax;
                    if (nmax >= 0 && nmin < 99 && nmax - nmin > 5) continue;
                    best_s = st;
                    break;
                }
                if (best_s < 0) {
                    sal[static_cast<std::size_t>(n)] = 0.0f;
                    continue;
                }
                const int fret = n - kOpen[best_s];
                used[static_cast<std::size_t>(best_s)] = n;
                if (fret > 0) {
                    fmin = std::min(fmin, fret);
                    fmax = std::max(fmax, fret);
                }
            }
        }

        if (std::getenv("P2M_DBG")) {
            static int frame = 0;
            ++frame;
            std::fprintf(stderr, "f%4d%s:", frame, onset ? "*" : " ");
            for (int n = 0; n < 128; ++n) if (sal[static_cast<std::size_t>(n)] > 0.0f) std::fprintf(stderr, " %d(%.3f)", n, sal[static_cast<std::size_t>(n)]);
            std::fprintf(stderr, "\n");
        }
        last_sal_ = sal;
        // Temporal integration: per-note salience is smoothed over ~10 ms so candidates that flicker
        // between frames (transients, shared partials) don't trigger, while sustained notes do.
        float sm_max = 0.0f;
        for (int n = 0; n < 128; ++n) {
            auto& v = smooth_[static_cast<std::size_t>(n)];
            v += (sal[static_cast<std::size_t>(n)] - v) * 0.45f;
            sm_max = std::max(sm_max, v);
        }
        int on_count = 0;
        for (const auto& ns : notes_) on_count += ns.on ? 1 : 0;
        if (onset) since_onset_ = 0;
        else ++since_onset_;
        const bool attack_window = since_onset_ < kAttackHold; // let fundamentals emerge before deciding
        for (int n = set_.low_note; n <= set_.high_note; ++n) {
            auto& ns = notes_[static_cast<std::size_t>(n)];
            const float v = smooth_[static_cast<std::size_t>(n)];
            if (ns.on) {
                ns.age++;
                ns.peak = std::max(ns.peak * 0.9995f, v);
                if (v < std::max(abs_thr * kOffAbs, kOffRel * ns.peak) || level_ < gate * 0.7f) ns.below++;
                else ns.below = 0;
                ns.slow += (v - ns.slow) * 0.08f;
                if (ns.below >= kOffFrames) {
                    ns.on = false;
                    ns.refractory = 250; // ≈ 1 s: a string that just stopped must be re-picked (onset) to retrigger
                    out.push_back({MidiEvent::NoteOff, n, 0.0f, 0.0f, off});
                    --on_count;
                } else if (onset && ns.age > 20 && sal[static_cast<std::size_t>(n)] > kRepluck * std::max(1e-6f, ns.slow)) { // re-pluck: this string's own energy jumped
                    out.push_back({MidiEvent::NoteOff, n, 0.0f, 0.0f, off});
                    ns.vel = velocityFromLevel();
                    ns.age = 0;
                    out.push_back({MidiEvent::NoteOn, n, ns.vel, 0.0f, off});
                }
            } else {
                if (ns.refractory > 0) {
                    --ns.refractory;
                    if (!onset) {
                        ns.above = 0;
                        continue;
                    }
                    ns.refractory = 0;
                }
                const bool strong = v > abs_thr * 1.3f && v > kOnRel * sm_max;
                ns.above = strong ? ns.above + 1 : 0;
                // Notes sitting exactly on a partial of a lower sounding/candidate note (octave, 12th, 2 oct…)
                // must prove themselves for longer: bright pick attacks otherwise read as extra notes.
                int need = (n >= 76 ? 3 : 2);
                for (int iv : {12, 19, 24, 28, 31, 34, 36}) {
                    const int m = n - iv;
                    if (m < 0) break;
                    if (notes_[static_cast<std::size_t>(m)].on || smooth_[static_cast<std::size_t>(m)] > 0.1f * v || sal[static_cast<std::size_t>(m)] > 0.0f) { need = kHarmHold; break; }
                }
                // Right after a pick attack, high candidates with nothing sounding below them are usually the
                // upper partials of a low note whose fundamental hasn't resolved yet: give the low note time.
                if (since_onset_ < 8 && n >= 59) {
                    bool lower_on = false;
                    for (int m = set_.low_note; m < n; ++m) if (notes_[static_cast<std::size_t>(m)].on) { lower_on = true; break; }
                    if (!lower_on) need = std::max(need, kHighWait);
                }
                bool ghost = false;
                for (int iv : {12, 19, 24}) {
                    const int m = n - iv;
                    if (m >= 0 && notes_[static_cast<std::size_t>(m)].on && v < kGhostOn * smooth_[static_cast<std::size_t>(m)]) { ghost = true; break; }
                }
                if (ghost) ns.above = 0;
                if (ns.above >= need && on_count < 6 && !attack_window) {
                    ns.on = true;
                    ns.vel = velocityFromLevel();
                    ns.peak = v;
                    ns.slow = v;
                    ns.below = 0;
                    ns.age = 0;
                    ns.above = 0;
                    out.push_back({MidiEvent::NoteOn, n, ns.vel, 0.0f, off});
                    ++on_count;
                }
            }
        }
        // Telemetry pitch: strongest note.
        int strongest = -1;
        float sv = 0.0f;
        for (int n = 0; n < 128; ++n)
            if (sal[static_cast<std::size_t>(n)] > sv) { sv = sal[static_cast<std::size_t>(n)]; strongest = n; }
        pitch_hz_ = strongest >= 0 ? 440.0f * std::pow(2.0f, (strongest - 69) / 12.0f) : 0.0f;
        confidence_ = std::clamp(sv / (abs_thr * 4.0f), 0.0f, 1.0f);
    }

    // ───────────── Joint estimation: sparse non-negative template fit ─────────────
    // The frame's log-frequency magnitude spectrum (1/3-semitone bins) is explained as a non-negative mix of
    // harmonic guitar-note templates (3 spectral slopes × inharmonicity per note), solved with multiplicative
    // NNLS updates warm-started from the previous frame plus an L1 sparsity term. Shared partials (octaves,
    // fifths, chords) are apportioned jointly instead of greedily.
    // Observation = [long-window bins below kSplitHz] ++ [short-window bins kSplitHz..kTopHz (every 2nd bin)],
    // straight from the zero-padded magnitude spectra. Templates are built with the exact Hann main-lobe
    // kernel of the window that observes each region, so they match what the FFT actually sees.
    static constexpr float kSplitHz = 700.0f, kTopHz = 6500.0f;
    static constexpr int kTplNotes = 89 - 38;   // templates for MIDI 38..88
    static constexpr int kSlopes = 3;
    static constexpr int kCols = kTplNotes * kSlopes;
    int obs_long_ = 0, obs_short_ = 0, obs_n_ = 0, short_b0_ = 0;

    void buildTemplates() {
        obs_long_ = static_cast<int>(binOf(kSplitHz));
        short_b0_ = obs_long_;
        obs_short_ = (static_cast<int>(binOf(kTopHz)) - short_b0_) / 2;
        obs_n_ = obs_long_ + obs_short_;
        // Window kernels (magnitude of the zero-padded transform of each Hann window, normalised to peak 1).
        auto kernel = [&](int L) {
            std::array<float, kPad> buf{};
            for (int i = 0; i < L; ++i) buf[static_cast<std::size_t>(i)] = 0.5f - 0.5f * std::cos(2.0f * static_cast<float>(M_PI) * (i + 0.5f) / L);
            alignas(vdsp::kFftAlign) std::array<float, kPad / 2> re{}, im{}; // see platform/VDsp.hpp (kFftAlign)
            DSPSplitComplex sc{re.data(), im.data()};
            vDSP_ctoz(reinterpret_cast<const DSPComplex*>(buf.data()), 2, &sc, 1, kPad / 2);
            vDSP_fft_zrip(fft_, &sc, 1, kLog2Pad, kFFTDirection_Forward);
            std::vector<float> k(64);
            const float peak = std::abs(re[0]);
            for (int d = 0; d < 64; ++d) k[static_cast<std::size_t>(d)] = std::sqrt(re[static_cast<std::size_t>(d)] * re[static_cast<std::size_t>(d)] + im[static_cast<std::size_t>(d)] * im[static_cast<std::size_t>(d)]) / std::max(1e-9f, peak);
            return k;
        };
        const auto kl = kernel(kLong), ks = kernel(kShort);
        W_.assign(static_cast<std::size_t>(obs_n_) * kCols, 0.0f);
        // Six partial-amplitude shapes per note: three spectral slopes, a weak-fundamental shape (bridge
        // pickup / high strings), a strong-2nd-harmonic shape and a strong-3rd shape — enough variety that one
        // real string can be explained by its own templates instead of recruiting octave/fifth "ghost" notes.
        auto shapeAmp = [](int k, int h) {
            const float fh = static_cast<float>(h);
            switch (k) {
                case 0: return std::pow(fh, -0.5f);
                case 1: return std::pow(fh, -1.0f);
                case 2: return std::pow(fh, -1.6f);
                case 3: return (h == 1 ? 0.25f : 1.0f) * std::pow(fh, -0.8f);
                case 4: return (h == 2 ? 1.6f : (h == 4 ? 1.2f : 1.0f)) * std::pow(fh, -0.9f);
                default: return (h == 3 ? 1.6f : (h == 6 ? 1.2f : 1.0f)) * std::pow(fh, -0.9f);
            }
        };
        for (int nn = 0; nn < kTplNotes; ++nn) {
            const float f0 = 440.0f * std::pow(2.0f, (38 + nn - 69) / 12.0f);
            for (int k = 0; k < kSlopes; ++k) {
                float* col = &W_[static_cast<std::size_t>(nn * kSlopes + k) * obs_n_];
                for (int h = 1; h <= 60; ++h) {
                    const float fh = f0 * static_cast<float>(h) * std::sqrt(1.0f + 0.00008f * static_cast<float>(h * h));
                    if (fh > kTopHz) break;
                    const float amp = shapeAmp(k, h);
                    const float bc = binOf(fh);
                    if (fh < kSplitHz) {
                        for (int d = -60; d <= 60; ++d) {
                            const int b = static_cast<int>(std::lround(bc)) + d;
                            const int dist = std::abs(static_cast<int>(std::lround(static_cast<float>(b) - bc)));
                            if (b < 1 || b >= obs_long_ || dist >= 64) continue;
                            col[b] = std::max(col[b], amp * kl[static_cast<std::size_t>(dist)]);
                        }
                    } else {
                        for (int d = -60; d <= 60; ++d) {
                            const int b = static_cast<int>(std::lround(bc)) + d;
                            const int dist = std::abs(static_cast<int>(std::lround(static_cast<float>(b) - bc)));
                            if (b < short_b0_ || dist >= 64) continue;
                            const int o = obs_long_ + (b - short_b0_) / 2;
                            if (o >= obs_n_) continue;
                            col[o] = std::max(col[o], amp * ks[static_cast<std::size_t>(dist)]);
                        }
                    }
                }
                float n2 = 0.0f;
                vDSP_svesq(col, 1, &n2, static_cast<vDSP_Length>(obs_n_));
                const float inv = 1.0f / std::sqrt(std::max(1e-12f, n2));
                vDSP_vsmul(col, 1, &inv, col, 1, static_cast<vDSP_Length>(obs_n_));
            }
        }
        WtW_.assign(static_cast<std::size_t>(kCols) * kCols, 0.0f);
        // WᵀW: W_ holds one template per row (kCols × obs_n), so WᵀW = W_ · W_ᵀ.
        std::vector<float> Wt(static_cast<std::size_t>(obs_n_) * kCols);
        vDSP_mtrans(W_.data(), 1, Wt.data(), 1, static_cast<vDSP_Length>(obs_n_), kCols);
        vDSP_mmul(W_.data(), 1, Wt.data(), 1, WtW_.data(), 1, kCols, kCols, static_cast<vDSP_Length>(obs_n_));
        H_.assign(kCols, 0.0f);
        logx_.assign(static_cast<std::size_t>(obs_n_), 0.0f);
    }

    float nnlsSalience(std::array<float, 128>& sal) {
        static const float kLam = tune("P2M_LAM", 0.2f), kComp = tune("P2M_COMP", 1.0f), kIt = tune("P2M_IT", 40.0f), kDecay = tune("P2M_HDECAY", 0.6f);
        if (W_.empty()) buildTemplates();
        float energy = 0.0f;
        for (int b = 0; b < obs_long_; ++b) {
            const float v = std::pow(spec_long_raw_[static_cast<std::size_t>(b)], kComp);
            logx_[static_cast<std::size_t>(b)] = v;
            energy += v * v;
        }
        for (int o = 0; o < obs_short_; ++o) {
            const int b = short_b0_ + 2 * o;
            const float v = std::pow(std::max(spec_short_raw_[static_cast<std::size_t>(b)], spec_short_raw_[static_cast<std::size_t>(b) + 1]), kComp);
            logx_[static_cast<std::size_t>(obs_long_ + o)] = v;
            energy += v * v;
        }
        const float norm = std::sqrt(energy) + 1e-9f;
        std::array<float, kCols> Wtx{};
        vDSP_mmul(W_.data(), 1, logx_.data(), 1, Wtx.data(), 1, kCols, 1, static_cast<vDSP_Length>(obs_n_));
        for (int c = 0; c < kCols; ++c) H_[static_cast<std::size_t>(c)] = H_[static_cast<std::size_t>(c)] * kDecay + 1e-3f * norm;
        const float lam = kLam * norm;
        std::array<float, kCols> WtWH{};
        for (int it = 0; it < static_cast<int>(kIt); ++it) {
            vDSP_mmul(WtW_.data(), 1, H_.data(), 1, WtWH.data(), 1, kCols, 1, kCols);
            for (int c = 0; c < kCols; ++c) H_[static_cast<std::size_t>(c)] *= Wtx[static_cast<std::size_t>(c)] / (WtWH[static_cast<std::size_t>(c)] + lam + 1e-9f);
        }
        for (int nn = 0; nn < kTplNotes; ++nn) {
            const int note = 38 + nn;
            if (note < set_.low_note || note > set_.high_note) continue;
            float a = 0.0f;
            for (int k = 0; k < kSlopes; ++k) a += H_[static_cast<std::size_t>(nn * kSlopes + k)];
            sal[static_cast<std::size_t>(note)] = a / norm;
        }
        // Ghost-partial suppression: a note sitting on the 2nd/3rd/4th harmonic of a stronger note must carry a
        // real share of the energy to survive (kGhost × the lower note's activation).
        for (int n = 127; n >= 0; --n) {
            if (sal[static_cast<std::size_t>(n)] <= 0.0f) continue;
            for (int iv : {12, 19, 24}) {
                const int m = n - iv;
                if (m < 0) continue;
                const float low = sal[static_cast<std::size_t>(m)];
                if (low > 0.0f && sal[static_cast<std::size_t>(n)] < kGhost * low) { sal[static_cast<std::size_t>(n)] = 0.0f; break; }
            }
        }
        float mx = 0.0f;
        for (float v : sal) mx = std::max(mx, v);
        std::array<float, 128> sorted = sal;
        std::sort(sorted.begin(), sorted.end(), std::greater<float>());
        const float sixth = sorted[5];
        for (auto& v : sal) if (v < sixth || v < kNnRel * mx || v <= 0.0f) v = 0.0f;
        if (level_ < 0.0005f) sal.fill(0.0f);
        return kNnAbs * (0.5f + (1.0f - set_.sensitivity));
    }
    static inline const float kNnRel = tune("P2M_NNREL", 0.12f);
    static inline const float kGhost = tune("P2M_GHOST", 0.0f);
    static inline const float kNnAbs = tune("P2M_NNABS", 0.08f);

    std::vector<float> W_, WtW_, H_, logx_, spec_long_raw_, spec_short_raw_;
    std::array<float, 128> last_sal_{};

public:
    const std::array<float, 128>& lastSalience() const { return last_sal_; }

private:

    // Keep only true spectral peaks (local maxima): a note's partial must be a peak, not the skirt of a
    // neighbouring partial's main lobe — this is what keeps short windows from smearing low notes.
    static void peaksOnly(std::vector<float>& s) {
        const std::size_t N = s.size();
        std::vector<float> o(N, 0.0f);
        for (std::size_t b = 1; b + 1 < N; ++b)
            if (s[b] > s[b - 1] && s[b] >= s[b + 1] && s[b] > 0.0f) o[b] = s[b];
        s.swap(o);
    }

    void whiten(std::vector<float>& s) const {
        // Magnitude compression + subtraction of a smoothed spectral envelope (≈ 1/3 octave), keep positives.
        const int N = kPad / 2;
        std::vector<float> c(static_cast<std::size_t>(N));
        for (int b = 0; b < N; ++b) c[static_cast<std::size_t>(b)] = std::pow(s[static_cast<std::size_t>(b)], 0.6f);
        std::vector<float> pre(static_cast<std::size_t>(N) + 1, 0.0f);
        for (int b = 0; b < N; ++b) pre[static_cast<std::size_t>(b) + 1] = pre[static_cast<std::size_t>(b)] + c[static_cast<std::size_t>(b)];
        for (int b = 0; b < N; ++b) {
            const float hz = static_cast<float>(b) * static_cast<float>(fa_) / kPad;
            const int half = std::max(8, static_cast<int>(binOf(std::max(60.0f, hz * 0.12f))));
            const int lo = std::max(0, b - half), hi = std::min(N, b + half + 1);
            const float env = (pre[static_cast<std::size_t>(hi)] - pre[static_cast<std::size_t>(lo)]) / static_cast<float>(hi - lo);
            s[static_cast<std::size_t>(b)] = std::max(0.0f, c[static_cast<std::size_t>(b)] - 0.9f * env);
        }
    }

    float salience(int note, const std::vector<float>& s) const {
        const float f0 = 440.0f * std::pow(2.0f, (note - 69) / 12.0f);
        const float nyq = static_cast<float>(fa_) * 0.45f;
        float acc = 0.0f;
        int missing = 0;
        for (int h = 1; h <= 14; ++h) {
            const float fh = f0 * static_cast<float>(h) * std::sqrt(1.0f + 0.00006f * static_cast<float>(h * h)); // slight inharmonicity
            if (fh > nyq) break;
            const float g = 1.0f / (1.0f + 0.08f * static_cast<float>(h - 1));
            const float a = peakNear(s, binOf(fh), std::max(2.0f, binOf(fh) * 0.022f));
            if (h <= 3 && a <= 0.0f) ++missing;
            acc += g * a;
        }
        if (missing >= 2) acc *= 0.4f; // needs fundamental/low partial support
        return acc;
    }

    // Ratio of odd-partial (1,3,5) energy of `note` to its even partials — distinguishes a real low note
    // from the octave below being a mis-read of a higher note's harmonics.
    float oddSupport(int note, const std::vector<float>& s) const {
        const float f0 = 440.0f * std::pow(2.0f, (note - 69) / 12.0f);
        float odd = 0.0f, even = 1e-6f;
        for (int h = 1; h <= 6; ++h) {
            const float fh = f0 * static_cast<float>(h);
            if (fh > static_cast<float>(fa_) * 0.45f) break;
            const float a = peakNear(s, binOf(fh), std::max(2.0f, binOf(fh) * 0.022f));
            ((h & 1) ? odd : even) += a;
        }
        return odd / (odd + even);
    }

    static constexpr float kCancel = 0.8f;
    void cancel(int note, std::vector<float>& s, int win_len) const {
        const float f0 = 440.0f * std::pow(2.0f, (note - 69) / 12.0f);
        const float nyq = static_cast<float>(fa_) * 0.45f;
        std::array<float, 16> amp{};
        std::array<int, 16> at{};
        int H = 0;
        for (int h = 1; h <= 14; ++h) {
            const float fh = f0 * static_cast<float>(h) * std::sqrt(1.0f + 0.00006f * static_cast<float>(h * h));
            if (fh > nyq) break;
            amp[static_cast<std::size_t>(h)] = peakNear(s, binOf(fh), std::max(2.0f, binOf(fh) * 0.022f));
            at[static_cast<std::size_t>(h)] = last_peak_bin_;
            H = h;
        }
        for (int h = 1; h <= H; ++h) {
            // Spectral smoothness: only remove what a smooth harmonic envelope explains (shared partials survive).
            const float a0 = amp[static_cast<std::size_t>(std::max(1, h - 1))], a1 = amp[static_cast<std::size_t>(h)],
                        a2 = amp[static_cast<std::size_t>(std::min(H, h + 1))];
            const float smooth = std::min(a1, (a0 + a1 + a2) / 3.0f * 1.1f);
            const int c = at[static_cast<std::size_t>(h)];
            const int w = 2 * kPad / win_len + 1; // Hann main-lobe half-width in padded bins
            for (int b = std::max(1, c - w); b <= std::min(kPad / 2 - 1, c + w); ++b) {
                const float frac = (a1 > 1e-9f) ? std::min(1.0f, kCancel * smooth / a1) : 0.0f;
                s[static_cast<std::size_t>(b)] *= (1.0f - frac);
            }
        }
    }

    double host_sr_ = 48000.0, fa_ = 16000.0;
    int decim_ = 3, dec_phase_ = 0, hop_count_ = 0;
    std::array<Biquad, 2> aa_{};
    std::vector<float> hist_;
    int hpos_ = 0;
    // Owns the vDSP FFT setup and is movable, so a whole detector (history, tracked notes) can be swapped
    // between component instances on the audio thread without allocating or double-freeing.
    struct FftHandle {
        FFTSetup p = nullptr;
        FftHandle() = default;
        FftHandle(const FftHandle&) = delete;
        FftHandle& operator=(const FftHandle&) = delete;
        FftHandle(FftHandle&& o) noexcept : p(o.p) { o.p = nullptr; }
        FftHandle& operator=(FftHandle&& o) noexcept { if (this != &o) { reset(); p = o.p; o.p = nullptr; } return *this; }
        FftHandle& operator=(FFTSetup s) noexcept { reset(); p = s; return *this; }
        ~FftHandle() { reset(); }
        void reset() noexcept { if (p) vDSP_destroy_fftsetup(p); p = nullptr; }
        operator FFTSetup() const noexcept { return p; }
        explicit operator bool() const noexcept { return p != nullptr; }
    } fft_;
    std::vector<float> win_long_, win_short_, spec_long_, spec_short_, prev_logspec_, yin_buf_;
    std::array<NoteState, 128> notes_{};
    std::array<float, 128> smooth_{};
    int since_onset_ = 100;
    DetectorSettings set_{};
    float level_ = 0.0f, flux_avg_ = 0.0f, flux_prev_ = 0.0f, flux_prev2_ = 0.0f;
    float pitch_hz_ = 0.0f, confidence_ = 0.0f;
    int mono_note_ = -1, mono_stable_ = 0, mono_off_count_ = 0;
    float mono_vel_ = 0.8f, mono_level_slow_ = 0.0f;
    mutable int last_peak_bin_ = 0;
};

} // namespace rig::midi
