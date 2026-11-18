#pragma once
// Headroom synth layer: MIDI effects + an 8-voice virtual-analog polysynth with its own FX.
//
// Oscillators: PolyBLEP saw/square, triangle, sine, 3-voice unison, sub, noise.
// Filter: zero-delay-feedback 4-pole transistor ladder (Zavalishin TPT) with per-stage saturation.
// Envelopes: exponential ADSR (amp + filter). LFO → pitch / cutoff. Glide. Velocity. Guitar-follow:
// voice level tracks the picked string's envelope for expressive, guitar-like dynamics.
// FX: stereo chorus, tempo delay, FDN plate.
#include "PitchToMidi.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace rig::midi {

// Guitar Follow's full-level point: the follow gain is 1 when the guitar's short-term RMS (the Follow detector's
// ~10 ms envelope, 1.0 FS = 1 V at the input jack) is 1/kFollowGain = 0.0447 FS, i.e. -27 dBFS, rising to its 1.2 cap
// at -25.4 dBFS and falling linearly (in amplitude) below. -27 dBFS is the median 100 ms RMS of playing at the level
// the app plays guitar: the DI player normalises tracks to 0.35 FS peak, and a passive single coil measures roughly
// 30-50 mV RMS while played (Elliott Sound Products, "Guitar Pickup Output Voltages"; 0.15-0.45 V peaks). Until
// 2026-11-13 the gain was 9 (full level at -19 dBFS), which only the raw test DIs, mastered to about -1 dBFS peak
// (~8 dB hotter), ever reached, so any Follow setting cut the synth by 6-15 dB at a real playing level.
inline constexpr float kFollowGain = 22.4f;

// ───────────────────────────── MIDI effects ─────────────────────────────
struct MidiFxSettings {
    int octave = 0, transpose = 0;
    std::string harmony = "off";
    int scale_root = 4;
    std::string scale = "chromatic";
    std::string arp = "off";
    std::string arp_rate = "1/16";
    bool hold = false;
    float bpm = 120.0f;
};

class MidiFx {
public:
    void prepare(double sr) { sr_ = sr; held_.clear(); sounding_.clear(); arp_phase_ = 0.0; arp_idx_ = 0; arp_note_ = -1; }
    void setBpm(float b) noexcept { set_.bpm = b; }  // tempo follow (allocation-free, audio thread)
    void setSettings(const MidiFxSettings& s) { set_ = s; }
    const MidiFxSettings& settings() const { return set_; }

    // Transforms detector events into synth events; also runs the arpeggiator clock.
    void process(const std::vector<MidiEvent>& in, uint32_t frames, std::vector<MidiEvent>& out) {
        for (const auto& e : in) {
            if (e.type == MidiEvent::PitchBend) {
                out.push_back(e);
                continue;
            }
            if (e.type == MidiEvent::NoteOn) {
                if (set_.hold && held_.empty() && !sounding_.empty()) releaseAll(e.offset, out); // new phrase replaces latch
                held_.push_back({e.note, e.velocity});
                if (set_.arp == "off") {
                    for (int n : expand(e.note)) noteOn(n, e.velocity, e.offset, e.note, out);
                }
            } else {
                held_.erase(std::remove_if(held_.begin(), held_.end(), [&](const Held& h) { return h.note == e.note; }), held_.end());
                if (!set_.hold && set_.arp == "off") {
                    for (auto it = sounding_.begin(); it != sounding_.end();) {
                        if (it->src == e.note) {
                            out.push_back({MidiEvent::NoteOff, it->out, 0.0f, 0.0f, e.offset});
                            it = sounding_.erase(it);
                        } else ++it;
                    }
                }
            }
        }
        if (set_.arp != "off") runArp(frames, out);
        else if (arp_note_ >= 0) {
            out.push_back({MidiEvent::NoteOff, arp_note_, 0.0f, 0.0f, 0});
            arp_note_ = -1;
        }
    }

    void releaseAll(uint32_t off, std::vector<MidiEvent>& out) {
        for (const auto& s : sounding_) out.push_back({MidiEvent::NoteOff, s.out, 0.0f, 0.0f, off});
        sounding_.clear();
        if (arp_note_ >= 0) out.push_back({MidiEvent::NoteOff, arp_note_, 0.0f, 0.0f, off});
        arp_note_ = -1;
    }

    void sounding(std::vector<std::pair<int, float>>& v) const {
        v.clear();
        for (const auto& s : sounding_) v.emplace_back(s.out, s.vel);
        if (arp_note_ >= 0) v.emplace_back(arp_note_, arp_vel_);
    }

private:
    struct Held { int note; float vel; };
    struct Sounding { int src; int out; float vel; };

