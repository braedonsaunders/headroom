// Headroom-native rack components for the guitar → MIDI → instrument path:
//   900001 Pitch → MIDI  (polyphonic / mono-with-bends detector; writes the lane's MIDI bus)
//   900002 MIDI FX       (octave, transpose, harmony, scale, arpeggiator, hold)
//   900003 Instrument    (sound library: VA synth, FM keys & bells, drawbar organ, 128 macOS GM
//                         instruments via DLSMusicDevice, and any installed AU instrument)
//   900004 Poly Synth    (the full virtual-analog synth with every parameter exposed)
//   900006 Bloom         (dynamic arpeggiator played by the guitar: see Bloom.hpp)
// They live inside normal rack lanes (typically lane B of a Split), so instruments can be followed by
// any audio unit — reverbs, delays, even an amp.
#include "../core/IComponent.hpp"
#include "MidiComponents.hpp"
#include "PitchToMidi.hpp"
#include "NeuralGuitarMidi.hpp"
#include "Synth.hpp"
#include "Bloom.hpp"
#include "../plugins/PluginHost.hpp"
#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>
#endif
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace rig {

using catalog::ComponentSchema;
using catalog::EngineFamily;
using catalog::ParamKind;
using catalog::ParamSchema;

// ───────────────────────────── schemas ─────────────────────────────
namespace {

constexpr ParamSchema kP2M[] = {
    {"mode", "Mode", ParamKind::Float, 0.0f, 0.0f, 1.0f, "", ""},
    {"sens", "Sensitivity", ParamKind::Float, 0.5f, 0.0f, 1.0f, "", ""},
    {"bend", "Bend Range", ParamKind::Float, 2.0f, 0.0f, 24.0f, "", "st"},
    {"low", "Lowest Note", ParamKind::Float, 40.0f, 0.0f, 108.0f, "", ""},
    {"high", "Highest Note", ParamKind::Float, 88.0f, 0.0f, 108.0f, "", ""},
    {"vel", "Velocity Curve", ParamKind::Float, 0.5f, 0.0f, 1.0f, "", ""},
    {"thru", "Guitar Thru", ParamKind::Float, 1.0f, 0.0f, 1.0f, "", ""},
    {"engine", "Engine", ParamKind::Float, 0.0f, 0.0f, 1.0f, "", ""},
    {"Pwr", "On/Off", ParamKind::Float, 1.0f, 0.0f, 1.0f, "", ""},
};
constexpr ParamSchema kMFX[] = {
    {"octave", "Octave", ParamKind::Float, 0.0f, -3.0f, 3.0f, "", ""},
    {"transpose", "Transpose", ParamKind::Float, 0.0f, -12.0f, 12.0f, "", "st"},
    {"harmony", "Harmony", ParamKind::Float, 0.0f, 0.0f, 7.0f, "", ""},
    {"scale", "Scale", ParamKind::Float, 0.0f, 0.0f, 7.0f, "", ""},
    {"root", "Key", ParamKind::Float, 4.0f, 0.0f, 11.0f, "", ""},
    {"arp", "Arpeggiator", ParamKind::Float, 0.0f, 0.0f, 4.0f, "", ""},
    {"rate", "Arp Rate", ParamKind::Float, 2.0f, 0.0f, 5.0f, "", ""},
    {"hold", "Hold", ParamKind::Float, 0.0f, 0.0f, 1.0f, "", ""},
    {"bpm", "Tempo", ParamKind::Float, 120.0f, 0.0f, 300.0f, "", "bpm"},
    {"sync", "Sync to Rig Tempo", ParamKind::Float, 1.0f, 0.0f, 1.0f, "", ""},
    {"Pwr", "On/Off", ParamKind::Float, 1.0f, 0.0f, 1.0f, "", ""},
};
constexpr ParamSchema kBLM[] = {
    {"trigger", "Trigger", ParamKind::Float, 0.0f, 0.0f, 3.0f, "", ""},
    {"engage", "Engage", ParamKind::Float, 0.0f, 0.0f, 1.0f, "", ""},
    {"pattern", "Pattern", ParamKind::Float, 4.0f, 0.0f, 7.0f, "", ""},
    {"rate", "Rate", ParamKind::Float, 3.0f, 0.0f, 5.0f, "", ""},
    {"motion", "Motion", ParamKind::Float, 0.5f, 0.0f, 1.0f, "", ""},
    {"touch", "Touch", ParamKind::Float, 0.6f, 0.0f, 1.0f, "", ""},
    {"range", "Range", ParamKind::Float, 1.0f, 0.0f, 3.0f, "", ""},          // option index: 1..4 octaves
    {"extend", "Extend", ParamKind::Float, 1.0f, 0.0f, 4.0f, "", ""},
    {"key", "Key", ParamKind::Float, 0.0f, 0.0f, 12.0f, "", ""},
    {"scale", "Scale", ParamKind::Float, 0.0f, 0.0f, 6.0f, "", ""},
    {"gate", "Gate", ParamKind::Float, 0.6f, 0.05f, 1.0f, "", ""},
    {"swing", "Swing", ParamKind::Float, 0.0f, 0.0f, 1.0f, "", ""},
    {"clock", "Clock", ParamKind::Float, 1.0f, 0.0f, 1.0f, "", ""},
    {"bend", "Bend Follow", ParamKind::Float, 1.0f, 0.0f, 1.0f, "", ""},
    {"choke", "Mute Choke", ParamKind::Float, 0.7f, 0.0f, 1.0f, "", ""},
    {"bloom", "Bloom Time", ParamKind::Float, 0.6f, 0.0f, 2.5f, "", "s"},   // continuous seconds (0.1 s floor in the engine)
    {"voice", "Voice", ParamKind::Float, 2.0f, 0.0f, 2.0f, "", ""},
    {"tone", "String Tone", ParamKind::Float, 0.5f, 0.0f, 1.0f, "", ""},
    {"dry", "Dry", ParamKind::Float, 0.5f, 0.0f, 1.0f, "", ""},
    {"level", "String Level", ParamKind::Float, 0.5f, 0.0f, 1.0f, "", ""},
    {"retrig", "Retrigger", ParamKind::Float, 0.0f, 0.0f, 1.0f, "", ""},
    {"Pwr", "On/Off", ParamKind::Float, 1.0f, 0.0f, 1.0f, "", ""},
};
constexpr ParamSchema kINST[] = {
    {"tone", "Tone", ParamKind::Float, 0.5f, 0.0f, 1.0f, "", ""},
    {"attack", "Attack", ParamKind::Float, 0.5f, 0.0f, 1.0f, "", ""},
    {"release", "Release", ParamKind::Float, 0.5f, 0.0f, 1.0f, "", ""},
    {"vibrato", "Vibrato", ParamKind::Float, 0.0f, 0.0f, 1.0f, "", ""},
    {"follow", "Guitar Follow", ParamKind::Float, 0.3f, 0.0f, 1.0f, "", ""},
    {"level", "Level", ParamKind::Float, 0.7f, 0.0f, 1.0f, "", ""},
    {"mix", "Mix", ParamKind::Float, 1.0f, 0.0f, 1.0f, "", ""},
    {"Pwr", "On/Off", ParamKind::Float, 1.0f, 0.0f, 1.0f, "", ""},
};

constexpr ParamSchema kNAM[] = {
    {"input", "Input", ParamKind::Float, 0.0f, -24.0f, 24.0f, "", "dB"},
    {"output", "Output", ParamKind::Float, 0.0f, -24.0f, 24.0f, "", "dB"},
    {"gate", "Noise Gate", ParamKind::Float, 0.0f, 0.0f, 1.0f, "", ""},
    {"bass", "Bass", ParamKind::Float, 0.5f, 0.0f, 1.0f, "", ""},
    {"mid", "Mid", ParamKind::Float, 0.5f, 0.0f, 1.0f, "", ""},
    {"treble", "Treble", ParamKind::Float, 0.5f, 0.0f, 1.0f, "", ""},
    {"stack_react", "Interactive Stack", ParamKind::Float, 0.35f, 0.0f, 1.0f, "", ""},
    {"sag", "Power Sag", ParamKind::Float, 0.20f, 0.0f, 1.0f, "", ""},
    {"normalize", "Normalize", ParamKind::Float, 1.0f, 0.0f, 1.0f, "", ""},
    {"Pwr", "On/Off", ParamKind::Float, 1.0f, 0.0f, 1.0f, "", ""},
};

std::vector<ParamSchema> makeSynthSchema() {
    std::vector<ParamSchema> v;
    for (const auto& d : midi::synthParamDefs()) {
        const float mx = d.steps ? static_cast<float>(d.steps - 1) : 1.0f;
        v.push_back({d.id, d.name, ParamKind::Float, d.steps ? std::round(d.def * mx) : d.def, 0.0f, mx, "", ""});
    }
    v.push_back({"Pwr", "On/Off", ParamKind::Float, 1.0f, 0.0f, 1.0f, "", ""});
    return v;
}
const std::vector<ParamSchema>& synthSchemaParams() {
    static const std::vector<ParamSchema> v = makeSynthSchema();
    return v;
}

const std::vector<ComponentSchema>& extras() {
    static const std::vector<ComponentSchema> v = {
        {900001, "Pitch to MIDI", "Polyphonic Pitch → MIDI", "HeadroomPitchToMidi", EngineFamily::ModernDSP, -1, false, std::span<const ParamSchema>(kP2M)},
        {900002, "MIDI FX", "MIDI Harmony & Arpeggiator", "HeadroomMidiFx", EngineFamily::ModernDSP, -1, false, std::span<const ParamSchema>(kMFX)},
        {900003, "Instrument", "Instrument Sound Library", "HeadroomInstrument", EngineFamily::ModernDSP, -1, false, std::span<const ParamSchema>(kINST)},
        {900004, "Poly Synth", "Analog Poly Synth", "HeadroomPolySynth", EngineFamily::ModernDSP, -1, false,
         std::span<const ParamSchema>(synthSchemaParams().data(), synthSchemaParams().size())},
        {900005, "NAM Capture", "Neural Amp Modeler Capture", "HeadroomNAM", EngineFamily::ModernDSP, -1, false, std::span<const ParamSchema>(kNAM)},
        {900006, "Bloom", "Bloom Dynamic Arpeggiator", "HeadroomBloom", EngineFamily::ModernDSP, -1, false, std::span<const ParamSchema>(kBLM)},
    };
    return v;
}

std::atomic<MidiSinkFn> g_sink{nullptr};

// Live view of every Pitch → MIDI unit (for UI telemetry), keyed by rack slot.
struct LiveSnap {
    std::mutex m;
    std::vector<std::pair<int, float>> notes;
    float pitch = 0.0f, conf = 0.0f, level = 0.0f;
} g_live;
struct UnitSnap {
    std::vector<std::pair<int, float>> notes;
    float pitch = 0.0f, conf = 0.0f, level = 0.0f;
    int slot = -1;
};
std::mutex g_units_m;
std::map<const void*, UnitSnap> g_units;
// Live view of every Bloom unit (chord, key, ladder, clock), keyed by unit; slot set by the rack.
struct BloomSnap { midi::BloomLive live; int slot = -1; };
std::mutex g_bloom_m;
std::map<const void*, BloomSnap> g_bloom;
std::atomic<void*> g_sink_ctx{nullptr};

const char* const kGM[128] = {
    "Acoustic Grand Piano", "Bright Acoustic Piano", "Electric Grand Piano", "Honky-tonk Piano", "Electric Piano 1", "Electric Piano 2", "Harpsichord", "Clavinet",
    "Celesta", "Glockenspiel", "Music Box", "Vibraphone", "Marimba", "Xylophone", "Tubular Bells", "Dulcimer",
    "Drawbar Organ", "Percussive Organ", "Rock Organ", "Church Organ", "Reed Organ", "Accordion", "Harmonica", "Tango Accordion",
    "Nylon Guitar", "Steel Guitar", "Jazz Guitar", "Clean Electric Guitar", "Muted Guitar", "Overdriven Guitar", "Distortion Guitar", "Guitar Harmonics",
    "Acoustic Bass", "Fingered Bass", "Picked Bass", "Fretless Bass", "Slap Bass 1", "Slap Bass 2", "Synth Bass 1", "Synth Bass 2",
    "Violin", "Viola", "Cello", "Contrabass", "Tremolo Strings", "Pizzicato Strings", "Orchestral Harp", "Timpani",
    "String Ensemble 1", "String Ensemble 2", "Synth Strings 1", "Synth Strings 2", "Choir Aahs", "Voice Oohs", "Synth Voice", "Orchestra Hit",
    "Trumpet", "Trombone", "Tuba", "Muted Trumpet", "French Horn", "Brass Section", "Synth Brass 1", "Synth Brass 2",
    "Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax", "Oboe", "English Horn", "Bassoon", "Clarinet",
    "Piccolo", "Flute", "Recorder", "Pan Flute", "Blown Bottle", "Shakuhachi", "Whistle", "Ocarina",
    "Square Lead", "Sawtooth Lead", "Calliope Lead", "Chiff Lead", "Charang Lead", "Voice Lead", "Fifths Lead", "Bass + Lead",
    "New Age Pad", "Warm Pad", "Polysynth Pad", "Choir Pad", "Bowed Pad", "Metallic Pad", "Halo Pad", "Sweep Pad",
    "Rain FX", "Soundtrack FX", "Crystal FX", "Atmosphere FX", "Brightness FX", "Goblins FX", "Echoes FX", "Sci-fi FX",
    "Sitar", "Banjo", "Shamisen", "Koto", "Kalimba", "Bagpipe", "Fiddle", "Shanai",
    "Tinkle Bell", "Agogo", "Steel Drums", "Woodblock", "Taiko Drum", "Melodic Tom", "Synth Drum", "Reverse Cymbal",
    "Guitar Fret Noise", "Breath Noise", "Seashore", "Bird Tweet", "Telephone Ring", "Helicopter", "Applause", "Gunshot"};

} // namespace

namespace catalog {
const ComponentSchema* findExtraComponent(int32_t table_id) noexcept {
    if (plugins::isPluginTable(table_id)) return plugins::findSchema(table_id); // hosted AU plugins
    for (const auto& c : extras())
        if (c.table_id == table_id) return &c;
    return nullptr;
}
std::span<const ComponentSchema> extraComponents() noexcept { return {extras().data(), extras().size()}; }
} // namespace catalog

void setGlobalMidiSink(MidiSinkFn fn, void* ctx) {
    g_sink_ctx.store(ctx);
    g_sink.store(fn);
}

bool isMidiComponentTable(int32_t t) noexcept { return (t >= 900001 && t <= 900004) || t == 900006; }

std::string liveMidiJson() {
    std::string units = ",\"units\":{";
    {
        std::lock_guard<std::mutex> lk(g_units_m);
        bool first = true;
        for (const auto& kv : g_units) {
            const auto& u = kv.second;
            if (u.slot < 0) continue;
            units += (first ? "" : ",");
            first = false;
            units += "\"" + std::to_string(u.slot) + "\":{\"notes\":[";
            for (std::size_t i = 0; i < u.notes.size(); ++i)
                units += (i ? "," : "") + std::string("[") + std::to_string(u.notes[i].first) + "," + std::to_string(static_cast<int>(u.notes[i].second * 127.0f)) + "]";
            char b2[80];
            std::snprintf(b2, sizeof b2, "],\"pitch\":%.2f,\"confidence\":%.3f}", u.pitch, u.conf);
            units += b2;
        }
    }
    units += "}";
    units += ",\"bloom\":{";
    {
        std::lock_guard<std::mutex> lk(g_bloom_m);
        bool first = true;
        for (const auto& kv : g_bloom) {
            const auto& b = kv.second.live;
            if (kv.second.slot < 0) continue;
            char buf[512];
            std::snprintf(buf, sizeof buf,
                          "%s\"%d\":{\"active\":%s,\"engaged\":%s,\"idx\":%d,\"chord\":\"%s\",\"root\":%d,\"key\":%d,\"key_conf\":%.2f,\"note\":%d,\"step\":%d,"
                          "\"bpm\":%.1f,\"feel\":%s,\"swing\":%.3f,\"strength\":%.2f,\"muted\":%s,\"bend\":%.1f,\"tune\":%.1f,"
                          "\"ring_db\":%.1f,\"beat\":%.3f,\"div\":%d,\"bloom\":%.2f,\"ladder\":[",
                          first ? "" : ",", kv.second.slot, b.active ? "true" : "false", b.engaged ? "true" : "false", b.idx, b.chord, b.root, b.key, b.key_conf, b.note, b.step,
                          b.bpm, b.feel_locked ? "true" : "false", b.swing, b.strength, b.muted ? "true" : "false", b.bend_cents, b.tune_cents,
                          std::max(-120.0f, b.ring_db), b.beat, b.div, b.bloom_progress);
            units += buf;
            for (int i = 0; i < b.ladder_n; ++i) units += (i ? "," : "") + std::to_string(b.ladder[i]);
            units += "]}";
            first = false;
        }
    }
    units += "}";
    std::lock_guard<std::mutex> lk(g_live.m);
    std::string o = "{\"notes\":[";
    for (std::size_t i = 0; i < g_live.notes.size(); ++i) {
        o += (i ? "," : "");
        o += "[" + std::to_string(g_live.notes[i].first) + "," + std::to_string(static_cast<int>(g_live.notes[i].second * 127.0f)) + "]";
    }
    char buf[96];
    std::snprintf(buf, sizeof buf, "],\"pitch\":%.2f,\"confidence\":%.3f,\"level\":%.4f", g_live.pitch, g_live.conf, g_live.level);
    return o + buf + units + "}";
}

// ───────────────────────────── shared instrument engines ─────────────────────────────
namespace {

// Two-operator FM voice bank (electric pianos, bells, mallets, FM bass).
class FMEngine {
public:
    struct Patch {
        float ratio = 1.0f, index = 2.0f, index_decay = 1.5f, carrier_decay = 1.2f, sustain = 0.0f, release = 0.4f;
        float ratio2 = 14.0f, tine = 0.25f; // second (tine) modulator for EP bark
        float detune = 0.0f;
    };
    void prepare(double sr) {
        sr_ = sr;
        for (auto& v : v_) v = Voice{};
    }
    void setPatch(const Patch& p) { p_ = p; }
    void noteOn(int note, float vel) {
        Voice* pick = &v_[0];
        for (auto& v : v_) if (!v.active) { pick = &v; break; }
        for (auto& v : v_) if (v.active && v.note == note) pick = &v;
        if (pick->active && pick->note != note) {
            uint32_t oldest = 0;
            for (auto& v : v_) if (v.age >= oldest) { oldest = v.age; pick = &v; }
        }
        *pick = Voice{};
        pick->active = true;
        pick->gate = true;
        pick->note = note;
        pick->vel = vel;
        pick->f = 440.0f * std::pow(2.0f, (note - 69) / 12.0f);
        pick->env = 1.0f;
        pick->menv = 1.0f;
    }
    void noteOff(int note) {
        for (auto& v : v_) if (v.active && v.note == note) v.gate = false;
    }
    void render(float* L, float* R, uint32_t n, float bend, float vib_depth, float tone, float rel_scale) {
        const float fs = static_cast<float>(sr_);
        const float cdec = std::exp(-1.0f / (p_.carrier_decay * fs));
        const float mdec = std::exp(-1.0f / (p_.index_decay * fs));
        const float rel = std::exp(-1.0f / (std::max(0.01f, p_.release * rel_scale) * fs));
        for (uint32_t i = 0; i < n; ++i) {
            vib_ += 5.5f / fs;
            if (vib_ >= 1.0f) vib_ -= 1.0f;
            const float pm = std::pow(2.0f, (bend + vib_depth * 0.3f * std::sin(6.2831853f * vib_)) / 12.0f);
            float l = 0.0f, r = 0.0f;
            for (auto& v : v_) {
                if (!v.active) continue;
                ++v.age;
                const float f = v.f * pm;
                v.pm += f * p_.ratio / fs;
                v.pm2 += f * p_.ratio2 / fs;
                v.pc += f * (1.0f + p_.detune) / fs;
                v.pm -= std::floor(v.pm);
                v.pm2 -= std::floor(v.pm2);
                v.pc -= std::floor(v.pc);
                const float idx = p_.index * (0.4f + 0.9f * v.vel) * (0.5f + tone) * v.menv;
                const float mod = idx * std::sin(6.2831853f * v.pm) + p_.tine * v.vel * v.menv * v.menv * std::sin(6.2831853f * v.pm2);
                const float s = std::sin(6.2831853f * v.pc + mod);
                if (v.gate) {
                    v.env = p_.sustain + (v.env - p_.sustain) * cdec;
                    v.menv *= mdec;
                } else {
                    v.env *= rel;
                    v.menv *= rel;
                    if (v.env < 1e-4f) v.active = false;
                }
                const float out = s * v.env * (0.25f + 0.75f * v.vel) * 0.28f;
                const float pan = 0.5f + 0.25f * std::clamp((static_cast<float>(v.note) - 60.0f) / 24.0f, -1.0f, 1.0f);
                l += out * (1.0f - pan) * 1.41f;
                r += out * pan * 1.41f;
            }
            L[i] = l;
            R[i] = r;
        }
    }

private:
    struct Voice {
        bool active = false, gate = false;
        int note = 60;
        float vel = 0.8f, f = 261.6f, pc = 0, pm = 0, pm2 = 0, env = 0, menv = 0;
        uint32_t age = 0;
    };
    double sr_ = 48000.0;
    Patch p_{};
    std::array<Voice, 10> v_{};
    float vib_ = 0.0f;
};

// Tonewheel-style additive organ with key click and rotary-ish chorus.
class OrganEngine {
public:
    void prepare(double sr) {
        sr_ = sr;
        for (auto& v : v_) v = Voice{};
        dl_.assign(2048, 0.0f);
    }
    void setDrawbars(const std::array<float, 9>& d) { bars_ = d; }
    void noteOn(int note, float vel) {
        for (auto& v : v_) if (v.active && v.note == note) { v.gate = true; return; }
        for (auto& v : v_) if (!v.active) { v = Voice{}; v.active = true; v.gate = true; v.note = note; v.vel = vel; v.click = 1.0f; return; }
    }
    void noteOff(int note) {
        for (auto& v : v_) if (v.active && v.note == note) v.gate = false;
    }
    void render(float* L, float* R, uint32_t n, float bend, float vib, float tone) {
        static constexpr float kRatios[9] = {0.5f, 1.5f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 8.0f};
        const float fs = static_cast<float>(sr_);
        const float pm = std::pow(2.0f, bend / 12.0f);
        for (uint32_t i = 0; i < n; ++i) {
            float s = 0.0f;
            for (auto& v : v_) {
                if (!v.active) continue;
                const float f0 = 440.0f * std::pow(2.0f, (v.note - 69) / 12.0f) * pm;
                float acc = 0.0f;
                for (int b = 0; b < 9; ++b) {
                    v.ph[static_cast<std::size_t>(b)] += f0 * kRatios[b] / fs;
                    v.ph[static_cast<std::size_t>(b)] -= std::floor(v.ph[static_cast<std::size_t>(b)]);
                    acc += bars_[static_cast<std::size_t>(b)] * std::sin(6.2831853f * v.ph[static_cast<std::size_t>(b)]);
                }
                v.amp += ((v.gate ? 1.0f : 0.0f) - v.amp) * (v.gate ? 0.01f : 0.004f);
                v.click *= 0.995f;
                if (!v.gate && v.amp < 1e-4f) v.active = false;
                s += (acc * 0.11f + v.click * 0.05f * std::sin(6.2831853f * v.ph[8] * 3.0f)) * v.amp;
            }
            // Soft tube-ish saturation, tone tilt, rotary chorus.
            s = std::tanh(s * (1.0f + 2.0f * tone)) * 0.7f;
            rot_ += (0.8f + 5.0f * vib) / fs;
            rot_ -= std::floor(rot_);
            dl_[static_cast<std::size_t>(dp_)] = s;
            auto tap = [&](float ms) {
                float rp = static_cast<float>(dp_) - ms * 0.001f * fs;
                while (rp < 0) rp += 2048.0f;
                const int i0 = static_cast<int>(rp) & 2047;
                const float fr = rp - std::floor(rp);
                return dl_[static_cast<std::size_t>(i0)] + fr * (dl_[static_cast<std::size_t>((i0 + 1) & 2047)] - dl_[static_cast<std::size_t>(i0)]);
            };
            const float a = tap(3.0f + 1.8f * std::sin(6.2831853f * rot_)), b = tap(3.0f + 1.8f * std::cos(6.2831853f * rot_));
            dp_ = (dp_ + 1) & 2047;
            L[i] = 0.6f * s + 0.5f * a;
            R[i] = 0.6f * s + 0.5f * b;
        }
    }

private:
    struct Voice {
        bool active = false, gate = false;
        int note = 60;
        float vel = 0.8f, amp = 0.0f, click = 0.0f;
        std::array<float, 9> ph{};
    };
    double sr_ = 48000.0;
    std::array<Voice, 10> v_{};
    std::array<float, 9> bars_{0.8f, 0.8f, 0.8f, 0, 0, 0, 0, 0, 0};
    std::vector<float> dl_;
    int dp_ = 0;
    float rot_ = 0.0f;
};

#ifdef __APPLE__
using InstrumentDesc = AudioComponentDescription;
// Hosts an Audio Unit instrument (Apple DLSMusicDevice for the GM bank, or any installed 'aumu').
class AUInstrument {
public:
    bool openGeneralMidi(double sr) {
        return open({kAudioUnitType_MusicDevice, kAudioUnitSubType_DLSSynth, kAudioUnitManufacturer_Apple, 0, 0}, sr);
    }
    ~AUInstrument() { close(); }
    // Exchange hosted instances (audio-thread handoff; each AudioUnit stays owned by exactly one object).
    void swapWith(AUInstrument& o) noexcept {
        std::swap(unit_, o.unit_);
        std::swap(ts_, o.ts_);
    }
    bool open(const AudioComponentDescription& d, double sr) {
        close();
        AudioComponent comp = AudioComponentFindNext(nullptr, &d);
        if (!comp || AudioComponentInstanceNew(comp, &unit_) != noErr) return false;
        AudioStreamBasicDescription fmt{};
        fmt.mSampleRate = sr;
        fmt.mFormatID = kAudioFormatLinearPCM;
        fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
        fmt.mBytesPerPacket = fmt.mBytesPerFrame = sizeof(float);
        fmt.mFramesPerPacket = 1;
        fmt.mChannelsPerFrame = 2;
        fmt.mBitsPerChannel = 32;
        AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &fmt, sizeof(fmt));
        UInt32 maxf = 4096;
        AudioUnitSetProperty(unit_, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxf, sizeof(maxf));
        if (AudioUnitInitialize(unit_) != noErr) {
            close();
            return false;
        }
        ts_ = AudioTimeStamp{};
        ts_.mFlags = kAudioTimeStampSampleTimeValid;
        return true;
    }
    void close() {
        if (unit_) {
            AudioUnitUninitialize(unit_);
            AudioComponentInstanceDispose(unit_);
            unit_ = nullptr;
        }
    }
    bool ok() const { return unit_ != nullptr; }
    void program(int p) {
        if (unit_) MusicDeviceMIDIEvent(unit_, 0xC0, static_cast<UInt32>(p & 127), 0, 0);
    }
    void bendRange(int semis) {
        if (!unit_) return;
        MusicDeviceMIDIEvent(unit_, 0xB0, 101, 0, 0);
        MusicDeviceMIDIEvent(unit_, 0xB0, 100, 0, 0);
        MusicDeviceMIDIEvent(unit_, 0xB0, 6, static_cast<UInt32>(std::clamp(semis, 1, 24)), 0);
    }
    void event(const midi::MidiEvent& e, int bend_range) {
        if (!unit_) return;
        if (e.type == midi::MidiEvent::NoteOn)
            MusicDeviceMIDIEvent(unit_, 0x90, static_cast<UInt32>(e.note & 127), static_cast<UInt32>(std::clamp(static_cast<int>(e.velocity * 127.0f), 1, 127)), e.offset);
        else if (e.type == midi::MidiEvent::NoteOff)
            MusicDeviceMIDIEvent(unit_, 0x80, static_cast<UInt32>(e.note & 127), 0, e.offset);
        else {
            const int v = std::clamp(8192 + static_cast<int>(e.bend / static_cast<float>(std::max(1, bend_range)) * 8191.0f), 0, 16383);
            MusicDeviceMIDIEvent(unit_, 0xE0, static_cast<UInt32>(v & 127), static_cast<UInt32>((v >> 7) & 127), e.offset);
        }
    }
    void render(float* L, float* R, uint32_t n) {
        if (!unit_) {
            std::memset(L, 0, n * sizeof(float));
            std::memset(R, 0, n * sizeof(float));
            return;
        }
        alignas(16) uint8_t mem[sizeof(AudioBufferList) + sizeof(AudioBuffer)];
        auto* abl = reinterpret_cast<AudioBufferList*>(mem);
        abl->mNumberBuffers = 2;
        abl->mBuffers[0] = {1, static_cast<UInt32>(n * sizeof(float)), L};
        abl->mBuffers[1] = {1, static_cast<UInt32>(n * sizeof(float)), R};
        AudioUnitRenderActionFlags flags = 0;
        if (AudioUnitRender(unit_, &flags, &ts_, 0, n, abl) != noErr) {
            std::memset(L, 0, n * sizeof(float));
            std::memset(R, 0, n * sizeof(float));
        }
        ts_.mSampleTime += n;
    }

private:
    AudioUnit unit_ = nullptr;
    AudioTimeStamp ts_{};
};
#else
// No hosted instruments off macOS yet: GM / AU sounds keep their list positions (presets stay portable) and
// render silence. docs/PORTING.md: GM via a SoundFont player, third-party instruments via VST3 hosting.
struct InstrumentDesc {};
class AUInstrument {
public:
    bool openGeneralMidi(double) { return false; }
    bool open(const InstrumentDesc&, double) { return false; }
    void close() {}
    bool ok() const { return false; }
    void program(int) {}
    void bendRange(int) {}
    void event(const midi::MidiEvent&, int) {}
    void render(float* L, float* R, uint32_t n) {
        std::memset(L, 0, n * sizeof(float));
        std::memset(R, 0, n * sizeof(float));
    }
    void swapWith(AUInstrument&) noexcept {}
};
#endif

struct InstalledAU {
    std::string name;
    InstrumentDesc desc;
};

const std::vector<InstalledAU>& installedInstruments() {
    static std::vector<InstalledAU> list = [] {
        std::vector<InstalledAU> v;
#ifdef __APPLE__
        AudioComponentDescription d{kAudioUnitType_MusicDevice, 0, 0, 0, 0};
        AudioComponent c = nullptr;
        while ((c = AudioComponentFindNext(c, &d))) {
            AudioComponentDescription cd{};
            AudioComponentGetDescription(c, &cd);
            if (cd.componentSubType == kAudioUnitSubType_DLSSynth && cd.componentManufacturer == kAudioUnitManufacturer_Apple) continue;
            if (cd.componentSubType == 'msyn' || cd.componentSubType == kAudioUnitSubType_Sampler) continue; // need soundbanks
            CFStringRef name = nullptr;
            AudioComponentCopyName(c, &name);
            char buf[256] = {0};
            if (name) {
                CFStringGetCString(name, buf, sizeof buf, kCFStringEncodingUTF8);
                CFRelease(name);
            }
            v.push_back({buf, cd});
        }
#endif
        return v;
    }();
    return list;
}

struct SoundDef {
    enum Kind { Synth, FM, Organ, GM, AU } kind;
    std::string label;
    int index; // preset index / GM program / AU index
};

const std::vector<SoundDef>& soundList() {
    static std::vector<SoundDef> v = [] {
        std::vector<SoundDef> s;
        const char* fm[] = {"Electric Piano", "Bright Tine Piano", "Wurly Keys", "Glass Bells", "Tubular Bells", "Marimba", "FM Bass", "Celesta"};
        for (int i = 0; i < 8; ++i) s.push_back({SoundDef::FM, std::string("Keys · ") + fm[i], i});
        const char* org[] = {"Full Drawbars", "Jazz Organ", "Gospel Organ", "Soft Flute Organ"};
        for (int i = 0; i < 4; ++i) s.push_back({SoundDef::Organ, std::string("Organ · ") + org[i], i});
        const auto& ps = midi::synthPresets();
        for (int i = 0; i < static_cast<int>(ps.size()); ++i) s.push_back({SoundDef::Synth, "Synth · " + ps[static_cast<std::size_t>(i)].name, i});
        for (int i = 0; i < 128; ++i) s.push_back({SoundDef::GM, std::string("GM · ") + kGM[i], i});
        const auto& aus = installedInstruments();
        for (int i = 0; i < static_cast<int>(aus.size()); ++i) s.push_back({SoundDef::AU, "AU · " + aus[static_cast<std::size_t>(i)].name, i});
        return s;
    }();
    return v;
}

} // namespace

