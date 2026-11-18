#pragma once
// Bloom (unit 900006): a dynamic arpeggiator played by the guitar itself.
//
// It sits after Pitch → MIDI. It reads the tracked notes from the lane's MIDI bus and the dry guitar audio, and writes
// two things: arpeggio notes on the MIDI bus for any Instrument that follows, and an audio arpeggio made from the
// guitar's own strings ("Strings" voice) mixed with the dry guitar, before the amp.
//
//   Chord memory   tracked notes, debounced across a strum. A bend that the tracker relabels as a new note stays
//                  the same chord tone.
//   Harmony        completes the chord from the key: a power chord gets its diatonic third, one note becomes the
//                  chord on its scale degree, Extend stacks 7th / 9th / 11th (13th on major chords). Non-diatonic
//                  chords fall back to template matching. The key is auto-detected from the notes (the same
//                  Temperley + chord-function method as the Listen pane) or set by hand.
//   Patterns       walk on a ladder of chord tones over Range octaves. On a chord change the next note is the
//                  nearest tone of the new chord (voice leading), never a restart from the root.
//   Trigger        Bloom: let a chord ring for Bloom Time and it blooms into an arpeggio; pick again and it gets
//                  out of the way. Always, Engage (footswitch / pad) and Freeze (engage captures the chord and
//                  holds it while you play over it).
//   Hands          pick strength → density, range and velocity (Touch); palm mutes → staccato (Mute Choke);
//                  muting the strings with the fretting hand stops it; bends and vibrato bend the arpeggio
//                  (per-string heterodyne PLLs on the audio, so it works on the polyphonic tracker too), and the
//                  arpeggio is tuned to the guitar's own tuning.
//   Feel           tempo, beat phase, swing and accents learned from the picking (circular grid fit over the last
//                  bars of onsets, phase-locked while you play), or the rig / host tempo.
//   Strings        for each tracked note, pitch-synchronous averaging over several periods of the input isolates
//                  that string's harmonic spectrum; the voices replay those spectra (band-limited mip tables) with
//                  a fresh pick: your guitar, re-plucked, with your captured pick click.
//
// Real-time safe: everything is allocated in prepare(); process() never allocates or locks.
#include "MidiEvent.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef BLOOM_TRACE
#define BTRACE(...) std::fprintf(stderr, __VA_ARGS__)
#else
#define BTRACE(...) ((void)0)
#endif