    static bool inScale(int pc, const std::string& sc) {
        static const std::vector<std::pair<std::string, std::vector<int>>> kScales = {
            {"major", {0, 2, 4, 5, 7, 9, 11}}, {"minor", {0, 2, 3, 5, 7, 8, 10}}, {"dorian", {0, 2, 3, 5, 7, 9, 10}},
            {"mixolydian", {0, 2, 4, 5, 7, 9, 10}}, {"pentatonic_minor", {0, 3, 5, 7, 10}}, {"pentatonic_major", {0, 2, 4, 7, 9}},
            {"blues", {0, 3, 5, 6, 7, 10}}};
        for (const auto& kv : kScales)
            if (kv.first == sc) return std::find(kv.second.begin(), kv.second.end(), pc) != kv.second.end();
        return true;
    }
    int quantize(int n) const {
        if (set_.scale == "chromatic") return n;
        for (int d = 0; d < 6; ++d) {
            if (inScale(((n - d - set_.scale_root) % 12 + 12) % 12, set_.scale)) return n - d;
            if (inScale(((n + d - set_.scale_root) % 12 + 12) % 12, set_.scale)) return n + d;
        }
        return n;
    }
    // Diatonic step above `n` within the scale (falls back to major/minor thirds).
    int diatonicUp(int n, int steps) const {
        if (set_.scale == "chromatic") return n + (steps == 2 ? 4 : 7);
        int m = n, k = 0;
        while (k < steps && m < n + 13) {
            ++m;
            if (inScale(((m - set_.scale_root) % 12 + 12) % 12, set_.scale)) ++k;
        }
        return m;
    }
    std::vector<int> expand(int in) const {
        const int base = quantize(in + set_.transpose + 12 * set_.octave);
        std::vector<int> v{base};
        const auto& h = set_.harmony;
        if (h == "octave") v.push_back(base + 12);
        else if (h == "fifth") v.push_back(base + 7);
        else if (h == "power") { v.push_back(base + 7); v.push_back(base + 12); }
        else if (h == "third") v.push_back(diatonicUp(base, 2));
        else if (h == "major") { v.push_back(base + 4); v.push_back(base + 7); }
        else if (h == "minor") { v.push_back(base + 3); v.push_back(base + 7); }
        else if (h == "sus4") { v.push_back(base + 5); v.push_back(base + 7); }
        for (auto& n : v) n = std::clamp(n, 0, 127);
        return v;
    }
    void noteOn(int n, float vel, uint32_t off, int src, std::vector<MidiEvent>& out) {
        for (auto it = sounding_.begin(); it != sounding_.end();) {
            if (it->out == n) {
                out.push_back({MidiEvent::NoteOff, n, 0.0f, 0.0f, off});
                it = sounding_.erase(it);
            } else ++it;
        }
        out.push_back({MidiEvent::NoteOn, n, vel, 0.0f, off});
        sounding_.push_back({src, n, vel});
    }
    void runArp(uint32_t frames, std::vector<MidiEvent>& out) {
        double div = 4.0; // 1/16
        if (set_.arp_rate == "1/8") div = 2.0;
        else if (set_.arp_rate == "1/8T") div = 3.0;
        else if (set_.arp_rate == "1/16T") div = 6.0;
        else if (set_.arp_rate == "1/32") div = 8.0;
        else if (set_.arp_rate == "1/4") div = 1.0;
        const double step = sr_ * 60.0 / std::max(30.0f, set_.bpm) / div;
        std::vector<int> pool;
        for (const auto& h : held_)
            for (int n : expand(h.note)) pool.push_back(n);
        std::sort(pool.begin(), pool.end());
        pool.erase(std::unique(pool.begin(), pool.end()), pool.end());
        for (uint32_t i = 0; i < frames; ++i) {
            arp_phase_ += 1.0;
            if (arp_note_ >= 0 && arp_phase_ >= step * 0.8) {
                out.push_back({MidiEvent::NoteOff, arp_note_, 0.0f, 0.0f, i});
                arp_note_ = -1;
            }
            if (arp_phase_ >= step) {
                arp_phase_ -= step;
                if (pool.empty()) continue;
                const int sz = static_cast<int>(pool.size());
                int idx = 0;
                if (set_.arp == "up") idx = arp_idx_ % sz;
                else if (set_.arp == "down") idx = sz - 1 - (arp_idx_ % sz);
                else if (set_.arp == "updown") {
                    const int period = std::max(1, 2 * sz - 2);
                    const int k = arp_idx_ % period;
                    idx = k < sz ? k : period - k;
                } else idx = static_cast<int>(rng_() % static_cast<uint32_t>(sz));
                ++arp_idx_;
                arp_note_ = pool[static_cast<std::size_t>(std::clamp(idx, 0, sz - 1))];
                arp_vel_ = held_.empty() ? 0.8f : held_.back().vel;
                out.push_back({MidiEvent::NoteOn, arp_note_, arp_vel_, 0.0f, i});
            }
        }
    }

    double sr_ = 48000.0;
    MidiFxSettings set_{};
    std::vector<Held> held_;
    std::vector<Sounding> sounding_;
    double arp_phase_ = 0.0;
    int arp_idx_ = 0, arp_note_ = -1;
    float arp_vel_ = 0.8f;
    std::minstd_rand rng_{1234};
};