int instrumentSoundIndex(const std::string& needle) {
    const auto& l = soundList();
    for (std::size_t i = 0; i < l.size(); ++i)
        if (l[i].label == needle) return static_cast<int>(i);
    for (std::size_t i = 0; i < l.size(); ++i)
        if (l[i].label.find(needle) != std::string::npos) return static_cast<int>(i);
    return -1;
}

std::string instrumentSoundLabel(int index) {
    const auto& l = soundList();
    if (index < 0 || index >= static_cast<int>(l.size())) return {};
    return l[static_cast<std::size_t>(index)].label;
}

std::string instrumentSoundFamily(int index) {
    const auto& l = soundList();
    if (index < 0 || index >= static_cast<int>(l.size())) return {};
    const auto& s = l[static_cast<std::size_t>(index)];
    switch (s.kind) {
        case SoundDef::FM: return s.index == 6 ? "Bass" : (s.index <= 2 ? "Keys" : "Mallets");
        case SoundDef::Organ: return "Organ";
        case SoundDef::Synth: {
            static const char* k[] = {"Pad", "Synth Lead", "Bass", "Brass", "Synth", "Strings", "Organ", "Synth Lead", "Bass", "Pad", "Synth Lead", "Choir"};
            return s.index < 12 ? k[s.index] : "Synth";
        }
        case SoundDef::GM: {
            const int p = s.index;
            if (p < 4) return "Piano";
            if (p < 8) return "Keys";
            if (p < 16) return "Mallets";
            if (p < 24) return "Organ";
            if (p < 32) return "Guitar";
            if (p < 40) return "Bass";
            if (p < 52) return p == 46 ? "Harp" : "Strings";
            if (p < 55) return "Choir";
            if (p < 56) return "Strings";
            if (p < 64) return "Brass";
            if (p < 72) return "Sax & Reed";
            if (p < 80) return "Woodwind";
            if (p < 88) return "Synth Lead";
            if (p < 96) return "Pad";
            if (p < 104) return "Synth FX";
            if (p < 112) return "World";
            return "Percussion";
        }
        default: return "AU Instrument";
    }
}