namespace rig::midi {

struct BloomSettings {
    int trigger = 0;        // 0 Bloom, 1 Always, 2 Engage, 3 Freeze
    bool engage = false;
    int pattern = 4;        // 0 Up, 1 Down, 2 Up/Down, 3 Converge, 4 Walk, 5 Melody, 6 Pedal, 7 Random
    int rate = 3;           // 0 1/4, 1 1/8, 2 1/8T, 3 1/16, 4 1/16T, 5 1/32
    float motion = 0.5f;    // expression macro: density and note length
    float touch = 0.6f;     // how much pick strength drives the arpeggio's loudness (never which notes it plays)
    int range = 2;          // octaves 1..4
    int extend = 1;         // 0 As Played, 1 Triad, 2 7th, 3 9th, 4 11th
    int key = -1;           // -1 auto, 0..11 manual tonic
    int scale = 0;          // manual scale: 0 Major, 1 Minor, 2 Dorian, 3 Mixolydian, 4 Phrygian, 5 Lydian, 6 Harm. Minor
    float gate = 0.6f;      // note length, fraction of a step (1 = let ring)
    float swing = 0.0f;     // 0 straight .. 1 dotted
    bool feel = true;       // clock: learned from the playing (falls back to the rig tempo until locked)
    float bend = 1.0f;      // bend follow
    float choke = 0.7f;     // palm-mute response and fretting-hand stop
    float bloom_s = 0.6f;   // ring time before a Bloom
    int voice = 2;          // 0 Synth (MIDI only), 1 Strings (audio only), 2 Both
    float tone = 0.5f;      // Strings brightness / ring
    float dry = 0.5f;       // dry guitar level while arpeggiating (Bloom / Always / Engage)
    float level = 0.5f;     // Strings level (0.5 = the guitar's own level)
    int retrig = 0;         // 0 Chord: a new chord restarts the figure; 1 Pick: every pick attack restarts it
};

// Live state for the UI (copied out once per block).
struct BloomLive {
    bool active = false, feel_locked = false, muted = false, engaged = false;
    int key = -1;                 // 0..11 major, 12..23 minor; -1 none yet
    float key_conf = 0.0f;
    char chord[24] = {0};
    int root = -1;
    int ladder[48] = {0};
    int ladder_n = 0;
    int note = -1;                // last arpeggio note
    int idx = -1;                 // its index in `ladder` (-1: a melody note above it, or none)
    int step = 0;
    float bpm = 120.0f, swing = 0.5f, strength = 0.5f, bend_cents = 0.0f, tune_cents = 0.0f, ring_db = -120.0f;
    double beat = 0.0;
    int div = 4;
    float bloom_progress = 0.0f;  // 0..1 while a ringing chord approaches its Bloom
};

namespace bloom_detail {

inline constexpr double kPi = 3.14159265358979323846;
inline float dbOf(float a) { return 20.0f * std::log10(std::max(a, 1e-9f)); }
inline double midiHz(double n) { return 440.0 * std::pow(2.0, (n - 69.0) / 12.0); }

// ───────────────────────── key from notes (mirrors ListenAnalyzer::noteKey / decideKey) ─────────────────────────
// Duration-weighted Temperley profile for single-note playing, harmonic function of chords from half-second windows
// with ≥ 3 simultaneous notes, blended by how chordal the playing is; sticky decision. Same defaults as the Listen
// pane (validated there on GuitarSet's held-out players).
class KeyTracker {
public:
    void reset() {
        hist_.fill(0); fn_.fill(0); fnW_ = 0; polyT_ = soundT_ = ev_ = 0; winH_.fill(0); winLow_ = -1; winPoly_ = 0; winT_ = 0;
        key_ = -1; lead_.fill(0); conf_ = 0;
    }
    // `on[p]` true while MIDI note p sounds.
    void update(const std::array<uint8_t, 128>& on, double dt) {
        const double decay = std::exp(-dt / kTau);
        int n = 0, low = 999;
        std::array<double, 12> now{};
        for (int p = 0; p < 128; ++p) if (on[static_cast<std::size_t>(p)]) { ++n; now[static_cast<std::size_t>(p % 12)] += 1.0; low = std::min(low, p); }
        for (auto& v : hist_) v *= decay;
        polyT_ *= decay; soundT_ *= decay;
        for (auto& v : fn_) v *= decay;
        fnW_ *= decay;
        for (int i = 0; i < 12; ++i) hist_[static_cast<std::size_t>(i)] += now[static_cast<std::size_t>(i)] * dt;
        if (n > 0) { soundT_ += dt; ev_ = ev_ * decay + dt; }
        if (n >= 3) polyT_ += dt;
        for (int i = 0; i < 12; ++i) winH_[static_cast<std::size_t>(i)] += now[static_cast<std::size_t>(i)] * dt;
        if (low < 999 && (winLow_ < 0 || low < winLow_)) winLow_ = low;
        winPoly_ = std::max(winPoly_, n);
        winT_ += dt;
        if (winT_ >= 0.5) { chordWindow(); winH_.fill(0); winLow_ = -1; winPoly_ = 0; winT_ = 0; }
        if (ev_ < 0.5) return;
        static const double MAJ[12] = {0.748, 0.060, 0.488, 0.082, 0.670, 0.460, 0.096, 0.715, 0.104, 0.366, 0.057, 0.400};
        static const double MIN[12] = {0.712, 0.084, 0.474, 0.618, 0.049, 0.460, 0.105, 0.747, 0.404, 0.067, 0.133, 0.330};
        std::array<double, 24> ks{}, cf{}, sc{};
        auto corr = [&](const double* prof, int root) {
            double mx = 0, mp = 0;
            for (int i = 0; i < 12; ++i) { mx += hist_[static_cast<std::size_t>(i)]; mp += prof[i]; }
            mx /= 12; mp /= 12;
            double sxy = 0, sxx = 0, syy = 0;
            for (int i = 0; i < 12; ++i) {
                const double x = hist_[static_cast<std::size_t>((i + root) % 12)] - mx, y = prof[i] - mp;
                sxy += x * y; sxx += x * x; syy += y * y;
            }
            return (sxx > 0 && syy > 0) ? sxy / std::sqrt(sxx * syy) : 0.0;
        };
        for (int k = 0; k < 12; ++k) { ks[static_cast<std::size_t>(k)] = corr(MAJ, k); ks[static_cast<std::size_t>(12 + k)] = corr(MIN, k); }
        auto zs = [](std::array<double, 24>& v) {
            double m = 0, q = 0;
            for (double x : v) m += x;
            m /= 24;
            for (double x : v) q += (x - m) * (x - m);
            q = std::sqrt(q / 24);
            if (q < 1e-9) q = 1;
            for (double& x : v) x = (x - m) / q;
        };
        zs(ks);
        const bool haveChords = fnW_ > 0.25;
        if (haveChords) { for (int k = 0; k < 24; ++k) cf[static_cast<std::size_t>(k)] = fn_[static_cast<std::size_t>(k)] / fnW_; zs(cf); }
        const double chordal = soundT_ > 0 ? polyT_ / soundT_ : 0.0;
        const double a = haveChords ? std::clamp((chordal - 0.4) / 0.3, 0.0, 1.0) : 0.0;
        for (int k = 0; k < 24; ++k) sc[static_cast<std::size_t>(k)] = (1 - a) * ks[static_cast<std::size_t>(k)] + a * cf[static_cast<std::size_t>(k)];
        decide(sc, dt);
    }
    int key() const { return key_; }
    float confidence() const { return static_cast<float>(conf_); }

private:
    static constexpr double kTau = 35.0;
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
    void chordWindow() {
        double tot = 0, mx = 0; int act = 0;
        for (double v : winH_) { tot += v; mx = std::max(mx, v); }
        for (double v : winH_) act += v > 0.15 * mx;
        if (!(winPoly_ >= 3 && tot > 0 && act >= 2)) return;
        static const int TQ[9][5] = {{0, 4, 7, -1, -1}, {0, 3, 7, -1, -1}, {0, 4, 7, 10, -1}, {0, 4, 7, 11, -1}, {0, 3, 7, 10, -1},
                                     {0, 3, 6, -1, -1}, {0, 3, 6, 10, -1}, {0, 5, 7, -1, -1}, {0, 2, 7, -1, -1}};
        static const int TC[9] = {0, 1, 0, 0, 1, 3, 3, 0, 0};
        double best = -1e9; int br = -1, bq = 0;
        for (int r = 0; r < 12; ++r)
            for (int q = 0; q < 9; ++q) {
                double sc = 0; int len = 0;
                for (int j = 0; j < 5 && TQ[q][j] >= 0; ++j) { sc += winH_[static_cast<std::size_t>((r + TQ[q][j]) % 12)]; ++len; }
                sc = sc / tot - 0.12 * len / 3.0 + ((winLow_ >= 0 && winLow_ % 12 == r) ? 0.1 : 0.0);
                if (sc > best) { best = sc; br = r; bq = q; }
            }
        if (best <= 0.5) return;
        const int qc = TC[bq];
        for (int k = 0; k < 12; ++k) {
            const int deg = (br - k + 12) % 12;
            fn_[static_cast<std::size_t>(k)] += winT_ * fnMaj(deg, qc == 3 ? 2 : qc);
            fn_[static_cast<std::size_t>(12 + k)] += winT_ * fnMin(deg, qc == 3 ? 2 : qc);
        }
        fnW_ += winT_;
    }
    void decide(const std::array<double, 24>& sc, double dt) {
        int best = 0;
        for (int k = 1; k < 24; ++k) if (sc[static_cast<std::size_t>(k)] > sc[static_cast<std::size_t>(best)]) best = k;
        if (key_ < 0) { if (ev_ > 4.0) { key_ = best; lead_.fill(0.0); } }
        else {
            const double k = std::min(1.0, dt / 5.0);
            for (int i = 0; i < 24; ++i) lead_[static_cast<std::size_t>(i)] += (sc[static_cast<std::size_t>(i)] - sc[static_cast<std::size_t>(key_)] - lead_[static_cast<std::size_t>(i)]) * k;
            int rb = -1;
            for (int i = 0; i < 24; ++i) if (i != key_ && (rb < 0 || lead_[static_cast<std::size_t>(i)] > lead_[static_cast<std::size_t>(rb)])) rb = i;
            const double m = ev_ < 20 ? 0.08 : 0.25;
            if (rb >= 0 && lead_[static_cast<std::size_t>(rb)] > m) { key_ = rb; lead_.fill(0.0); }
        }
        if (key_ < 0) return;
        double z = 0;
        for (int k = 0; k < 24; ++k) z += std::exp((sc[static_cast<std::size_t>(k)] - sc[static_cast<std::size_t>(key_)]) / 0.35);
        conf_ = std::clamp((1.0 / z) * (0.55 + 0.45 * std::min(1.0, ev_ / 10.0)), 0.0, 1.0);
    }
    std::array<double, 12> hist_{}, winH_{};
    std::array<double, 24> fn_{}, lead_{};
    double fnW_ = 0, polyT_ = 0, soundT_ = 0, ev_ = 0, winT_ = 0, conf_ = 0;
    int winLow_ = -1, winPoly_ = 0, key_ = -1;
};

// ───────────────────────── harmony ─────────────────────────
inline const int* scaleSteps(int s) {
    static const int S[7][7] = {{0, 2, 4, 5, 7, 9, 11}, {0, 2, 3, 5, 7, 8, 10}, {0, 2, 3, 5, 7, 9, 10}, {0, 2, 4, 5, 7, 9, 10},
                                {0, 1, 3, 5, 7, 8, 10}, {0, 2, 4, 6, 7, 9, 11}, {0, 2, 3, 5, 7, 8, 11}};
    return S[std::clamp(s, 0, 6)];
}

struct Chord {
    int root = -1;            // pitch class
    int tones[7] = {0};       // pitch classes in role order: root, 3rd, 5th, 7th, 9th, 11th/13th
    int n = 0;
    char name[24] = {0};
};

inline void chordName(Chord& c) {
    static const char* kN[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
    c.name[0] = 0;
    if (c.root < 0 || c.n == 0) return;
    bool has[12] = {false};
    for (int i = 0; i < c.n; ++i) has[(c.tones[i] - c.root + 12) % 12] = true;
    auto app = [&](const char* t) { const std::size_t l = std::strlen(c.name); std::snprintf(c.name + l, sizeof c.name - l, "%s", t); };
    const bool M3 = has[4], m3 = has[3], P5 = has[7], d5 = has[6], a5 = has[8];
    bool dimish = false, sus2 = false, sus4 = false, five = false;
    const char* q = "";
    if (M3 && a5 && !P5) q = "+";
    else if (m3 && d5 && !P5 && !M3) { dimish = true; q = has[9] ? "dim7" : has[10] ? "m7b5" : "dim"; }
    else if (m3 && !M3) q = "m";
    else if (!M3 && !m3) {
        if (has[5]) { q = "sus4"; sus4 = true; }
        else if (has[2]) { q = "sus2"; sus2 = true; }
        else five = true;
    }
    app(kN[c.root]);
    app(q);
    if (dimish) return;
    const bool ext = has[10] || has[11] || (has[2] && !sus2) || (has[5] && !sus4) || has[9];
    if (five && !ext) { app("5"); return; }
    if (has[11]) app("maj7");
    else if (has[10]) app("7");
    if (has[2] && !sus2) app((has[10] || has[11]) ? "(9)" : "add9");
    if (has[5] && !sus4) app("(11)");
    if (has[9]) app((has[10] || has[11]) ? "(13)" : "6");
    if (five) app("(no3)");
}

// Completes a chord from the held notes (pitch classes + the lowest note) and the key.
// key_tonic < 0: no key (template matching only).
inline Chord buildChord(const int* notes, int nn, int key_tonic, int scale, int extend) {
    Chord c;
    if (nn <= 0) return c;
    int lowest = 999;
    bool held[12] = {false};
    for (int i = 0; i < nn; ++i) { held[notes[i] % 12] = true; lowest = std::min(lowest, notes[i]); }
    const int lowpc = lowest % 12;
    int npc = 0;
    for (bool h : held) npc += h;
    auto push = [&](int pc) {
        pc = (pc % 12 + 12) % 12;
        for (int i = 0; i < c.n; ++i) if (c.tones[i] == pc) return;
        if (c.n < 7) c.tones[c.n++] = pc;
    };
    if (extend <= 0) {  // as played: held pitch classes, lowest first
        c.root = lowpc;
        push(lowpc);
        for (int k = 1; k < 12; ++k) if (held[(lowpc + k) % 12]) push(lowpc + k);
        chordName(c);
        return c;
    }
    // Diatonic: every held pitch class must belong to the chord built on some scale degree (up to its 13th).
    if (key_tonic >= 0) {
        const int* st = scaleSteps(scale);
        bool diatonic = true;
        for (int pc = 0; pc < 12 && diatonic; ++pc) {
            if (!held[pc]) continue;
            bool in = false;
            for (int d = 0; d < 7; ++d) in |= (key_tonic + st[d]) % 12 == pc;
            diatonic = in;
        }
        if (diatonic) {
            double best = -1e9; int bd = -1;
            for (int d = 0; d < 7; ++d) {
                int ct[7];
                for (int k = 0; k < 7; ++k) ct[k] = (key_tonic + st[(d + 2 * k) % 7]) % 12;   // 1 3 5 7 9 11 13
                double sc = 0;
                for (int pc = 0; pc < 12; ++pc) {
                    if (!held[pc]) continue;
                    int role = 6;  // every diatonic pitch class is one of the seven stacked thirds
                    for (int k = 0; k < 7; ++k) if (ct[k] == pc) { role = k; break; }
                    sc += role <= 2 ? 2.0 : role == 3 ? 1.0 : role == 6 ? -1.0 : 0.4;
                }
                if (ct[0] == lowpc) sc += 1.6;
                if (held[ct[0]]) sc += 0.6;
                if (npc == 1 && ct[0] == lowpc) sc += 3.0;                      // one note: the chord on its degree
                if (d == 0) sc += 0.15;                                          // tie-break towards the tonic
                if (sc > best) { best = sc; bd = d; }
            }
            if (bd >= 0) {
                int ct[7];
                for (int k = 0; k < 7; ++k) ct[k] = (key_tonic + st[(bd + 2 * k) % 7]) % 12;
                c.root = ct[0];
                push(ct[0]); push(ct[1]); push(ct[2]);
                const bool majorish = (ct[1] - ct[0] + 12) % 12 == 4;
                if (extend >= 2) push(ct[3]);
                if (extend >= 3) push(ct[4]);
                if (extend >= 4) push(majorish ? ct[6] : ct[5]);  // 11th clashes on major chords: 13th instead
                for (int pc = 0; pc < 12; ++pc) if (held[pc]) push(pc);         // anything played stays in
                chordName(c);
                return c;
            }
        }
    }
    // Templates (non-diatonic or no key yet).
    struct T { int iv[4]; int n; int q; };  // q: 0 major, 1 minor, 2 neutral (5/sus), 3 dim, 4 aug
    static const T kT[] = {{{0, 4, 7, -1}, 3, 0}, {{0, 3, 7, -1}, 3, 1}, {{0, 7, -1, -1}, 2, 2}, {{0, 3, 6, -1}, 3, 3},
                           {{0, 4, 8, -1}, 3, 4}, {{0, 2, 7, -1}, 3, 2}, {{0, 5, 7, -1}, 3, 2}, {{0, 4, 7, 10}, 4, 0},
                           {{0, 4, 7, 11}, 4, 0}, {{0, 3, 7, 10}, 4, 1}, {{0, 3, 6, 10}, 4, 3}, {{0, 3, 6, 9}, 4, 3}};
    double best = -1e9; int br = lowpc, bt = 2;
    for (int r = 0; r < 12; ++r) {
        if (!held[r]) continue;
        for (int t = 0; t < static_cast<int>(sizeof kT / sizeof kT[0]); ++t) {
            bool in[12] = {false};
            for (int j = 0; j < kT[t].n; ++j) in[(r + kT[t].iv[j]) % 12] = true;
            double sc = 0;
            for (int pc = 0; pc < 12; ++pc) {
                if (held[pc] && in[pc]) sc += 2;
                else if (held[pc]) sc -= 3;
                else if (in[pc]) sc -= ((pc - r + 12) % 12 == 7) ? 0.3 : 1.0;
            }
            if (r == lowpc) sc += 1.5;
            if (sc > best) { best = sc; br = r; bt = t; }
        }
    }
    const T& t = kT[bt];
    c.root = br;
    for (int j = 0; j < t.n; ++j) push(br + t.iv[j]);
    if (extend >= 2 && t.n < 4) {
        // the key's own 7th when it has one over this root, else a dominant 7th (V of a minor key, borrowed chords)
        bool maj7 = false;
        if (key_tonic >= 0 && t.q == 0) { const int* st = scaleSteps(scale); for (int d = 0; d < 7; ++d) maj7 |= (key_tonic + st[d]) % 12 == (br + 11) % 12 && !((key_tonic + st[(d + 6) % 7]) % 12 == (br + 10) % 12); }
        push(br + (t.q == 3 ? 9 : maj7 ? 11 : 10));
    }
    if (extend >= 3 && t.q != 3) push(br + 2);
    if (extend >= 4 && t.q != 3) push(br + (t.q == 0 ? 9 : 5));
    for (int pc = 0; pc < 12; ++pc) if (held[pc]) push(pc);
    chordName(c);
    return c;
}

// ───────────────────────── radix-2 FFT (N = 512) ─────────────────────────
class Fft512 {
public:
    static constexpr int N = 512;
    Fft512() {
        for (int k = 0; k < N / 2; ++k) tw_[static_cast<std::size_t>(k)] = std::polar(1.0f, static_cast<float>(-2.0 * kPi * k / N));
        for (int i = 0; i < N; ++i) {
            int r = 0;
            for (int b = 0; b < 9; ++b) if (i & (1 << b)) r |= 1 << (8 - b);
            rev_[static_cast<std::size_t>(i)] = static_cast<uint16_t>(r);
        }
    }
    void run(std::complex<float>* x, bool inverse) const {
        for (int i = 0; i < N; ++i) { const int r = rev_[static_cast<std::size_t>(i)]; if (r > i) std::swap(x[i], x[r]); }
        for (int len = 2; len <= N; len <<= 1) {
            const int step = N / len;
            for (int i = 0; i < N; i += len)
                for (int j = 0; j < len / 2; ++j) {
                    std::complex<float> w = tw_[static_cast<std::size_t>(j * step)];
                    if (inverse) w = std::conj(w);
                    const std::complex<float> u = x[i + j], v = x[i + j + len / 2] * w;
                    x[i + j] = u + v;
                    x[i + j + len / 2] = u - v;
                }
        }
        if (inverse) for (int i = 0; i < N; ++i) x[i] /= static_cast<float>(N);
    }
private:
    std::array<std::complex<float>, N / 2> tw_{};
    std::array<uint16_t, N> rev_{};
};

// ───────────────────────── per-string heterodyne frequency tracker ─────────────────────────
// Follows one tracked note's pitch inside a chord. Harmonics 1–3 are each mixed down with a complex oscillator at
// h·f_osc and low-passed by four one-poles (≈ 0.07·f, 4–14 Hz, so a chord tone a whole step away sits ≥ 40 dB down);
// the baseband phase step every kDec samples gives each harmonic's frequency. The reference frequency goes through
// the same filters, so the reading has no lag bias when the oscillator moves; the oscillator re-centres slowly
// (τ 30 ms, aimed ahead along the bend). Octave-doubled strings share every partial with the string below, which drags a reading back towards
// the unbent pitch; a bend moves harmonic h by h times as many Hz, out of the stationary partial's reach, so the
// reading furthest from the string's resting pitch wins.
struct NotePll {
    static constexpr int kDec = 32, kH = 3;
    struct Sub {
        double c = 1, s = 0, cw = 1, sw = 0, zr[4] = {0, 0, 0, 0}, zi[4] = {0, 0, 0, 0}, pr = 0, pi = 0, err = 0, ref[5] = {0, 0, 0, 0, 0};
        float amp = 0;
    };
    int note = -1;
    double f_nom = 0, f_osc = 0, f_est = 0, sr = 48000, a = 0.01, a_dec = 0.2, k_err = 0.05, k_osc = 0.002, slope_ = 0;
    Sub h_[kH];
    int cnt = 0, updates = 0, still = 0;
    float amp = 0, base_cents = 0, cents = 0;
    bool locked = false, base_set = false;
    int64_t born = 0;

    void start(int n, double rate, int64_t t) {
        note = n; sr = rate; f_nom = midiHz(n); f_osc = f_est = f_nom;
        const double fc = std::clamp(0.07 * f_nom, 4.0, 14.0);
        a = 1.0 - std::exp(-2.0 * kPi * fc / sr);
        a_dec = 1.0 - std::pow(1.0 - a, kDec);
        k_err = 1.0 - std::exp(-kDec / (0.012 * sr));
        k_osc = 1.0 - std::exp(-kDec / (0.03 * sr));
        slope_ = 0;
        for (int h = 0; h < kH; ++h) {
            Sub& q = h_[h];
            q = Sub{};
            for (double& r : q.ref) r = (h + 1) * f_osc;
            q.err = 0;
        }
        setW();
        cnt = updates = still = 0; amp = 0; base_cents = cents = 0; locked = base_set = false; born = t;
    }
    void setW() {
        for (int h = 0; h < kH; ++h) {
            const double w = 2.0 * kPi * (h + 1) * f_osc / sr;
            if ((h + 1) * f_osc > 0.45 * sr) { h_[h].cw = 1; h_[h].sw = 0; continue; }
            h_[h].cw = std::cos(w); h_[h].sw = std::sin(w);
        }
    }
    inline void tick(float x, int64_t t) {
        for (int h = 0; h < kH; ++h) {
            Sub& q = h_[h];
            const double nc = q.c * q.cw - q.s * q.sw, ns = q.s * q.cw + q.c * q.sw;
            q.c = nc; q.s = ns;
            double br = x * q.c, bi = -x * q.s;
            for (int k = 0; k < 4; ++k) { q.zr[k] += a * (br - q.zr[k]); q.zi[k] += a * (bi - q.zi[k]); br = q.zr[k]; bi = q.zi[k]; }
        }
        if (++cnt < kDec) return;
        cnt = 0;
        const double age = static_cast<double>(t - born) / sr;
        double fh[kH] = {0, 0, 0};
        float amx = 0;
        const bool measure = updates++ > 0;
        for (int h = 0; h < kH; ++h) {
            Sub& q = h_[h];
            const double g = 1.0 / std::sqrt(q.c * q.c + q.s * q.s); q.c *= g; q.s *= g;
            const double br = q.zr[3], bi = q.zi[3];
            const double m = std::sqrt(br * br + bi * bi);
            q.amp = static_cast<float>(2.0 * m);
            amx = std::max(amx, q.amp);
            // the reference (h·f_osc) through the same four poles and the same smoothing as the reading
            double r = (h + 1) * f_osc;
            for (int k = 0; k < 4; ++k) { q.ref[k] += a_dec * (r - q.ref[k]); r = q.ref[k]; }
            q.ref[4] += (r - q.ref[4]) * k_err;
            if (measure && m > 1e-7 && (q.pr != 0 || q.pi != 0)) {
                const double re = br * q.pr + bi * q.pi, im = bi * q.pr - br * q.pi;
                const double inst = std::atan2(im, re) * sr / (2.0 * kPi * kDec);
                q.err += (inst - q.err) * k_err;
            }
            q.pr = br; q.pi = bi;
            fh[h] = (q.ref[4] + q.err) / (h + 1);
        }
        amp = h_[0].amp;
        if (!measure) return;
        // pick: amplitude-weighted mean at rest; during a bend, the harmonic reading furthest from the resting pitch
        const double rest = f_nom * std::pow(2.0, base_cents / 1200.0);
        double wsum = 0, mean = 0, far = 0; int fi = -1;
        for (int h = 0; h < kH; ++h) {
            if ((h + 1) * f_osc > 0.45 * sr || h_[h].amp < 0.15f * amx || h_[h].amp < 1e-6f) continue;
            mean += h_[h].amp * fh[h]; wsum += h_[h].amp;
            const double d = std::fabs(fh[h] - rest);
            if (fi < 0 || d > far) { far = d; fi = h; }
        }
        if (wsum <= 0) return;
        mean /= wsum;
        const double prev = f_est;
        f_est = std::clamp((base_set && fi >= 0 && far > rest * 0.006) ? fh[fi] : mean, f_nom * 0.8, f_nom * 1.3);
        // The reading is the true pitch ~95 ms ago whatever the oscillator does (the reference rides through the same
        // filters), so steering the oscillator is open-loop: aim it where the bend is heading (slope × the delay) so
        // every harmonic stays inside its pass band while the string moves.
        slope_ += ((f_est - prev) * sr / kDec - slope_) * k_err;
        const double aim = f_est + slope_ * 0.1;
        const double f_new = std::clamp(f_osc + (aim - f_osc) * k_osc, f_nom * 0.8, f_nom * 1.3);
        if (std::fabs(f_new - f_osc) > 1e-4) { f_osc = f_new; setW(); }
        still = (std::fabs(f_est - prev) < 0.0015 * f_nom && age > 0.06) ? still + 1 : 0;
        if (still > 16) locked = true;
        cents = static_cast<float>(1200.0 * std::log2(f_est / f_nom));
        // the string's own tuning: its reading once settled past the attack and the filters' delay
        if (!base_set && locked && age > 0.1) { base_cents = cents; if (age > 0.2) base_set = true; }
    }
    double f() const { return f_est; }
    float bendCents() const { return locked && base_set ? cents - base_cents : 0.0f; }
};

// ───────────────────────── onsets, pick strength, palm mutes, fretting-hand stops ─────────────────────────
class Onsets {
public:
    struct Event { bool onset = false, choke = false; float strength_db = 0; int64_t onset_t = 0; };
    void prepare(double sr) {
        sr_ = sr;
        hp_a_ = static_cast<float>(std::exp(-2.0 * kPi * 80.0 / sr));
        fa_ = coef(0.0003); fr_ = coef(0.012); sa_ = coef(0.06);
        refr_ = static_cast<int64_t>(0.06 * sr);
        reset();
    }
    void reset() { back_.fill(0); bpos_ = 0; hx_ = hy_ = 0; f_ = s_ = 0; last_ = -(1LL << 40); peak_ = 0; peak_open_ = false; muted_ = false; hist_.fill(0); hpos_ = 0; hacc_ = 0; hcnt_ = 0; mute_check_ = -1; }
    inline Event tick(float x, int64_t t) {
        Event e;
        const float y = hp_a_ * (hy_ + x - hx_); hx_ = x; hy_ = y;
        const float r = std::fabs(y);
        f_ += (r - f_) * (r > f_ ? fa_ : fr_);
        s_ += (f_ - s_) * sa_;
        if ((t & 3) == 0) { back_[static_cast<std::size_t>(bpos_)] = f_; bpos_ = (bpos_ + 1) % kBack; }
        if (peak_open_) {
            peak_ = std::max(peak_, f_);
            if (t - last_ > static_cast<int64_t>(0.02 * sr_)) { peak_open_ = false; strength_db_ = dbOf(peak_); mute_check_ = last_ + static_cast<int64_t>(0.09 * sr_); }
        }
        if (mute_check_ >= 0 && t >= mute_check_) { muted_ = strength_db_ - dbOf(f_) > 15.0f; mute_check_ = -1; }
        if (f_ > 2.2f * s_ + 1e-5f && f_ > kFloor && t - last_ > refr_) {
            last_ = t; peak_ = f_; peak_open_ = true; e.onset = true;
            // The detector fires once the attack stands out of what was already ringing, which on a re-strum can be
            // the third or fourth string. The attack began where the envelope last stopped falling: walk back to it.
            int back = 0;
            float cur = f_;
            const float quiet = 0.08f * f_;   // out of silence the walk stops where the attack rose out of the floor
            for (int k = 1; k < kBack - 1; ++k) {
                const float v = back_[static_cast<std::size_t>((bpos_ - 1 - k + 2 * kBack) % kBack)];
                if (v > cur * 1.02f) break;
                cur = std::min(cur, v);
                back = k;
                if (v < quiet || static_cast<double>(k * 4) > 0.03 * sr_) break;
            }
            // land on the last point that was still at the floor of that walk
            e.onset_t = t - static_cast<int64_t>(back) * 4;
            onset_t_ = e.onset_t;
        }
        // fretting-hand stop: the fast envelope falls ≥ 20 dB within ~100 ms while a note had been ringing (no fresh
        // attack). A string's own decay takes seconds for that; a palm-muted chug decays inside the 220 ms guard.
        hacc_ = std::max(hacc_, f_);
        if (++hcnt_ >= kChunk) {
            float mx = 0;
            for (float v : hist_) mx = std::max(mx, v);
            if (t - last_ > static_cast<int64_t>(0.22 * sr_) && mx > 4 * kFloor && hacc_ < 0.1f * mx && !choked_) { e.choke = true; choked_ = true; }
            if (hacc_ > 0.3f * mx) choked_ = false;
            hist_[static_cast<std::size_t>(hpos_)] = hacc_; hpos_ = (hpos_ + 1) % kH; hacc_ = 0; hcnt_ = 0;
        }
        if (e.onset) choked_ = false;
        e.strength_db = strength_db_;
        return e;
    }
    bool muted() const { return muted_; }
    float ringDb() const { return dbOf(s_); }
    float strengthDb() const { return strength_db_; }
    int64_t lastOnset() const { return last_; }
    int64_t onsetTime() const { return onset_t_; }
private:
    static constexpr float kFloor = 0.0035f;  // ≈ −49 dBFS
    static constexpr int kH = 20, kChunk = 256;  // 20 × 256 samples ≈ 107 ms at 48 kHz
    static constexpr int kBack = 512;            // envelope history every 4 samples: ≥ 30 ms up to 68 kHz
    float coef(double tau) const { return static_cast<float>(1.0 - std::exp(-1.0 / (tau * sr_))); }
    double sr_ = 48000;
    float hp_a_ = 0.99f, fa_ = 0.1f, fr_ = 0.01f, sa_ = 0.001f, hx_ = 0, hy_ = 0, f_ = 0, s_ = 0, peak_ = 0, strength_db_ = -30.0f;
    int64_t refr_ = 2880, last_ = 0, mute_check_ = -1, onset_t_ = 0;
    std::array<float, kBack> back_{};
    int bpos_ = 0;
    bool peak_open_ = false, muted_ = false, choked_ = false;
    std::array<float, kH> hist_{};
    int hpos_ = 0, hcnt_ = 0;
    float hacc_ = 0;
};

// ───────────────────────── groove: tempo, phase, swing and accents from the picking ─────────────────────────
// Tempo: a histogram of every inter-onset interval in the last 8 s (all pairs, so swung off-beats still give exact
// beat-to-beat intervals), scored for each candidate beat at its multiples plus its half-beat, with a log-tempo prior
// around the rig tempo. Phase: the densest, strongest cluster of onsets on the beat circle. Swing: where the off-beat
// cluster sits. Accents: onset strength per 16th of the beat.
class Groove {
public:
    void reset() { n_ = 0; head_ = 0; locked_ = false; period_ = 0.5; t0_ = 0; swing_ = 0.5f; acc_ = {1.0f, 0.78f, 0.88f, 0.78f}; conf_ = 0; last_est_ = -1e9; lastLockT_ = -1e9; }
    void onset(double t, float strength_db, double prior_bpm) {
        on_[static_cast<std::size_t>(head_)] = {t, strength_db};
        head_ = (head_ + 1) % kN; n_ = std::min(n_ + 1, kN);
        if (t - last_est_ < 0.1) return;
        last_est_ = t;
        estimate(t, prior_bpm);
    }
    bool locked() const { return locked_; }
    double period() const { return period_; }
    double beatAt(double t) const { return (t - t0_) / period_; }
    float swing() const { return swing_; }
    float accent(int slot) const { return acc_[static_cast<std::size_t>(slot & 3)]; }
    float confidence() const { return conf_; }
private:
    struct On { double t; float s; };
    static constexpr int kN = 64;
    static constexpr double kBin = 0.005, kMaxIoi = 2.6;
    static constexpr int kBins = static_cast<int>(kMaxIoi / kBin) + 8;
    void estimate(double now, double prior_bpm) {
        On v[kN]; int m = 0;
        float smin = 1e9f, smax = -1e9f;
        for (int i = 0; i < n_; ++i) {
            const On& o = on_[static_cast<std::size_t>((head_ - 1 - i + kN) % kN)];
            if (now - o.t > 8.0) break;
            v[m++] = o; smin = std::min(smin, o.s); smax = std::max(smax, o.s);
        }
        if (m < 6) return;
        double w[kN];
        for (int i = 0; i < m; ++i) w[i] = 0.4 + 0.6 * (smax > smin ? (v[i].s - smin) / (smax - smin) : 1.0);
        // IOI histogram (5 ms bins), Gaussian-smoothed (σ 15 ms)
        double raw[kBins] = {0}, H[kBins] = {0};
        for (int i = 0; i < m; ++i)
            for (int j = 0; j < m; ++j) {
                const double d = v[i].t - v[j].t;
                if (d <= 0.05 || d > kMaxIoi) continue;
                raw[static_cast<int>(d / kBin + 0.5)] += w[i] * w[j];
            }
        for (int b = 0; b < kBins; ++b) {
            if (raw[b] == 0) continue;
            for (int k = -7; k <= 7; ++k) { const int q = b + k; if (q >= 0 && q < kBins) H[q] += raw[b] * std::exp(-0.5 * (k * kBin / 0.015) * (k * kBin / 0.015)); }
        }
        auto at = [&](double x) { const double f = x / kBin; const int b = static_cast<int>(f); if (b < 0 || b + 1 >= kBins) return 0.0; return H[b] + (H[b + 1] - H[b]) * (f - b); };
        const double center = std::clamp(prior_bpm > 0 ? prior_bpm : 100.0, 70.0, 150.0) * 0.85 + 100.0 * 0.15;
        double best = -1, bb = 0;
        auto score = [&](double bpm) {
            const double P = 60.0 / bpm;
            double sc = 0.5 * at(P / 2);
            for (int k = 1; k <= 4 && k * P <= kMaxIoi; ++k) sc += at(k * P) / k;
            const double l = std::log2(bpm / center);
            return sc * std::exp(-0.5 * (l / 0.5) * (l / 0.5));
        };
        const double lo = locked_ ? 60.0 / period_ * 0.9 : 60.0, hi = locked_ ? 60.0 / period_ * 1.1 : 200.0;
        for (double b = lo; b <= hi; b += 1.0) { const double sc = score(b); if (sc > best) { best = sc; bb = b; } }
        const double b0 = bb;
        for (double d = -1.0; d <= 1.0001; d += 0.1) { const double sc = score(b0 + d); if (sc > best) { best = sc; bb = b0 + d; } }
        const double P = 60.0 / bb;
        // phase: weighted onset density on the beat circle (64 bins, σ ≈ 20 ms)
        constexpr int kPh = 64;
        double dens[kPh] = {0};
        const double sig = std::clamp(0.02 / P, 0.01, 0.06);
        for (int i = 0; i < m; ++i) {
            const double u = v[i].t / P - std::floor(v[i].t / P);
            for (int q = 0; q < kPh; ++q) {
                double du = static_cast<double>(q) / kPh - u;
                du -= std::round(du);
                dens[q] += w[i] * w[i] * std::exp(-0.5 * (du / sig) * (du / sig));
            }
        }
        int bq = 0;
        for (int q = 1; q < kPh; ++q) if (dens[q] > dens[bq]) bq = q;
        // refine the peak to the weighted mean of the onsets near it
        double cre = 0, cim = 0;
        for (int i = 0; i < m; ++i) {
            const double u = v[i].t / P - std::floor(v[i].t / P);
            double du = u - static_cast<double>(bq) / kPh; du -= std::round(du);
            if (std::fabs(du) < 0.08) { cre += w[i] * std::cos(2 * kPi * u); cim += w[i] * std::sin(2 * kPi * u); }
        }
        double phase = std::atan2(cim, cre) / (2 * kPi);
        if (phase < 0) phase += 1.0;
        const double t0 = phase * P;
        // how well the onsets sit on this beat's 16ths (with swing allowed): the confidence
        double fitw = 0, totw = 0, offw = 0, offpos = 0, slotW[4] = {0, 0, 0, 0};
        for (int i = 0; i < m; ++i) {
            double u = (v[i].t - t0) / P; u -= std::floor(u);
            totw += w[i];
            const double q16 = u * 4.0, near16 = std::round(q16);
            if (std::fabs(q16 - near16) < 0.18) fitw += w[i];
            if (u > 0.3 && u < 0.82) { offw += w[i]; offpos += w[i] * u; }
            if (std::fabs(q16 - near16) < 0.3) slotW[static_cast<int>(near16) & 3] += w[i];
        }
        // a swung off-beat cluster (between the straight 8th and a dotted feel) counts as on the grid
        float sw = 0.5f;
        if (offw > 0.15 * totw) {
            const double pos = offpos / offw;
            if (pos > 0.555 && pos < 0.76) {  // below ~0.55 it is human timing, not swing
                sw = static_cast<float>(pos);
                for (int i = 0; i < m; ++i) {
                    double u = (v[i].t - t0) / P; u -= std::floor(u);
                    const double q16 = u * 4.0;
                    if (std::fabs(u - pos) < 0.06 && std::fabs(q16 - std::round(q16)) >= 0.18) fitw += w[i];
                }
            }
        }
        conf_ = static_cast<float>(totw > 0 ? fitw / totw : 0.0);
        if (conf_ < 0.7f) { if (locked_ && now - lastLockT_ > 6.0) locked_ = false; return; }
        double total = 0;
        for (double x : slotW) total += x;
        for (int q = 0; q < 4; ++q) acc_[static_cast<std::size_t>(q)] = static_cast<float>(std::clamp(0.72 + 1.1 * (total > 0 ? slotW[q] / total : 0.25), 0.72, 1.0));
        if (locked_) {
            // keep the beat count continuous: shift the new grid by whole beats to sit on the old one
            const double oldBeat = (now - t0_) / period_, newBeat = (now - t0) / P;
            t0_ = t0 - std::round(oldBeat - newBeat) * P;
        } else t0_ = t0;
        period_ = P;
        swing_ = sw;
        locked_ = true;
        lastLockT_ = now;
    }
    std::array<On, kN> on_{};
    int n_ = 0, head_ = 0;
    bool locked_ = false;
    double period_ = 0.5, t0_ = 0, last_est_ = -1e9, lastLockT_ = -1e9;
    float swing_ = 0.5f, conf_ = 0;
    std::array<float, 4> acc_{{1.0f, 0.78f, 0.88f, 0.78f}};
};

// ───────────────────────── Strings: the guitar's own notes, re-plucked ─────────────────────────
class Strings {
public:
    static constexpr int M = Fft512::N;          // samples per captured cycle
    static constexpr int kTones = 8, kMips = 7, kVoices = 10;

    void prepare(double sr) {
        sr_ = sr;
        int sz = 1;
        while (sz < static_cast<int>(0.6 * sr)) sz <<= 1;
        ring_.assign(static_cast<std::size_t>(sz), 0.0f);
        mask_ = sz - 1; wpos_ = 0;
        for (auto& t : tones_) { t.mip.assign(static_cast<std::size_t>(kMips * (M + 3)), 0.0f); t.note = -1; t.used = 0; }
        click_.assign(static_cast<std::size_t>(std::max(16, static_cast<int>(0.008 * sr))), 0.0f);
        click_n_ = 0; click_fill_ = -1;
        for (auto& v : voices_) v.on = false;
        buildFallback();
    }
    inline void write(float x) {
        ring_[static_cast<std::size_t>(wpos_ & mask_)] = x; ++wpos_;
        if (click_fill_ >= 0) {
            const float y = 0.97f * (chy_ + x - chx_); chx_ = x; chy_ = y;  // light high-pass: the pick, not the strings
            click_[static_cast<std::size_t>(click_fill_++)] = y;
            if (click_fill_ >= static_cast<int>(click_.size())) { click_n_ = click_fill_; click_fill_ = -1; }
        }
    }
    void onsetClick() { click_fill_ = 0; }

    // Pitch-synchronous average of the last K periods of note `note` at frequency `hz` → one band-limited cycle.
    // Called at most once per note per block from the audio thread (≈ 512·K + 7 FFTs of 512 points).
    bool capture(int note, double hz, uint64_t stamp) {
        if (hz <= 20.0 || hz > sr_ * 0.25) return false;
        const double T = refinePeriod(sr_ / hz);
        const int K = std::clamp(static_cast<int>(std::lround(0.085 * hz)), 4, 24);
        const double span = K * T;
        if (span + 8 > static_cast<double>(mask_)) return false;
        const double start = static_cast<double>(wpos_) - 4.0 - span;
        float cyc[M];
        double wsum = 0;
        for (int j = 0; j < M; ++j) cyc[j] = 0.0f;
        for (int k = 0; k < K; ++k) {
            const double wk = 0.5 - 0.5 * std::cos(2.0 * kPi * (k + 0.5) / K);  // Hann across periods
            wsum += wk;
            for (int j = 0; j < M; ++j) cyc[j] += static_cast<float>(wk) * readAt(start + (k + static_cast<double>(j) / M) * T);
        }
        if (wsum <= 0) return false;
        double rms = 0, mean = 0;
        for (int j = 0; j < M; ++j) { cyc[j] = static_cast<float>(cyc[j] / wsum); mean += cyc[j]; }
        mean /= M;
        for (int j = 0; j < M; ++j) { cyc[j] -= static_cast<float>(mean); rms += cyc[j] * cyc[j]; }
        rms = std::sqrt(rms / M);
        if (rms < 2e-5) return false;
        // slot: same note, else least recently used
        Tone* tn = nullptr;
        for (auto& t : tones_) if (t.note == note) tn = &t;
        if (!tn) { tn = &tones_[0]; for (auto& t : tones_) if (t.used < tn->used) tn = &t; }
        std::complex<float> X[M];
        for (int j = 0; j < M; ++j) X[j] = {cyc[j], 0.0f};
        fft_.run(X, false);
        X[0] = 0;
        tn->note = note; tn->used = stamp; tn->rms = static_cast<float>(rms);
        bakeMips(*tn, X);
        return true;
    }
    // Starts a voice for `note`: the nearest captured tone (same pitch class first), transposed.
    void noteOn(int note, double hz, float vel, double gate_s, float tone, float level_lin) {
        const int src = nearest(note);  // an index, never a pointer: knob edits move this object (adoptRuntimeStateFrom)
        Voice* v = nullptr;
        for (auto& x : voices_) if (!x.on) { v = &x; break; }
        if (!v) { v = &voices_[0]; for (auto& x : voices_) if (x.age > v->age) v = &x; }
        for (auto& x : voices_) if (x.on && x.note == note) x.rel = true;  // same string: the new pluck damps the old
        *v = Voice{};
        v->on = true; v->note = note; v->src = src; v->hz = hz; v->ph = 0;
        int lvl = 0;
        while (lvl < kMips - 1 && (256 >> lvl) * hz > 0.45 * sr_) ++lvl;
        v->mip = lvl;
        const float amp = (src >= 0 ? std::min(0.5f, tones_[static_cast<std::size_t>(src)].rms * 1.8f) : 0.05f) * vel * level_lin;
        v->amp = amp;
        v->gate = static_cast<int64_t>(std::max(0.02, gate_s) * sr_);
        // ring: bright tone rings longer; brightness: an opening low-pass that closes like a string
        v->decay = static_cast<float>(std::exp(-1.0 / ((0.5 + 2.2 * tone) * sr_)));
        v->rel_k = static_cast<float>(std::exp(-1.0 / (0.05 * sr_)));
        v->cut = static_cast<float>(std::min(0.45 * sr_, hz * (6.0 + 70.0 * tone * vel)));
        v->cut_end = static_cast<float>(std::min(0.45 * sr_, hz * (2.5 + 10.0 * tone)));
        v->cut_k = static_cast<float>(std::exp(-1.0 / (0.12 * sr_)));
        v->click = click_n_ > 0 ? 0 : -1;
        v->click_amp = 0.35f * vel * (0.4f + tone) * level_lin;
        v->age = 0;
    }
    void releaseAll(double fade_s) {
        const float k = static_cast<float>(std::exp(-1.0 / (std::max(0.005, fade_s) * sr_)));
        for (auto& v : voices_) if (v.on) { v.rel = true; v.rel_k = std::min(v.rel_k, k); }
    }
    void setPitchRatio(double r) { ratio_ = r; }
    inline float render() {
        float out = 0;
        for (auto& v : voices_) {
            if (!v.on) continue;
            ++v.age;
            if (!v.rel && v.age >= v.gate) v.rel = true;
            // envelope: 1.5 ms attack, string decay, release
            if (v.age < static_cast<int64_t>(0.0015 * sr_)) v.env = static_cast<float>(v.age) / static_cast<float>(0.0015 * sr_);
            else v.env *= v.rel ? v.rel_k : v.decay;
            if (v.age > 64 && v.env < 1e-4f) { v.on = false; continue; }
            const float* tab = (v.src >= 0 && tones_[static_cast<std::size_t>(v.src)].note >= 0 ? tones_[static_cast<std::size_t>(v.src)].mip.data() : fallback_.data()) + v.mip * (M + 3);
            const double inc = v.hz * ratio_ * M / sr_;
            v.ph += inc; if (v.ph >= M) v.ph -= M;
            const int i0 = static_cast<int>(v.ph);
            const float fr = static_cast<float>(v.ph - i0);
            // cubic Hermite (table carries 3 wrap samples)
            const float xm1 = tab[(i0 + M - 1) % M], x0 = tab[i0], x1 = tab[i0 + 1], x2 = tab[i0 + 2];
            const float c1 = 0.5f * (x1 - xm1), c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
            float s = ((c3 * fr + c2) * fr + c1) * fr + x0;
            // closing brightness (two one-poles)
            v.cut = v.cut_end + (v.cut - v.cut_end) * v.cut_k;
            const float g = 1.0f - std::exp(-2.0f * static_cast<float>(kPi) * v.cut / static_cast<float>(sr_));
            v.lp1 += g * (s - v.lp1); v.lp2 += g * (v.lp1 - v.lp2);
            s = v.lp2 * v.env * v.amp;
            if (v.click >= 0) {
                if (v.click < click_n_) s += click_[static_cast<std::size_t>(v.click++)] * v.click_amp * (1.0f - static_cast<float>(v.click) / static_cast<float>(click_n_));
                else v.click = -1;
            }
            out += s;
        }
        return out;
    }
    bool hasTone() const { for (const auto& t : tones_) if (t.note >= 0) return true; return false; }
    // Tests: the captured cycle (mip 0, M samples) for `note`, or nullptr.
    const float* toneTable(int note) const { for (const auto& t : tones_) if (t.note == note) return t.mip.data(); return nullptr; }

private:
    struct Tone { int note = -1; uint64_t used = 0; float rms = 0; std::vector<float> mip; };
    struct Voice {
        bool on = false, rel = false;
        int note = -1, mip = 0, click = -1;
        int src = -1;                // index into tones_ (-1: the fallback pluck)
        double hz = 0, ph = 0;
        float amp = 0, env = 0, decay = 0.9999f, rel_k = 0.999f, cut = 1000, cut_end = 500, cut_k = 0.999f, lp1 = 0, lp2 = 0, click_amp = 0;
        int64_t gate = 0, age = 0;
    };
    float readAt(double pos) const {
        const double fl = std::floor(pos);
        const int64_t i = static_cast<int64_t>(fl);
        const float fr = static_cast<float>(pos - fl);
        const float a = ring_[static_cast<std::size_t>(i & mask_)], b = ring_[static_cast<std::size_t>((i + 1) & mask_)];
        return a + (b - a) * fr;
    }
    // The string's exact period: normalised autocorrelation over ±30 cents of the estimate, parabolic peak.
    double refinePeriod(double T0) const {
        const int L = std::min(static_cast<int>(0.05 * sr_), static_cast<int>(mask_ / 2) - static_cast<int>(2 * T0) - 8);
        if (L < static_cast<int>(2 * T0)) return T0;
        const double base = static_cast<double>(wpos_) - 4.0 - L - T0 * 1.03;
        double r[21];
        for (int c = 0; c < 21; ++c) {
            const double Tc = T0 * std::pow(2.0, (c - 10) * 3.0 / 1200.0);
            double xy = 0, xx = 0, yy = 0;
            for (int i = 0; i < L; i += 2) {
                const double a = readAt(base + i), b = readAt(base + i + Tc);
                xy += a * b; xx += a * a; yy += b * b;
            }
            r[c] = (xx > 0 && yy > 0) ? xy / std::sqrt(xx * yy) : 0.0;
        }
        int bi = 10;
        for (int c = 0; c < 21; ++c) if (r[c] > r[bi]) bi = c;
        double off = 0;
        if (bi > 0 && bi < 20) { const double den = r[bi - 1] - 2 * r[bi] + r[bi + 1]; if (den < 0) off = 0.5 * (r[bi - 1] - r[bi + 1]) / den; }
        return T0 * std::pow(2.0, ((bi - 10) + off) * 3.0 / 1200.0);
    }
    void bakeMips(Tone& t, const std::complex<float>* X) {
        std::complex<float> Y[M];
        // normalise to unit RMS: Parseval over the bins
        double e = 0;
        for (int k = 1; k < M; ++k) e += std::norm(X[k]);
        const float nrm = e > 0 ? static_cast<float>(std::sqrt(M * M / e)) : 1.0f;
        for (int l = 0; l < kMips; ++l) {
            const int H = 256 >> l;
            for (int k = 0; k < M; ++k) {
                const int h = k <= M / 2 ? k : M - k;
                Y[k] = (h >= 1 && h < H) ? X[k] * nrm : std::complex<float>(0, 0);
            }
            fft_.run(Y, true);
            float* d = &t.mip[static_cast<std::size_t>(l * (M + 3))];
            for (int j = 0; j < M; ++j) d[j] = Y[j].real();
            d[M] = d[0]; d[M + 1] = d[1]; d[M + 2] = d[2];
        }
    }
    void buildFallback() {  // a soft plucked-string spectrum until a real string is captured
        fallback_.assign(static_cast<std::size_t>(kMips * (M + 3)), 0.0f);
        for (int l = 0; l < kMips; ++l) {
            const int H = 256 >> l;
            float* d = &fallback_[static_cast<std::size_t>(l * (M + 3))];
            for (int j = 0; j < M; ++j) {
                double s = 0;
                for (int h = 1; h < std::min(H, 40); ++h) s += std::sin(2.0 * kPi * h * j / M) * std::sin(kPi * h * 0.18) / (h * 1.0);
                d[j] = static_cast<float>(s * 0.9);
            }
            d[M] = d[0]; d[M + 1] = d[1]; d[M + 2] = d[2];
        }
    }
    int nearest(int note) const {
        int best = -1, bs = 1 << 30;
        for (int k = 0; k < kTones; ++k) {
            const Tone& t = tones_[static_cast<std::size_t>(k)];
            if (t.note < 0) continue;
            const int d = std::abs(t.note - note);
            // the smallest transposition wins (a string's body and pick sit at its own pitch: octave jumps sound
            // like a tape at the wrong speed), with a nudge towards the same pitch class
            const int score = d * 10 + (t.note % 12 == note % 12 ? 0 : 45);
            if (score < bs) { bs = score; best = k; }
        }
        return best;
    }
    double sr_ = 48000, ratio_ = 1.0;
    std::vector<float> ring_, click_, fallback_;
    int64_t wpos_ = 0; int mask_ = 0, click_n_ = 0, click_fill_ = -1;
    float chx_ = 0, chy_ = 0;
    std::array<Tone, kTones> tones_{};
    std::array<Voice, kVoices> voices_{};
    Fft512 fft_;
};

} // namespace bloom_detail

// ───────────────────────── Bloom ─────────────────────────
class Bloom {
public:
    static constexpr int kPll = 8;

