#include "Showcase.hpp"
#include "../audio/WavIO.hpp"
#include "../core/RackGraph.hpp"
#include "../dsp/AmpModeling.hpp"
#include "../midi/MidiComponents.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>

namespace rig::library {
namespace {

using Params = std::vector<std::pair<std::string, float>>;

// Table ids used below.
enum : int32_t {
    kP2M = 900001, kMidiFx = 900002, kInst = 900003, kPolySynth = 900004, kNam = 900005,
    kTremolo = 13000, kTS808 = 15000, kRecto = 16000, kPhaser = 20000, kFlanger = 28000, kChorus = 29000,
    kBlackface = 30000, kRotor = 31000, kRoom = 33000, kPlexi = 34000, kTopBoost = 38000, kFuzzFace = 41000,
    kMuff = 43000, kEnvFilter = 47000, kComp = 48000, kJcm800 = 56000, kTweed = 62000, kGerman = 81000,
    kShimmer = 95000, kPingPong = 98000, kTempoDelay = 107000, kIrVerb = 118000, kPlateSpring = 127000,
    k5150 = 134000, kDimension = 142000, kCosmic = 148000, kHall224 = 154000, kHall480 = 155000,
    kDiffDelay = 158000, kScanner = 164000,
};

int sound(const char* label) {
    const int i = instrumentSoundIndex(label);
    return i < 0 ? 0 : i;
}

// Lane-balance request: lane B (or a whole MIDI-only rig) relative to lane A, in dB, measured with the
// given DI clip. Level nodes are (table id, ordinal) of the units whose "level" is trimmed.
struct Balance {
    float b_minus_a_db = -2.0f;
    std::vector<std::pair<int32_t, int>> level_nodes;
};

struct Spec {
    std::string name, comment, di;
    std::function<void(class Builder&)> build;
    bool balance = false;
    Balance bal;
};

class Builder {
public:
    explicit Builder(const ComponentPresetBank* bank) : bank_(bank) {
        preset::RackPreset empty;
        empty.root_rack.table_id = 100;
        g.buildFromPreset(empty, 48000.0, 512);
    }

    Params init(int32_t tid, const Params& over) const {
        Params v;
        if (bank_)
            if (const auto* cp = bank_->initFor(tid)) v = cp->params;
        for (const auto& kv : over) {
            bool set = false;
            for (auto& e : v)
                if (e.first == kv.first) { e.second = kv.second; set = true; }
            if (!set) v.push_back(kv);
        }
        return v;
    }
    int add(int32_t tid, const Params& over = {}) { return g.insertComponent(tid, 9999, init(tid, over)); }
    int split(float xfade = 0.5f) { return split_ = g.insertSplit(9999, xfade); }
    int lane(char l, int32_t tid, const Params& over = {}) { return g.insertIntoLane(split_, l, 99, tid, init(tid, over)); }
    int splitId() const { return split_; }

    // Active slot of the nth unit with this table id.
    std::size_t slot(int32_t tid, int nth = 0) const {
        for (const auto& v : g.activeNodeViews())
            if (v.table_id == tid && nth-- == 0) return v.slot_idx;
        return 9999;
    }
    void macro(int col, int32_t tid, const std::string& param, float mn, float mx, const std::string& label, int nth = 0, bool add_target = false) {
        g.assignMacro(col, slot(tid, nth), param, mn, mx, label, add_target);
    }
    void macroMix(int col, float mn, float mx, const std::string& label) { g.assignMacroSplit(col, split_, mn, mx, label, false); }

