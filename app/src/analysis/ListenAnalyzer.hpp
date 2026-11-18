// Headroom — "Listen" analysis: what the player is doing, measured on the DRY input (before the rig).
//
//   ListenCore     single-threaded analysis over a stream of (dry input, rig output) samples:
//                  - mono tuner: YIN on a ~16 kHz decimated copy (27 Hz … 3.5 kHz: 5/6-string bass to violin),
//                    refined at full rate on a multiple of the period (sub-0.2-cent resolution), tracked with
//                    outlier rejection; pitch trail (3 s) and bend-landing detection
//                  - polyphonic tuner: every configured string at once from one strum. Long-window FFT pair +
//                    phase-vocoder frequency per partial; each string combines its first harmonics, down-weighting
//                    partials that collide with another sounding string's harmonics (B3 = 3·E2, E4 = 4·E2 = 3·A2…)
//                  - key from the shipped guitar→MIDI transcriber's notes: duration-weighted Temperley profile for
//                    single-note playing + harmonic function of chords heard where ≥ 3 notes sound together, blended by
//                    how chordal the playing is; a smoothed-lead decision keeps it steady (GuitarSet held-out players:
//                    74 % exact, comping 83 %). Chroma (harmonic-suppressed peaks) still drives the live chord readout.
//                  - display spectra (in/out, 72 log bands), tone descriptors, input level / peak hold / clip /
//                    noise floor / mains hum
//   ListenService  owns a ListenCore on its own thread; reads a lock-free ring the audio callback fills.
//                  Lazy: runs only while /api/listen has been polled in the last few seconds.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifdef __APPLE__
#include <Accelerate/Accelerate.h>
#endif
#include "../midi/NeuralGuitarMidi.hpp"

namespace rig::listen {

constexpr double kPi = 3.14159265358979323846;

inline int nextPow2(int v) { int n = 1; while (n < v) n <<= 1; return n; }
inline double hzToMidi(double hz) { return 69.0 + 12.0 * std::log2(hz / 440.0); }
inline double midiToHz(double m, double a4 = 440.0) { return a4 * std::pow(2.0, (m - 69.0) / 12.0); }

// Real FFT returning the standard DFT X[k] = Σ x[n] e^{-2πikn/N}, k = 0..N/2.
class RealFFT {
public:
    explicit RealFFT(int n) : n_(n) {
#ifdef __APPLE__
        log2n_ = static_cast<int>(std::lround(std::log2(n)));
        setup_ = vDSP_create_fftsetup(static_cast<vDSP_Length>(log2n_), kFFTRadix2);
        re_.resize(static_cast<std::size_t>(n / 2));
        im_.resize(static_cast<std::size_t>(n / 2));
#endif
    }
    ~RealFFT() {
#ifdef __APPLE__
        if (setup_) vDSP_destroy_fftsetup(setup_);
#endif
    }
    RealFFT(const RealFFT&) = delete;
    RealFFT& operator=(const RealFFT&) = delete;
    int size() const { return n_; }
    void forward(const float* in, std::vector<std::complex<float>>& out) {
        out.resize(static_cast<std::size_t>(n_ / 2 + 1));
#ifdef __APPLE__
        DSPSplitComplex sc{re_.data(), im_.data()};
        vDSP_ctoz(reinterpret_cast<const DSPComplex*>(in), 2, &sc, 1, static_cast<vDSP_Length>(n_ / 2));
        vDSP_fft_zrip(setup_, &sc, 1, static_cast<vDSP_Length>(log2n_), kFFTDirection_Forward);
        out[0] = {re_[0] * 0.5f, 0.0f};
        out[static_cast<std::size_t>(n_ / 2)] = {im_[0] * 0.5f, 0.0f};
        for (int k = 1; k < n_ / 2; ++k) out[static_cast<std::size_t>(k)] = {re_[static_cast<std::size_t>(k)] * 0.5f, im_[static_cast<std::size_t>(k)] * 0.5f};
#else
        std::vector<std::complex<float>> a(static_cast<std::size_t>(n_));
        for (int i = 0; i < n_; ++i) a[static_cast<std::size_t>(i)] = in[i];
        for (int i = 1, j = 0; i < n_; ++i) {
            int bit = n_ >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(a[static_cast<std::size_t>(i)], a[static_cast<std::size_t>(j)]);
        }
        for (int len = 2; len <= n_; len <<= 1) {
            const double ang = -2 * kPi / len;
            const std::complex<float> wl(static_cast<float>(std::cos(ang)), static_cast<float>(std::sin(ang)));
            for (int i = 0; i < n_; i += len) {
                std::complex<float> w(1.0f, 0.0f);
                for (int j = 0; j < len / 2; ++j) {
                    auto u = a[static_cast<std::size_t>(i + j)], v = a[static_cast<std::size_t>(i + j + len / 2)] * w;
                    a[static_cast<std::size_t>(i + j)] = u + v;
                    a[static_cast<std::size_t>(i + j + len / 2)] = u - v;
                    w *= wl;
                }
            }
        }
        for (int k = 0; k <= n_ / 2; ++k) out[static_cast<std::size_t>(k)] = a[static_cast<std::size_t>(k)];
#endif
    }

private:
    int n_;
#ifdef __APPLE__
    int log2n_ = 0;
    FFTSetup setup_ = nullptr;
    rig::vdsp::FftVector<float> re_, im_; // 64-byte aligned: else vDSP_fft_zrip's result depends on the address
#endif
};

struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void lowpass(double fc, double fs, double q) {
        const double w = 2 * kPi * fc / fs, c = std::cos(w), s = std::sin(w), al = s / (2 * q), a0 = 1 + al;
        b0 = (1 - c) / 2 / a0; b1 = (1 - c) / a0; b2 = b0; a1 = -2 * c / a0; a2 = (1 - al) / a0; z1 = z2 = 0;
    }
    double run(double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
};

// One spectrum frame (Hann) with helpers for dBFS magnitude and phase-vocoder frequency.
struct Frame {
    std::vector<std::complex<float>> X;
    std::vector<float> db;  // dBFS of a full-scale sine = 0
    double binHz = 1;
};

// Host rate → 24 kHz for the neural transcriber (same Kaiser-windowed sinc as the Pitch→MIDI unit).
class To24k {
public:
    void prepare(double host_sr) {
        step_ = host_sr / 24000.0;
        const double fc = 10800.0 / host_sr;
        taps_.assign(kPhases * kTaps, 0.0f);
        auto bessel0 = [](double x) { double s = 1, t = 1; for (int k = 1; k < 25; ++k) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; };
        for (int ph = 0; ph < kPhases; ++ph) {
            const double frac = double(ph) / kPhases;
            double sum = 0;
            for (int k = 0; k < kTaps; ++k) {
                const double x = (k - (kTaps / 2 - 1)) - frac;
                const double sinc = std::abs(x) < 1e-9 ? 2 * fc : std::sin(2 * kPi * fc * x) / (kPi * x);
                const double r = x / (kTaps / 2.0);
                const double w = std::abs(r) <= 1 ? bessel0(8.0 * std::sqrt(1 - r * r)) / bessel0(8.0) : 0.0;
                taps_[static_cast<std::size_t>(ph * kTaps + k)] = static_cast<float>(sinc * w); sum += sinc * w;
            }
            for (int k = 0; k < kTaps; ++k) taps_[static_cast<std::size_t>(ph * kTaps + k)] /= static_cast<float>(sum);
        }
        hist_.assign(2 * kTaps, 0.0f); hpos_ = 0; t_ = 0.0;
    }
    bool push(float x, float& out) {
        hist_[static_cast<std::size_t>(hpos_)] = x; hist_[static_cast<std::size_t>(hpos_ + kTaps)] = x; hpos_ = (hpos_ + 1) % kTaps;
        t_ += 1.0;
        if (t_ < step_) return false;
        t_ -= step_;
        const int ph = std::min(kPhases - 1, static_cast<int>(t_ * kPhases));
        const float* h = &hist_[static_cast<std::size_t>(hpos_)];
        const float* w = &taps_[static_cast<std::size_t>(ph * kTaps)];
        float acc = 0; for (int k = 0; k < kTaps; ++k) acc += w[kTaps - 1 - k] * h[k];
        out = acc; return true;
    }
private:
    static constexpr int kTaps = 32, kPhases = 64;
    std::vector<float> taps_, hist_;
    int hpos_ = 0; double step_ = 2.0, t_ = 0.0;
};

class ListenCore {
public:
    static constexpr int kBands = 72;
    static constexpr int kTrail = 150;
    static constexpr double kBandLo = 40.0, kBandHi = 16000.0;

    ListenCore() { prepare(48000.0); }

    void prepare(double sr) {
        sr_ = sr > 1000 ? sr : 48000.0;
        hop_ = std::max(256, static_cast<int>(std::lround(sr_ * 0.02)));
        dec_ = std::max(1, static_cast<int>(std::lround(sr_ / 16000.0)));
        rateDec_ = sr_ / dec_;
        aa1_.lowpass(rateDec_ * 0.42, sr_, 0.5412);
        aa2_.lowpass(rateDec_ * 0.42, sr_, 1.3066);
        nDisp_ = nextPow2(static_cast<int>(sr_ * 0.085));
        nChroma_ = nextPow2(static_cast<int>(sr_ * 0.30));
        histLen_ = nextPow2(static_cast<int>(sr_ * 1.6));
        decLen_ = nextPow2(static_cast<int>(rateDec_ * 0.6));
        inHist_.assign(static_cast<std::size_t>(histLen_), 0.0f);
        outHist_.assign(static_cast<std::size_t>(histLen_), 0.0f);
        decHist_.assign(static_cast<std::size_t>(decLen_), 0.0f);
        histPos_ = 0; decPos_ = 0; decPhase_ = 0; sinceHop_ = 0; samples_ = 0; hops_ = 0;
        rs_.prepare(sr_); if (nnOk_) nn_.reset(); sounding_.fill(-1.0);
        fftDisp_ = std::make_unique<RealFFT>(nDisp_);
        fftChroma_ = std::make_unique<RealFFT>(nChroma_);
        setPolyWindow();
        bandIn_.fill(-120.0f); bandOut_.fill(-120.0f); slowOut_.fill(-120.0f); slowOutW_ = 0;
        trail_.assign(kTrail, std::nanf(""));
        resetKey();
        rmsHist_.clear();
        peakHoldDb_ = -120; peakHoldT_ = 0; floorDb_ = NAN; clipT_ = -100; humT_ = -100;
        polyActiveT_ = -100;
        smooth_ = NAN; pendingJump_ = NAN; rawHist_.clear();
        chord_ = {}; candChord_ = {}; candCount_ = 0; prog_.clear();
        bend_ = {}; plateau_ = {}; lastPlateau_ = {};
    }

