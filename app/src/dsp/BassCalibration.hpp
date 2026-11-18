#pragma once
// Bass-register loudness calibration: a deterministic synthetic bass DI and the bass-amp tables. (Which rigs
// count as bass rigs: RackGraph::calibratesAsBass.) RackGraph::calibrateLoudness renders this instead of the guitar-register calibration DI
// (amp::makeCalibrationDI) for bass rigs, so a bass rig played with a bass lands at the same loudness as a
// guitar preset played with a guitar (the guitar DI under-drives a bass rig's low end, and bass rigs came out
// about 4-5 LU hotter than guitar presets on real bass playing).
#include "AmpModeling.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace rig::amp {

// The bass DI is denser than real playing: rendered through 14 bass rigs, real-dynamics bass performances (the
// library QA's fingerstyle clip and an octave-down guitar riff) landed 1.7 LU under guitar presets played with
// guitar clips. Bass rigs therefore aim this much above the guitar target.
inline constexpr float kBassCalibrationTrimDb = 1.7f;

// Bass amps in the catalog: Ampeg SVT 1969 (54000), Gallien-Krueger 800RB (150000), Ampeg SVT-CL (177000).
inline bool isBassAmpTable(int32_t t) noexcept { return t == 54000 || t == 150000 || t == 177000; }

// A fingerstyle passive-pickup bass line in E (open E to the 12th fret: root eighths, octaves, a fifth walk-up,
// a sustained low note and a ghost note), Karplus-Strong strings with a finger (not pick) excitation, a pluck-
// position comb and a ~2.5 kHz passive pickup / cable roll-off. Levelled like makeCalibrationDI (about
// -19 dBFS RMS). Original synthesis; deliberately not the library QA's clip.
inline std::vector<float> makeBassCalibrationDI(double sr, float seconds = 4.0f) {
    const std::size_t n = static_cast<std::size_t>(sr * seconds);
    std::vector<float> out(n, 0.0f);
    uint32_t rng = 0x2468ACEu;
    auto rnd = [&rng]() {
        rng = rng * 1664525u + 1013904223u;
        return static_cast<float>((rng >> 8) & 0xFFFF) / 32768.0f - 1.0f;
    };
    struct Note { float t, hz, dur, vel; bool mute; };
    std::vector<Note> notes;
    const float E1 = 41.20f, G1 = 49.0f, A1 = 55.0f, B1 = 61.74f, D2 = 73.42f, E2 = 82.41f, G2 = 98.0f, A2 = 110.0f;
    const float e8 = 0.3125f; // eighths at 96 BPM
    // Bar 1: root eighths on E with an octave pop.
    const float bar1[8] = {E1, E1, E2, E1, E1, E1, E2, D2};
    for (int i = 0; i < 8; ++i) notes.push_back({0.02f + i * e8 * 0.5f, bar1[i], e8 * 0.48f, i % 2 ? 0.75f : 0.9f, false});
    // Walk-up to A and a sustained A.
    const float walk[4] = {G1, A1, B1, D2};
    for (int i = 0; i < 4; ++i) notes.push_back({1.27f + i * e8 * 0.5f, walk[i], e8 * 0.46f, 0.85f, false});
    notes.push_back({1.90f, A1, 0.85f, 0.95f, false});
    // Ghost note, then a higher fill and the low E ringing out.
    notes.push_back({2.78f, E2, 0.06f, 0.5f, true});
    const float fill[4] = {G2, A2, G2, E2};
    for (int i = 0; i < 4; ++i) notes.push_back({2.86f + i * e8 * 0.5f, fill[i], e8 * 0.46f, 0.8f, false});
    notes.push_back({3.20f, E1, 0.78f, 1.0f, false});

    for (const auto& nt : notes) {
        const std::size_t start = static_cast<std::size_t>(nt.t * sr);
        const std::size_t len = static_cast<std::size_t>(nt.dur * sr);
        const std::size_t period = std::max<std::size_t>(2, static_cast<std::size_t>(sr / nt.hz));
        std::vector<float> line(period, 0.0f);
        // Finger excitation: a smooth half-sine bump plus a little dark noise, then a pluck-position comb (1/5).
        const std::size_t w = std::max<std::size_t>(4, period / 5);
        float lp = 0.0f;
        for (std::size_t i = 0; i < period; ++i) {
            const float bump = i < w ? std::pow(std::sin(3.14159265f * static_cast<float>(i) / static_cast<float>(w)), 2.0f) : 0.0f;
            lp += 0.3f * (rnd() - lp);
            line[i] = bump + 0.08f * lp;
        }
        float mean = 0.0f;
        for (float v : line) mean += v;
        mean /= static_cast<float>(period);
        for (auto& v : line) v -= mean;
        const std::size_t d = std::max<std::size_t>(1, period / 5);
        for (std::size_t i = period; i-- > d;) line[i] -= 0.6f * line[i - d];
        const float damp = nt.mute ? 0.990f : 0.99985f, mix = nt.mute ? 0.6f : 0.22f;
        std::size_t idx = 0;
        for (std::size_t i = 0; i < len && start + i < n; ++i) {
            const float cur = line[idx];
            const float nxt = line[(idx + 1) % period];
            line[idx] = damp * ((1.0f - mix) * cur + mix * 0.5f * (cur + nxt));
            idx = (idx + 1) % period;
            const float rel = (i + 1024 > len) ? static_cast<float>(len - i) / 1024.0f : 1.0f;
            out[start + i] += cur * nt.vel * rel;
        }
    }
    // Passive pickup + cable: gentle resonant roll-off around 2.5 kHz; DC / subsonic high-pass at 28 Hz.
    {
        Svf pu, hp;
        pu.set(Svf::Type::LowPass, sr, 2500.0f, 0.9f);
        hp.set(Svf::Type::HighPass, sr, 28.0f, 0.7f);
        for (auto& v : out) v = hp.process(pu.process(v));
    }
    double e = 0.0;
    for (float v : out) e += static_cast<double>(v) * v;
    if (e > 0.0) {
        const float rms = static_cast<float>(std::sqrt(e / static_cast<double>(out.size())));
        const float g = 0.112f / rms;
        for (auto& v : out) v = std::clamp(v * g, -0.95f, 0.95f);
    }
    return out;
}

} // namespace rig::amp