    void prepare(double sr) {
        sr_ = sr;
        onsets_.prepare(sr);
        groove_.reset();
        key_.reset();
        strings_.prepare(sr);
        kDryUp_ = static_cast<float>(1.0 - std::exp(-1.0 / (0.002 * sr)));
        kDryDown_ = static_cast<float>(1.0 - std::exp(-1.0 / (0.12 * sr)));
        resetState();
    }
    void resetState() {
        held_.fill(0); heldVel_.fill(0); alias_.fill(-1); src_off_.fill(0); cap_at_.fill(-1); off_t_.fill(-(1LL << 40)); clus_n_ = 0; win_started_ = true;
        for (auto& p : pll_) p.note = -1;
        mem_n_ = 0; snap_n_ = 0; chord_ = {}; ladder_n_ = 0; win_n_ = 0; win_until_ = -1;
        active_ = false; arp_note_ = -1; beat_ = 0; phrase_k_ = 0; phrase_origin_ = 0; seed_ = 0;
        pend_n_ = 0; t_ = 0; dry_g_ = 1.0f; bend_out_ = 0; last_bend_sent_ = 0; melody_ = -1; melody_t_ = -1;
        cap_pending_ = 0; prev_engage_ = false; last_strength_ = 0.5f; strength_mean_ = -30.0f; strength_init_ = false;
        onset_s_ = -(1LL << 40); onset_used_ = true;
    }
    void setSettings(const BloomSettings& s) {
        const bool rebuild = s.extend != set_.extend || s.key != set_.key || s.scale != set_.scale || s.range != set_.range || s.pattern != set_.pattern;
        set_ = s;
        if (rebuild) rebuildLadder();
    }
    const BloomSettings& settings() const { return set_; }