    void setReference(double a4) { a4_ = std::clamp(a4, 400.0, 480.0); }
    // Key from transcribed notes (the shipped guitar→MIDI model). Without it, the chroma path is used.
    bool setModelPath(const std::string& path) {
        nnOk_ = !path.empty() && nn_.load(path);
        rs_.prepare(sr_);
        sounding_.fill(-1.0);
        return nnOk_;
    }
    // String targets as (fractional) MIDI notes, low → high. Empty = chromatic only (no polyphonic tuner).
    void setStrings(std::vector<double> midi) {
        strings_ = std::move(midi);
        if (strings_.size() > 12) strings_.resize(12);
        polyStr_.assign(strings_.size(), {});
        setPolyWindow();
    }
    void resetKey() { keyHist_.fill(0.0); bassHist_.fill(0.0); chordKey_.fill(0.0); chordKeyW_ = 0; cadence_.fill(0.0);
        chordFn_.fill(0.0); noteHist_.fill(0.0); winH_.fill(0.0); polyT_ = soundT_ = chordFnW_ = noteEvidence_ = winT_ = chordal_ = 0; winLow_ = -1; winPoly_ = 0; keyEvidence_ = 0; key_ = -1; keyCand_ = -1; keyCandCount_ = 0; keyCandT_ = 0; keyAlt_ = -1; keyConf_ = 0; chromaShort_.fill(0.0); lastSignalT_ = -100; }
    double time() const { return static_cast<double>(samples_) / sr_; }

    void feed(const float* in, const float* out, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            const float x = std::isfinite(in[i]) ? in[i] : 0.0f;
            inHist_[static_cast<std::size_t>(histPos_)] = x;
            outHist_[static_cast<std::size_t>(histPos_)] = out ? (std::isfinite(out[i]) ? out[i] : 0.0f) : 0.0f;
            histPos_ = (histPos_ + 1) & (histLen_ - 1);
            if (nnOk_ && nnAwake_) {   // transcriber sleeps through silence (it is most of the analysis cost)
                float y24;
                if (rs_.push(x, y24)) {
                    nnEv_.clear();
                    nn_.process(&y24, 1, nnEv_);
                    for (const auto& e : nnEv_) if (e.pitch >= 0 && e.pitch < 128) sounding_[static_cast<std::size_t>(e.pitch)] = e.on ? double(samples_) / sr_ : -1.0;
                }
            }
            const double y = aa2_.run(aa1_.run(x));
            if (++decPhase_ >= dec_) { decPhase_ = 0; decHist_[static_cast<std::size_t>(decPos_)] = static_cast<float>(y); decPos_ = (decPos_ + 1) & (decLen_ - 1); }
            ++samples_;
            if (++sinceHop_ >= hop_) { sinceHop_ = 0; analyzeHop(); }
        }
    }

    // ---- results ---------------------------------------------------------------------------------
    struct PolyString { double hz = 0, cents = 0, levelDb = -120; bool present = false; double lastT = -100; };
    struct Chord { int root = -1, quality = -1, bass = -1; double conf = 0, start = 0, dur = 0; };
    struct Bend { int to = -1; double cents = 0, from = 0, t = -100; };

    std::string json() const {
        std::string o;
        o.reserve(4096);
        char b[256];
        const double t = time();
        auto num = [&](double v, const char* fmt = "%.2f") { if (!std::isfinite(v)) { o += "null"; return; } std::snprintf(b, sizeof b, fmt, v); o += b; };
        o += "{\"ok\":true,\"sr\":"; num(sr_, "%.0f");
        o += ",\"t\":"; num(t, "%.3f");
        o += ",\"a4\":"; num(a4_, "%.2f");
        const bool signal = signal_;
        // level
        o += ",\"level\":{\"peak_db\":"; num(peakDb_, "%.1f");
        o += ",\"hold_db\":"; num(peakHoldDb_, "%.1f");
        o += ",\"rms_db\":"; num(rmsDb_, "%.1f");
        o += ",\"floor_db\":"; num(floorDb_, "%.1f");
        o += ",\"signal\":"; o += signal ? "true" : "false";
        o += ",\"silent_s\":"; num(signal ? 0.0 : t - lastSignalT_, "%.1f");
        o += ",\"clip\":"; o += (t - clipT_ < 2.0) ? "true" : "false";
        o += ",\"hum_hz\":"; if (t - humT_ < 6.0) num(humHz_, "%.0f"); else o += "null";
        o += ",\"hum_db\":"; if (t - humT_ < 6.0) num(humDb_, "%.1f"); else o += "null";
        o += "}";
        // pitch
        o += ",\"pitch\":";
        if (voiced_ && std::isfinite(smooth_)) {
            o += "{\"hz\":"; num(midiToHz(smooth_), "%.4f");
            o += ",\"midi\":"; num(smooth_, "%.4f");
            o += ",\"raw\":"; num(lastRaw_, "%.4f");
            o += ",\"conf\":"; num(pitchConf_, "%.3f");
            o += ",\"stable\":"; o += stable_ ? "true" : "false";
            o += "}";
        } else o += "null";
        o += ",\"trail\":{\"step_ms\":"; num(1000.0 * hop_ / sr_, "%.1f");
        o += ",\"midi\":[";
        for (int i = 0; i < kTrail; ++i) {
            if (i) o += ",";
            num(trail_[static_cast<std::size_t>((trailPos_ + i) % kTrail)], "%.3f");
        }
        o += "]}";
        o += ",\"bend\":";
        if (t - bend_.t < 2.5 && bend_.to >= 0) {
            o += "{\"to\":"; num(bend_.to, "%.0f"); o += ",\"from\":"; num(bend_.from, "%.3f"); o += ",\"cents\":"; num(bend_.cents, "%.1f");
            o += ",\"age\":"; num(t - bend_.t, "%.2f"); o += "}";
        } else o += "null";
        // poly
        o += ",\"poly\":{\"active\":"; o += (t - polyActiveT_ < 1.2) ? "true" : "false";
        o += ",\"strings\":[";
        for (std::size_t s = 0; s < strings_.size(); ++s) {
            const auto& p = polyStr_[s];
            if (s) o += ",";
            o += "{\"target\":"; num(strings_[s], "%.2f");
            const bool fresh = p.present && t - p.lastT < 1.2;
            o += ",\"present\":"; o += fresh ? "true" : "false";
            o += ",\"hz\":"; if (fresh) num(p.hz, "%.4f"); else o += "null";
            o += ",\"cents\":"; if (fresh) num(p.cents, "%.2f"); else o += "null";
            o += ",\"level_db\":"; num(p.levelDb, "%.1f");
            o += "}";
        }
        o += "]}";
        // chroma + key + chord
        o += ",\"chroma\":[";
        for (int i = 0; i < 12; ++i) { if (i) o += ","; num(chromaShort_[static_cast<std::size_t>(i)], "%.3f"); }
        o += "],\"key_profile\":[";
        {
            double mx = 1e-12; for (double v : keyHist_) mx = std::max(mx, v);
            for (int i = 0; i < 12; ++i) { if (i) o += ","; num(keyHist_[static_cast<std::size_t>(i)] / mx, "%.3f"); }
        }
        o += "],\"key\":";
        if (key_ >= 0) {
            o += "{\"root\":"; num(key_ % 12, "%.0f"); o += ",\"minor\":"; o += key_ >= 12 ? "true" : "false";
            o += ",\"conf\":"; num(keyConf_, "%.3f");
            o += ",\"alt_root\":"; num(keyAlt_ >= 0 ? keyAlt_ % 12 : -1, "%.0f"); o += ",\"alt_minor\":"; o += keyAlt_ >= 12 ? "true" : "false";
            o += ",\"evidence_s\":"; num(keyEvidence_, "%.1f");
            o += ",\"state\":\""; o += signal ? (keyEvidence_ < KP().settleS ? "listening" : "live") : "held"; o += "\"";
            o += "}";
        } else {
            o += "{\"root\":null,\"state\":\"listening\",\"evidence_s\":"; num(keyEvidence_, "%.1f"); o += "}";
        }
        auto chordJson = [&](const Chord& c, bool withDur) {
            o += "{\"root\":"; num(c.root, "%.0f"); o += ",\"quality\":\""; o += qualityName(c.quality); o += "\"";
            o += ",\"bass\":"; num(c.bass, "%.0f");
            if (withDur) { o += ",\"dur\":"; num(c.dur, "%.2f"); }
            else { o += ",\"conf\":"; num(c.conf, "%.2f"); o += ",\"age\":"; num(t - c.start, "%.2f"); }
            o += "}";
        };
        o += ",\"chord\":";
        if (chord_.root >= 0) chordJson(chord_, false); else o += "null";
        o += ",\"chordal\":"; o += (signal && singleNote_ < 0 && chord_.root >= 0 && t - chordSeenT_ < 0.3) ? "true" : "false";
        o += ",\"note\":";
        if (singleNote_ >= 0 && signal) num(singleNote_, "%.0f"); else o += "null";
        o += ",\"prog\":[";
        for (std::size_t i = 0; i < prog_.size(); ++i) { if (i) o += ","; chordJson(prog_[i], true); }
        o += "]";
        // spectra + tone
        o += ",\"spec\":{\"f0\":40,\"f1\":16000,\"in\":[";
        for (int i = 0; i < kBands; ++i) { if (i) o += ","; num(bandIn_[static_cast<std::size_t>(i)], "%.1f"); }
        o += "],\"out\":[";
        for (int i = 0; i < kBands; ++i) { if (i) o += ","; num(bandOut_[static_cast<std::size_t>(i)], "%.1f"); }
        o += "]}";
        o += ",\"tone\":";
        if (slowOutW_ > 2.0) {
            o += "{\"low_hz\":"; num(toneLow_, "%.0f"); o += ",\"mid_hz\":"; num(toneMid_, "%.0f"); o += ",\"top_hz\":"; num(toneTop_, "%.0f");
            o += ",\"tilt_db\":"; num(toneTilt_, "%.1f"); o += "}";
        } else o += "null";
        o += "}";
        return o;
    }

    static const char* qualityName(int q) {
        static const char* names[] = {"maj", "min", "5", "7", "maj7", "m7", "sus2", "sus4", "dim", "aug", "m7b5", "add9", "6", "m6", "dim7", "9", "m9"};
        return (q >= 0 && q < static_cast<int>(sizeof(names) / sizeof(names[0]))) ? names[q] : "";
    }

    // test hooks
    bool voiced() const { return voiced_; }
    double pitchMidi() const { return smooth_; }
    const std::vector<PolyString>& poly() const { return polyStr_; }
    int key() const { return key_; }
    const Chord& chord() const { return chord_; }

private:
    // ------------------------------------------------------------------------------------------------
    double sr_ = 48000, rateDec_ = 16000, a4_ = 440;
    int hop_ = 960, dec_ = 3, nDisp_ = 4096, nChroma_ = 16384, nPoly_ = 16384, polyR_ = 4096;
    int histLen_ = 65536, decLen_ = 8192, histPos_ = 0, decPos_ = 0, decPhase_ = 0, sinceHop_ = 0;
    uint64_t samples_ = 0, hops_ = 0;
    std::vector<float> inHist_, outHist_, decHist_;
    Biquad aa1_, aa2_;
    std::unique_ptr<RealFFT> fftDisp_, fftChroma_, fftPoly_;
    std::vector<float> win_, tmp_;
    std::vector<std::complex<float>> spec_;

    // level
    double peakDb_ = -120, rmsDb_ = -120, peakHoldDb_ = -120, peakHoldT_ = 0, floorDb_ = NAN, clipT_ = -100;
    double humHz_ = 0, humDb_ = -120, humT_ = -100;
    bool signal_ = false;
    double lastSignalT_ = -100;
    std::deque<float> rmsHist_, recentRms_;
    double onsetT_ = -100;