namespace {

FMEngine::Patch fmPatch(int i) {
    FMEngine::Patch p;
    switch (i) {
        case 0: p = {1.0f, 1.8f, 1.2f, 2.2f, 0.0f, 0.35f, 14.0f, 0.35f, 0.0f}; break;     // EP
        case 1: p = {1.0f, 2.6f, 0.6f, 1.8f, 0.0f, 0.3f, 14.0f, 0.6f, 0.002f}; break;     // bright tine
        case 2: p = {1.0f, 3.2f, 0.4f, 1.4f, 0.0f, 0.25f, 3.0f, 0.2f, 0.0f}; break;       // wurly
        case 3: p = {3.5f, 2.0f, 2.5f, 3.5f, 0.0f, 1.2f, 7.0f, 0.15f, 0.001f}; break;     // glass bells
        case 4: p = {3.53f, 3.0f, 2.8f, 4.5f, 0.0f, 2.0f, 1.41f, 0.3f, 0.0f}; break;      // tubular
        case 5: p = {4.0f, 1.6f, 0.08f, 0.6f, 0.0f, 0.2f, 10.0f, 0.1f, 0.0f}; break;      // marimba
        case 6: p = {1.0f, 2.2f, 0.25f, 4.0f, 0.55f, 0.15f, 2.0f, 0.1f, 0.0f}; break;     // FM bass
        default: p = {4.0f, 1.2f, 0.6f, 2.0f, 0.0f, 0.8f, 9.0f, 0.2f, 0.0f}; break;      // celesta
    }
    return p;
}

std::array<float, 9> organBars(int i) {
    switch (i) {
        case 1: return {0.8f, 0.8f, 0.8f, 0, 0, 0, 0, 0, 0};
        case 2: return {0.8f, 0.8f, 0.8f, 0.6f, 0.3f, 0.3f, 0, 0.2f, 0.5f};
        case 3: return {0.0f, 0.6f, 0.8f, 0.4f, 0, 0, 0, 0, 0};
        default: return {0.8f, 0.8f, 0.8f, 0.8f, 0.8f, 0.8f, 0.8f, 0.8f, 0.8f};
    }
}

void forwardToSink(const std::vector<midi::MidiEvent>& ev) {
    if (auto fn = g_sink.load()) {
        void* ctx = g_sink_ctx.load();
        for (const auto& e : ev) fn(ctx, e);
    }
}

} // namespace