// ───────────────────────────── Synth ─────────────────────────────
struct SynthParamDef {
    const char* id;
    const char* name;
    float def;      // normalized default
    int steps;      // 0 = continuous, else number of options
    const char* labels; // '|' separated when stepped
};

inline const std::vector<SynthParamDef>& synthParamDefs() {
    static const std::vector<SynthParamDef> defs = {
        {"osc1_wave", "Osc 1", 0.0f, 4, "Saw|Square|Triangle|Sine"},
        {"osc1_level", "Osc 1 Level", 0.8f, 0, ""},
        {"osc2_wave", "Osc 2", 1.0f / 3.0f, 4, "Saw|Square|Triangle|Sine"},
        {"osc2_semi", "Osc 2 Pitch", 0.5f, 0, ""},
        {"osc2_detune", "Osc 2 Detune", 0.58f, 0, ""},
        {"osc2_level", "Osc 2 Level", 0.5f, 0, ""},
        {"sub_level", "Sub", 0.2f, 0, ""},
        {"noise_level", "Noise", 0.0f, 0, ""},
        {"unison", "Unison", 0.0f, 3, "1|2|3"},
        {"spread", "Detune Spread", 0.3f, 0, ""},
        {"cutoff", "Cutoff", 0.55f, 0, ""},
        {"resonance", "Resonance", 0.25f, 0, ""},
        {"env_amt", "Filter Env", 0.5f, 0, ""},
        {"keytrack", "Key Track", 0.5f, 0, ""},
        {"drive", "Drive", 0.2f, 0, ""},
        {"fenv_a", "F Attack", 0.05f, 0, ""},
        {"fenv_d", "F Decay", 0.4f, 0, ""},
        {"fenv_s", "F Sustain", 0.3f, 0, ""},
        {"fenv_r", "F Release", 0.35f, 0, ""},
        {"amp_a", "Attack", 0.05f, 0, ""},
        {"amp_d", "Decay", 0.4f, 0, ""},
        {"amp_s", "Sustain", 0.8f, 0, ""},
        {"amp_r", "Release", 0.4f, 0, ""},
        {"lfo_rate", "LFO Rate", 0.4f, 0, ""},
        {"lfo_pitch", "Vibrato", 0.0f, 0, ""},
        {"lfo_cutoff", "LFO → Filter", 0.0f, 0, ""},
        {"glide", "Glide", 0.0f, 0, ""},
        {"vel_sens", "Velocity", 0.5f, 0, ""},
        {"follow", "Guitar Follow", 0.4f, 0, ""},
        {"fx_chorus", "Chorus", 0.3f, 0, ""},
        {"fx_delay", "Delay", 0.15f, 0, ""},
        {"fx_delay_time", "Delay Time", 0.45f, 0, ""},
        {"fx_reverb", "Reverb", 0.3f, 0, ""},
        {"level", "Level", 0.7f, 0, ""},
    };
    return defs;
}

struct SynthPreset {
    std::string name;
    std::vector<std::pair<std::string, float>> values; // overrides (normalized)
};