    // in: the lane's MIDI (tracked notes). mono: the dry guitar. outMidi: arpeggio notes. strings: the Strings voice
    // (written for every frame). dry: per-frame dry gain.
    void process(const std::vector<MidiEvent>& in, const float* mono, uint32_t n, double bpm, double ppq, bool host_playing,
                 std::vector<MidiEvent>& outMidi, float* strings, float* dry) {
        std::size_t ei = 0;
        bool mono_bend_seen = false;
        float mono_bend = 0;
        bpm_ = bpm;
        out_ = &outMidi;
        for (uint32_t i = 0; i < n; ++i, ++t_) {
            const float x = mono[i];
            strings_.write(x);
            for (auto& p : pll_) if (p.note >= 0) p.tick(x, t_);
            const auto ev = onsets_.tick(x, t_);
            if (ev.onset) onOnset(i, outMidi);
            if (ev.choke && set_.choke > 0.2f) onChoke(i, outMidi);
            while (ei < in.size() && in[ei].offset <= i) {
                const MidiEvent& e = in[ei++];
                if (e.type == MidiEvent::PitchBend) { mono_bend_seen = true; mono_bend = e.bend; continue; }
                if (e.type == MidiEvent::NoteOn) noteOn(e.note, e.velocity);
                else noteOff(e.note);
            }
            // a pick attack the tracker reports no new note for (the same chord struck again): it still restarts the
            // figure in Retrigger Pick, and counts as the chord's attack otherwise
            if (!onset_used_ && t_ - onset_s_ > static_cast<int64_t>(0.1 * sr_)) {
                onset_used_ = true;
                if (mem_n_ > 0 && set_.retrig == 1) newPhrase(onset_s_);
            }
            for (int k = 0; k < pend_n_;) {
                if (pend_[k].at <= t_) {
                    outMidi.push_back({MidiEvent::NoteOff, pend_[k].note, 0.0f, 0.0f, i});
                    pend_[k] = pend_[--pend_n_];
                } else ++k;
            }
            advanceClock();
            if (active_) stepTick(i, outMidi);
            strings[i] = (set_.voice >= 1) ? strings_.render() : 0.0f;
            const float target = (active_ && set_.trigger != 3) ? set_.dry : 1.0f;
            dry_g_ += (target - dry_g_) * (target > dry_g_ ? kDryUp_ : kDryDown_);
            dry[i] = dry_g_;
        }
        const double bdt = static_cast<double>(n) / sr_;
        key_.update(held_, bdt);
        capturePending();
        updateTrigger(n, ppq, host_playing, outMidi);
        updateBend(mono_bend_seen, mono_bend, n, outMidi);
        publish();
    }