// ───────────────────────────── components ─────────────────────────────
namespace {

float midiSlotLevel(const AudioBlock& b) {
    double e = 0.0;
    for (uint32_t i = 0; i < b.num_frames; ++i) e += static_cast<double>(b.in[0][i]) * b.in[0][i];
    return b.num_frames ? static_cast<float>(std::sqrt(e / b.num_frames)) : 0.0f;
}

std::mutex g_nn_path_m;
std::string g_nn_path;

// Host rate -> 24 kHz for the neural detector: Kaiser-windowed sinc (32 taps, cutoff 10.8 kHz), streaming,
// allocation-free after prepare(). Training audio was resampled with scipy's polyphase Kaiser filter.
class To24k {
public:
    void prepare(double host_sr) {
        step_ = host_sr / 24000.0;
        const double fc = 10800.0 / host_sr;  // normalised cutoff (cycles/sample at host rate)
        taps_.assign(kPhases * kTaps, 0.0f);
        auto bessel0 = [](double x) { double s = 1, t = 1; for (int k = 1; k < 25; ++k) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; };
        const double beta = 8.0;
        for (int ph = 0; ph < kPhases; ++ph) {
            const double frac = double(ph) / kPhases;
            double sum = 0;
            for (int k = 0; k < kTaps; ++k) {
                const double x = (k - (kTaps / 2 - 1)) - frac;       // distance from the output instant
                const double sinc = std::abs(x) < 1e-9 ? 2 * fc : std::sin(2 * M_PI * fc * x) / (M_PI * x);
                const double r = x / (kTaps / 2.0);
                const double w = std::abs(r) <= 1 ? bessel0(beta * std::sqrt(1 - r * r)) / bessel0(beta) : 0.0;
                taps_[ph * kTaps + k] = float(sinc * w); sum += sinc * w;
            }
            for (int k = 0; k < kTaps; ++k) taps_[ph * kTaps + k] /= float(sum);
        }
        hist_.assign(2 * kTaps, 0.0f); hpos_ = 0; t_ = 0.0;
    }
    // Pushes one host sample; returns true and writes `out` when a 24 kHz sample is due.
    inline bool push(float x, float& out) {
        hist_[hpos_] = x; hist_[hpos_ + kTaps] = x; hpos_ = (hpos_ + 1) % kTaps;
        t_ += 1.0;
        if (t_ < step_) return false;
        t_ -= step_;
        const int ph = std::min(kPhases - 1, int(t_ * kPhases));     // output instant lies t_ samples back
        const float* h = &hist_[hpos_];                                 // oldest..newest, contiguous
        const float* w = &taps_[ph * kTaps];
        float acc = 0; for (int k = 0; k < kTaps; ++k) acc += w[kTaps - 1 - k] * h[k];
        out = acc; return true;
    }
private:
    static constexpr int kTaps = 32, kPhases = 64;
    std::vector<float> taps_, hist_;
    int hpos_ = 0; double step_ = 2.0, t_ = 0.0;
};

} // namespace