inline const std::vector<SynthPreset>& synthPresets() {
    static const std::vector<SynthPreset> p = {
        {"Analog Pad", {{"osc1_wave", 0}, {"osc2_wave", 0}, {"osc2_detune", 0.62f}, {"unison", 1}, {"spread", 0.45f}, {"cutoff", 0.48f}, {"resonance", 0.15f},
                        {"env_amt", 0.25f}, {"amp_a", 0.45f}, {"amp_r", 0.6f}, {"fenv_a", 0.5f}, {"fenv_s", 0.6f}, {"fx_chorus", 0.6f}, {"fx_reverb", 0.55f}, {"follow", 0.25f}}},
        {"Saw Lead", {{"osc1_wave", 0}, {"osc2_wave", 0}, {"osc2_detune", 0.56f}, {"unison", 0.5f}, {"cutoff", 0.62f}, {"resonance", 0.35f}, {"env_amt", 0.45f},
                      {"amp_a", 0.02f}, {"amp_s", 0.9f}, {"glide", 0.25f}, {"lfo_pitch", 0.12f}, {"fx_delay", 0.3f}, {"fx_reverb", 0.25f}, {"follow", 0.5f}}},
        {"Sub Bass", {{"osc1_wave", 1.0f / 3.0f}, {"osc2_wave", 1.0f}, {"osc2_semi", 0.25f}, {"osc2_level", 0.6f}, {"sub_level", 0.8f}, {"cutoff", 0.32f},
                      {"resonance", 0.2f}, {"env_amt", 0.35f}, {"fenv_d", 0.3f}, {"amp_a", 0.0f}, {"amp_r", 0.2f}, {"fx_chorus", 0.0f}, {"fx_reverb", 0.05f}, {"drive", 0.45f}}},
        {"Brass Stab", {{"osc1_wave", 0}, {"osc2_wave", 0}, {"osc2_detune", 0.6f}, {"cutoff", 0.35f}, {"resonance", 0.1f}, {"env_amt", 0.75f}, {"fenv_a", 0.12f},
                        {"fenv_d", 0.35f}, {"fenv_s", 0.4f}, {"amp_a", 0.1f}, {"fx_reverb", 0.35f}, {"vel_sens", 0.8f}}},
        {"Glass Pluck", {{"osc1_wave", 2.0f / 3.0f}, {"osc2_wave", 1.0f}, {"osc2_semi", 0.75f}, {"osc2_level", 0.35f}, {"cutoff", 0.5f}, {"env_amt", 0.7f},
                         {"fenv_d", 0.3f}, {"fenv_s", 0.0f}, {"amp_d", 0.45f}, {"amp_s", 0.0f}, {"amp_r", 0.45f}, {"fx_delay", 0.35f}, {"fx_reverb", 0.45f}, {"follow", 0.0f}}},
        {"String Machine", {{"osc1_wave", 0}, {"osc2_wave", 1.0f / 3.0f}, {"osc2_semi", 0.75f}, {"unison", 1}, {"spread", 0.6f}, {"cutoff", 0.55f}, {"env_amt", 0.1f},
                            {"amp_a", 0.55f}, {"amp_r", 0.6f}, {"lfo_pitch", 0.06f}, {"lfo_rate", 0.5f}, {"fx_chorus", 0.85f}, {"fx_reverb", 0.6f}}},
        {"Drawbar Organ", {{"osc1_wave", 1.0f}, {"osc2_wave", 1.0f}, {"osc2_semi", 0.75f}, {"osc2_level", 0.6f}, {"sub_level", 0.5f}, {"cutoff", 0.85f},
                           {"env_amt", 0.0f}, {"amp_a", 0.0f}, {"amp_s", 1.0f}, {"amp_r", 0.1f}, {"lfo_pitch", 0.08f}, {"lfo_rate", 0.65f}, {"fx_chorus", 0.5f}, {"follow", 0.0f}}},
        {"Square Lead", {{"osc1_wave", 1.0f / 3.0f}, {"osc2_wave", 1.0f / 3.0f}, {"osc2_detune", 0.54f}, {"cutoff", 0.58f}, {"resonance", 0.3f}, {"glide", 0.3f},
                         {"lfo_pitch", 0.1f}, {"fx_delay", 0.35f}, {"follow", 0.6f}}},
        {"Octave Bass", {{"osc1_wave", 0}, {"osc2_wave", 1.0f / 3.0f}, {"osc2_semi", 0.25f}, {"osc2_level", 0.7f}, {"cutoff", 0.38f}, {"resonance", 0.35f},
                         {"env_amt", 0.55f}, {"fenv_d", 0.25f}, {"fenv_s", 0.1f}, {"amp_r", 0.15f}, {"drive", 0.35f}, {"fx_reverb", 0.0f}}},
        {"Shimmer Swell", {{"osc1_wave", 2.0f / 3.0f}, {"osc2_wave", 0}, {"osc2_semi", 1.0f}, {"osc2_level", 0.4f}, {"unison", 1}, {"spread", 0.5f}, {"cutoff", 0.6f},
                           {"amp_a", 0.7f}, {"amp_r", 0.8f}, {"fx_chorus", 0.5f}, {"fx_delay", 0.45f}, {"fx_delay_time", 0.6f}, {"fx_reverb", 0.85f}, {"follow", 0.15f}}},
        {"Acid", {{"osc1_wave", 0}, {"osc2_level", 0.0f}, {"sub_level", 0.0f}, {"cutoff", 0.25f}, {"resonance", 0.8f}, {"env_amt", 0.8f}, {"fenv_d", 0.25f},
                  {"fenv_s", 0.0f}, {"amp_s", 0.9f}, {"glide", 0.2f}, {"drive", 0.6f}, {"fx_delay", 0.25f}, {"fx_reverb", 0.1f}}},
        {"Choir Air", {{"osc1_wave", 2.0f / 3.0f}, {"osc2_wave", 2.0f / 3.0f}, {"osc2_detune", 0.65f}, {"noise_level", 0.12f}, {"unison", 1}, {"spread", 0.55f},
                       {"cutoff", 0.45f}, {"resonance", 0.4f}, {"amp_a", 0.6f}, {"amp_r", 0.7f}, {"lfo_cutoff", 0.15f}, {"fx_chorus", 0.7f}, {"fx_reverb", 0.75f}}},
    };
    return p;
}

class Synth {
public:
    static constexpr int kVoices = 8;