    void allOff(std::vector<MidiEvent>& out, uint32_t off) {
        for (int k = 0; k < pend_n_; ++k) out.push_back({MidiEvent::NoteOff, pend_[k].note, 0.0f, 0.0f, off});
        pend_n_ = 0;
        arp_note_ = -1;
    }
    const BloomLive& live() const { return live_; }
    const bloom_detail::Strings& strings() const { return strings_; }

private:
    using Chord = bloom_detail::Chord;
    struct Pending { int note; int64_t at; };

    // ── notes in ──
    // Keyed: the chord of a phrase is what the tracker reports inside the strum window that a pick attack opens
    // (60 ms from its first note). Notes after the window (ghosts, sympathetic strings, the tracker's second
    // thoughts) never change it; the next attack does. Same attack, same notes → same chord → same figure.
    void noteOn(int note, float vel) {
        if (note < 0 || note > 127) return;
        // a bend the tracker relabels: the new note continues the bent string's chord tone
        for (auto& p : pll_) {
            if (p.note < 0 || p.note == note || !held_[static_cast<std::size_t>(p.note)]) continue;
            const int d = note - p.note;
            if (std::abs(d) >= 1 && std::abs(d) <= 3 && p.bendCents() * (d > 0 ? 1.0f : -1.0f) >= 100.0f * std::abs(d) - 60.0f) {
                alias_[static_cast<std::size_t>(note)] = static_cast<int16_t>(p.note);
                return;
            }
        }
        bool others = false;
        for (int q = 0; q < 128; ++q) if (held_[static_cast<std::size_t>(q)] && q != note) { others = true; break; }
        // a note that is new to the chord, or the same string picked again (re-attacked while held, or ended just now)
        bool in_mem = false;
        for (int k = 0; k < mem_n_; ++k) in_mem |= mem_[k] == note;
        const bool renewed = !in_mem || held_[static_cast<std::size_t>(note)] || t_ - off_t_[static_cast<std::size_t>(note)] < static_cast<int64_t>(0.03 * sr_);
        held_[static_cast<std::size_t>(note)] = 1;
        heldVel_[static_cast<std::size_t>(note)] = vel;
        startPll(note);
        last_vel_ = vel;
        if (t_ <= win_until_) { addToWindow(note, vel); return; }
        const bool fresh_attack = !onset_used_ && t_ - onset_s_ <= static_cast<int64_t>(0.12 * sr_);
        // out of silence without a detected attack: only a real note (not a faint tracker guess), and never in the
        // middle of a Bloom (nothing was picked, so nothing changes)
        const bool from_silence = !others && vel >= 0.3f && !(set_.trigger == 0 && active_);
        if (fresh_attack || from_silence) {
            // a new attack (or a note out of silence the onset detector missed: a hammer-on from nothing, a soft pick)
            openWindow(fresh_attack ? onset_s_ : t_ - static_cast<int64_t>(0.012 * sr_));
            addToWindow(note, vel);
            return;
        }
        // No attack heard, but two or more new / re-picked notes within 50 ms: a strum the onset detector missed (a soft
        // chord over a louder one ringing, the same chord struck again). One such note alone is a ghost or a melody note.
        if (!renewed || vel < 0.2f) return;
        if (clus_n_ > 0 && t_ - clus_t0_ <= static_cast<int64_t>(0.05 * sr_)) {
            if (clus_n_ < 16) { clus_note_[clus_n_] = note; clus_vel_[clus_n_] = vel; ++clus_n_; }
            if (clus_n_ >= 2) {
                if (set_.trigger == 0 && active_ && !set_.engage) stop(0, *out_, 0.06);   // picked again: get out of the way
                const int cn = clus_n_;   // openWindow clears the cluster
                openWindow(clus_t0_ - static_cast<int64_t>(0.012 * sr_));
                for (int k = 0; k < cn; ++k) addToWindow(clus_note_[k], clus_vel_[k]);
            }
        } else { clus_n_ = 1; clus_t0_ = t_; clus_note_[0] = note; clus_vel_[0] = vel; }
    }
    void openWindow(int64_t origin) {
        BTRACE("%.4f openWindow origin %.4f\n", double(t_) / sr_, double(origin) / sr_);
        onset_used_ = true;
        clus_n_ = 0;
        win_n_ = 0;
        win_until_ = t_ + static_cast<int64_t>(0.06 * sr_);
        win_origin_ = origin;
        win_started_ = false;
    }
    void addToWindow(int note, float vel) {
        bool dup = false;
        for (int k = 0; k < win_n_; ++k) if (win_note_[k] == note) { win_vel_[k] = std::max(win_vel_[k], vel); dup = true; }
        if (!dup && win_n_ < 16) { win_note_[win_n_] = note; win_vel_[win_n_] = vel; ++win_n_; }
        commitWindow();
        if (!win_started_) { win_started_ = true; newPhrase(win_origin_); }
    }
    // The window's notes, without the tracker's faint guesses (under 40 % of the strongest note's velocity).
    void commitWindow() {
        float mx = 0;
        for (int k = 0; k < win_n_; ++k) mx = std::max(mx, win_vel_[k]);
        int nn = 0;
        for (int k = 0; k < win_n_; ++k) if (win_vel_[k] >= 0.4f * mx) mem_[nn++] = win_note_[k];
        if (nn == 0) return;
        std::sort(mem_, mem_ + nn);
        mem_n_ = nn;
        // the melody note (Melody pattern) is the top of what was just picked: never a note nobody picked
        melody_ = mem_[nn - 1]; melody_t_ = t_;
        if (set_.trigger == 3 && active_) return;   // frozen: the snapshot plays on
        rebuildLadder();
        for (int k = 0; k < nn; ++k) cap_at_[static_cast<std::size_t>(mem_[k])] = t_ + static_cast<int64_t>(0.07 * sr_);
        cap_pending_ = 1;
    }
    void noteOff(int note) {
        if (note < 0 || note > 127) return;
        if (alias_[static_cast<std::size_t>(note)] >= 0) {
            const int src = alias_[static_cast<std::size_t>(note)];
            alias_[static_cast<std::size_t>(note)] = -1;
            if (!held_[static_cast<std::size_t>(src)] || src_off_[static_cast<std::size_t>(src)]) releaseHeld(src);
            return;
        }
        for (int q = 0; q < 128; ++q) if (alias_[static_cast<std::size_t>(q)] == note) { src_off_[static_cast<std::size_t>(note)] = 1; return; }
        releaseHeld(note);
    }
    void releaseHeld(int note) {
        held_[static_cast<std::size_t>(note)] = 0;
        off_t_[static_cast<std::size_t>(note)] = t_;
        src_off_[static_cast<std::size_t>(note)] = 0;
        for (auto& p : pll_) if (p.note == note) p.note = -1;
    }
    void startPll(int note) {
        bloom_detail::NotePll* slot = nullptr;
        for (auto& p : pll_) if (p.note == note) { slot = &p; break; }
        if (!slot) for (auto& p : pll_) if (p.note < 0) { slot = &p; break; }
        if (!slot) { slot = &pll_[0]; for (auto& p : pll_) if (p.born < slot->born) slot = &p; }
        slot->start(note, sr_, t_);
    }
    void capturePending() {
        if (!cap_pending_ || set_.voice == 0) return;
        bool left = false, did = false;
        for (int p = 0; p < 128; ++p) {
            const int64_t at = cap_at_[static_cast<std::size_t>(p)];
            if (at < 0) continue;
            if (!held_[static_cast<std::size_t>(p)]) { cap_at_[static_cast<std::size_t>(p)] = -1; continue; }
            if (t_ < at || did) { left = true; continue; }
            double hz = bloom_detail::midiHz(p);
            for (const auto& q : pll_) if (q.note == p && q.locked) hz = q.f();
            strings_.capture(p, hz, static_cast<uint64_t>(t_));
            cap_at_[static_cast<std::size_t>(p)] = -1;
            did = true;  // one capture per block keeps the block cost flat
        }
        cap_pending_ = left ? 1 : 0;
    }