void setNeuralGuitarModelPath(const std::string& path) { std::lock_guard<std::mutex> lk(g_nn_path_m); g_nn_path = path; }

namespace {

class PitchToMidiComponent final : public SchemaComponentBase {
public:
    int subBlockSafety() const noexcept override { return 2; } // writes or reads the lane's MIDI bus within the block
    bool monoPreserving() const noexcept override { return true; } // audio: its input (Thru) or silence on both channels
    using SchemaComponentBase::SchemaComponentBase;

protected:
    void onPrepare(double sr, uint32_t) override {
        det_.prepare(sr);
        ev_.reserve(256);
        nev_.reserve(64);
        { std::lock_guard<std::mutex> lk(g_units_m); g_units[this]; }  // registry entry made here, not on the audio thread
        std::string path; { std::lock_guard<std::mutex> lk(g_nn_path_m); path = g_nn_path; }
        nn_ok_ = !path.empty() && nn_.load(path);   // weights shared across instances (loaded once)
        if (nn_ok_) if (const char* lg = std::getenv("HEADROOM_P2M_LEGATO")) nn_.setLegato(std::strtof(lg, nullptr));  // experiments
        if (nn_ok_) if (const char* lr = std::getenv("HEADROOM_P2M_LEGRUN")) nn_.setLegatoRun(std::atoi(lr));
        if (nn_ok_) if (const char* c = std::getenv("HEADROOM_P2M_CONFIRM"))  // "conf[,hold]"; 0 disables
            nn_.setConfirm(std::strtof(c, nullptr), std::strchr(c, ',') ? std::atoi(std::strchr(c, ',') + 1) : 2);
        if (nn_ok_) if (const char* g = std::getenv("HEADROOM_P2M_GHOST")) {  // "confirm,attack,creep"; "0,0,1" disables
            float v[3] = {0.0f, 0.06f, 0.4f}; const char* q = g;
            for (int k = 0; k < 3 && q; ++k) { v[k] = std::strtof(q, nullptr); q = std::strchr(q, ','); if (q) ++q; }
            nn_.setGhost(v[0], v[1], v[2]);
        }
        rs_.prepare(sr);
        onProcessControlRate();
    }
    void onReset() override { det_.prepare(host_sr_); if (nn_ok_) nn_.reset(); rs_.prepare(host_sr_); }
    void onProcessControlRate() override {
        midi::DetectorSettings d;
        d.poly = getParamOr("mode", 0.0f) < 0.5f;
        d.sensitivity = std::clamp(getParamOr("sens", 0.5f), 0.0f, 1.0f);
        d.bend_range = std::clamp(static_cast<int>(std::lround(getParamOr("bend", 2.0f))), 1, 24);
        d.low_note = std::clamp(static_cast<int>(std::lround(getParamOr("low", 40.0f))), 20, 100);
        d.high_note = std::clamp(static_cast<int>(std::lround(getParamOr("high", 88.0f))), d.low_note + 1, 110);
        d.velocity_curve = getParamOr("vel", 0.5f);
        if (d.poly != det_.settings().poly) {
            std::vector<midi::MidiEvent> offs;
            det_.allNotesOff(offs);
        }
        det_.setSettings(d);
        thru_ = getParamOr("thru", 1.0f) >= 0.5f;
        // Neural engine handles poly mode; mono keeps the YIN tracker (continuous pitch bends).
        neural_ = nn_ok_ && d.poly && getParamOr("engine", 0.0f) < 0.5f;
        low_ = d.low_note; high_ = d.high_note; velc_ = d.velocity_curve;
        if (nn_ok_) nn_.setSensitivity(d.sensitivity);  // 0.5 = the model's tuned threshold
    }
    void onProcessAudio(AudioBlock& b) override {
        std::array<float, kMaxBlockSize> mono{};
        const uint32_t n = std::min<uint32_t>(b.num_frames, kMaxBlockSize);
        const float* r = (b.num_channels > 1 && b.in[1]) ? b.in[1] : b.in[0];
        for (uint32_t i = 0; i < n; ++i) mono[i] = 0.5f * (b.in[0][i] + r[i]);
        ev_.clear();
        if (neural_) {
            float y;
            for (uint32_t i = 0; i < n; ++i) {
                if (!rs_.push(mono[i], y)) continue;
                nev_.clear();
                nn_.process(&y, 1, nev_);
                for (const auto& e : nev_) {
                    if (e.pitch < low_ || e.pitch > high_) continue;
                    if (e.on) {
                        const float v = std::clamp(std::pow(e.velocity, 0.5f + velc_), 0.05f, 1.0f);
                        ev_.push_back({midi::MidiEvent::NoteOn, e.pitch, v, 0.0f, i});
                        nn_active_[e.pitch] = 1; nn_vel_[e.pitch] = v;
                    } else if (nn_active_[e.pitch]) {
                        ev_.push_back({midi::MidiEvent::NoteOff, e.pitch, 0.0f, 0.0f, i});
                        nn_active_[e.pitch] = 0;
                    }
                }
            }
            // input level envelope (drives instruments' Guitar Follow and the meters)
            float e2 = 0; for (uint32_t i = 0; i < n; ++i) e2 += mono[i] * mono[i];
            const float rms = std::sqrt(e2 / std::max<uint32_t>(1, n));
            nn_level_ = std::max(rms, nn_level_ * 0.92f);
        } else {
            det_.process(mono.data(), n, ev_);
        }
        if (b.midi) b.midi->insert(b.midi->end(), ev_.begin(), ev_.end());
        forwardToSink(ev_);
        for (uint32_t c = 0; c < b.num_channels; ++c) {
            if (!b.out[c]) continue;
            if (thru_) std::memcpy(b.out[c], b.in[c] ? b.in[c] : b.in[0], n * sizeof(float));
            else std::memset(b.out[c], 0, n * sizeof(float));
        }
        const float lvl = neural_ ? nn_level_ : det_.level();
        live_level_.store(lvl);
        auto notesInto = [&](std::vector<std::pair<int, float>>& out) {
            if (!neural_) { det_.activeNotes(out); return; }
            out.clear();
            for (int k = 0; k < 128; ++k) if (nn_active_[k]) out.emplace_back(k, nn_vel_[k]);
        };
        if (g_live.m.try_lock()) {
            notesInto(g_live.notes);
            g_live.pitch = neural_ ? 0.0f : det_.pitchHz();
            g_live.conf = neural_ ? 1.0f : det_.confidence();
            g_live.level = lvl;
            g_live.m.unlock();
        }
        if (g_units_m.try_lock()) {
            auto it = g_units.find(this);
            if (it == g_units.end()) { g_units_m.unlock(); return; }
            auto& u = it->second;
            notesInto(u.notes);
            u.pitch = neural_ ? 0.0f : det_.pitchHz();
            u.conf = neural_ ? 1.0f : det_.confidence();
            u.level = lvl;
            u.slot = slot_;
            g_units_m.unlock();
        }
    }
    void setSlotIndex(int s) override { slot_ = s; }

public:
    // Knob edits rebuild the unit: keep the detector's history and tracked notes so held notes don't drop.
    void adoptRuntimeStateFrom(IComponent& previous) noexcept override {
        auto* old = dynamic_cast<PitchToMidiComponent*>(&previous);
        if (!old || host_sr_ != old->host_sr_) return;
        const midi::DetectorSettings s = det_.settings();
        std::swap(det_, old->det_);
        det_.setSettings(s);
        if (nn_ok_ && old->nn_ok_) {
            std::swap(nn_, old->nn_);
            std::swap(rs_, old->rs_);
            nn_active_ = old->nn_active_; nn_vel_ = old->nn_vel_; nn_level_ = old->nn_level_;
            nn_.setSensitivity(s.sensitivity);
        }
    }
    ~PitchToMidiComponent() override {
        std::lock_guard<std::mutex> lk(g_units_m);
        g_units.erase(this);
    }

protected:

private:
    midi::PitchToMidi det_;
    midi::NeuralGuitarMidi nn_;
    To24k rs_;
    std::vector<midi::NeuralGuitarMidi::Note> nev_;
    std::array<uint8_t, 128> nn_active_{};
    std::array<float, 128> nn_vel_{};
    float nn_level_ = 0.0f;
    bool nn_ok_ = false, neural_ = false;
    int low_ = 40, high_ = 88;
    float velc_ = 0.5f;
    std::vector<midi::MidiEvent> ev_;
    bool thru_ = true;
    int slot_ = -1;
    std::atomic<float> live_level_{0.0f};
};

class MidiFxComponent final : public SchemaComponentBase {
public:
    int subBlockSafety() const noexcept override { return 2; } // writes or reads the lane's MIDI bus within the block
    bool monoPreserving() const noexcept override { return true; } // audio passes through unchanged
    using SchemaComponentBase::SchemaComponentBase;

protected:
    void onPrepare(double sr, uint32_t) override {
        fx_.prepare(sr);
        onProcessControlRate();
    }
    void onReset() override { fx_.prepare(host_sr_); }
    void onProcessControlRate() override {
        static const char* kHarm[] = {"off", "octave", "fifth", "power", "third", "major", "minor", "sus4"};
        static const char* kScale[] = {"chromatic", "major", "minor", "dorian", "mixolydian", "pentatonic_minor", "pentatonic_major", "blues"};
        static const char* kArp[] = {"off", "up", "down", "updown", "random"};
        static const char* kRate[] = {"1/4", "1/8", "1/16", "1/8T", "1/16T", "1/32"};
        auto idx = [&](const char* id, int mx, float def) { return std::clamp(static_cast<int>(std::lround(getParamOr(id, def))), 0, mx); };
        midi::MidiFxSettings m;
        m.octave = std::clamp(static_cast<int>(std::lround(getParamOr("octave", 0.0f))), -3, 3);
        m.transpose = std::clamp(static_cast<int>(std::lround(getParamOr("transpose", 0.0f))), -12, 12);
        m.harmony = kHarm[idx("harmony", 7, 0)];
        m.scale = kScale[idx("scale", 7, 0)];
        m.scale_root = idx("root", 11, 4);
        m.arp = kArp[idx("arp", 4, 0)];
        m.arp_rate = kRate[idx("rate", 5, 2)];
        m.hold = getParamOr("hold", 0.0f) >= 0.5f;
        m.bpm = std::clamp(getParamOr("bpm", 120.0f), 30.0f, 300.0f);
        sync_ = getParamOr("sync", 1.0f) >= 0.5f;
        last_bpm_ = -1.0;  // re-apply the rig tempo on the next block when synced
        fx_.setSettings(m);
    }
    void onProcessAudio(AudioBlock& b) override {
        b.copyInToOut();
        if (!b.midi) return;
        out_.clear();
        if (sync_ && b.bpm > 0.0 && b.bpm != last_bpm_) {  // follow the rig / host tempo
            fx_.setBpm(static_cast<float>(std::clamp(b.bpm, 30.0, 300.0)));
            last_bpm_ = b.bpm;
        }
        fx_.process(*b.midi, b.num_frames, out_);
        b.midi->swap(out_);
    }

public:
    // Knob edits rebuild the unit: keep held/latched notes, the arpeggiator clock and sounding harmony.
    void adoptRuntimeStateFrom(IComponent& previous) noexcept override {
        auto* old = dynamic_cast<MidiFxComponent*>(&previous);
        if (!old || host_sr_ != old->host_sr_) return;
        std::swap(fx_, old->fx_);
        onProcessControlRate();  // re-apply this unit's (new) settings to the adopted state
    }

private:
    bool sync_ = true;
    double last_bpm_ = -1.0;
    midi::MidiFx fx_;
    std::vector<midi::MidiEvent> out_;
};

// Bloom: the arpeggiator played by the guitar (Bloom.hpp). Reads the lane's tracked notes and the guitar audio; writes
// the arpeggio on the MIDI bus (Synth / Both) and dry × duck + the Strings voice on the audio (Strings / Both).
class BloomComponent final : public SchemaComponentBase {
public:
    int subBlockSafety() const noexcept override { return 2; } // reads and rewrites the lane's MIDI bus within the block
    bool monoPreserving() const noexcept override { return true; } // one signal on both channels in, one out
    using SchemaComponentBase::SchemaComponentBase;
    ~BloomComponent() override {
        std::lock_guard<std::mutex> lk(g_bloom_m);
        g_bloom.erase(this);
    }
    void setSlotIndex(int s) override { slot_ = s; }

protected:
    void onPrepare(double sr, uint32_t) override {
        bl_.prepare(sr);
        out_.reserve(512);
        { std::lock_guard<std::mutex> lk(g_bloom_m); g_bloom[this]; }  // registry entry made here, not on the audio thread
        onProcessControlRate();
    }
    void onReset() override { bl_.prepare(host_sr_); onProcessControlRate(); }
    void onProcessControlRate() override {
        auto idx = [&](const char* id, int mn, int mx, float def) { return std::clamp(static_cast<int>(std::lround(getParamOr(id, def))), mn, mx); };
        midi::BloomSettings m;
        m.trigger = idx("trigger", 0, 3, 0);
        m.engage = getParamOr("engage", 0.0f) >= 0.5f;
        m.pattern = idx("pattern", 0, 7, 4);
        m.rate = idx("rate", 0, 5, 3);
        m.motion = std::clamp(getParamOr("motion", 0.5f), 0.0f, 1.0f);
        m.touch = std::clamp(getParamOr("touch", 0.6f), 0.0f, 1.0f);
        m.range = idx("range", 0, 3, 1) + 1;
        m.extend = idx("extend", 0, 4, 1);
        m.key = idx("key", 0, 12, 0) - 1;
        m.scale = idx("scale", 0, 6, 0);
        m.gate = std::clamp(getParamOr("gate", 0.6f), 0.05f, 1.0f);
        m.swing = std::clamp(getParamOr("swing", 0.0f), 0.0f, 1.0f);
        m.feel = getParamOr("clock", 1.0f) >= 0.5f;
        m.bend = std::clamp(getParamOr("bend", 1.0f), 0.0f, 1.0f);
        m.choke = std::clamp(getParamOr("choke", 0.7f), 0.0f, 1.0f);
        m.bloom_s = std::clamp(getParamOr("bloom", 0.6f), 0.1f, 2.5f);
        m.voice = idx("voice", 0, 2, 2);
        m.tone = std::clamp(getParamOr("tone", 0.5f), 0.0f, 1.0f);
        m.dry = std::clamp(getParamOr("dry", 0.5f), 0.0f, 1.0f);
        m.level = std::clamp(getParamOr("level", 0.5f), 0.0f, 1.0f);
        m.retrig = getParamOr("retrig", 0.0f) >= 0.5f ? 1 : 0;
        bl_.setSettings(m);
    }
    void onProcessAudio(AudioBlock& b) override {
        const uint32_t n = std::min<uint32_t>(b.num_frames, kMaxBlockSize);
        std::array<float, kMaxBlockSize> mono{}, str{}, dry{};
        const float* r = (b.num_channels > 1 && b.in[1]) ? b.in[1] : b.in[0];
        for (uint32_t i = 0; i < n; ++i) mono[i] = 0.5f * (b.in[0][i] + r[i]);
        static const std::vector<midi::MidiEvent> kNone;
        out_.clear();
        bl_.process(b.midi ? *b.midi : kNone, mono.data(), n, b.bpm, b.ppq_position, b.transport_playing, out_, str.data(), dry.data());
        if (b.midi) {
            for (std::size_t i = 1; i < out_.size(); ++i)  // insertion sort by offset (stable, no allocation)
                for (std::size_t j = i; j > 0 && out_[j].offset < out_[j - 1].offset; --j) std::swap(out_[j], out_[j - 1]);
            b.midi->swap(out_);
        }
        for (uint32_t c = 0; c < b.num_channels; ++c) {
            if (!b.out[c]) continue;
            const float* in = b.in[c] ? b.in[c] : b.in[0];
            for (uint32_t i = 0; i < n; ++i) b.out[c][i] = in[i] * dry[i] + str[i];
        }
        if (g_bloom_m.try_lock()) {
            auto it = g_bloom.find(this);
            if (it != g_bloom.end()) { it->second.live = bl_.live(); it->second.slot = slot_; }
            g_bloom_m.unlock();
        }
    }

public:
    // Knob edits (and the Engage footswitch) rebuild the unit: keep the whole performance state — chord memory,
    // captured strings, sounding voices, learned groove and key, the clock.
    void adoptRuntimeStateFrom(IComponent& previous) noexcept override {
        auto* old = dynamic_cast<BloomComponent*>(&previous);
        if (!old || host_sr_ != old->host_sr_) return;
        std::swap(bl_, old->bl_);
        onProcessControlRate();
    }

private:
    midi::Bloom bl_;
    std::vector<midi::MidiEvent> out_;
    int slot_ = -1;
};

class InstrumentComponent final : public SchemaComponentBase {
public:
    int subBlockSafety() const noexcept override { return 2; } // writes or reads the lane's MIDI bus within the block
    using SchemaComponentBase::SchemaComponentBase;