    void prepare(double sr) {
        sr_ = sr;
        params_.clear();
        for (const auto& d : synthParamDefs()) params_.push_back(d.def);
        applyPreset("Analog Pad");
        for (auto& v : voices_) v = Voice{};
        dl_.assign(static_cast<std::size_t>(sr * 2.0), 0.0f);
        dr_.assign(static_cast<std::size_t>(sr * 2.0), 0.0f);
        ch_.assign(4096, 0.0f);
        for (int i = 0; i < 4; ++i) {
            rv_[static_cast<std::size_t>(i)].assign(static_cast<std::size_t>(sr * kRvTimes[i]) + 1, 0.0f);
            rvp_[static_cast<std::size_t>(i)] = 0;
        }
        dpos_ = 0;
        cpos_ = 0;
    }

    int paramIndex(const std::string& id) const {
        const auto& d = synthParamDefs();
        for (std::size_t i = 0; i < d.size(); ++i)
            if (id == d[i].id) return static_cast<int>(i);
        return -1;
    }
    void setParam(const std::string& id, float norm) {
        const int i = paramIndex(id);
        if (i >= 0) params_[static_cast<std::size_t>(i)] = std::clamp(norm, 0.0f, 1.0f);
    }
    float param(const std::string& id) const {
        const int i = paramIndex(id);
        return i >= 0 ? params_[static_cast<std::size_t>(i)] : 0.0f;
    }
    const std::vector<float>& params() const { return params_; }
    bool applyPreset(const std::string& name) {
        for (const auto& p : synthPresets()) {
            if (p.name != name) continue;
            const auto& d = synthParamDefs();
            for (std::size_t i = 0; i < d.size(); ++i) params_[i] = d[i].def;
            for (const auto& kv : p.values) setParam(kv.first, kv.second);
            preset_ = name;
            return true;
        }
        return false;
    }
    const std::string& presetName() const { return preset_; }
    void setFollowLevel(float lv) { follow_level_ = lv; }

    static std::string formatParam(const SynthParamDef& d, float n) {
        char buf[48];
        const std::string id = d.id;
        if (d.steps > 0) {
            std::string l = d.labels;
            const int idx = std::clamp(static_cast<int>(std::lround(n * (d.steps - 1))), 0, d.steps - 1);
            std::size_t st = 0;
            for (int k = 0; k < idx; ++k) st = l.find('|', st) + 1;
            return l.substr(st, l.find('|', st) - st);
        }
        if (id == "cutoff") { std::snprintf(buf, sizeof buf, "%.0f Hz", cutoffHz(n)); return buf; }
        if (id == "osc2_semi") { std::snprintf(buf, sizeof buf, "%+d st", static_cast<int>(std::lround((n - 0.5f) * 48.0f))); return buf; }
        if (id == "osc2_detune") { std::snprintf(buf, sizeof buf, "%+.0f ct", (n - 0.5f) * 100.0f); return buf; }
        if (id.rfind("amp_", 0) == 0 || id.rfind("fenv_", 0) == 0) {
            if (id.back() == 's') { std::snprintf(buf, sizeof buf, "%.0f%%", n * 100.0f); return buf; }
            std::snprintf(buf, sizeof buf, "%.0f ms", envTime(n) * 1000.0f); return buf;
        }
        if (id == "lfo_rate") { std::snprintf(buf, sizeof buf, "%.2f Hz", 0.1f * std::pow(150.0f, n)); return buf; }
        if (id == "fx_delay_time") { std::snprintf(buf, sizeof buf, "%.0f ms", 60.0f + 900.0f * n); return buf; }
        std::snprintf(buf, sizeof buf, "%.1f", n * 10.0f);
        return buf;
    }

    // Renders `n` frames, consuming sorted events.
    void render(const std::vector<MidiEvent>& events, float* outL, float* outR, uint32_t n) {
        cache();
        std::size_t ei = 0;
        for (uint32_t i = 0; i < n; ++i) {
            while (ei < events.size() && events[ei].offset <= i) handle(events[ei++]);
            float l = 0.0f, r = 0.0f;
            lfo_ph_ += c_.lfo_inc;
            if (lfo_ph_ >= 1.0f) lfo_ph_ -= 1.0f;
            const float lfo = std::sin(6.2831853f * lfo_ph_);
            follow_env_ += (follow_level_ - follow_env_) * 0.002f;
            for (auto& v : voices_) {
                if (!v.active) continue;
                const float s = v.tick(c_, lfo, bend_, follow_env_, sr_, rng_);
                l += s * v.pan_l;
                r += s * v.pan_r;
            }
            fx(l, r);
            outL[i] = l * c_.level;
            outR[i] = r * c_.level;
        }
        while (ei < events.size()) handle(events[ei++]);
    }

    void allNotesOff() {
        for (auto& v : voices_) v.release();
    }

private:
    static float cutoffHz(float n) { return 30.0f * std::pow(600.0f, n); }
    static float envTime(float n) { return 0.001f + 6.0f * n * n * n; }