    int keyTonic(int* scale) const {
        if (set_.key >= 0) { *scale = set_.scale; return set_.key; }
        const int k = key_.key();
        if (k < 0) return -1;
        *scale = k >= 12 ? 1 : 0;
        return k % 12;
    }
    // The ladder: the chord's tones from the phrase's lowest note up Range octaves. Its seed (for Walk / Random) is
    // the chord itself, so a chord always walks the same way.
    void rebuildLadder() {
        const int* src = (set_.trigger == 3 && snap_n_ > 0) ? snap_ : mem_;
        const int nn = (set_.trigger == 3 && snap_n_ > 0) ? snap_n_ : mem_n_;
        if (nn <= 0) { ladder_n_ = 0; return; }
        int sc = 0;
        const int tonic = keyTonic(&sc);
        chord_ = bloom_detail::buildChord(src, nn, tonic, sc, set_.extend);
        int low = 127;
        for (int i = 0; i < nn; ++i) low = std::min(low, src[i]);
        const int oct = std::clamp(set_.range, 1, 4);
        bool in[12] = {false};
        for (int i = 0; i < chord_.n; ++i) in[chord_.tones[i]] = true;
        int nl = 0;
        for (int p = low; p <= std::min(low + 12 * oct, 108) && nl < 48; ++p) if (in[p % 12]) ladder_[nl++] = p;
        ladder_n_ = nl;
        uint32_t h = 2166136261u;
        auto mix = [&](uint32_t v) { h = (h ^ v) * 16777619u; };
        mix(static_cast<uint32_t>(low));
        for (int i = 0; i < nl; ++i) mix(static_cast<uint32_t>(ladder_[i]));
        mix(static_cast<uint32_t>(set_.pattern));
        seed_ = h;
    }