    std::vector<ExtraParam> extraParams() const override {
        static const char* kGMFamilies[16] = {"Piano", "Chromatic Percussion", "Organ", "Guitar", "Bass", "Strings", "Ensemble", "Brass",
                                               "Reed", "Pipe", "Synth Lead", "Synth Pad", "Synth FX", "Ethnic", "Percussive", "Sound FX"};
        ExtraParam p;
        p.id = "sound";
        p.name = "Sound";
        for (const auto& s : soundList()) {
            p.labels.push_back(s.label);
            switch (s.kind) {
                case SoundDef::FM: p.groups.push_back("Keys"); break;
                case SoundDef::Organ: p.groups.push_back("Organ"); break;
                case SoundDef::Synth: p.groups.push_back("Synth"); break;
                case SoundDef::GM: p.groups.push_back(std::string("GM ") + kGMFamilies[s.index / 8]); break;
                default: p.groups.push_back("AU Instruments"); break;
            }
        }
        return {p};
    }

protected:
    void onPrepare(double sr, uint32_t) override {
        if (params_.find("sound") == params_.end()) params_["sound"].num_val = 0.0f;
        synth_.prepare(sr);
        fm_.prepare(sr);
        organ_.prepare(sr);
        loaded_ = -1;
        onProcessControlRate();
    }
    void onReset() override {
        synth_.allNotesOff();
    }
    void onProcessControlRate() override {
        const auto& list = soundList();
        const int sel = std::clamp(static_cast<int>(std::lround(getParamOr("sound", 0.0f))), 0, static_cast<int>(list.size()) - 1);
        const auto& sd = list[static_cast<std::size_t>(sel)];
        if (sel != loaded_) {
            loaded_ = sel;
            kind_ = sd.kind;
            if (sd.kind == SoundDef::Synth) synth_.applyPreset(midi::synthPresets()[static_cast<std::size_t>(sd.index)].name);
            else if (sd.kind == SoundDef::FM) fm_.setPatch(fmPatch(sd.index));
            else if (sd.kind == SoundDef::Organ) organ_.setDrawbars(organBars(sd.index));
            else if (sd.kind == SoundDef::GM) {
                if (!au_.ok() || au_kind_ != 0) {
                    au_.openGeneralMidi(host_sr_);
                    au_kind_ = 0;
                    au_.bendRange(12);
                }
                au_.program(sd.index);
            } else if (sd.kind == SoundDef::AU) {
                au_.open(installedInstruments()[static_cast<std::size_t>(sd.index)].desc, host_sr_);
                au_kind_ = 1 + sd.index;
                au_.bendRange(12);
            }
            base_params_ = synth_.params();
        }
        tone_ = getParamOr("tone", 0.5f);
        attack_ = getParamOr("attack", 0.5f);
        release_ = getParamOr("release", 0.5f);
        vibrato_ = getParamOr("vibrato", 0.0f);
        follow_ = getParamOr("follow", 0.3f);
        level_ = getParamOr("level", 0.7f);
        mix_ = getParamOr("mix", 1.0f);
        if (kind_ == SoundDef::Synth && !base_params_.empty()) {
            // Macro controls offset the preset's own values.
            auto at = [&](const char* id) { return base_params_[static_cast<std::size_t>(synth_.paramIndex(id))]; };
            synth_.setParam("cutoff", std::clamp(at("cutoff") + (tone_ - 0.5f) * 0.6f, 0.0f, 1.0f));
            synth_.setParam("amp_a", std::clamp(at("amp_a") + (attack_ - 0.5f) * 0.8f, 0.0f, 1.0f));
            synth_.setParam("amp_r", std::clamp(at("amp_r") + (release_ - 0.5f) * 0.8f, 0.0f, 1.0f));
            synth_.setParam("lfo_pitch", std::clamp(at("lfo_pitch") + vibrato_ * 0.5f, 0.0f, 1.0f));
            synth_.setParam("follow", follow_);
            synth_.setParam("level", 0.85f);
        }
        tone_lp_.setLowPass(host_sr_, 400.0f * std::pow(45.0f, tone_), 0.6f);
        tone_lp2_.setLowPass(host_sr_, 400.0f * std::pow(45.0f, tone_), 0.6f);
    }
    void onProcessAudio(AudioBlock& b) override {
        const uint32_t n = std::min<uint32_t>(b.num_frames, kMaxBlockSize);
        std::array<float, kMaxBlockSize> L{}, R{};
        static const std::vector<midi::MidiEvent> kNone;
        const auto& ev = b.midi ? *b.midi : kNone;
        float bend = last_bend_;
        for (const auto& e : ev)
            if (e.type == midi::MidiEvent::PitchBend) bend = e.bend;
        // Guitar Follow's level smoothing is a time, not a per-call step: 0.2 per 128-frame block at 48 kHz (~13 ms)
        // at any host buffer size, so offline renders (512-frame blocks) follow the guitar as the app does.
        const float follow_k = 1.0f - std::pow(0.8f, static_cast<float>(n) * 375.0f / static_cast<float>(host_sr_));
        follow_env_ += (std::min(1.0f, midiSlotLevel(b) * midi::kFollowGain) - follow_env_) * follow_k; // full level: see kFollowGain
        switch (kind_) {
            case SoundDef::Synth:
                synth_.setFollowLevel(midiSlotLevel(b));
                synth_.render(ev, L.data(), R.data(), n);
                break;
            case SoundDef::FM:
                for (const auto& e : ev) {
                    if (e.type == midi::MidiEvent::NoteOn) fm_.noteOn(e.note, e.velocity);
                    else if (e.type == midi::MidiEvent::NoteOff) fm_.noteOff(e.note);
                }
                fm_.render(L.data(), R.data(), n, bend, vibrato_, tone_, 0.3f + 1.4f * release_);
                break;
            case SoundDef::Organ:
                for (const auto& e : ev) {
                    if (e.type == midi::MidiEvent::NoteOn) organ_.noteOn(e.note, e.velocity);
                    else if (e.type == midi::MidiEvent::NoteOff) organ_.noteOff(e.note);
                }
                organ_.render(L.data(), R.data(), n, bend, vibrato_, tone_);
                break;
            default:
                for (const auto& e : ev) au_.event(e, 12);
                au_.render(L.data(), R.data(), n);
                for (uint32_t i = 0; i < n; ++i) {
                    L[i] = tone_lp_.process(L[i]) * 1.6f;
                    R[i] = tone_lp2_.process(R[i]) * 1.6f;
                }
                break;
        }
        last_bend_ = bend;
        const float lv = 1.6f * level_ * level_;
        const float fol = (kind_ == SoundDef::Synth) ? 1.0f : (1.0f - follow_ + follow_ * std::clamp(follow_env_ * 1.3f, 0.0f, 1.2f));
        for (uint32_t i = 0; i < n; ++i) {
            const float il = b.in[0][i], ir = (b.num_channels > 1 && b.in[1]) ? b.in[1][i] : il;
            if (b.out[0]) b.out[0][i] = (1.0f - mix_) * il + mix_ * L[i] * lv * fol;
            if (b.num_channels > 1 && b.out[1]) b.out[1][i] = (1.0f - mix_) * ir + mix_ * R[i] * lv * fol;
        }
    }

public:
    // Knob edits rebuild the unit: when the selected sound is unchanged, keep sounding voices and the hosted
    // AU instance (no re-open, no cut-off notes). A different sound starts fresh by design.
    void adoptRuntimeStateFrom(IComponent& previous) noexcept override {
        auto* old = dynamic_cast<InstrumentComponent*>(&previous);
        if (!old || host_sr_ != old->host_sr_ || loaded_ != old->loaded_ || au_kind_ != old->au_kind_) return;
        std::swap(synth_, old->synth_);
        std::swap(fm_, old->fm_);
        std::swap(organ_, old->organ_);
        au_.swapWith(old->au_);
        std::swap(base_params_, old->base_params_);
        last_bend_ = old->last_bend_;
        follow_env_ = old->follow_env_;
        tone_lp_ = old->tone_lp_;
        tone_lp2_ = old->tone_lp2_;
        onProcessControlRate();  // same sound: only re-applies this unit's tone/attack/release/level
    }

private:
    midi::Synth synth_;
    FMEngine fm_;
    OrganEngine organ_;
    AUInstrument au_;
    int au_kind_ = -1;
    int loaded_ = -1;
    SoundDef::Kind kind_ = SoundDef::FM;
    std::vector<float> base_params_;
    float tone_ = 0.5f, attack_ = 0.5f, release_ = 0.5f, vibrato_ = 0.0f, follow_ = 0.3f, level_ = 0.7f, mix_ = 1.0f;
    float last_bend_ = 0.0f, follow_env_ = 0.0f;
    struct OnePoleLP {
        float a = 1.0f, z = 0.0f;
        void setLowPass(double sr, float hz, float) { a = 1.0f - std::exp(-6.2831853f * std::min(hz, 20000.0f) / static_cast<float>(sr)); }
        float process(float x) { z += a * (x - z); return z; }
    } tone_lp_, tone_lp2_;
};

class PolySynthComponent final : public SchemaComponentBase {
public:
    int subBlockSafety() const noexcept override { return 2; } // writes or reads the lane's MIDI bus within the block
    using SchemaComponentBase::SchemaComponentBase;

protected:
    void onPrepare(double sr, uint32_t) override {
        synth_.prepare(sr);
        onProcessControlRate();
    }
    void onReset() override { synth_.allNotesOff(); }
    void onProcessControlRate() override {
        for (const auto& d : midi::synthParamDefs()) {
            const float mx = d.steps ? static_cast<float>(d.steps - 1) : 1.0f;
            const float raw = getParamOr(d.id, d.steps ? std::round(d.def * mx) : d.def);
            synth_.setParam(d.id, mx > 0.0f ? raw / mx : raw);
        }
    }
    void onProcessAudio(AudioBlock& b) override {
        const uint32_t n = std::min<uint32_t>(b.num_frames, kMaxBlockSize);
        std::array<float, kMaxBlockSize> L{}, R{};
        static const std::vector<midi::MidiEvent> kNone;
        synth_.setFollowLevel(midiSlotLevel(b));
        synth_.render(b.midi ? *b.midi : kNone, L.data(), R.data(), n);
        for (uint32_t i = 0; i < n; ++i) {
            if (b.out[0]) b.out[0][i] = L[i];
            if (b.num_channels > 1 && b.out[1]) b.out[1][i] = R[i];
        }
    }

public:
    // Knob edits rebuild the unit: keep sounding voices and effect tails, then apply the new parameters.
    void adoptRuntimeStateFrom(IComponent& previous) noexcept override {
        auto* old = dynamic_cast<PolySynthComponent*>(&previous);
        if (!old || host_sr_ != old->host_sr_) return;
        std::swap(synth_, old->synth_);
        onProcessControlRate();
    }

private:
    midi::Synth synth_;
};

} // namespace

std::unique_ptr<IComponent> makeMidiComponent(const catalog::ComponentSchema& s) {
    switch (s.table_id) {
        case 900001: return std::make_unique<PitchToMidiComponent>(s);
        case 900002: return std::make_unique<MidiFxComponent>(s);
        case 900003: return std::make_unique<InstrumentComponent>(s);
        case 900004: return std::make_unique<PolySynthComponent>(s);
        case 900006: return std::make_unique<BloomComponent>(s);
        default: return nullptr;
    }
}

std::vector<std::pair<std::string, float>> polySynthPresetParams(const std::string& preset) {
    std::vector<std::pair<std::string, float>> out;
    for (const auto& p : midi::synthPresets()) {
        if (p.name != preset) continue;
        for (const auto& d : midi::synthParamDefs()) {
            float n = d.def;
            for (const auto& kv : p.values)
                if (kv.first == d.id) n = kv.second;
            const float mx = d.steps ? static_cast<float>(d.steps - 1) : 1.0f;
            out.emplace_back(d.id, d.steps ? std::round(n * mx) : n);
        }
    }
    return out;
}

} // namespace rig