    struct Cached {
        int w1 = 0, w2 = 0, unison = 1;
        float l1 = 0.8f, l2 = 0.5f, sub = 0.2f, noise = 0.0f, semi2 = 0.0f, det2 = 0.0f, spread = 0.0f;
        float cutoff = 0.5f, res = 0.2f, env_amt = 0.5f, keytrack = 0.5f, drive = 0.2f;
        float fa, fd, fs, fr, aa, ad, as, ar;
        float lfo_inc = 0.0f, lfo_pitch = 0.0f, lfo_cut = 0.0f, glide = 0.0f, vel = 0.5f, follow = 0.0f;
        float chorus = 0.0f, delay = 0.0f, delay_time = 0.4f, reverb = 0.0f, level = 0.7f;
    };

    void cache() {
        auto P = [&](const char* id) { return param(id); };
        c_.w1 = std::clamp(static_cast<int>(std::lround(P("osc1_wave") * 3.0f)), 0, 3);
        c_.w2 = std::clamp(static_cast<int>(std::lround(P("osc2_wave") * 3.0f)), 0, 3);
        c_.unison = 1 + std::clamp(static_cast<int>(std::lround(P("unison") * 2.0f)), 0, 2);
        c_.l1 = P("osc1_level");
        c_.l2 = P("osc2_level");
        c_.sub = P("sub_level");
        c_.noise = P("noise_level");
        c_.semi2 = std::round((P("osc2_semi") - 0.5f) * 48.0f);
        c_.det2 = (P("osc2_detune") - 0.5f) * 1.0f;
        c_.spread = P("spread") * 0.35f;
        c_.cutoff = P("cutoff");
        c_.res = P("resonance");
        c_.env_amt = P("env_amt");
        c_.keytrack = P("keytrack");
        c_.drive = P("drive");
        auto coef = [&](float t) { return 1.0f - std::exp(-1.0f / (std::max(0.0005f, t) * static_cast<float>(sr_))); };
        c_.fa = coef(envTime(P("fenv_a")));
        c_.fd = coef(envTime(P("fenv_d")) * 0.4f);
        c_.fs = P("fenv_s");
        c_.fr = coef(envTime(P("fenv_r")) * 0.4f);
        c_.aa = coef(envTime(P("amp_a")));
        c_.ad = coef(envTime(P("amp_d")) * 0.4f);
        c_.as = P("amp_s");
        c_.ar = coef(envTime(P("amp_r")) * 0.4f);
        c_.lfo_inc = 0.1f * std::pow(150.0f, P("lfo_rate")) / static_cast<float>(sr_);
        c_.lfo_pitch = P("lfo_pitch") * 0.6f;
        c_.lfo_cut = P("lfo_cutoff") * 3.0f;
        c_.glide = P("glide") > 0.001f ? coef(0.005f + 0.6f * P("glide") * P("glide")) : 1.0f;
        c_.vel = P("vel_sens");
        c_.follow = P("follow");
        c_.chorus = P("fx_chorus");
        c_.delay = P("fx_delay");
        c_.delay_time = 0.06f + 0.9f * P("fx_delay_time");
        c_.reverb = P("fx_reverb");
        c_.level = 1.4f * P("level") * P("level");
    }

    struct Voice {
        bool active = false, gate = false;
        int note = 60;
        float vel = 0.8f, freq = 261.6f, target_freq = 261.6f;
        std::array<float, 3> ph1{}, ph2{};
        float phs = 0.0f;
        float aenv = 0.0f, fenv = 0.0f;
        bool a_attack = false, f_attack = false;
        std::array<float, 4> z{};
        float pan_l = 0.707f, pan_r = 0.707f;
        uint32_t age = 0;

        void start(int n, float v, bool legato, float glide_coef) {
            note = n;
            vel = v;
            target_freq = 440.0f * std::pow(2.0f, (n - 69) / 12.0f);
            if (!active || glide_coef >= 1.0f) freq = target_freq;
            if (!legato || !active) {
                a_attack = true;
                f_attack = true;
            }
            active = true;
            gate = true;
            age = 0;
        }
        void release() { gate = false; }

        static float polyblep(float t, float dt) {
            if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
            if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
            return 0.0f;
        }
        static float osc(int w, float ph, float dt) {
            switch (w) {
                case 0: return 2.0f * ph - 1.0f - polyblep(ph, dt);
                case 1: {
                    float s = ph < 0.5f ? 1.0f : -1.0f;
                    s += polyblep(ph, dt);
                    float p2 = ph + 0.5f;
                    if (p2 >= 1.0f) p2 -= 1.0f;
                    s -= polyblep(p2, dt);
                    return s * 0.8f;
                }
                case 2: return (ph < 0.5f ? 4.0f * ph - 1.0f : 3.0f - 4.0f * ph) * 0.9f;
                default: return std::sin(6.2831853f * ph);
            }
        }