    // pitch
    bool voiced_ = false, stable_ = false;
    double smooth_ = NAN, pendingJump_ = NAN, lastRaw_ = NAN, pitchConf_ = 0;
    int pendingCount_ = 0, unvoicedHops_ = 0;
    std::deque<double> rawHist_;
    std::vector<float> trail_;
    int trailPos_ = 0;
    struct Plateau { double center = NAN, start = 0, end = 0; int n = 0; bool valid = false; double sum = 0; };
    Plateau plateau_, lastPlateau_;
    bool glideSeen_ = false;
    double voicedSince_ = 0;
    Bend bend_;

    // poly
    std::vector<double> strings_;
    std::vector<PolyString> polyStr_;
    double polyActiveT_ = -100;

    // harmony
    std::array<double, 12> keyHist_{}, chromaShort_{}, bassHist_{};
    std::array<double, 24> chordKey_{}, cadence_{}, chordFn_{}, lead_{};
    std::array<double, 12> noteHist_{}, winH_{};
    std::array<double, 128> sounding_{};
    double polyT_ = 0, soundT_ = 0, chordFnW_ = 0, noteEvidence_ = 0, winT_ = 0, chordal_ = 0;
    int winLow_ = -1, winPoly_ = 0;
    rig::midi::NeuralGuitarMidi nn_;
    std::vector<rig::midi::NeuralGuitarMidi::Note> nnEv_;
    To24k rs_;
    bool nnOk_ = false, nnAwake_ = false;
    double chordKeyW_ = 0;
    double keyEvidence_ = 0, keyConf_ = 0;
    int key_ = -1, keyAlt_ = -1, keyCand_ = -1, keyCandCount_ = 0, singleNote_ = -1;
    double keyCandT_ = 0;
    Chord chord_, candChord_;
    int candCount_ = 0;
    double chordSeenT_ = -100;
    std::vector<Chord> prog_;

    // display
    std::array<float, kBands> bandIn_{}, bandOut_{}, slowOut_{};
    double slowOutW_ = 0, toneLow_ = 0, toneMid_ = 0, toneTop_ = 0, toneTilt_ = 0;

    // ------------------------------------------------------------------------------------------------
    void setPolyWindow() {
        // Resolve adjacent string fundamentals: bin ≤ (smallest gap between neighbouring targets) / 5.
        double minGap = 1e9, lowHz = 1e9;
        std::vector<double> s = strings_;
        std::sort(s.begin(), s.end());
        for (std::size_t i = 0; i < s.size(); ++i) {
            lowHz = std::min(lowHz, midiToHz(s[i], a4_));
            if (i) minGap = std::min(minGap, midiToHz(s[i], a4_) - midiToHz(s[i - 1], a4_));
        }
        double want = 0.30;  // seconds
        if (minGap < 1e8 && minGap > 0.5) want = std::max(want, 5.0 / minGap);
        if (lowHz < 60) want = std::max(want, 0.6);
        want = std::min(want, 0.75);
        const int n = nextPow2(static_cast<int>(sr_ * want));
        if (!fftPoly_ || fftPoly_->size() != n) fftPoly_ = std::make_unique<RealFFT>(n);
        nPoly_ = n;
        polyR_ = n / 4;
        if (nPoly_ + polyR_ + 16 > histLen_) {  // keep the history long enough
            histLen_ = nextPow2(nPoly_ + polyR_ + 16);
            inHist_.assign(static_cast<std::size_t>(histLen_), 0.0f);
            outHist_.assign(static_cast<std::size_t>(histLen_), 0.0f);
            histPos_ = 0;
        }
    }

    // Copy the newest n samples (ending `lag` samples ago) into dst, oldest first.
    void recent(const std::vector<float>& ring, int len, int pos, int n, int lag, float* dst) const {
        int start = (pos - lag - n) & (len - 1);
        for (int i = 0; i < n; ++i) dst[i] = ring[static_cast<std::size_t>((start + i) & (len - 1))];
    }