    // ── onsets & gestures ──
    void onOnset(uint32_t off, std::vector<MidiEvent>& out) {
        BTRACE("%.4f onset (at %.4f)\n", double(t_) / sr_, double(onsets_.onsetTime()) / sr_);
        strings_.onsetClick();
        pending_strength_ = true;
        onset_s_ = onsets_.onsetTime();
        onset_used_ = false;
        if (set_.trigger == 0 && active_ && !set_.engage) stop(off, out, 0.06);
    }
    void onChoke(uint32_t off, std::vector<MidiEvent>& out) {
        BTRACE("%.4f choke\n", double(t_) / sr_);
        // the fretting hand muted the strings: stop, and forget the chord (it was not let ring)
        if (set_.trigger <= 1 && !set_.engage) {
            if (active_) stop(off, out, 0.03);
            mem_n_ = 0; ladder_n_ = 0;
        }
    }
    void updateStrength() {
        // pick strength of the last attack vs the running average of attacks: loudness only (Touch), never which notes
        const float db = onsets_.strengthDb();
        if (!strength_init_) { strength_mean_ = db; strength_init_ = true; }
        strength_mean_ += (db - strength_mean_) * 0.15f;
        last_strength_ = std::clamp(0.5f + (db - strength_mean_) / 12.0f, 0.0f, 1.0f);
        groove_.onset(static_cast<double>(onset_s_) / sr_, db, bpm_);
    }

    // A new phrase starts at `origin` (the attack, in samples). Always / Engage restart the figure there when the chord
    // changed (Retrigger Chord) or on every attack (Retrigger Pick); Bloom and Freeze start theirs when they begin.
    void newPhrase(int64_t origin) {
        phrase_pending_ = origin;
        if (!active_ || set_.trigger == 0 || set_.trigger == 3) return;
        const uint32_t prev = playing_seed_;
        if (set_.retrig == 0 && prev == seed_ && ladder_n_ > 0) return;   // the same chord struck again plays on
        beginPhrase(origin);
    }
    void beginPhrase(int64_t origin) {
        BTRACE("%.4f beginPhrase origin %.4f ladder0 %d n %d\n", double(t_) / sr_, double(origin) / sr_, ladder_n_ ? ladder_[0] : -1, ladder_n_);
        div_ = currentDiv();
        from_attack_ = origin < t_;
        phrase_period_ = beatPeriodSec();   // fixed for the phrase: the figure runs on its own clock, from its start
        if (!from_attack_ && set_.feel && groove_.locked()) {
            // Bloom / Engage / Freeze begin with no pick under them: enter on the next 16th of the learned groove
            const double now_b = groove_.beatAt(static_cast<double>(t_) / sr_);
            const double next = std::ceil(now_b * 4.0 - 1e-6) / 4.0;
            origin = t_ + static_cast<int64_t>((next - now_b) * phrase_period_ * sr_);
        }
        phrase_origin_ = origin;
        phrase_k_ = 0;
        playing_seed_ = seed_;
        catchUp(true);
    }
    void updateTrigger(uint32_t n, double ppq, bool host_playing, std::vector<MidiEvent>& out) {
        if (pending_strength_ && t_ - onsets_.lastOnset() > static_cast<int64_t>(0.025 * sr_)) { pending_strength_ = false; updateStrength(); }
        const bool eng = set_.engage, rising = eng && !prev_engage_;
        prev_engage_ = eng;
        bool want = active_;
        const double since = static_cast<double>(t_ - onsets_.lastOnset()) / sr_;
        progress_ = 0;
        switch (set_.trigger) {
            case 0:  // Bloom
                if (eng) want = mem_n_ > 0;
                else if (!active_) {
                    const bool ringing = onsets_.ringDb() > -52.0f;
                    if (mem_n_ > 0 && ringing) progress_ = static_cast<float>(std::clamp(since / std::max(0.05f, set_.bloom_s), 0.0, 1.0));
                    want = mem_n_ > 0 && ringing && since >= set_.bloom_s;
                } else if (!eng && prev_eng_forced_) want = false;
                break;
            case 1: want = mem_n_ > 0; break;
            case 2: want = eng && mem_n_ > 0; break;
            case 3:
                if ((rising || (eng && snap_n_ == 0)) && mem_n_ > 0) { std::copy(mem_, mem_ + mem_n_, snap_); snap_n_ = mem_n_; rebuildLadder(); }
                want = eng && snap_n_ > 0;
                if (!eng) snap_n_ = 0;
                break;
        }
        prev_eng_forced_ = set_.trigger == 0 && eng;
        if (want && !active_) start(ppq, host_playing);
        else if (!want && active_) stop(n > 0 ? n - 1 : 0, out, 0.08);
        if (active_ && ladder_n_ == 0) rebuildLadder();
    }
    void start(double ppq, bool host_playing) {
        active_ = true;
        rebuildLadder();
        if (!(set_.feel && groove_.locked()) && host_playing) beat_ = ppq;
        // Always: the figure starts at the attack that brought the chord (already behind us: catch up from there);
        // Bloom / Engage / Freeze: it starts now, the moment they begin
        const bool from_attack = set_.trigger == 1 && phrase_pending_ >= 0 && t_ - phrase_pending_ < static_cast<int64_t>(0.25 * sr_);
        beginPhrase(from_attack ? phrase_pending_ : t_);
    }
    void stop(uint32_t off, std::vector<MidiEvent>& out, double fade_s) {
        BTRACE("%.4f stop\n", double(t_) / sr_);
        active_ = false;
        allOff(out, off);
        strings_.releaseAll(fade_s);
    }

    // ── clock ──
    int currentDiv() const {
        static const int kDivs[7] = {1, 2, 3, 4, 6, 8, 12};
        const int base = std::clamp(set_.rate, 0, 5);
        return kDivs[std::clamp(base + static_cast<int>(std::lround((set_.motion - 0.5) * 4.0)), 0, 6)];   // Motion: the pedal
    }
    double swingRatio() const {
        double s = 0.5 + 0.25 * set_.swing;
        if (set_.feel && groove_.locked()) s = std::max(s, static_cast<double>(groove_.swing()));
        return s;
    }
    double warp(double x) const {
        if (div_ != 1 && div_ != 2 && div_ != 4 && div_ != 8) return x;
        const double s = swingRatio();
        const double b = std::floor(x), u = x - b;
        return b + (u < 0.5 ? u * 2.0 * s : s + (u - 0.5) * 2.0 * (1.0 - s));
    }
    // Step k of the phrase, in samples from its start (the pick): the subdivision, swung on the phrase's own beats
    // (its start is beat 0), at the tempo the phrase began with. Nothing else moves it, so it is the same every time.
    int64_t stepTime(long long k) const {
        const double x = warp(static_cast<double>(k) / div_);
        return phrase_origin_ + static_cast<int64_t>(std::llround(x * phrase_period_ * sr_));
    }
    double beatPeriodSec() const {
        if (set_.feel && groove_.locked()) return groove_.period();
        return 60.0 / std::clamp(bpm_, 30.0, 300.0);
    }
    void advanceClock() {
        double inc = 1.0 / (beatPeriodSec() * sr_);
        if (set_.feel && groove_.locked()) {
            const double target = groove_.beatAt(static_cast<double>(t_) / sr_);
            double e = target - beat_;
            e -= std::round(e);
            inc += e / (0.25 * sr_);
        }
        beat_ += inc;
    }
    // Steps already behind (a phrase started at an attack the notes arrived after): skip all but the latest, which
    // plays now. The tracker's delay is the same every time, so the same steps are skipped every time.
    void catchUp(bool) {
        while (stepTime(phrase_k_ + 1) <= t_) ++phrase_k_;
    }
    void stepTick(uint32_t i, std::vector<MidiEvent>& out) {
        const int d = currentDiv();
        if (d != div_) {   // Motion moved: same phrase, new subdivision, from the next step of it
            const double rel = static_cast<double>(t_ - phrase_origin_) / (phrase_period_ * sr_) * d;
            div_ = d;
            phrase_k_ = std::max<long long>(0, static_cast<long long>(std::ceil(rel - 1e-9)));
        }
        if (t_ >= stepTime(phrase_k_)) {
            catchUp(false);
            // A figure that starts at a pick: the steps inside the strum window are counted, not played. The pick is
            // already sounding there, and the chord is only known once the window closes, so the first note heard is
            // always the full chord's, on the grid: the same every time.
            if (!(from_attack_ && t_ <= win_until_)) fire(i, phrase_k_, out);
            ++phrase_k_;
        }
    }