        float tick(const Cached& c, float lfo, float bend, float follow_env, double sr, std::minstd_rand& rng) {
            ++age;
            freq += (target_freq - freq) * c.glide;
            const float pitch_mul = std::pow(2.0f, (bend + lfo * c.lfo_pitch) / 12.0f);
            const float f = freq * pitch_mul;
            const float fsr = static_cast<float>(sr);
            // Oscillators
            float s1 = 0.0f;
            static constexpr float kUni[3][3] = {{0, 0, 0}, {-0.5f, 0.5f, 0}, {-1, 0, 1}};
            for (int u = 0; u < c.unison; ++u) {
                const float df = f * std::pow(2.0f, kUni[c.unison - 1][u] * c.spread / 12.0f);
                const float dt = df / fsr;
                ph1[static_cast<std::size_t>(u)] += dt;
                if (ph1[static_cast<std::size_t>(u)] >= 1.0f) ph1[static_cast<std::size_t>(u)] -= 1.0f;
                s1 += osc(c.w1, ph1[static_cast<std::size_t>(u)], dt);
            }
            s1 /= std::sqrt(static_cast<float>(c.unison));
            const float f2 = f * std::pow(2.0f, (c.semi2 + c.det2) / 12.0f);
            const float dt2 = f2 / fsr;
            ph2[0] += dt2;
            if (ph2[0] >= 1.0f) ph2[0] -= 1.0f;
            const float s2 = osc(c.w2, ph2[0], dt2);
            phs += 0.5f * f / fsr;
            if (phs >= 1.0f) phs -= 1.0f;
            const float sub = phs < 0.5f ? 0.7f : -0.7f;
            const float nz = (static_cast<float>(rng() % 20000) / 10000.0f - 1.0f);
            float x = c.l1 * s1 + c.l2 * s2 + c.sub * sub + c.noise * nz;
            // Envelopes (exponential ADSR)
            if (gate) {
                if (a_attack) { aenv += (1.15f - aenv) * c.aa; if (aenv >= 1.0f) { aenv = 1.0f; a_attack = false; } }
                else aenv += (c.as - aenv) * c.ad;
                if (f_attack) { fenv += (1.15f - fenv) * c.fa; if (fenv >= 1.0f) { fenv = 1.0f; f_attack = false; } }
                else fenv += (c.fs - fenv) * c.fd;
            } else {
                aenv += (0.0f - aenv) * c.ar;
                fenv += (0.0f - fenv) * c.fr;
                if (aenv < 1e-4f) { active = false; return 0.0f; }
            }
            // ZDF ladder filter (4 x TPT one-pole, global feedback solved implicitly)
            const float velm = 1.0f - c.vel + c.vel * vel;
            float cut = 30.0f * std::pow(600.0f, c.cutoff) * std::pow(2.0f, (c.env_amt * 7.0f * fenv * velm) + c.keytrack * (note - 60) / 12.0f + lfo * c.lfo_cut);
            cut = std::clamp(cut, 20.0f, fsr * 0.45f);
            const float g = std::tan(3.14159265f * cut / fsr);
            const float G = g / (1.0f + g);
            const float k = 4.0f * c.res * 0.98f;
            x *= 1.0f + 3.0f * c.drive;
            // Zero-delay feedback: solve the loop exactly from the stage states (Zavalishin ladder).
            float sigma = G * G * G * (z[0] / (1.0f + g)) + G * G * (z[1] / (1.0f + g)) + G * (z[2] / (1.0f + g)) + z[3] / (1.0f + g);
            const float G4 = G * G * G * G;
            float u = (x - k * sigma) / (1.0f + k * G4);
            u = std::tanh(u);
            for (int st = 0; st < 4; ++st) {
                const float v = (u - z[static_cast<std::size_t>(st)]) * G;
                const float y = v + z[static_cast<std::size_t>(st)];
                z[static_cast<std::size_t>(st)] = y + v;
                u = y;
            }
            const float out = u * (1.0f + 0.5f * k * 0.25f);
            // Level: envelope × velocity × optional guitar-follow.
            const float fol = 1.0f - c.follow + c.follow * std::clamp(follow_env * kFollowGain, 0.0f, 1.2f);
            return out * aenv * velm * fol * 0.35f;
        }
    };

    void handle(const MidiEvent& e) {
        if (e.type == MidiEvent::PitchBend) {
            bend_ = e.bend;
            return;
        }
        if (e.type == MidiEvent::NoteOff) {
            for (auto& v : voices_)
                if (v.active && v.gate && v.note == e.note) v.release();
            return;
        }
        // Note on: reuse same-note voice, else free, else oldest released, else oldest.
        Voice* pick = nullptr;
        for (auto& v : voices_) if (v.active && v.note == e.note) { pick = &v; break; }
        if (!pick) for (auto& v : voices_) if (!v.active) { pick = &v; break; }
        if (!pick) {
            uint32_t best = 0;
            for (auto& v : voices_) if (!v.gate && v.age >= best) { best = v.age; pick = &v; }
        }
        if (!pick) {
            uint32_t best = 0;
            for (auto& v : voices_) if (v.age >= best) { best = v.age; pick = &v; }
        }
        // Legato (glide in, no envelope retrigger) only when this voice is still held on another note: that is the only
        // case where it glides. A fresh voice starts at its pitch, so skipping its attack just lost the pick (and the
        // filter envelope) on every note that overlapped another one: 73-89 % of a poly-tracked guitar's notes.
        const bool legato = c_.glide < 1.0f && pick->active && pick->gate && pick->note != e.note;
        const int idx = static_cast<int>(pick - voices_.data());
        const float pan = 0.5f + 0.35f * ((idx % 2) ? 1.0f : -1.0f) * (0.3f + 0.1f * static_cast<float>(idx / 2)) * (c_.spread * 2.0f + 0.3f);
        pick->pan_l = std::cos(pan * 1.5707963f) * 1.4142f;
        pick->pan_r = std::sin(pan * 1.5707963f) * 1.4142f;
        pick->start(e.note, e.velocity, legato, c_.glide);
    }