    void spectrum(RealFFT& fft, const std::vector<float>& ring, int lag, Frame& f) {
        const int n = fft.size();
        tmp_.resize(static_cast<std::size_t>(n));
        recent(ring, histLen_, histPos_, n, lag, tmp_.data());
        if (static_cast<int>(win_.size()) != n) {
            win_.resize(static_cast<std::size_t>(n));
            for (int i = 0; i < n; ++i) win_[static_cast<std::size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2 * kPi * i / n));
        }
        double mean = 0;
        for (int i = 0; i < n; ++i) mean += tmp_[static_cast<std::size_t>(i)];
        mean /= n;
        for (int i = 0; i < n; ++i) tmp_[static_cast<std::size_t>(i)] = static_cast<float>((tmp_[static_cast<std::size_t>(i)] - mean) * win_[static_cast<std::size_t>(i)]);
        fft.forward(tmp_.data(), f.X);
        f.binHz = sr_ / n;
        f.db.resize(f.X.size());
        const double scale = 4.0 / n;  // Hann sum = n/2; a sine of amplitude A peaks at A·n/4
        for (std::size_t k = 0; k < f.X.size(); ++k) f.db[k] = static_cast<float>(20.0 * std::log10(std::abs(f.X[k]) * scale + 1e-12));
    }

    static double parabolic(double a, double b, double c) {  // vertex offset in [-0.5, 0.5] for samples a,b,c around a max/min at b
        const double d = a - 2 * b + c;
        return std::fabs(d) < 1e-20 ? 0.0 : std::clamp(0.5 * (a - c) / d, -0.5, 0.5);
    }

    void analyzeHop() {
        ++hops_;
        const double t = time();
        measureLevel(t);
        trackPitch(t);
        displaySpectra();
        if (hops_ % 2 == 0) harmonic(t);
    }

    // ---- level / floor -------------------------------------------------------------------------------
    void measureLevel(double t) {
        double pk = 0, ss = 0;
        int clipN = 0;
        for (int i = 0; i < hop_; ++i) {
            const float x = inHist_[static_cast<std::size_t>((histPos_ - hop_ + i) & (histLen_ - 1))];
            pk = std::max(pk, static_cast<double>(std::fabs(x)));
            ss += double(x) * x;
            if (std::fabs(x) >= 0.989f) ++clipN;
        }
        if (clipN >= 2) clipT_ = t;
        peakDb_ = 20 * std::log10(pk + 1e-12);
        rmsDb_ = 10 * std::log10(ss / hop_ + 1e-24);
        if (peakDb_ >= peakHoldDb_) { peakHoldDb_ = peakDb_; peakHoldT_ = t; }
        else if (t - peakHoldT_ > 1.5) peakHoldDb_ = std::max(peakDb_, peakHoldDb_ - 20.0 * hop_ / sr_);
        // Noise floor: 8th percentile of the last ~12 s of 20 ms RMS, ignoring digital silence.
        if (rmsDb_ > -140) rmsHist_.push_back(static_cast<float>(rmsDb_));
        while (rmsHist_.size() > 600) rmsHist_.pop_front();
        if (rmsHist_.size() >= 25 && hops_ % 5 == 0) {
            // Only trust it when the history has real gaps: the 8th percentile must sit well under the median
            // (continuous playing leaves no gaps, and its quietest moments are note tails, not noise).
            std::vector<float> v(rmsHist_.begin(), rmsHist_.end());
            const std::size_t k = v.size() * 8 / 100, md = v.size() / 2;
            std::nth_element(v.begin(), v.begin() + static_cast<long>(md), v.end());
            const float med = v[md];
            std::nth_element(v.begin(), v.begin() + static_cast<long>(k), v.begin() + static_cast<long>(md));
            if (med - v[k] >= 14.0f || v[k] < -75.0f) floorDb_ = v[k];
        } else if (rmsHist_.empty()) floorDb_ = NAN;
        // Playing: clearly above a measured floor, or simply loud enough (continuous playing leaves no gaps
        // to measure the floor in, so the 8th percentile can sit on the decay tails).
        const double fl = std::isfinite(floorDb_) ? floorDb_ : -120.0;
        signal_ = rmsDb_ > -58.0 || (rmsDb_ > fl + 12.0 && rmsDb_ > -84.0);
        // attack: 80 ms RMS (≥ 2 periods of a low B) jumps ≥ 6 dB over the quietest of the previous 200 ms
        {
            const int n80 = static_cast<int>(sr_ * 0.08);
            double s80 = 0;
            for (int i = 0; i < n80; ++i) { const float x = inHist_[static_cast<std::size_t>((histPos_ - n80 + i) & (histLen_ - 1))]; s80 += double(x) * x; }
            const double r80 = 10 * std::log10(s80 / n80 + 1e-24);
            double mn = 1e9;
            for (float v : recentRms_) mn = std::min(mn, double(v));
            if (recentRms_.size() >= 6 && r80 > -55.0 && r80 - mn >= 6.0) onsetT_ = t;
            recentRms_.push_back(static_cast<float>(r80));
            while (recentRms_.size() > 10) recentRms_.pop_front();
        }
        if (signal_) lastSignalT_ = t;
        const bool awake = rmsDb_ > -72.0 || t - lastSignalT_ < 1.0;
        if (awake && !nnAwake_ && nnOk_) { nn_.reset(); sounding_.fill(-1.0); }
        nnAwake_ = awake;
    }

    // ---- mono tuner ----------------------------------------------------------------------------------
    void trackPitch(double t) {
        // Lowest pitch: 5 semitones under the instrument's lowest string (chords share a long common period,
        // which would otherwise read as a sub-octave "note"); chromatic mode goes down to 26 Hz.
        double fMin = 26.0;
        if (!strings_.empty()) fMin = std::max(26.0, midiToHz(*std::min_element(strings_.begin(), strings_.end()) - 5.0, a4_));
        const int tauMax = static_cast<int>(rateDec_ / fMin);
        const int tauMin = std::max(2, static_cast<int>(rateDec_ / 3600.0));
        const int W = tauMax;
        const int need = W + tauMax + 2;
        static thread_local std::vector<float> x;
        static thread_local std::vector<double> d;
        x.resize(static_cast<std::size_t>(need));
        recent(decHist_, decLen_, decPos_, need, 0, x.data());
        d.assign(static_cast<std::size_t>(tauMax + 2), 0.0);
        double raw = NAN, conf = 0;
        if (signal_) {
            // YIN difference + cumulative-mean normalisation
            for (int tau = 1; tau <= tauMax + 1; ++tau) {
                double s = 0;
                const float* a = x.data() + (need - W);
                const float* bb = a - tau;
                for (int j = 0; j < W; ++j) { const double df = double(a[j]) - bb[j]; s += df * df; }
                d[static_cast<std::size_t>(tau)] = s;
            }
            std::vector<double> dn(d.size(), 1.0);
            double run = 0;
            for (int tau = 1; tau <= tauMax + 1; ++tau) { run += d[static_cast<std::size_t>(tau)]; dn[static_cast<std::size_t>(tau)] = run > 0 ? d[static_cast<std::size_t>(tau)] * tau / run : 1.0; }
            int best = -1;
            for (int tau = tauMin; tau <= tauMax; ++tau) {
                if (dn[static_cast<std::size_t>(tau)] < 0.15) {
                    while (tau + 1 <= tauMax && dn[static_cast<std::size_t>(tau + 1)] < dn[static_cast<std::size_t>(tau)]) ++tau;
                    best = tau;
                    break;
                }
            }
            if (best < 0) {
                double mn = 1e9;
                for (int tau = tauMin; tau <= tauMax; ++tau) if (dn[static_cast<std::size_t>(tau)] < mn) { mn = dn[static_cast<std::size_t>(tau)]; best = tau; }
            }
            if (best > tauMin && best < tauMax) {
                const double ap = dn[static_cast<std::size_t>(best)];
                conf = std::clamp(1.0 - ap, 0.0, 1.0);
                if (ap < 0.32) {
                    const double tauF = best + parabolic(dn[static_cast<std::size_t>(best - 1)], dn[static_cast<std::size_t>(best)], dn[static_cast<std::size_t>(best + 1)]);
                    const double f = refinePeriod(rateDec_ / tauF);
                    if (std::isfinite(f) && f > 24 && f < 4200) raw = hzToMidi(f);
                }
            }
        }
        pitchConf_ = conf;
        lastRaw_ = raw;
        updateTrack(t, raw);
    }

    // Full-rate refinement on a multiple of the period (≈100 ms of lag): sub-sample min of the difference
    // function. Two stages (≈4 periods, then the full multiple) so a coarse estimate that is a few % off at
    // high pitches can't land the long lag on the wrong cycle.
    double refinePeriod(double fCoarse) {
        double f = fCoarse;
        const double T0 = sr_ / fCoarse;
        const int mFull = std::clamp(static_cast<int>(std::floor(sr_ * 0.1 / T0)), 1, 48);
        if (mFull > 6) {
            const double f1 = refineAt(f, 4);
            if (!std::isfinite(f1)) return fCoarse;
            f = f1;
        }
        const double f2 = refineAt(f, mFull);
        if (!std::isfinite(f2)) return f;
        return std::fabs(1200 * std::log2(f2 / fCoarse)) < 80 ? f2 : fCoarse;
    }

    double refineAt(double fEst, int m) {
        const double T = sr_ / fEst;
        const double L0 = m * T;
        const int W = std::max(static_cast<int>(sr_ * 0.05), static_cast<int>(T * 3.0));
        const int span = std::clamp(static_cast<int>(std::ceil(L0 * 0.025)) + 2, 3, std::max(3, static_cast<int>(T * 0.45)));
        const int Lmax = static_cast<int>(L0) + span + 2;
        static thread_local std::vector<float> x;
        // Isolate the fundamental (3 cascaded band-passes, Q 2.5, ≥ −30 dB at the 2nd harmonic): inharmonic upper
        // partials run sharp on wound strings, and a tuner should read the string's fundamental, like a strobe.
        // Pre-roll ≥ 14 filter time constants so the cascade's start-up ringing has died away.
        const double Q = 2.5, tau = Q / (kPi * fEst);
        const int pre = static_cast<int>(std::min(sr_ * 0.45, 14.0 * tau * sr_));
        const int need = W + Lmax + 2 + pre;
        if (need > histLen_ - 8) return NAN;
        x.resize(static_cast<std::size_t>(need));
        recent(inHist_, histLen_, histPos_, need, 0, x.data());
        if (fEst < sr_ * 0.2) {
            const double w0 = 2 * kPi * fEst / sr_, al = std::sin(w0) / (2 * Q), a0 = 1 + al;
            const double b0 = al / a0, b2 = -al / a0, a1 = -2 * std::cos(w0) / a0, a2 = (1 - al) / a0;
            for (int st = 0; st < 3; ++st) {
                double z1 = 0, z2 = 0;
                for (int i = 0; i < need; ++i) {
                    const double in = x[static_cast<std::size_t>(i)], y = b0 * in + z1;
                    z1 = -a1 * y + z2; z2 = b2 * in - a2 * y;
                    x[static_cast<std::size_t>(i)] = static_cast<float>(y);
                }
            }
        }
        const float* a = x.data() + (need - W);
        auto diff = [&](int L) { double s = 0; const float* bb = a - L; for (int j = 0; j < W; ++j) { const double df = double(a[j]) - bb[j]; s += df * df; } return s; };
        int bestL = -1;
        double bestV = 1e300;
        std::vector<double> v(static_cast<std::size_t>(2 * span + 1));
        const int lo = static_cast<int>(std::lround(L0)) - span;
        for (int i = 0; i <= 2 * span; ++i) {
            const int L = lo + i;
            if (L < 2) { v[static_cast<std::size_t>(i)] = 1e300; continue; }
            v[static_cast<std::size_t>(i)] = diff(L);
            if (v[static_cast<std::size_t>(i)] < bestV) { bestV = v[static_cast<std::size_t>(i)]; bestL = i; }
        }
        if (bestL <= 0 || bestL >= 2 * span) return NAN;
        const double Lf = lo + bestL + parabolic(v[static_cast<std::size_t>(bestL - 1)], v[static_cast<std::size_t>(bestL)], v[static_cast<std::size_t>(bestL + 1)]);
        return sr_ * m / Lf;
    }

    void updateTrack(double t, double raw) {
        const bool was = voiced_;
        if (std::isfinite(raw)) {
            unvoicedHops_ = 0;
            rawHist_.push_back(raw);
            while (rawHist_.size() > 16) rawHist_.pop_front();
            if (!std::isfinite(smooth_) || !was) { smooth_ = raw; pendingCount_ = 0; voicedSince_ = t; }
            else if (std::fabs(raw - smooth_) > 0.55) {
                // jump: needs two hops that agree, so one octave slip can't move the needle
                if (std::isfinite(pendingJump_) && std::fabs(raw - pendingJump_) < 0.3) { if (++pendingCount_ >= 1) { smooth_ = raw; pendingCount_ = 0; pendingJump_ = NAN; } }
                else { pendingJump_ = raw; pendingCount_ = 0; }
                if (std::fabs(raw - smooth_) < 3.5 && std::fabs(raw - smooth_) > 0.55) glideSeen_ = true;
            } else {
                pendingJump_ = NAN;
                const double a = std::fabs(raw - smooth_) > 0.12 ? 0.6 : 0.3;  // fast on glides, calm when settled
                smooth_ += a * (raw - smooth_);
            }
            voiced_ = true;
        } else if (++unvoicedHops_ > 3) {
            voiced_ = false;
            rawHist_.clear();
        }
        // stability: last 12 raw within ±1.2 cents (std)
        stable_ = false;
        if (voiced_ && rawHist_.size() >= 10) {
            double m = 0; for (double r : rawHist_) m += r; m /= rawHist_.size();
            double v = 0; for (double r : rawHist_) v += (r - m) * (r - m);
            stable_ = std::sqrt(v / rawHist_.size()) * 100 < 1.5;
        }
        trail_[static_cast<std::size_t>(trailPos_)] = voiced_ ? static_cast<float>(smooth_) : std::nanf("");
        trailPos_ = (trailPos_ + 1) % kTrail;
        detectBend(t);
    }

    // Bend landing: a held note (plateau), a continuous glide up 0.6–3.4 semitones, a new plateau.
    void detectBend(double t) {
        if (!voiced_) { plateau_ = {}; lastPlateau_ = {}; glideSeen_ = false; return; }
        const double m = smooth_;
        if (std::isfinite(plateau_.center) && std::fabs(m - plateau_.center) < 0.18) {
            plateau_.sum += m; ++plateau_.n; plateau_.center = plateau_.sum / plateau_.n; plateau_.end = t;
            if (plateau_.valid && bend_.t >= plateau_.start && t - plateau_.start < 0.8 && rawHist_.size() >= 4) {
                std::vector<double> r(rawHist_.end() - 4, rawHist_.end());
                std::sort(r.begin(), r.end());
                bend_.cents = (0.5 * (r[1] + r[2]) - bend_.to) * 100.0;
            }
            if (!plateau_.valid && plateau_.end - plateau_.start >= 0.11) {
                plateau_.valid = true;
                if (lastPlateau_.valid && t - lastPlateau_.end < 0.9) {
                    const double iv = plateau_.center - lastPlateau_.center;
                    if (iv > 0.6 && iv < 3.4 && glideSeen_) {
                        const int to = static_cast<int>(std::lround(plateau_.center));
                        bend_ = {to, (plateau_.center - to) * 100.0, lastPlateau_.center, t};
                    }
                }
                glideSeen_ = false;
            }
        } else {
            if (plateau_.valid) lastPlateau_ = plateau_;
            plateau_ = {m, t, t, 1, false, m};
            if (lastPlateau_.valid && m > lastPlateau_.center + 0.15) glideSeen_ = glideSeen_ || (m < lastPlateau_.center + 3.4);
        }
    }

    // ---- display spectra / tone ----------------------------------------------------------------------
    void displaySpectra() {
        static thread_local Frame fi, fo;
        spectrum(*fftDisp_, inHist_, 0, fi);
        spectrum(*fftDisp_, outHist_, 0, fo);
        auto bands = [&](const Frame& f, std::array<float, kBands>& dst, bool smoothIt) {
            for (int b = 0; b < kBands; ++b) {
                const double fl = kBandLo * std::pow(kBandHi / kBandLo, double(b) / kBands);
                const double fh = kBandLo * std::pow(kBandHi / kBandLo, double(b + 1) / kBands);
                const double kl = fl / f.binHz, kh = fh / f.binHz;
                double v;
                if (kh - kl >= 1.0) {
                    v = -140;
                    for (int k = static_cast<int>(std::ceil(kl)); k <= static_cast<int>(kh) && k < static_cast<int>(f.db.size()); ++k) v = std::max(v, double(f.db[static_cast<std::size_t>(k)]));
                } else {
                    const double kc = std::sqrt(fl * fh) / f.binHz;
                    const int k0 = static_cast<int>(kc);
                    const double fr = kc - k0;
                    v = f.db[static_cast<std::size_t>(k0)] * (1 - fr) + f.db[static_cast<std::size_t>(k0 + 1)] * fr;
                }
                v = std::max(v, -120.0);
                float& o = dst[static_cast<std::size_t>(b)];
                if (!smoothIt) o = static_cast<float>(v);
                else o += static_cast<float>((v > o ? 0.55 : 0.16) * (v - o));
            }
        };
        bands(fi, bandIn_, true);
        bands(fo, bandOut_, true);
        if (signal_) {
            const double a = 0.02;
            slowOutW_ += hop_ / sr_;
            for (int b = 0; b < kBands; ++b) {
                float& s = slowOut_[static_cast<std::size_t>(b)];
                if (s <= -119) s = bandOut_[static_cast<std::size_t>(b)];
                else s += static_cast<float>(a * (bandOut_[static_cast<std::size_t>(b)] - s));
            }
            if (hops_ % 10 == 0) describeTone();
        }
    }

    void describeTone() {
        auto fAt = [&](int b) { return kBandLo * std::pow(kBandHi / kBandLo, (b + 0.5) / kBands); };
        double ref = 0; int n = 0;
        for (int b = 0; b < kBands; ++b) { const double f = fAt(b); if (f >= 300 && f <= 2000) { ref += slowOut_[static_cast<std::size_t>(b)]; ++n; } }
        if (!n) return;
        ref /= n;
        double best = -1e9;
        for (int b = 0; b < kBands; ++b) { const double f = fAt(b); if (f >= 250 && f <= 3000 && slowOut_[static_cast<std::size_t>(b)] > best) { best = slowOut_[static_cast<std::size_t>(b)]; toneMid_ = f; } }
        toneLow_ = kBandLo;
        for (int b = kBands - 1; b >= 0; --b) { const double f = fAt(b); if (f < 250 && slowOut_[static_cast<std::size_t>(b)] < ref - 12) { toneLow_ = f; break; } }
        toneTop_ = kBandHi;
        for (int b = 0; b < kBands; ++b) {
            const double f = fAt(b);
            if (f > 2000 && slowOut_[static_cast<std::size_t>(b)] < ref - 15) { toneTop_ = f; break; }
        }
        double lo = 0, hi = 0; int nl = 0, nh = 0;
        for (int b = 0; b < kBands; ++b) { const double f = fAt(b); if (f >= 100 && f < 400) { lo += slowOut_[static_cast<std::size_t>(b)]; ++nl; } if (f >= 2000 && f < 6000) { hi += slowOut_[static_cast<std::size_t>(b)]; ++nh; } }
        toneTilt_ = (nl && nh) ? hi / nh - lo / nl : 0;
    }

    // ---- chroma, key, chord, hum, poly ---------------------------------------------------------------
    struct Peak { double f, amp, db; int pc; double dev, w; };

    void harmonic(double t) {
        static thread_local Frame fc;
        spectrum(*fftChroma_, inHist_, 0, fc);
        const double dt = 2.0 * hop_ / sr_;
        if (!signal_) {
            detectHum(fc, t);
            singleNote_ = -1;
        }
        // peaks
        double gmax = -140;
        const int k0 = std::max(2, static_cast<int>(55.0 / fc.binHz)), k1 = std::min(static_cast<int>(fc.db.size()) - 3, static_cast<int>(3200.0 / fc.binHz));
        for (int k = k0; k <= k1; ++k) gmax = std::max(gmax, double(fc.db[static_cast<std::size_t>(k)]));
        std::vector<Peak> pk;
        if (signal_) {
            for (int k = k0; k <= k1; ++k) {
                const float v = fc.db[static_cast<std::size_t>(k)];
                if (v < gmax - 42 || v < -95) continue;
                if (!(v > fc.db[static_cast<std::size_t>(k - 1)] && v >= fc.db[static_cast<std::size_t>(k + 1)] && v > fc.db[static_cast<std::size_t>(k - 2)] && v >= fc.db[static_cast<std::size_t>(k + 2)])) continue;
                const double off = parabolic(fc.db[static_cast<std::size_t>(k - 1)], v, fc.db[static_cast<std::size_t>(k + 1)]);
                const double f = (k + off) * fc.binHz;
                const double midi = 69 + 12 * std::log2(f / a4_);
                const int r = static_cast<int>(std::lround(midi));
                pk.push_back({f, std::pow(10.0, v / 20.0), double(v), ((r % 12) + 12) % 12, midi - r, 1.0});
            }
            std::sort(pk.begin(), pk.end(), [](const Peak& a, const Peak& b) { return a.amp > b.amp; });
            if (pk.size() > 48) pk.resize(48);
            std::sort(pk.begin(), pk.end(), [](const Peak& a, const Peak& b) { return a.f < b.f; });
            // harmonic suppression: partials 3,5,6,7,9,10 of a stronger-or-comparable lower peak
            static const int H[] = {3, 5, 6, 7, 9, 10, 12, 15};
            for (std::size_t i = 0; i < pk.size(); ++i)
                for (std::size_t j = 0; j < i; ++j) {
                    if (pk[j].amp < 0.18 * pk[i].amp) continue;
                    const double r = pk[i].f / pk[j].f;
                    bool hit = false;
                    for (int h : H) if (std::fabs(1200 * std::log2(r / h)) < 30) { hit = true; break; }
                    if (hit) { pk[i].w *= 0.15; break; }
                }
        }
        std::array<double, 12> frame{};
        double tot = 0;
        int bass = -1;
        double maxAmp = 0;
        for (auto& p : pk) maxAmp = std::max(maxAmp, p.amp * p.w);
        for (auto& p : pk) {
            if (std::fabs(p.dev) > 0.4) continue;
            const double w = std::sqrt(p.amp) * p.w * (1.0 - 1.6 * std::max(0.0, std::fabs(p.dev) - 0.15));
            frame[static_cast<std::size_t>(p.pc)] += w;
            tot += w;
            if (bass < 0 && p.f < 420 && p.amp * p.w >= 0.22 * maxAmp) bass = p.pc;
        }
        if (nnOk_) noteKey(t, dt);
        if (signal_ && tot > 0) {
            for (auto& v : frame) v /= tot;
            for (int i = 0; i < 12; ++i) chromaShort_[static_cast<std::size_t>(i)] += 0.4 * (frame[static_cast<std::size_t>(i)] - chromaShort_[static_cast<std::size_t>(i)]);
            const double decay = std::exp(-dt / KP().tau);
            for (int i = 0; i < 12; ++i) keyHist_[static_cast<std::size_t>(i)] = keyHist_[static_cast<std::size_t>(i)] * decay + frame[static_cast<std::size_t>(i)] * dt;
            // the lowest strong note leans towards the tonic: give it extra weight in the key histogram
            if (bass >= 0) keyHist_[static_cast<std::size_t>(bass)] += KP().bass * dt;
            // tonic evidence: where the bass sits (and the chord roots it implies), over the same memory
            for (auto& v : bassHist_) v *= decay;
            if (bass >= 0) bassHist_[static_cast<std::size_t>(bass)] += dt;
            if (chord_.root >= 0) bassHist_[static_cast<std::size_t>(chord_.root)] += 0.5 * dt;
            // Harmonic function: each sounding chord votes for the keys it plays a strong role in (I, V, IV, vi…),
            // weighted by how long it lasts. A key is heard through its chords far more than its note counts.
            for (auto& v : chordKey_) v *= decay;
            for (auto& v : cadence_) v *= decay;
            chordKeyW_ *= decay;
            // Only real chord playing votes: ≥ 3 strong pitch classes, a chord held ≥ minAge (solo lines' passing dyads don't count).
            int strong = 0; double mxc = 0;
            for (double v : chromaShort_) mxc = std::max(mxc, v);
            for (double v : chromaShort_) strong += v > KP().strong * mxc;
            if (chord_.root >= 0 && t - chordSeenT_ < 0.3 && t - chord_.start >= KP().minAge && strong >= 3) { addChordFunction(chord_.root, chord_.quality, dt); chordKeyW_ += dt; }
            keyEvidence_ = keyEvidence_ * decay + dt;
            if (!nnOk_) estimateKey(dt);
            estimateChord(t, bass);
        } else {
            for (auto& v : chromaShort_) v *= 0.85;
            if (chord_.root >= 0) chord_.dur = t - chord_.start;
        }
        // Polyphonic readings only from windows that start after the latest attack: a window straddling a new
        // strum mixes the old ringing strings with the new ones.
        if (!strings_.empty() && signal_ && t - onsetT_ > double(nPoly_ + polyR_) / sr_) polyTune(t);
    }

    // Tuned on 120 GuitarSet excerpts (tests/listen_bench --key-eval): ~0.5 key changes per 20–30 s clip (was 2.3)
    // and the first key after ~6 s. LK_* environment variables override them for experiments.
    struct KeyParams { double tau = 35, margin = 0.06, hold = 4.0, first = 4.0, bass = 0, relMargin = 0.10, ton = 0.2, cw = 0, cfull = 6, strong = 0.3, minAge = 0.4, cad = 0,
                       blendLo = 0.4, blendHi = 0.7, zMargin = 0.5, zRel = 0.8, leadTau = 5.0, settleS = 20, mEarly = 0.08, mLate = 0.25, relMul = 1.0; int prof = 0; bool raw = false; };
    static const KeyParams& KP() {
        static const KeyParams k = [] {
            KeyParams p;
            auto env = [](const char* n, double d) { const char* v = std::getenv(n); return v ? std::atof(v) : d; };
            p.tau = env("LK_TAU", p.tau); p.margin = env("LK_MARGIN", p.margin); p.hold = env("LK_HOLD", p.hold);
            p.first = env("LK_FIRST", p.first); p.bass = env("LK_BASS", p.bass); p.relMargin = env("LK_REL", p.relMargin); p.ton = env("LK_TON", p.ton); p.prof = static_cast<int>(env("LK_PROF", p.prof)); p.cw = env("LK_CW", p.cw); p.cfull = env("LK_CFULL", p.cfull); p.strong = env("LK_STRONG", p.strong); p.minAge = env("LK_MINAGE", p.minAge); p.cad = env("LK_CAD", p.cad);
            p.blendLo = env("LK_BLO", p.blendLo); p.blendHi = env("LK_BHI", p.blendHi); p.zMargin = env("LK_ZM", p.zMargin); p.zRel = env("LK_ZREL", p.zRel); p.raw = env("LK_RAW", 0) > 0;
            p.leadTau = env("LK_LTAU", p.leadTau); p.settleS = env("LK_SETTLE", p.settleS); p.mEarly = env("LK_ME", p.mEarly); p.mLate = env("LK_ML", p.mLate); p.relMul = env("LK_RELM", p.relMul);
            return p;
        }();
        return k;
    }
    // ---- key from transcribed notes ----------------------------------------------------------------------
    // Validated on GuitarSet's held-out players (tests/listen_bench --key-eval2, and the symbolic study that
    // picked it): duration-weighted Temperley profile for single-note playing, harmonic function of chords
    // recognised in windows where ≥ 3 notes sound together for chordal playing, blended by how chordal the
    // playing is (z-scored so the two evidence types share a scale).
    void noteKey(double t, double dt) {
        const double decay = std::exp(-dt / KP().tau);
        int n = 0, low = 999;
        std::array<double, 12> now{};
        for (int p = 0; p < 128; ++p) {
            const double on = sounding_[static_cast<std::size_t>(p)];
            if (on < 0) continue;
            if (t - on > 6.0) { sounding_[static_cast<std::size_t>(p)] = -1.0; continue; }  // stuck note guard
            ++n; now[static_cast<std::size_t>(p % 12)] += 1.0; low = std::min(low, p);
        }
        for (auto& v : noteHist_) v *= decay;
        polyT_ *= decay; soundT_ *= decay;
        for (auto& v : chordFn_) v *= decay;
        chordFnW_ *= decay;
        for (int i = 0; i < 12; ++i) noteHist_[static_cast<std::size_t>(i)] += now[static_cast<std::size_t>(i)] * dt;
        if (n > 0) { soundT_ += dt; noteEvidence_ = noteEvidence_ * decay + dt; }
        if (n >= 3) polyT_ += dt;
        // half-second chord windows
        for (int i = 0; i < 12; ++i) winH_[static_cast<std::size_t>(i)] += now[static_cast<std::size_t>(i)] * dt;
        if (low < 999 && (winLow_ < 0 || low < winLow_)) winLow_ = low;
        winPoly_ = std::max(winPoly_, n);
        winT_ += dt;
        if (winT_ >= 0.5) {
            double tot = 0, mx = 0; int act = 0;
            for (double v : winH_) { tot += v; mx = std::max(mx, v); }
            for (double v : winH_) act += v > 0.15 * mx;
            if (winPoly_ >= 3 && tot > 0 && act >= 2) {
                static const int TQ[9][5] = {{0, 4, 7, -1, -1}, {0, 3, 7, -1, -1}, {0, 4, 7, 10, -1}, {0, 4, 7, 11, -1}, {0, 3, 7, 10, -1},
                                             {0, 3, 6, -1, -1}, {0, 3, 6, 10, -1}, {0, 5, 7, -1, -1}, {0, 2, 7, -1, -1}};
                static const int TC[9] = {0, 1, 0, 0, 1, 3, 3, 0, 0};   // class: 0 M, 1 m, 3 d
                double best = -1e9; int br = -1, bq = 0;
                for (int r = 0; r < 12; ++r)
                    for (int q = 0; q < 9; ++q) {
                        double sc = 0; int len = 0;
                        for (int j = 0; j < 5 && TQ[q][j] >= 0; ++j) { sc += winH_[static_cast<std::size_t>((r + TQ[q][j]) % 12)]; ++len; }
                        sc = sc / tot - 0.12 * len / 3.0 + ((winLow_ >= 0 && winLow_ % 12 == r) ? 0.1 : 0.0);
                        if (sc > best) { best = sc; br = r; bq = q; }
                    }
                if (best > 0.5) {
                    const int qc = TC[bq];
                    for (int k = 0; k < 12; ++k) {
                        const int deg = (br - k + 12) % 12;
                        chordFn_[static_cast<std::size_t>(k)] += winT_ * fnMaj(deg, qc == 3 ? 2 : qc);
                        chordFn_[static_cast<std::size_t>(12 + k)] += winT_ * fnMin(deg, qc == 3 ? 2 : qc);
                    }
                    chordFnW_ += winT_;
                }
            }
            winH_.fill(0.0); winLow_ = -1; winPoly_ = 0; winT_ = 0;
        }
        if (noteEvidence_ < 0.5) return;
        // scores
        static const double MAJ[12] = {0.748, 0.060, 0.488, 0.082, 0.670, 0.460, 0.096, 0.715, 0.104, 0.366, 0.057, 0.400};
        static const double MIN[12] = {0.712, 0.084, 0.474, 0.618, 0.049, 0.460, 0.105, 0.747, 0.404, 0.067, 0.133, 0.330};
        std::array<double, 24> ks{}, cf{}, sc{};
        auto corr = [&](const double* prof, int root) {
            double mx = 0, mp = 0;
            for (int i = 0; i < 12; ++i) { mx += noteHist_[static_cast<std::size_t>(i)]; mp += prof[i]; }
            mx /= 12; mp /= 12;
            double sxy = 0, sxx = 0, syy = 0;
            for (int i = 0; i < 12; ++i) { const double x = noteHist_[static_cast<std::size_t>((i + root) % 12)] - mx, y = prof[i] - mp; sxy += x * y; sxx += x * x; syy += y * y; }
            return (sxx > 0 && syy > 0) ? sxy / std::sqrt(sxx * syy) : 0.0;
        };
        for (int k = 0; k < 12; ++k) { ks[static_cast<std::size_t>(k)] = corr(MAJ, k); ks[static_cast<std::size_t>(12 + k)] = corr(MIN, k); }
        auto zs = [](std::array<double, 24>& v) { double m = 0, q = 0; for (double x : v) m += x; m /= 24; for (double x : v) q += (x - m) * (x - m); q = std::sqrt(q / 24); if (q < 1e-9) q = 1; for (double& x : v) x = (x - m) / q; };
        zs(ks);
        const bool haveChords = chordFnW_ > 0.25;
        if (haveChords) { for (int k = 0; k < 24; ++k) cf[static_cast<std::size_t>(k)] = chordFn_[static_cast<std::size_t>(k)] / chordFnW_; zs(cf); }
        const double chordal = soundT_ > 0 ? polyT_ / soundT_ : 0.0;
        const double a = haveChords ? std::clamp((chordal - KP().blendLo) / (KP().blendHi - KP().blendLo), 0.0, 1.0) : 0.0;
        for (int k = 0; k < 24; ++k) sc[static_cast<std::size_t>(k)] = (1 - a) * ks[static_cast<std::size_t>(k)] + a * cf[static_cast<std::size_t>(k)];
        chordal_ = chordal;
        decideKey(sc, dt, noteEvidence_);
    }
    static double fnMaj(int deg, int c) {
        if (c == 0) return deg == 0 ? 3.0 : deg == 7 ? 2.0 : deg == 5 ? 1.6 : deg == 10 ? 0.4 : -0.5;
        if (c == 1) return deg == 9 ? 1.2 : deg == 2 ? 1.2 : deg == 4 ? 0.8 : deg == 5 ? 0.3 : -0.5;
        return deg == 11 ? 0.6 : -0.5;
    }
    static double fnMin(int deg, int c) {
        if (c == 0) return deg == 7 ? 1.6 : deg == 3 ? 1.0 : deg == 8 ? 1.2 : deg == 10 ? 1.0 : deg == 5 ? 0.4 : -0.5;
        if (c == 1) return deg == 0 ? 3.0 : deg == 5 ? 1.6 : deg == 7 ? 1.0 : -0.5;
        return deg == 2 ? 0.8 : -0.5;
    }
    // Sticky decision over blended scores (z units): first key after `first` s of notes; a challenger must lead by
    // `zMargin` (more for the relative major/minor) continuously for `hold` seconds.
    void decideKey(const std::array<double, 24>& sc, double dt, double evidence) {
        int best = 0, second = -1;
        for (int k = 1; k < 24; ++k) if (sc[static_cast<std::size_t>(k)] > sc[static_cast<std::size_t>(best)]) best = k;
        for (int k = 0; k < 24; ++k) if (k != best && (second < 0 || sc[static_cast<std::size_t>(k)] > sc[static_cast<std::size_t>(second)])) second = k;
        if (KP().raw) { key_ = best; keyAlt_ = second; keyEvidence_ = evidence; return; }
        // Each rival's lead over the shown key, smoothed over `leadTau` seconds; switching is easy while the
        // picture is forming (< settleS of notes) and needs a clear, sustained lead once it has settled.
        if (key_ < 0) { if (evidence > KP().first) { key_ = best; lead_.fill(0.0); } }
        else {
            const double k = std::min(1.0, dt / KP().leadTau);
            for (int i = 0; i < 24; ++i) lead_[static_cast<std::size_t>(i)] += (sc[static_cast<std::size_t>(i)] - sc[static_cast<std::size_t>(key_)] - lead_[static_cast<std::size_t>(i)]) * k;
            int rb = -1;
            for (int i = 0; i < 24; ++i) if (i != key_ && (rb < 0 || lead_[static_cast<std::size_t>(i)] > lead_[static_cast<std::size_t>(rb)])) rb = i;
            const bool relative = rb >= 0 && (rb % 12) == ((key_ % 12) + (key_ >= 12 ? 3 : 9)) % 12 && (rb >= 12) != (key_ >= 12);
            const double m = (evidence < KP().settleS ? KP().mEarly : KP().mLate) * (relative ? KP().relMul : 1.0);
            if (rb >= 0 && lead_[static_cast<std::size_t>(rb)] > m) { key_ = rb; lead_.fill(0.0); }
        }
        keyEvidence_ = evidence;
        if (key_ < 0) return;
        double z = 0;
        for (int k = 0; k < 24; ++k) z += std::exp((sc[static_cast<std::size_t>(k)] - sc[static_cast<std::size_t>(key_)]) / 0.35);
        keyConf_ = std::clamp((1.0 / z) * (0.55 + 0.45 * std::min(1.0, evidence / 10.0)), 0.0, 1.0);
        keyAlt_ = best != key_ ? best : second;
    }

    // Diatonic chord-function weights by scale degree of the chord root and triad class (M major, m minor, d diminished).
    void addChordFunction(int root, int quality, double dt) {
        static const int QC[] = {0, 1, 2, 0, 0, 1, 2, 2, 3, 4, 3, 0, 0, 1, 3, 0, 1};   // 0 M, 1 m, 2 open (5/sus), 3 d, 4 aug
        const int qc = (quality >= 0 && quality < 17) ? QC[quality] : 4;
        if (qc == 4) return;
        auto fmaj = [](int deg, int c) {
            if (c == 0) return deg == 0 ? 3.0 : deg == 7 ? 2.0 : deg == 5 ? 1.6 : deg == 10 ? 0.4 : -0.5;
            if (c == 1) return deg == 9 ? 1.2 : deg == 2 ? 1.2 : deg == 4 ? 0.8 : deg == 5 ? 0.3 : -0.5;
            return deg == 11 ? 0.6 : -0.5;
        };
        auto fmin = [](int deg, int c) {
            if (c == 0) return deg == 7 ? 1.6 : deg == 3 ? 1.0 : deg == 8 ? 1.2 : deg == 10 ? 1.0 : deg == 5 ? 0.4 : -0.5;
            if (c == 1) return deg == 0 ? 3.0 : deg == 5 ? 1.6 : deg == 7 ? 1.0 : -0.5;
            return deg == 2 ? 0.8 : -0.5;
        };
        for (int k = 0; k < 12; ++k) {
            const int deg = (root - k + 12) % 12;
            double M, m;
            if (qc == 2) { M = 0.7 * std::max(fmaj(deg, 0), fmaj(deg, 1)); m = 0.7 * std::max(fmin(deg, 0), fmin(deg, 1)); }   // power/sus: no third
            else { M = fmaj(deg, qc == 3 ? 2 : qc); m = fmin(deg, qc == 3 ? 2 : qc); }
            chordKey_[static_cast<std::size_t>(k)] += M * dt;
            chordKey_[static_cast<std::size_t>(12 + k)] += m * dt;
        }
    }

    void estimateKey(double dt) {
        // Key profiles (major, minor): Temperley/Kostka–Payne, Krumhansl–Kessler, Albrecht–Shanahan, Bellman–Budge.
        static const double PROF[4][2][12] = {
            {{0.748, 0.060, 0.488, 0.082, 0.670, 0.460, 0.096, 0.715, 0.104, 0.366, 0.057, 0.400}, {0.712, 0.084, 0.474, 0.618, 0.049, 0.460, 0.105, 0.747, 0.404, 0.067, 0.133, 0.330}},
            {{6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88}, {6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17}},
            {{0.238, 0.006, 0.111, 0.006, 0.137, 0.094, 0.016, 0.214, 0.009, 0.080, 0.008, 0.081}, {0.220, 0.006, 0.104, 0.123, 0.019, 0.103, 0.012, 0.214, 0.062, 0.022, 0.061, 0.052}},
            {{16.80, 0.86, 12.95, 1.41, 13.49, 11.93, 1.25, 20.28, 1.80, 8.04, 0.62, 10.57}, {18.16, 0.69, 12.99, 13.34, 1.07, 11.15, 1.38, 21.07, 7.49, 1.53, 0.92, 10.21}},
        };
        const int pi = std::clamp(KP().prof, 0, 3);
        const double* MAJ = PROF[pi][0];
        const double* MIN = PROF[pi][1];
        auto corr = [&](const double* prof, int root) {
            double mx = 0, mp = 0;
            for (int i = 0; i < 12; ++i) { mx += keyHist_[static_cast<std::size_t>(i)]; mp += prof[i]; }
            mx /= 12; mp /= 12;
            double sxy = 0, sxx = 0, syy = 0;
            for (int i = 0; i < 12; ++i) {
                const double x = keyHist_[static_cast<std::size_t>((i + root) % 12)] - mx, y = prof[i] - mp;
                sxy += x * y; sxx += x * x; syy += y * y;
            }
            return (sxx > 0 && syy > 0) ? sxy / std::sqrt(sxx * syy) : 0.0;
        };
        std::array<double, 24> r{};
        for (int k = 0; k < 12; ++k) { r[static_cast<std::size_t>(k)] = corr(MAJ, k); r[static_cast<std::size_t>(12 + k)] = corr(MIN, k); }
        if (KP().cw > 0 && chordKeyW_ > 0.5) {
            const double conf = std::min(1.0, chordKeyW_ / KP().cfull);   // trust chords more as more of them are heard
            for (int k = 0; k < 24; ++k) r[static_cast<std::size_t>(k)] += KP().cw * conf * chordKey_[static_cast<std::size_t>(k)] / chordKeyW_;
        }
        if (KP().cad > 0) {
            double cm = 0; for (double v : cadence_) cm = std::max(cm, v);
            if (cm > 0) for (int k = 0; k < 24; ++k) r[static_cast<std::size_t>(k)] += KP().cad * std::min(1.0, cm / 2.0) * cadence_[static_cast<std::size_t>(k)] / cm;
        }
        if (KP().ton > 0) {
            double bm = 1e-12; for (double v : bassHist_) bm = std::max(bm, v);
            for (int k = 0; k < 24; ++k) r[static_cast<std::size_t>(k)] += KP().ton * bassHist_[static_cast<std::size_t>(k % 12)] / bm;
        }
        int best = 0, second = -1;
        for (int k = 1; k < 24; ++k) if (r[static_cast<std::size_t>(k)] > r[static_cast<std::size_t>(best)]) best = k;
        for (int k = 0; k < 24; ++k) if (k != best && (second < 0 || r[static_cast<std::size_t>(k)] > r[static_cast<std::size_t>(second)])) second = k;
        // hysteresis: a new key must beat the current one by a margin for ~1.5 s
        // Hysteresis: a challenger must beat the shown key by a margin, continuously, for `hold` seconds.
        // Relative major/minor share every note, so flipping between them needs its own (larger) margin.
        if (key_ < 0) { if (keyEvidence_ > KP().first) key_ = best; }
        else if (best != key_) {
            const bool relative = (best % 12) == ((key_ % 12) + (key_ >= 12 ? 3 : 9)) % 12 && (best >= 12) != (key_ >= 12);
            const double need = relative ? KP().relMargin : KP().margin;
            if (r[static_cast<std::size_t>(best)] > r[static_cast<std::size_t>(key_)] + need) {
                if (keyCand_ != best) { keyCand_ = best; keyCandT_ = 0; }
                keyCandT_ += dt;
                if (keyCandT_ >= KP().hold) { key_ = best; keyCandT_ = 0; keyCand_ = -1; }
            } else keyCandT_ = std::max(0.0, keyCandT_ - dt);
        } else { keyCandT_ = 0; keyCand_ = -1; }
        if (key_ < 0) return;
        double z = 0;
        for (int k = 0; k < 24; ++k) z += std::exp((r[static_cast<std::size_t>(k)] - r[static_cast<std::size_t>(key_)]) / 0.045);
        const double evid = std::min(1.0, keyEvidence_ / 8.0);
        keyConf_ = std::clamp((1.0 / z) * (0.55 + 0.45 * evid) + 0.0, 0.0, 1.0);
        keyAlt_ = best != key_ ? best : second;
    }

    struct Tmpl { int q; std::vector<int> iv; double prior; };
    static const std::vector<Tmpl>& templates() {
        static const std::vector<Tmpl> T = {
            {0, {0, 4, 7}, 0.0}, {1, {0, 3, 7}, 0.0}, {2, {0, 7}, -0.02}, {3, {0, 4, 7, 10}, -0.025}, {4, {0, 4, 7, 11}, -0.025},
            {5, {0, 3, 7, 10}, -0.025}, {6, {0, 2, 7}, -0.05}, {7, {0, 5, 7}, -0.05}, {8, {0, 3, 6}, -0.06}, {9, {0, 4, 8}, -0.08},
            {10, {0, 3, 6, 10}, -0.07}, {11, {0, 2, 4, 7}, -0.06}, {12, {0, 4, 7, 9}, -0.07}, {13, {0, 3, 7, 9}, -0.08},
            {14, {0, 3, 6, 9}, -0.09}, {15, {0, 2, 4, 7, 10}, -0.08}, {16, {0, 2, 3, 7, 10}, -0.08},
        };
        return T;
    }

    void estimateChord(double t, int bass) {
        const auto& c = chromaShort_;
        double norm = 0, mx = 0;
        int top = 0;
        for (int i = 0; i < 12; ++i) { norm += c[static_cast<std::size_t>(i)] * c[static_cast<std::size_t>(i)]; if (c[static_cast<std::size_t>(i)] > mx) { mx = c[static_cast<std::size_t>(i)]; top = i; } }
        norm = std::sqrt(norm);
        if (norm <= 0) return;
        int active = 0;
        for (double v : c) if (v > 0.35 * mx) ++active;
        // A single sustained pitch class (plus octaves) is a note, not a chord.
        double second = 0;
        for (int i = 0; i < 12; ++i) if (i != top) second = std::max(second, c[static_cast<std::size_t>(i)]);
        (void)active;
        singleNote_ = second < 0.22 * mx ? top : -1;
        Chord bestC;
        double best = -1e9;
        for (const auto& tm : templates())
            for (int root = 0; root < 12; ++root) {
                std::array<double, 12> v{};
                for (std::size_t j = 0; j < tm.iv.size(); ++j) v[static_cast<std::size_t>((root + tm.iv[j]) % 12)] = j == 0 ? 1.15 : (tm.iv[j] == 7 ? 0.9 : 1.0);
                double dot = 0, vn = 0;
                for (int i = 0; i < 12; ++i) { dot += v[static_cast<std::size_t>(i)] * c[static_cast<std::size_t>(i)]; vn += v[static_cast<std::size_t>(i)] * v[static_cast<std::size_t>(i)]; }
                double s = dot / (std::sqrt(vn) * norm) + tm.prior;
                // every chord tone must actually be there
                double weakest = 1e9;
                for (int iv : tm.iv) weakest = std::min(weakest, c[static_cast<std::size_t>((root + iv) % 12)] / mx);
                if (weakest < 0.12) s -= 0.25;
                if (bass >= 0) { if (bass == root) s += 0.06; else if (std::find(tm.iv.begin(), tm.iv.end(), (bass - root + 12) % 12) == tm.iv.end()) s -= 0.05; }
                if (s > best) { best = s; bestC = {root, tm.q, bass >= 0 && std::find(tm.iv.begin(), tm.iv.end(), (bass - root + 12) % 12) != tm.iv.end() ? bass : root, s, t, 0}; }
            }
        if (singleNote_ >= 0 || best < 0.72) return;  // not chordal enough: keep the last chord
        if (bestC.quality == 2 && bass != bestC.root) return;  // a melody's two overlapping notes aren't a power chord
        chordSeenT_ = t;
        const bool same = bestC.root == candChord_.root && bestC.quality == candChord_.quality && bestC.bass == candChord_.bass;
        candChord_ = bestC;
        candCount_ = same ? candCount_ + 1 : 0;
        const bool cur = bestC.root == chord_.root && bestC.quality == chord_.quality && bestC.bass == chord_.bass;
        if (cur) { chord_.conf = best; chord_.dur = t - chord_.start; return; }
        if (candCount_ >= 6) {   // ≈ 250 ms of agreement
            if (chord_.root >= 0) {
                chord_.dur = t - chord_.start;
                if (chord_.dur >= 0.25) {
                    if (!prog_.empty() && prog_.back().root == chord_.root && prog_.back().quality == chord_.quality && prog_.back().bass == chord_.bass) prog_.back().dur += chord_.dur;
                    else prog_.push_back(chord_);
                    if (prog_.size() > 8) prog_.erase(prog_.begin());
                }
            }
            // Cadence: a major/dominant chord resolving down a fifth (V→I, V7→i) marks its target as home.
            if (chord_.root >= 0 && bestC.root >= 0 && chord_.root == (bestC.root + 7) % 12 && chord_.dur >= 0.3) {
                static const int DOM[] = {1, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0};
                const bool minorTarget = bestC.quality == 1 || bestC.quality == 5 || bestC.quality == 13 || bestC.quality == 16;
                if (chord_.quality >= 0 && chord_.quality < 17 && DOM[chord_.quality]) cadence_[static_cast<std::size_t>(bestC.root + (minorTarget ? 12 : 0))] += 1.0;
            }
            chord_ = bestC;
            chord_.start = t;
        }
    }

    void detectHum(const Frame& f, double t) {
        auto prominence = [&](double hz, double& level) {
            const int k = static_cast<int>(std::lround(hz / f.binHz));
            double v = -140;
            for (int j = k - 1; j <= k + 1; ++j) v = std::max(v, double(f.db[static_cast<std::size_t>(j)]));
            std::vector<float> nb;
            for (int j = k - 14; j <= k + 14; ++j) if (std::abs(j - k) > 3 && j > 0) nb.push_back(f.db[static_cast<std::size_t>(j)]);
            std::nth_element(nb.begin(), nb.begin() + static_cast<long>(nb.size() / 2), nb.end());
            level = v;
            return v - nb[nb.size() / 2];
        };
        for (double base : {50.0, 60.0}) {
            double lvl = 0, l2 = 0, l3 = 0;
            const double p1 = prominence(base, lvl), p2 = prominence(2 * base, l2), p3 = prominence(3 * base, l3);
            const double p = std::max({p1, p2, p3});
            const double L = std::max({lvl, l2, l3});
            if (p > 12 && L > -100 && (p1 > 8 ? 1 : 0) + (p2 > 8 ? 1 : 0) + (p3 > 8 ? 1 : 0) >= 2) { humHz_ = base; humDb_ = L; humT_ = t; }
        }
    }

    // Polyphonic: every configured string from one long FFT pair (phase vocoder per partial).
    void polyTune(double t) {
        static thread_local Frame A, B;
        spectrum(*fftPoly_, inHist_, polyR_, A);
        spectrum(*fftPoly_, inHist_, 0, B);
        const double binHz = B.binHz;
        const int N = fftPoly_->size();
        double gmax = -140;
        for (std::size_t k = static_cast<std::size_t>(std::max(1.0, 25.0 / binHz)); k < B.db.size() && k * binHz < 5000; ++k) gmax = std::max(gmax, double(B.db[k]));
        const std::size_t S = strings_.size();
        std::vector<double> targetHz(S);
        for (std::size_t s = 0; s < S; ++s) targetHz[s] = midiToHz(strings_[s], a4_);

        struct PeakHit { bool ok = false; double hz = 0, db = -140, prom = 0; };
        auto findPeak = [&](double centerHz, double tolCents) {
            PeakHit h;
            const double lo = centerHz * std::pow(2.0, -tolCents / 1200), hi = centerHz * std::pow(2.0, tolCents / 1200);
            const int k0 = std::max(2, static_cast<int>(std::floor(lo / binHz))), k1 = std::min(static_cast<int>(B.db.size()) - 3, static_cast<int>(std::ceil(hi / binHz)));
            int kb = -1;
            double vb = -140;
            for (int k = k0; k <= k1; ++k) {
                const double v = B.db[static_cast<std::size_t>(k)];
                if (v > vb && v >= B.db[static_cast<std::size_t>(k - 1)] && v >= B.db[static_cast<std::size_t>(k + 1)]) { vb = v; kb = k; }
            }
            if (kb < 0) return h;
            // prominence over the local median (±16 bins, excluding the main lobe)
            std::vector<float> nb;
            for (int j = kb - 16; j <= kb + 16; ++j) if (std::abs(j - kb) > 3 && j > 0 && j < static_cast<int>(B.db.size())) nb.push_back(B.db[static_cast<std::size_t>(j)]);
            std::nth_element(nb.begin(), nb.begin() + static_cast<long>(nb.size() / 2), nb.end());
            h.prom = vb - nb[nb.size() / 2];
            h.db = vb;
            if (h.prom < 10 || vb < gmax - 48 || vb < -100) return h;
            // phase vocoder: phase advance over polyR_ samples
            const double ph = std::arg(B.X[static_cast<std::size_t>(kb)]) - std::arg(A.X[static_cast<std::size_t>(kb)]);
            double dev = ph - 2 * kPi * kb * polyR_ / N;
            dev = dev - 2 * kPi * std::round(dev / (2 * kPi));
            const double kf = kb + dev * N / (2 * kPi * polyR_);
            if (std::fabs(kf - kb) > 1.0) return h;  // ambiguous (two partials in one lobe): reject
            h.hz = kf * binHz;
            h.ok = h.hz >= lo && h.hz <= hi;
            return h;
        };
        // presence: a prominent fundamental (or 2nd harmonic for the lowest strings)
        std::vector<char> present(S, 0);
        std::vector<PeakHit> fund(S);
        for (std::size_t s = 0; s < S; ++s) {
            fund[s] = findPeak(targetHz[s], 90);
            if (fund[s].ok && fund[s].prom > 14) present[s] = 1;
            else if (targetHz[s] < 70) { auto h2 = findPeak(2 * targetHz[s], 90); if (h2.ok && h2.prom > 16) present[s] = 1; }
        }
        int nPresent = 0;
        for (std::size_t s = 0; s < S; ++s) {
            if (!present[s]) continue;
            struct E { double cents, w; int k; };
            std::vector<E> est;
            static const double HW[] = {1.0, 0.75, 0.55, 0.4, 0.3};
            for (int k = 1; k <= 5; ++k) {
                const double fk = k * targetHz[s];
                if (fk > 4500) break;
                double pen = 1.0;
                for (std::size_t o = 0; o < S; ++o) {
                    if (o == s || !present[o]) continue;
                    for (int j = 1; j <= 16; ++j) {
                        const double fj = j * targetHz[o];
                        if (std::fabs(1200 * std::log2(fk / fj)) < 45) pen = std::min(pen, j <= 4 ? 0.06 : j <= 8 ? 0.3 : 0.6);
                    }
                }
                const PeakHit h = k == 1 ? fund[s] : findPeak(fk, 70);
                if (!h.ok) continue;
                const double cents = 1200 * std::log2(h.hz / fk);
                const double w = HW[k - 1] * pen * std::clamp((h.prom - 6) / 20.0, 0.1, 1.5);
                est.push_back({cents, w, k});
            }
            if (est.empty()) continue;
            // weighted median, then the estimates near it
            std::sort(est.begin(), est.end(), [](const E& a, const E& b) { return a.cents < b.cents; });
            double W = 0; for (auto& e : est) W += e.w;
            double acc = 0, med = est.front().cents;
            for (auto& e : est) { acc += e.w; if (acc >= W / 2) { med = e.cents; break; } }
            std::vector<E> near;
            for (auto& e : est) if (std::fabs(e.cents - med) <= 5) near.push_back(e);
            double sw = 0, sc = 0;
            for (auto& e : near) { sw += e.w; sc += e.cents * e.w; }
            double c = sw > 0 ? sc / sw : med;
            // Stiff strings: partial k sits ≈ 866·B·(k²−1) cents above k·f1. With ≥ 3 partials, fit the
            // fundamental and B by weighted least squares (B clamped to steel-string values) and report f1.
            {
                int kmin = 99, kmax = 0;
                for (auto& e : near) { kmin = std::min(kmin, e.k); kmax = std::max(kmax, e.k); }
                if (near.size() >= 3 && kmax - kmin >= 2) {
                    double S = 0, Sx = 0, Sy = 0, Sxx = 0, Sxy = 0;
                    for (auto& e : near) { const double x = e.k * e.k - 1.0; S += e.w; Sx += e.w * x; Sy += e.w * e.cents; Sxx += e.w * x * x; Sxy += e.w * x * e.cents; }
                    const double den = S * Sxx - Sx * Sx;
                    if (den > 1e-9) {
                        const double beta = std::clamp((S * Sxy - Sx * Sy) / den, 0.0, 866.0 * 3e-4);
                        c = (Sy - beta * Sx) / S;
                    }
                }
            }
            auto& p = polyStr_[s];
            const bool fresh = p.present && t - p.lastT < 0.5 && std::fabs(c - p.cents) < 25;
            p.cents = fresh ? p.cents + 0.45 * (c - p.cents) : c;
            p.hz = targetHz[s] * std::pow(2.0, p.cents / 1200);
            p.levelDb = fund[s].db;
            p.present = true;
            p.lastT = t;
            ++nPresent;
        }
        // An open strum lights (nearly) every string; ordinary fretted chords only hit a few string targets.
        const int needN = std::max(3, static_cast<int>(S) - 1);
        if (nPresent >= needN) polyActiveT_ = t;
    }
};