    // ── the figure: a pure function of the chord (ladder + seed), the step and the melody note ──
    static uint32_t hash2(uint32_t a, uint32_t b) {
        uint32_t h = a ^ (b * 0x9e3779b9u);
        h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
        return h;
    }
    int noteAt(long long k) const {
        const int N = ladder_n_;
        if (N <= 0) return -1;
        const long long kk = k < 0 ? 0 : k;
        switch (set_.pattern) {
            case 0: return ladder_[kk % N];
            case 1: return ladder_[N - 1 - kk % N];
            case 2: {
                if (N == 1) return ladder_[0];
                const long long per = 2 * N - 2, m = kk % per;
                return ladder_[m < N ? m : per - m];
            }
            case 3: {
                const long long per = std::max(1, 2 * ((N + 1) / 2)), m = kk % per;
                return ladder_[std::clamp<long long>(m % 2 == 0 ? m / 2 : N - 1 - m / 2, 0, N - 1)];
            }
            case 4: {  // walk: a one-bar motif (16 steps) the chord's seed draws, then it repeats
                int idx = 0, dir = 1;
                const int m = static_cast<int>(kk % 16);
                for (int j = 1; j <= m; ++j) {
                    const uint32_t r = hash2(seed_, static_cast<uint32_t>(j)) % 100;
                    int st = r < 38 ? 1 : r < 58 ? 2 : r < 84 ? -1 : r < 94 ? -2 : 3;
                    if (dir < 0) st = -st;
                    if (hash2(seed_ ^ 0x5bd1e995u, static_cast<uint32_t>(j)) % 100 < 18) dir = -dir;
                    int nidx = idx + st;
                    if (nidx >= N) { nidx = std::max(0, N - 1 - (nidx - (N - 1))); dir = -1; }
                    if (nidx < 0) { nidx = std::min(N - 1, -nidx); dir = 1; }
                    idx = nidx;
                }
                return ladder_[std::clamp(idx, 0, N - 1)];
            }
            case 5: {  // melody on top: up through the chord below the melody note, landing on it every beat
                const bool fresh = melody_ >= 0 && t_ - melody_t_ < static_cast<int64_t>(6.0 * sr_);
                if (fresh && kk % std::max(1, div_) == 0) return melody_;
                int lim = N;
                if (fresh) while (lim > 1 && ladder_[lim - 1] >= melody_) --lim;
                return ladder_[kk % lim];
            }
            case 6: {  // pedal: the bass between every chord tone
                if (kk % 2 == 0 || N == 1) return ladder_[0];
                return ladder_[1 + (kk / 2) % (N - 1)];
            }
            default: {  // random, but the same for the same chord: a seeded one-bar motif
                const int m = static_cast<int>(kk % 16);
                int idx = static_cast<int>(hash2(seed_, static_cast<uint32_t>(m)) % static_cast<uint32_t>(N));
                if (m > 0 && N > 1) {
                    const int prev = static_cast<int>(hash2(seed_, static_cast<uint32_t>(m - 1)) % static_cast<uint32_t>(N));
                    if (idx == prev) idx = (idx + 1) % N;
                }
                return ladder_[idx];
            }
        }
    }
    int beatSlot() const {
        double fr = beat_ - std::floor(beat_);
        return std::clamp(static_cast<int>(std::floor(fr * 4.0 + 0.25)), 0, 3) & 3;
    }
    void fire(uint32_t i, long long k, std::vector<MidiEvent>& out) {
        if (ladder_n_ == 0) rebuildLadder();
        const int note = noteAt(k);
        BTRACE("%.4f fire k %lld note %d (ladder0 %d)\n", double(t_) / sr_, k, note, ladder_n_ ? ladder_[0] : -1);
        steps_ = static_cast<int>(k);
        if (note < 0) return;
        const double step_s = phrase_period_ / std::max(1, div_);
        const bool muted = onsets_.muted();
        double g = std::clamp(static_cast<double>(set_.gate) * (0.5 + set_.motion), 0.05, 1.25);
        if (muted) g *= 1.0 - 0.85 * set_.choke;
        g = std::max(g, 0.04);
        const double len = std::max(0.012, step_s * g);
        // accents from the step's place in the beat; Touch scales it all by how hard the phrase was picked
        const int slot = static_cast<int>((k * 4 / std::max(1, div_)) & 3);
        const float accent = (set_.feel && groove_.locked()) ? groove_.accent(slot) : kAcc[slot];
        const float base = 0.8f + set_.touch * (std::clamp(0.45f + 0.7f * (last_strength_ - 0.5f) + 0.35f * last_vel_, 0.15f, 1.0f) - 0.8f);
        const float vel = std::clamp(base * accent, 0.05f, 1.0f);
        if (set_.voice != 1) {
            for (int q = 0; q < pend_n_;) {
                if (pend_[q].note == note || g >= 1.0) { out.push_back({MidiEvent::NoteOff, pend_[q].note, 0.0f, 0.0f, i}); pend_[q] = pend_[--pend_n_]; }
                else ++q;
            }
            out.push_back({MidiEvent::NoteOn, note, vel, 0.0f, i});
            if (pend_n_ < static_cast<int>(pend_.size())) pend_[pend_n_++] = {note, t_ + static_cast<int64_t>((g >= 1.0 ? 8.0 : len) * sr_)};
        }
        if (set_.voice >= 1) {
            const double hz = bloom_detail::midiHz(note) * std::pow(2.0, tune_cents_ / 1200.0);
            strings_.noteOn(note, hz, vel, g >= 1.0 ? 4.0 : len, set_.tone, 2.0f * set_.level);
        }
        arp_note_ = note;
    }

    // ── bends & tuning ──
    void updateBend(bool mono_seen, float mono_bend, uint32_t n, std::vector<MidiEvent>& out) {
        float best = 0, mx = 0;
        float tune_sum = 0; int tune_n = 0;
        for (const auto& p : pll_) if (p.note >= 0) mx = std::max(mx, p.amp);
        for (const auto& p : pll_) {
            if (p.note < 0 || !p.locked || p.amp < 0.12f * mx || p.amp < 2e-4f) continue;
            const float b = p.bendCents();
            if (std::fabs(b) > std::fabs(best)) best = b;
            if (p.base_set) { tune_sum += p.base_cents; ++tune_n; }
        }
        if (tune_n > 0) tune_cents_ += (std::clamp(tune_sum / tune_n, -50.0f, 50.0f) - tune_cents_) * 0.05f;
        if (std::fabs(best) < 8.0f) best = 0;  // dead zone: finger pressure, not a bend
        float target = best / 100.0f;
        if (mono_seen) target = mono_bend;
        else if (have_mono_bend_) target = last_mono_bend_;
        if (mono_seen) { have_mono_bend_ = true; last_mono_bend_ = mono_bend; }
        const float k = 1.0f - std::exp(-static_cast<float>(n) / static_cast<float>(0.006 * sr_));
        bend_out_ += (target * set_.bend - bend_out_) * k;
        strings_.setPitchRatio(std::pow(2.0, bend_out_ / 12.0));
        const float send = bend_out_ + tune_cents_ / 100.0f;
        if (set_.voice != 1 && std::fabs(send - last_bend_sent_) > 0.004f) {
            out.push_back({MidiEvent::PitchBend, arp_note_ >= 0 ? arp_note_ : 60, 0.0f, send, n > 0 ? n - 1 : 0});
            last_bend_sent_ = send;
        }
    }

    void publish() {
        live_.active = active_;
        live_.feel_locked = groove_.locked();
        live_.muted = onsets_.muted();
        live_.key = set_.key >= 0 ? set_.key + (set_.scale == 1 || set_.scale == 2 || set_.scale == 4 || set_.scale == 6 ? 12 : 0) : key_.key();
        live_.key_conf = set_.key >= 0 ? 1.0f : key_.confidence();
        std::snprintf(live_.chord, sizeof live_.chord, "%s", (mem_n_ > 0 || snap_n_ > 0) ? chord_.name : "");
        live_.root = chord_.root;
        live_.ladder_n = ladder_n_;
        for (int i = 0; i < ladder_n_; ++i) live_.ladder[i] = ladder_[i];
        live_.note = arp_note_;
        live_.engaged = set_.engage;
        live_.idx = -1;
        for (int i = 0; i < ladder_n_ && arp_note_ >= 0; ++i) if (ladder_[i] == arp_note_) { live_.idx = i; break; }
        live_.step = steps_;
        live_.bpm = static_cast<float>(60.0 / beatPeriodSec());
        live_.swing = static_cast<float>(swingRatio());
        live_.strength = last_strength_;
        live_.bend_cents = bend_out_ * 100.0f;
        live_.tune_cents = tune_cents_;
        live_.ring_db = onsets_.ringDb();
        live_.beat = beat_;
        live_.div = div_;
        live_.bloom_progress = progress_;
    }

    static constexpr float kAcc[4] = {1.0f, 0.78f, 0.88f, 0.78f};
    double sr_ = 48000, bpm_ = 120;
    float kDryUp_ = 0.01f, kDryDown_ = 0.0002f;
    BloomSettings set_{};
    BloomLive live_{};
    bloom_detail::Onsets onsets_;
    bloom_detail::Groove groove_;
    bloom_detail::KeyTracker key_;
    bloom_detail::Strings strings_;
    std::array<bloom_detail::NotePll, kPll> pll_{};
    std::array<uint8_t, 128> held_{}, src_off_{};
    std::array<float, 128> heldVel_{};
    std::array<int16_t, 128> alias_{};
    std::array<int64_t, 128> cap_at_{};
    int mem_[128] = {0}, snap_[128] = {0};
    int mem_n_ = 0, snap_n_ = 0, cap_pending_ = 0;
    int win_note_[16] = {0};
    float win_vel_[16] = {0};
    int win_n_ = 0, clus_n_ = 0;
    int64_t win_until_ = -1, win_origin_ = 0, clus_t0_ = 0;
    bool win_started_ = true;
    int clus_note_[16] = {0};
    float clus_vel_[16] = {0};
    std::array<int64_t, 128> off_t_{};
    std::vector<MidiEvent>* out_ = nullptr;
    Chord chord_{};
    int ladder_[48] = {0};
    int ladder_n_ = 0;
    uint32_t seed_ = 0, playing_seed_ = 0;
    bool active_ = false, strength_init_ = false, prev_engage_ = false, prev_eng_forced_ = false, pending_strength_ = false, have_mono_bend_ = false;
    bool onset_used_ = true, from_attack_ = false;
    int arp_note_ = -1, steps_ = 0, div_ = 4, melody_ = -1;
    long long phrase_k_ = 0;
    double beat_ = 0, phrase_period_ = 0.5;
    int64_t phrase_origin_ = 0, phrase_pending_ = -1, onset_s_ = 0;
    std::array<Pending, 64> pend_{};
    int pend_n_ = 0;
    int64_t t_ = 0, melody_t_ = -1;
    float dry_g_ = 1.0f, bend_out_ = 0, last_bend_sent_ = 0, last_strength_ = 0.5f, strength_mean_ = -30.0f, last_vel_ = 0.7f;
    float tune_cents_ = 0, progress_ = 0, last_mono_bend_ = 0;
};

} // namespace rig::midi