    void fx(float& l, float& r) {
        // Stereo chorus (two modulated taps)
        if (c_.chorus > 0.001f) {
            ch_[static_cast<std::size_t>(cpos_)] = 0.5f * (l + r);
            cph_ += 0.6f / static_cast<float>(sr_);
            if (cph_ >= 1.0f) cph_ -= 1.0f;
            auto tap = [&](float ms) {
                float rp = static_cast<float>(cpos_) - ms * 0.001f * static_cast<float>(sr_);
                while (rp < 0) rp += 4096.0f;
                const int i0 = static_cast<int>(rp) & 4095;
                const float fr = rp - std::floor(rp);
                return ch_[static_cast<std::size_t>(i0)] + fr * (ch_[static_cast<std::size_t>((i0 + 1) & 4095)] - ch_[static_cast<std::size_t>(i0)]);
            };
            const float m1 = tap(9.0f + 3.0f * std::sin(6.2831853f * cph_));
            const float m2 = tap(11.0f + 3.0f * std::cos(6.2831853f * cph_));
            cpos_ = (cpos_ + 1) & 4095;
            l += c_.chorus * 0.8f * m1;
            r += c_.chorus * 0.8f * m2;
        }
        // Ping-pong delay
        if (c_.delay > 0.001f) {
            const int D = static_cast<int>(c_.delay_time * static_cast<float>(sr_));
            const int N = static_cast<int>(dl_.size());
            const int rp = (dpos_ - std::min(D, N - 1) + N) % N;
            const float yl = dl_[static_cast<std::size_t>(rp)], yr = dr_[static_cast<std::size_t>(rp)];
            dl_[static_cast<std::size_t>(dpos_)] = 0.5f * (l + r) + yr * 0.45f;
            dr_[static_cast<std::size_t>(dpos_)] = yl * 0.45f;
            dpos_ = (dpos_ + 1) % N;
            l += c_.delay * yl;
            r += c_.delay * yr;
        }
        // Small FDN plate
        if (c_.reverb > 0.001f) {
            std::array<float, 4> o{};
            for (int i = 0; i < 4; ++i) o[static_cast<std::size_t>(i)] = rv_[static_cast<std::size_t>(i)][static_cast<std::size_t>(rvp_[static_cast<std::size_t>(i)])];
            const float in = 0.3f * (l + r);
            const float h = 0.5f;
            const std::array<float, 4> m = {h * (o[0] + o[1] + o[2] + o[3]), h * (o[0] - o[1] + o[2] - o[3]), h * (o[0] + o[1] - o[2] - o[3]), h * (o[0] - o[1] - o[2] + o[3])};
            for (int i = 0; i < 4; ++i) {
                auto& lp = rlp_[static_cast<std::size_t>(i)];
                lp += 0.45f * (m[static_cast<std::size_t>(i)] - lp);
                auto& buf = rv_[static_cast<std::size_t>(i)];
                buf[static_cast<std::size_t>(rvp_[static_cast<std::size_t>(i)])] = in + 0.82f * lp;
                rvp_[static_cast<std::size_t>(i)] = (rvp_[static_cast<std::size_t>(i)] + 1) % static_cast<int>(buf.size());
            }
            l += c_.reverb * 0.6f * (o[0] + o[2]);
            r += c_.reverb * 0.6f * (o[1] + o[3]);
        }
    }

    static constexpr float kRvTimes[4] = {0.0297f, 0.0371f, 0.0411f, 0.0437f};
    double sr_ = 48000.0;
    std::vector<float> params_;
    std::string preset_ = "Analog Pad";
    Cached c_{};
    std::array<Voice, kVoices> voices_{};
    float bend_ = 0.0f, lfo_ph_ = 0.0f, follow_level_ = 0.0f, follow_env_ = 0.0f;
    std::minstd_rand rng_{99};
    std::vector<float> dl_, dr_, ch_;
    int dpos_ = 0, cpos_ = 0;
    float cph_ = 0.0f;
    std::array<std::vector<float>, 4> rv_{};
    std::array<int, 4> rvp_{};
    std::array<float, 4> rlp_{};
};

} // namespace rig::midi