// ---------------------------------------------------------------------------------------------------
// Thread wrapper. The audio callback writes into an SPSC ring (relaxed atomics, display-grade data);
// the service copies new samples on its own thread and feeds the core.
class ListenService {
public:
    static constexpr std::size_t kRingLen = 1u << 17;  // 2.7 s at 48 kHz (heap: 1 MB)
    std::unique_ptr<std::atomic<float>[]> ringIn{new std::atomic<float>[kRingLen]()}, ringOut{new std::atomic<float>[kRingLen]()};
    std::atomic<uint64_t> ringPos{0};

    // Audio thread: cheap check, then one store per sample per channel.
    bool wanted() const { return wanted_.load(std::memory_order_relaxed); }
    void write(const float* in, float inGain, const float* out, uint32_t n) {
        uint64_t p = ringPos.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < n; ++i, ++p) {
            ringIn[p & (kRingLen - 1)].store(in[i] * inGain, std::memory_order_relaxed);
            ringOut[p & (kRingLen - 1)].store(out[i], std::memory_order_relaxed);
        }
        ringPos.store(p, std::memory_order_release);
    }

    ~ListenService() { stop(); }
    void start(std::function<double()> sampleRate) {
        if (running_.exchange(true)) return;
        sr_ = std::move(sampleRate);
        th_ = std::thread([this] { loop(); });
    }
    void stop() {
        if (!running_.exchange(false)) return;
        { std::lock_guard<std::mutex> lk(idleMtx_); }  // pairs with the idle wait below (no lost wakeup)
        idleCv_.notify_one();
        if (th_.joinable()) th_.join();
    }
    // HTTP thread: mark interest and return the latest analysis.
    std::string poll() {
        touched_.store(nowMs(), std::memory_order_relaxed);
        wanted_.store(true, std::memory_order_relaxed);
        { std::lock_guard<std::mutex> lk(idleMtx_); }  // wake the analysis thread if it is parked
        idleCv_.notify_one();
        std::lock_guard<std::mutex> lk(mtx_);
        if (!fed_) return "{\"ok\":true,\"warming\":true}";
        return core_.json();
    }
    void setReference(double a4) { std::lock_guard<std::mutex> lk(mtx_); core_.setReference(a4); }
    void setStrings(std::vector<double> s) { std::lock_guard<std::mutex> lk(mtx_); core_.setStrings(std::move(s)); }
    void resetKey() { std::lock_guard<std::mutex> lk(mtx_); core_.resetKey(); }
    void setModelPath(const std::string& p) { std::lock_guard<std::mutex> lk(mtx_); modelPath_ = p; core_.setModelPath(p); }