    RackGraph g;

private:
    const ComponentPresetBank* bank_;
    int split_ = -1;
};

// ───────────── preset-tree helpers ─────────────
void collect(preset::PresetComponentNode& n, std::vector<preset::PresetComponentNode*>& out) {
    for (auto& c : n.children) {
        out.push_back(&c);
        collect(c, out);
    }
}
preset::PresetComponentNode* findNode(preset::RackPreset& p, int32_t tid, int nth) {
    std::vector<preset::PresetComponentNode*> all;
    collect(p.root_rack, all);
    for (auto* n : all)
        if (n->table_id == tid && nth-- == 0) return n;
    return nullptr;
}
float getP(const preset::PresetComponentNode& n, const std::string& id, float def) {
    for (const auto& pp : n.params)
        if (pp.id == id) return pp.float_val;
    return def;
}
void setP(preset::PresetComponentNode& n, const std::string& id, float v) {
    for (auto& pp : n.params)
        if (pp.id == id) { pp.float_val = v; pp.type = "float"; return; }
    preset::PresetParam pp;
    pp.id = id;
    pp.type = "float";
    pp.float_val = v;
    n.params.push_back(pp);
}

// K-ish loudness (80 Hz high-pass, mean square) of the rig fed with a DI clip, with the first split's
// crossfade forced to `xfade` (negative = leave as is).
float renderLoudness(const preset::RackPreset& src, const audio::WavData& di, float xfade, double seconds = 12.0) {
    preset::RackPreset p = src;
    if (xfade >= 0.0f)
        if (auto* m = findNode(p, 9000, 0)) setP(*m, "XFade", xfade);
    RackGraph g;
    const double sr = di.sample_rate;
    g.buildFromPreset(p, sr, 512);
    amp::LoudnessMeter meter;
    meter.prepare(sr);
    const std::size_t total = std::min<std::size_t>(di.left.size(), static_cast<std::size_t>(seconds * sr));
    StereoScratchBuffer in{}, out{};
    for (std::size_t off = 0; off < total; off += 512) {
        const uint32_t n = static_cast<uint32_t>(std::min<std::size_t>(512, total - off));
        for (uint32_t i = 0; i < n; ++i) {
            in.left[i] = di.left[off + i];
            in.right[i] = di.right.empty() ? di.left[off + i] : di.right[off + i];
        }
        AudioBlock b{};
        b.in = in.const_ptrs.data();
        b.out = out.ptrs.data();
        b.num_channels = 2;
        b.num_frames = n;
        b.sample_rate = sr;
        g.process(b);
        if (off > static_cast<std::size_t>(0.3 * sr))
            for (uint32_t i = 0; i < n; ++i) meter.add(out.left[i], out.right[i]);
    }
    return meter.meanSquareDb();
}

std::vector<Spec> specs() {
    std::vector<Spec> v;
    auto midiOnly = [&](std::string name, std::string comment, std::string di, std::function<void(Builder&)> fn) {
        v.push_back({std::move(name), std::move(comment), std::move(di), std::move(fn), false, {}});
    };
    auto stacked = [&](std::string name, std::string comment, std::string di, Balance bal, std::function<void(Builder&)> fn) {
        v.push_back({std::move(name), std::move(comment), std::move(di), std::move(fn), true, std::move(bal)});
    };
    const Params poly = {{"mode", 0.0f}, {"sens", 0.55f}, {"vel", 0.5f}};
    const Params mono = {{"mode", 1.0f}, {"sens", 0.55f}, {"bend", 2.0f}, {"vel", 0.55f}};
    const Params monoBend12 = {{"mode", 1.0f}, {"sens", 0.55f}, {"bend", 12.0f}, {"vel", 0.55f}};
    // Layered leads that were Mono (2026-11-13): Poly (neural) so strummed chords no longer drop the instrument out. Poly
    // sends notes only (no bends: the instrument steps by semitone), and Bend Range is unused. The shipped .rig files'
    // Instrument levels were set from work/hybrid/both_audible.py (both parts audible on lead/strum/riff), not this balancer.
    const Params polyLead = {{"mode", 0.0f}, {"sens", 0.55f}, {"bend", 2.0f}, {"vel", 0.55f}};

    // ═════════════ MIDI: the guitar plays instruments ═════════════
    midiOnly("Concert Grand", "Your guitar plays a concert grand. Poly tracking — strum or fingerpick chords and let them ring.", "strum",
             [=](Builder& b) {
                 b.add(kP2M, poly);
                 b.add(kInst, {{"sound", (float)sound("GM · Acoustic Grand Piano")}, {"tone", 0.62f}, {"level", 0.75f}});
                 b.add(kPlateSpring, {{"S1", 1.0f}, {"M2", 0.16f}});
                 b.macro(0, kInst, "tone", 0.2f, 1.0f, "Brightness");
                 b.macro(1, kPlateSpring, "M2", 0.0f, 0.5f, "Plate");
                 b.macro(2, kP2M, "sens", 0.2f, 0.9f, "Sensitivity");
                 b.macro(3, kP2M, "vel", 0.0f, 1.0f, "Dynamics");
             });
    midiOnly("Tine Lounge", "A bright tine electric piano through a stereo chorus and a gentle tremolo. Comping chords, neo-soul voicings.", "strum",
             [=](Builder& b) {
                 b.add(kP2M, poly);
                 b.add(kInst, {{"sound", (float)sound("Keys · Bright Tine Piano")}, {"tone", 0.55f}, {"release", 0.55f}, {"level", 0.75f}});
                 b.add(kTremolo, {{"Rt", 0.42f}, {"Int", 0.28f}});
                 b.add(kChorus, {{"Mx", 0.4f}, {"VD", 0.45f}, {"VR", 0.35f}});
                 b.add(kPlateSpring, {{"S1", 0.0f}, {"M2", 0.14f}});
                 b.macro(0, kInst, "tone", 0.2f, 1.0f, "Bark");
                 b.macro(1, kTremolo, "Int", 0.0f, 0.8f, "Tremolo");
                 b.macro(2, kChorus, "CI", 0.0f, 0.8f, "Chorus"); // the CE-1 has no mix control: Intensity sets how much chorus
                 b.macro(3, kPlateSpring, "M2", 0.0f, 0.45f, "Plate");
             });
    midiOnly("Wurly Soul", "Reedy electric piano, amp tremolo and a dripping spring. Play double-stops and short soul chords.", "strum",
             [=](Builder& b) {
                 b.add(kP2M, poly);
                 b.add(kInst, {{"sound", (float)sound("Keys · Wurly Keys")}, {"tone", 0.6f}, {"level", 0.78f}});
                 b.add(kTremolo, {{"Rt", 0.55f}, {"Int", 0.45f}});
                 b.add(kPlateSpring, {{"S1", 4.0f}, {"M2", 0.2f}});
                 b.macro(0, kTremolo, "Int", 0.0f, 0.9f, "Tremolo");
                 b.macro(1, kTremolo, "Rt", 0.1f, 0.9f, "Speed");
                 b.macro(2, kPlateSpring, "M2", 0.0f, 0.5f, "Spring");
             });
    midiOnly("Jazz Organ & Rotor", "Drawbar jazz organ through a rotating speaker cabinet. Hold chords and ride the Rotor Speed macro.", "strum",
             [=](Builder& b) {
                 b.add(kP2M, poly);
                 b.add(kInst, {{"sound", (float)sound("Organ · Jazz Organ")}, {"tone", 0.55f}, {"level", 0.72f}});
                 b.add(kRotor, {});
                 b.add(kRoom, {{"Wet", 0.18f}, {"Sz", 0.45f}});
                 b.macro(0, kRotor, "SF", 0.0f, 1.0f, "Rotor Speed");
                 b.macro(1, kInst, "tone", 0.2f, 1.0f, "Drawbars");
                 b.macro(2, kRoom, "Wet", 0.0f, 0.5f, "Room");
             });
    stacked("Gospel Sunday", "Gospel organ layered with a choir, rotor and a big church hall. Slow chord swells.", "strum",
            {-5.0f, {{kInst, 1}}}, [=](Builder& b) {
                b.add(kP2M, poly);
                b.split(0.5f);
                b.lane('A', kInst, {{"sound", (float)sound("Organ · Gospel Organ")}, {"level", 0.72f}});
                b.lane('A', kRotor, {});
                b.lane('B', kInst, {{"sound", (float)sound("GM · Choir Aahs")}, {"level", 0.6f}});
                b.add(kIrVerb, {{"IR", 10.0f}, {"W0", 0.32f}, {"D7", 1.0f}});
                b.macroMix(0, 0.15f, 0.85f, "Organ ↔ Choir");
                b.macro(1, kRotor, "SF", 0.0f, 1.0f, "Rotor Speed");
                b.macro(2, kIrVerb, "W0", 0.0f, 0.7f, "Church");
            });
    stacked("Piano & Horns", "One guitar, two players: a grand piano on the left lane, a punchy horn section an octave up on the right. Short, rhythmic chords.", "strum",
            {-3.0f, {{kInst, 1}}}, [=](Builder& b) {
                b.add(kP2M, poly);
                b.split(0.5f);
                b.lane('A', kInst, {{"sound", (float)sound("GM · Acoustic Grand Piano")}, {"tone", 0.6f}, {"level", 0.72f}});
                b.lane('B', kMidiFx, {{"octave", 1.0f}});
                b.lane('B', kInst, {{"sound", (float)sound("GM · Brass Section")}, {"tone", 0.7f}, {"level", 0.65f}});
                b.add(kIrVerb, {{"IR", 8.0f}, {"W0", 0.18f}, {"D7", 1.0f}});
                b.macroMix(0, 0.1f, 0.9f, "Piano ↔ Horns");
                b.macro(1, kInst, "tone", 0.2f, 1.0f, "Horn Bite", 1);
                b.macro(2, kIrVerb, "W0", 0.0f, 0.6f, "Hall");
                b.macro(3, kP2M, "vel", 0.0f, 1.0f, "Dynamics");
            });
    stacked("Brass Section Stabs", "Tight horn stabs — trumpets doubled with an analog brass synth. Play short, staccato chords.", "strum",
            {-6.0f, {{kInst, 1}}}, [=](Builder& b) {
                b.add(kP2M, {{"mode", 0.0f}, {"sens", 0.5f}, {"vel", 0.65f}});
                b.split(0.5f);
                b.lane('A', kInst, {{"sound", (float)sound("GM · Brass Section")}, {"tone", 0.75f}, {"level", 0.75f}});
                b.lane('B', kInst, {{"sound", (float)sound("Synth · Brass Stab")}, {"tone", 0.55f}, {"attack", 0.3f}, {"release", 0.3f}, {"level", 0.6f}});
                b.add(kRoom, {{"Wet", 0.2f}, {"Sz", 0.5f}});
                b.macroMix(0, 0.1f, 0.9f, "Real ↔ Synth");
                b.macro(1, kRoom, "Wet", 0.0f, 0.5f, "Room");
                b.macro(2, kP2M, "vel", 0.0f, 1.0f, "Punch");
            });
    stacked("Cinematic Strings & Choir", "Slow-attack string ensemble with a choir an octave above, in a huge hall. Let notes swell.", "pick",
            {-4.0f, {{kInst, 1}}}, [=](Builder& b) {
                b.add(kP2M, poly);
                b.split(0.5f);
                b.lane('A', kInst, {{"sound", (float)sound("GM · String Ensemble 1")}, {"tone", 0.55f}, {"level", 0.75f}});
                b.lane('B', kMidiFx, {{"octave", 1.0f}});
                b.lane('B', kInst, {{"sound", (float)sound("GM · Choir Aahs")}, {"tone", 0.5f}, {"level", 0.6f}});
                b.add(kIrVerb, {{"IR", 2.0f}, {"W0", 0.38f}, {"D7", 1.0f}});
                b.macroMix(0, 0.1f, 0.9f, "Strings ↔ Choir");
                b.macro(1, kIrVerb, "W0", 0.0f, 0.8f, "Hall");
                b.macro(2, kP2M, "sens", 0.2f, 0.9f, "Sensitivity");
            });
    stacked("Full Orchestra", "Piano, strings and a French horn section an octave down. Three instruments from one guitar.", "pick",
            {-4.0f, {{kInst, 2}}}, [=](Builder& b) {
                b.add(kP2M, poly);
                b.split(0.5f);
                b.lane('A', kInst, {{"sound", (float)sound("GM · Acoustic Grand Piano")}, {"level", 0.7f}});
                b.lane('A', kInst, {{"sound", (float)sound("GM · String Ensemble 2")}, {"level", 0.7f}, {"mix", 0.45f}});
                b.lane('B', kMidiFx, {{"octave", -1.0f}});
                b.lane('B', kInst, {{"sound", (float)sound("GM · French Horn")}, {"tone", 0.55f}, {"level", 0.6f}});
                b.add(kIrVerb, {{"IR", 8.0f}, {"W0", 0.3f}, {"D7", 1.0f}});
                b.macroMix(0, 0.1f, 0.9f, "Keys ↔ Horns");
                b.macro(1, kInst, "mix", 0.0f, 0.8f, "Strings", 1);
                b.macro(2, kIrVerb, "W0", 0.0f, 0.7f, "Hall");
            });
    midiOnly("Analog Poly Pad", "A wide, detuned analog pad that follows your picking dynamics, into a shimmering reverb.", "pick",
             [=](Builder& b) {
                 b.add(kP2M, poly);
                 auto ps = polySynthPresetParams("Analog Pad");
                 b.add(kPolySynth, ps);
                 b.add(kShimmer, {{"R", 0.3f}});
                 b.macro(0, kPolySynth, "cutoff", 0.2f, 0.85f, "Cutoff");
                 b.macro(1, kPolySynth, "amp_a", 0.0f, 0.7f, "Swell");
                 b.macro(2, kPolySynth, "follow", 0.0f, 1.0f, "Pick Follow");
                 b.macro(3, kShimmer, "R", 0.0f, 0.7f, "Shimmer");
             });
    midiOnly("Synth Lead Hero", "Fat saw lead with glide that tracks your bends. Mono tracking — play single-note lines.", "lead",
             [=](Builder& b) {
                 b.add(kP2M, monoBend12);
                 b.add(kInst, {{"sound", (float)sound("Synth · Saw Lead")}, {"tone", 0.6f}, {"vibrato", 0.15f}, {"level", 0.75f}});
                 b.add(kPingPong, {{"m", 0.22f}});
                 b.add(kHall480, {{"D13", 0.18f}});
                 b.macro(0, kInst, "tone", 0.2f, 1.0f, "Filter");
                 b.macro(1, kInst, "vibrato", 0.0f, 0.6f, "Vibrato");
                 b.macro(2, kPingPong, "m", 0.0f, 0.5f, "Delay");
                 b.macro(3, kHall480, "D13", 0.0f, 0.5f, "Hall");
             });
    midiOnly("Arp Machine", "Hold a chord and it becomes a 16th-note arpeggio of glassy plucks, bouncing across a ping-pong delay.", "strum",
             [=](Builder& b) {
                 b.add(kP2M, poly);
                 b.add(kMidiFx, {{"arp", 3.0f}, {"rate", 2.0f}, {"bpm", 112.0f}, {"hold", 1.0f}});
                 b.add(kInst, {{"sound", (float)sound("Synth · Glass Pluck")}, {"tone", 0.6f}, {"level", 0.72f}});
                 b.add(kPingPong, {{"m", 0.3f}});
                 b.add(kHall480, {{"D13", 0.16f}});
                 b.macro(0, kMidiFx, "arp", 0.0f, 1.0f, "Arp Mode");
                 b.macro(1, kMidiFx, "bpm", 0.27f, 0.53f, "Tempo");
                 b.macro(2, kInst, "tone", 0.2f, 1.0f, "Brightness");
                 b.macro(3, kPingPong, "m", 0.0f, 0.6f, "Delay");
             });
    midiOnly("Late Night Tenor", "Breathy tenor sax in a warm plate. Mono — phrase it like a horn player, slide into notes.", "lead",
             [=](Builder& b) {
                 b.add(kP2M, mono);
                 b.add(kInst, {{"sound", (float)sound("GM · Tenor Sax")}, {"tone", 0.6f}, {"vibrato", 0.2f}, {"level", 0.78f}});
                 b.add(kPlateSpring, {{"S1", 1.0f}, {"M2", 0.18f}});
                 b.macro(0, kInst, "vibrato", 0.0f, 0.7f, "Vibrato");
                 b.macro(1, kInst, "tone", 0.2f, 1.0f, "Tone");
                 b.macro(2, kPlateSpring, "M2", 0.0f, 0.5f, "Plate");
             });
    midiOnly("Silver Flute", "Concert flute with soft vibrato in a cathedral-sized space. Mono — play melodies.", "lead",
             [=](Builder& b) {
                 b.add(kP2M, mono);
                 b.add(kInst, {{"sound", (float)sound("GM · Flute")}, {"tone", 0.55f}, {"vibrato", 0.25f}, {"level", 0.78f}});
                 b.add(kIrVerb, {{"IR", 2.0f}, {"W0", 0.3f}, {"D7", 1.0f}});
                 b.macro(0, kInst, "vibrato", 0.0f, 0.7f, "Vibrato");
                 b.macro(1, kIrVerb, "W0", 0.0f, 0.7f, "Space");
             });
    midiOnly("Island Steel Pan", "Steel drums with a short room. Arpeggiated chords and bright melodies.", "pick",
             [=](Builder& b) {
                 b.add(kP2M, poly);
                 b.add(kInst, {{"sound", (float)sound("GM · Steel Drums")}, {"tone", 0.65f}, {"level", 0.75f}});
                 b.add(kRoom, {{"Wet", 0.2f}});
                 b.macro(0, kInst, "tone", 0.2f, 1.0f, "Brightness");
                 b.macro(1, kRoom, "Wet", 0.0f, 0.5f, "Room");
             });
    midiOnly("Mallet Dreams", "Vibraphone and celesta through a slow stereo chorus — lullaby textures from fingerpicked patterns.", "pick",
             [=](Builder& b) {
                 b.add(kP2M, poly);
                 b.add(kInst, {{"sound", (float)sound("GM · Vibraphone")}, {"level", 0.75f}});
                 b.add(kInst, {{"sound", (float)sound("Keys · Celesta")}, {"level", 0.6f}, {"mix", 0.35f}});
                 b.add(kDimension, {{"mix", 0.6f}});
                 b.add(kIrVerb, {{"IR", 8.0f}, {"W0", 0.3f}, {"D7", 1.0f}});
                 b.macro(0, kInst, "mix", 0.0f, 0.8f, "Celesta", 1);
                 b.macro(1, kIrVerb, "W0", 0.0f, 0.7f, "Space");
             });

    // ═════════════ GUITAR + MIDI: the rig and instruments blended ═════════════
    auto guitarLaneA = [](Builder& b, int32_t amp, const Params& ap, int32_t pedal = 0, const Params& pp = {}) {
        if (pedal) b.lane('A', pedal, pp);
        b.lane('A', amp, ap);
    };
    stacked("Crunch & Analog Pad", "Plexi crunch with a TS in front, and a warm analog pad underneath following your chords. Blend with Guitar ↔ Pad.", "strum",
            {-5.0f, {{kInst, 0}}}, [=](Builder& b) {
                b.split(0.5f);
                guitarLaneA(b, kPlexi, {{"V1", 0.62f}}, kTS808, {{"Dr", 0.35f}, {"Vol", 0.6f}});
                b.lane('B', kP2M, poly);
                b.lane('B', kInst, {{"sound", (float)sound("Synth · Analog Pad")}, {"attack", 0.65f}, {"release", 0.65f}, {"follow", 0.6f}, {"level", 0.6f}});
                b.add(kHall480, {{"D13", 0.14f}});
                b.macroMix(0, 0.15f, 0.85f, "Guitar ↔ Pad");
                b.macro(1, kPlexi, "V1", 0.3f, 0.9f, "Drive");
                b.macro(2, kInst, "attack", 0.2f, 0.95f, "Pad Swell");
                b.macro(3, kHall480, "D13", 0.0f, 0.4f, "Hall");
            });
    stacked("Sub Drop Metal", "Modern high-gain riffing with a sub-octave synth bass glued to your low strings. Huge palm mutes.", "metal",
            {-6.0f, {{kInst, 0}}}, [=](Builder& b) {
                b.split(0.5f);
                guitarLaneA(b, k5150, {{"L13", 1.0f}, {"L1", 0.62f}}, kTS808, {{"Dr", 0.0f}, {"Vol", 0.75f}, {"Tn", 0.6f}});
                b.lane('B', kP2M, {{"mode", 0.0f}, {"sens", 0.65f}, {"low", 28.0f}, {"high", 50.0f}, {"vel", 0.3f}});
                b.lane('B', kInst, {{"sound", (float)sound("Synth · Sub Bass")}, {"tone", 0.35f}, {"level", 0.6f}});
                b.macroMix(0, 0.1f, 0.75f, "Sub");
                b.macro(1, k5150, "L1", 0.35f, 0.9f, "Gain");
                b.macro(2, kInst, "tone", 0.1f, 0.8f, "Sub Tone");
            });
    stacked("Chime & Strings", "Top-boost chime with a slow string section blooming behind each chord, in a shared hall.", "strum",
            {-5.0f, {{kInst, 0}}}, [=](Builder& b) {
                b.split(0.5f);
                guitarLaneA(b, kTopBoost, {{"Br", 0.55f}});
                b.lane('B', kP2M, poly);
                b.lane('B', kInst, {{"sound", (float)sound("GM · String Ensemble 1")}, {"level", 0.6f}});
                b.add(kIrVerb, {{"IR", 8.0f}, {"W0", 0.22f}, {"D7", 1.0f}});
                b.macroMix(0, 0.15f, 0.85f, "Guitar ↔ Strings");
                b.macro(1, kTopBoost, "Br", 0.3f, 0.9f, "Drive");
                b.macro(2, kIrVerb, "W0", 0.0f, 0.6f, "Hall");
            });
    stacked("Lead & Brass Harmony", "A singing JCM lead doubled by a brass section a diatonic third up (E minor). Poly: melodies get a harmony line, strummed chords a brass stab with the third stacked on every note; bends stay in the guitar.", "lead",
            {-6.0f, {{kInst, 0}}}, [=](Builder& b) {
                b.split(0.5f);
                guitarLaneA(b, kJcm800, {{"V1", 0.7f}, {"Hg", 1.0f}});
                b.lane('B', kP2M, polyLead);
                b.lane('B', kMidiFx, {{"harmony", 4.0f}, {"scale", 2.0f}, {"root", 4.0f}});
                b.lane('B', kInst, {{"sound", (float)sound("GM · Brass Section")}, {"tone", 0.7f}, {"level", 0.6f}});
                b.add(kTempoDelay, {{"D3", 0.2f}, {"F1", 0.35f}});
                b.add(kHall480, {{"D13", 0.12f}});
                b.macroMix(0, 0.1f, 0.85f, "Guitar ↔ Brass");
                b.macro(1, kMidiFx, "root", 0.0f, 1.0f, "Key");
                b.macro(2, kJcm800, "V1", 0.4f, 1.0f, "Gain");
                b.macro(3, kTempoDelay, "D3", 0.0f, 0.5f, "Delay");
            });
    stacked("Shoegaze Organ Wall", "Fuzz into a cranked blackface, smeared by a drawbar organ and an endless cosmic reverb.", "strum",
            {-5.0f, {{kInst, 0}}}, [=](Builder& b) {
                b.split(0.5f);
                guitarLaneA(b, kBlackface, {{"Vol", 0.6f}}, kMuff, {{"S", 0.7f}, {"V", 0.45f}});
                b.lane('A', kFlanger, {{"Int", 0.35f}});
                b.lane('B', kP2M, poly);
                b.lane('B', kInst, {{"sound", (float)sound("Organ · Full Drawbars")}, {"level", 0.6f}});
                b.lane('B', kRotor, {});
                b.add(kCosmic, {{"mix", 0.42f}, {"revTime", 0.8f}});
                b.macroMix(0, 0.15f, 0.85f, "Guitar ↔ Organ");
                b.macro(1, kMuff, "S", 0.3f, 1.0f, "Fuzz");
                b.macro(2, kCosmic, "mix", 0.0f, 0.8f, "Wash");
                b.macro(3, kCosmic, "freeze", 0.0f, 1.0f, "Freeze");
            });
    stacked("Stadium Synth Double", "German high-gain lead with a saw-lead synth doubling every note and chord. Poly: solos and power chords both play; the synth steps by semitone under bends.", "lead",
            {-7.0f, {{kInst, 0}}}, [=](Builder& b) {
                b.split(0.5f);
                guitarLaneA(b, kGerman, {{"g", 0.65f}, {"c", 1.0f}});
                b.lane('B', kP2M, polyLead);
                b.lane('B', kInst, {{"sound", (float)sound("Synth · Saw Lead")}, {"level", 0.6f}});
                b.add(kPingPong, {{"m", 0.2f}});
                b.add(kHall480, {{"D13", 0.12f}});
                b.macroMix(0, 0.1f, 0.8f, "Synth Double");
                b.macro(1, kGerman, "g", 0.3f, 0.95f, "Gain");
                b.macro(2, kPingPong, "m", 0.0f, 0.5f, "Delay");
            });
    stacked("Neural Choir", "A Neural Amp Modeler capture with a choir swelling under it. Swap in your own capture on the NAM unit.", "strum",
            {-5.0f, {{kInst, 0}}}, [=](Builder& b) {
                b.split(0.5f);
                b.lane('A', kNam, {{"capture", 1.0f}});
                b.lane('B', kP2M, poly);
                b.lane('B', kInst, {{"sound", (float)sound("GM · Choir Aahs")}, {"follow", 0.9f}, {"level", 0.6f}}); // rides the capture's dynamics
                b.add(kIrVerb, {{"IR", 2.0f}, {"W0", 0.25f}, {"D7", 1.0f}});
                b.macroMix(0, 0.1f, 0.85f, "Guitar ↔ Choir");
                b.macro(1, kNam, "input", 0.3f, 0.75f, "Drive");
                b.macro(2, kIrVerb, "W0", 0.0f, 0.6f, "Hall");
            });
    stacked("Clean & Grand", "Sparkling blackface clean doubled by a grand piano — the classic ballad layer.", "pick",
            {-4.0f, {{kInst, 0}}}, [=](Builder& b) {
                b.split(0.5f);
                // Dyna Comp Output set the way players set it: unity, then a touch more (+0.7 dB on this DI). Its 50 kΩ
                // audio-taper Output pot puts unity near 2 o'clock; the old stored 0.265 sat ~29 dB down that taper and
                // left the guitar ~21 dB under bypass (the unit gives ~+10 dB of make-up at full Output).
                guitarLaneA(b, kBlackface, {{"Vol", 0.38f}}, kComp, {{"V", 0.75f}, {"S", 0.55f}});
                b.lane('B', kP2M, poly);
                b.lane('B', kInst, {{"sound", (float)sound("GM · Acoustic Grand Piano")}, {"level", 0.6f}});
                b.add(kPlateSpring, {{"S1", 1.0f}, {"M2", 0.14f}});
                b.macroMix(0, 0.1f, 0.85f, "Guitar ↔ Piano");
                b.macro(1, kPlateSpring, "M2", 0.0f, 0.45f, "Plate");
            });
    stacked("Funk Clav", "Snappy clean funk guitar with an envelope filter, doubled by a clavinet. Sixteenth-note chops.", "strum",
            {-3.0f, {{kInst, 0}}}, [=](Builder& b) {
                b.split(0.5f);
                guitarLaneA(b, kBlackface, {{"Vol", 0.42f}}, kComp, {{"V", 0.75f}, {"S", 0.55f}}); // +1.1 dB over bypass: see Clean & Grand
                b.lane('A', kEnvFilter, {});
                b.lane('B', kP2M, {{"mode", 0.0f}, {"sens", 0.5f}, {"vel", 0.7f}});
                b.lane('B', kInst, {{"sound", (float)sound("GM · Clavinet")}, {"tone", 0.7f}, {"level", 0.6f}});
                b.lane('B', kPhaser, {{"DW", 0.35f}});
                b.add(kRoom, {{"Wet", 0.12f}});
                b.macroMix(0, 0.1f, 0.85f, "Guitar ↔ Clav");
                b.macro(1, kInst, "tone", 0.3f, 1.0f, "Clav Bite");
                b.macro(2, kPhaser, "Pwr", 0.0f, 1.0f, "Phaser"); // the Small Stone has no mix: the macro switches it in and out
            });
    stacked("Twang & Tines", "Tweed edge-of-breakup with a tine electric piano tucked underneath. Country-soul comping.", "strum",
            {-5.0f, {{kInst, 0}}}, [=](Builder& b) {
                b.split(0.5f);
                guitarLaneA(b, kTweed, {{"VB", 0.55f}, {"VN", 0.45f}});
                b.lane('B', kP2M, poly);
                b.lane('B', kInst, {{"sound", (float)sound("Keys · Electric Piano")}, {"level", 0.6f}});
                b.lane('B', kChorus, {{"Mx", 0.35f}});
                b.add(kPlateSpring, {{"S1", 4.0f}, {"M2", 0.16f}});
                b.macroMix(0, 0.1f, 0.85f, "Guitar ↔ Keys");
                b.macro(1, kTweed, "VB", 0.3f, 0.9f, "Drive");
                b.macro(2, kPlateSpring, "M2", 0.0f, 0.45f, "Spring");
            });
    stacked("Octave Fuzz Synth", "Fuzz Face into a Plexi, with a square-wave lead an octave up riding along. Poly: fuzz solos and chords both play; the synth steps by semitone under bends.", "lead",
            {-7.0f, {{kInst, 0}}}, [=](Builder& b) {
                b.split(0.5f);
                guitarLaneA(b, kPlexi, {{"V1", 0.55f}}, kFuzzFace, {});
                b.lane('B', kP2M, polyLead);
                b.lane('B', kMidiFx, {{"octave", 1.0f}});
                b.lane('B', kInst, {{"sound", (float)sound("Synth · Square Lead")}, {"level", 0.6f}});
                b.add(kTempoDelay, {{"D3", 0.18f}});
                b.macroMix(0, 0.1f, 0.8f, "Octave Synth");
                b.macro(1, kTempoDelay, "D3", 0.0f, 0.5f, "Delay");
            });
    stacked("Ambient Bell Swells", "Chorused clean guitar with glass bells and a shimmer reverb that blooms into a pad.", "pick",
            {-5.0f, {{kInst, 0}}}, [=](Builder& b) {
                b.split(0.5f);
                guitarLaneA(b, kBlackface, {{"Vol", 0.35f}});
                b.lane('A', kChorus, {{"Mx", 0.35f}});
                b.lane('B', kP2M, poly);
                b.lane('B', kInst, {{"sound", (float)sound("Keys · Glass Bells")}, {"level", 0.6f}});
                b.add(kDiffDelay, {{"mix", 0.25f}});
                b.add(kShimmer, {{"R", 0.35f}});
                b.macroMix(0, 0.1f, 0.85f, "Guitar ↔ Bells");
                b.macro(1, kDiffDelay, "mix", 0.0f, 0.6f, "Delay");
                b.macro(2, kShimmer, "R", 0.0f, 0.8f, "Shimmer");
            });
    stacked("Metal Orchestra", "Tight Recto rhythm with strings and choir following the chord shapes. Power chords become epic.", "metal",
            {-5.0f, {{kInst, 0}, {kInst, 1}}}, [=](Builder& b) {
                b.split(0.5f);
                guitarLaneA(b, kRecto, {{"M", 3.0f}, {"Gn", 0.6f}}, kTS808, {{"Dr", 0.0f}, {"Vol", 0.75f}, {"Tn", 0.6f}});
                b.lane('B', kP2M, {{"mode", 0.0f}, {"sens", 0.6f}, {"low", 28.0f}});
                b.lane('B', kInst, {{"sound", (float)sound("GM · String Ensemble 1")}, {"level", 0.65f}});
                b.lane('B', kInst, {{"sound", (float)sound("GM · Choir Aahs")}, {"level", 0.6f}, {"mix", 0.4f}});
                b.lane('B', kIrVerb, {{"IR", 2.0f}, {"W0", 0.35f}, {"D7", 1.0f}});
                b.macroMix(0, 0.1f, 0.8f, "Orchestra");
                b.macro(1, kRecto, "Gn", 0.35f, 0.85f, "Gain");
                b.macro(2, kInst, "mix", 0.0f, 0.8f, "Choir", 1);
            });
    stacked("Sitar Drone", "Chimey top-boost clean with an electric sitar doubling the melody. Raga-rock lines; poly, so picked chords ring on the sitar too.", "pick",
            {-4.0f, {{kInst, 0}}}, [=](Builder& b) {
                b.split(0.5f);
                guitarLaneA(b, kTopBoost, {{"Br", 0.4f}});
                b.lane('B', kP2M, polyLead);
                b.lane('B', kInst, {{"sound", (float)sound("GM · Sitar")}, {"level", 0.6f}});
                b.add(kTempoDelay, {{"D3", 0.16f}});
                b.add(kPlateSpring, {{"S1", 3.0f}, {"M2", 0.14f}});
                b.macroMix(0, 0.1f, 0.85f, "Guitar ↔ Sitar");
                b.macro(1, kTempoDelay, "D3", 0.0f, 0.5f, "Delay");
            });
    return v;
}

} // namespace

std::vector<ShowcaseRig> buildShowcase(const ComponentPresetBank* bank, const std::string& di_dir, std::ostream* log) {
    std::map<std::string, audio::WavData> dis;
    const std::map<std::string, std::string> files = {{"strum", "Clean Strumming.wav"},
                                                      {"pick", "Dream Acoustic.wav"},
                                                      {"lead", "Lead DI.wav"},
                                                      {"metal", "Metal DI.wav"},
                                                      {"riff", "Rock DI.wav"}};
    for (const auto& [k, f] : files) {
        audio::WavData w;
        if (!audio::readWavFile(di_dir + "/" + f, w, nullptr) || w.left.empty()) continue;
        // Balance at the level the app plays guitar (RealtimeAudioServer::decodeTrack): mono, peak 0.35 FS. The files
        // are mastered ~8 dB hotter, and Guitar Follow and the amps respond to level, so lanes balanced on the raw
        // files came out synth-heavy in the app.
        float peak = 0.0f;
        for (std::size_t i = 0; i < w.left.size(); ++i) {
            const float m = w.right.empty() ? w.left[i] : 0.5f * (w.left[i] + w.right[i]);
            w.left[i] = m;
            peak = std::max(peak, std::abs(m));
        }
        const float g = peak > 1e-4f ? 0.35f / peak : 1.0f;
        for (auto& s : w.left) s *= g;
        w.right = w.left;
        dis[k] = std::move(w);
    }

    std::vector<ShowcaseRig> out;
    int number = 1;
    for (const auto& s : specs()) {
        Builder b(bank);
        s.build(b);
        ShowcaseRig rig;
        rig.preset = b.g.toPreset(s.name, "Headroom");
        rig.preset.info.comment = s.comment;
        auto& p = rig.preset;
        const auto di = dis.find(s.di);
        if (s.balance && di != dis.end()) {
            // Trim lane B's instrument levels until B sits `b_minus_a_db` relative to lane A.
            for (int iter = 0; iter < 3; ++iter) {
                const float a = renderLoudness(p, di->second, 0.0f);
                const float bb = renderLoudness(p, di->second, 1.0f);
                const float err = s.bal.b_minus_a_db - (bb - a);
                if (log) *log << "  " << s.name << " iter " << iter << ": A " << a << " dB, B " << bb << " dB, B-A " << (bb - a) << " dB\n";
                if (bb < -80.0f || std::fabs(err) < 0.75f) break;
                for (const auto& [tid, nth] : s.bal.level_nodes) {
                    if (auto* n = findNode(p, tid, nth)) {
                        // Instrument output ∝ level²; Poly Synth roughly ∝ level.
                        const float k = std::pow(10.0f, err / 20.0f);
                        const float lv = getP(*n, "level", 0.7f) * (tid == kInst ? std::sqrt(k) : k);
                        setP(*n, "level", std::clamp(lv, 0.05f, 1.0f));
                    }
                }
            }
        }
        if (s.balance && di != dis.end()) {
            // Levels maxed out: make up the rest with the split crossfade (gain B/A = x / (1 - x)).
            const float a = renderLoudness(p, di->second, 0.0f), bb = renderLoudness(p, di->second, 1.0f);
            const float err = s.bal.b_minus_a_db - (bb - a);
            if (bb > -80.0f && std::fabs(err) >= 0.75f)
                if (auto* m = findNode(p, 9000, 0)) {
                    const float r = std::pow(10.0f, err / 20.0f);
                    const float x = std::clamp(r / (1.0f + r), 0.3f, 0.7f);
                    setP(*m, "XFade", x);
                    if (log) *log << "  " << s.name << ": crossfade " << x << " for the remaining " << err << " dB\n";
                }
        }
        if (di != dis.end() && log) *log << "  " << s.name << ": full mix " << renderLoudness(p, di->second, -1.0f) << " dB\n";
        char num[8];
        std::snprintf(num, sizeof(num), "%02d ", number++);
        rig.file = std::string(num) + s.name + ".rig";
        out.push_back(std::move(rig));
    }
    return out;
}

} // namespace rig::library