private:
    static int64_t nowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
    void loop() {
        std::vector<float> in, out;
        uint64_t readPos = 0;
        double curSr = 0;
        bool active = false;
        while (running_.load()) {
            const bool want = nowMs() - touched_.load(std::memory_order_relaxed) < 4000;
            if (!want) {
                if (active) { wanted_.store(false, std::memory_order_relaxed); active = false; }
                // Idle CPU: nobody is polling, so park (zero wakeups) until poll() or stop() notifies. Only the
                // HTTP thread signals this; the audio thread just checks wanted_ and never touches the CV.
                std::unique_lock<std::mutex> lk(idleMtx_);
                idleCv_.wait(lk, [this] { return !running_.load() || nowMs() - touched_.load(std::memory_order_relaxed) < 4000; });
                continue;
            }
            const uint64_t pos = ringPos.load(std::memory_order_acquire);
            if (!active) { active = true; readPos = pos; }
            const double sr = sr_ ? sr_() : 48000.0;
            if (sr != curSr) { std::lock_guard<std::mutex> lk(mtx_); core_.prepare(sr); core_.setModelPath(modelPath_); curSr = sr; }
            if (pos - readPos > kRingLen / 2) readPos = pos - kRingLen / 4;  // fell behind: skip ahead
            const std::size_t n = static_cast<std::size_t>(pos - readPos);
            if (n > 0) {
                in.resize(n); out.resize(n);
                for (std::size_t i = 0; i < n; ++i) {
                    in[i] = ringIn[(readPos + i) & (kRingLen - 1)].load(std::memory_order_relaxed);
                    out[i] = ringOut[(readPos + i) & (kRingLen - 1)].load(std::memory_order_relaxed);
                }
                readPos = pos;
                std::lock_guard<std::mutex> lk(mtx_);
                core_.feed(in.data(), out.data(), n);
                fed_ = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(12));
        }
    }
    ListenCore core_;
    std::mutex mtx_;
    std::mutex idleMtx_;               // only for parking the idle analysis thread
    std::condition_variable idleCv_;
    std::thread th_;
    std::atomic<bool> running_{false}, wanted_{false};
    std::atomic<int64_t> touched_{0};
    std::function<double()> sr_;
    bool fed_ = false;
    std::string modelPath_;
};

}  // namespace rig::listen
