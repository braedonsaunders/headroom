// Bloom (900006) on synthetic guitar: harmony, key, groove, bend PLLs, string capture, triggers and gestures.
#undef NDEBUG
#include "../src/midi/Bloom.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace rig::midi;
namespace bd = rig::midi::bloom_detail;

static constexpr double kSr = 48000.0;
static constexpr uint32_t kBlock = 128;
static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("  FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

// ── a plucked string: harmonics 1/h shaped by the pluck position, high harmonics decay faster ──
struct Pluck {
    double hz, t0, amp, bend_from = 1e9, bend_to = 1e9, bend_cents = 0;
    double stop_at = 1e9;  // fretting-hand mute
    double ph[40] = {0};
    float sample(double t) {
        if (t < t0 || t >= stop_at) return 0.0f;
        const double age = t - t0;
        double cents = 0;
        if (t > bend_from) cents = bend_cents * std::min(1.0, (t - bend_from) / std::max(1e-3, bend_to - bend_from));
        const double f = hz * std::pow(2.0, cents / 1200.0);
        double s = 0;
        for (int h = 1; h <= 30 && h * f < 20000; ++h) {
            ph[h] += 2 * M_PI * h * f * 1.0002 / kSr;  // a trace of stiffness
            const double a = std::sin(M_PI * h * 0.2) / h * std::exp(-age * (0.8 + 0.15 * h));
            s += a * std::sin(ph[h]);
        }
        const double att = std::min(1.0, age / 0.002);
        return static_cast<float>(amp * att * s);
    }
};

struct Sim {
    Bloom bl;
    std::vector<Pluck> strings;
    std::vector<MidiEvent> pending;      // tracker events with absolute sample times in .offset (as int64 via vector index)
    std::vector<int64_t> pendingAt;
    std::vector<MidiEvent> outAll;       // arpeggio MIDI with absolute times
    std::vector<int64_t> outAt;
    std::vector<float> strOut, dryOut, input;
    double bpm = 120.0;
    int64_t t = 0;
    std::function<float(double)> extra;  // extra input (e.g. silence override)

    Sim(const BloomSettings& s) { bl.prepare(kSr); bl.setSettings(s); }
    void note(double at, int n, bool on, float vel = 0.8f) {
        pending.push_back({on ? MidiEvent::NoteOn : MidiEvent::NoteOff, n, vel, 0.0f, 0});
        pendingAt.push_back(static_cast<int64_t>(at * kSr));
    }
    void strum(double at, std::initializer_list<int> notes, double amp = 0.09, double spread = 0.008, double stop = 1e9) {
        // re-strumming: the strings already ringing stop (and the tracker ends their notes)
        // (each one when the pick reaches its string, low to high)
        int j = 0;
        for (auto& p : strings)
            if (p.t0 < at - 0.001 && p.stop_at > at) {
                p.stop_at = at + spread * j++;
                note(p.stop_at + 0.01, static_cast<int>(std::lround(69 + 12 * std::log2(p.hz / 440.0))), false);
            }
        int k = 0;
        for (int n : notes) {
            Pluck p{bd::midiHz(n), at + k * spread, amp};
            p.stop_at = stop;
            strings.push_back(p);
            note(at + k * spread + 0.012, n, true);  // the tracker reports ~12 ms after the attack
            if (stop < 1e9) note(stop + 0.03, n, false);
            ++k;
        }
    }
    void run(double seconds, const std::function<void(Sim&)>& perBlock = nullptr) {
        const int64_t end = t + static_cast<int64_t>(seconds * kSr);
        std::vector<MidiEvent> in, out;
        in.reserve(64); out.reserve(256);
        float mono[kBlock], str[kBlock], dry[kBlock];
        while (t < end) {
            in.clear(); out.clear();
            for (std::size_t i = 0; i < pending.size();) {
                if (pendingAt[i] < t + kBlock) {
                    MidiEvent e = pending[i];
                    e.offset = static_cast<uint32_t>(std::max<int64_t>(0, pendingAt[i] - t));
                    in.push_back(e);
                    pending.erase(pending.begin() + static_cast<long>(i));
                    pendingAt.erase(pendingAt.begin() + static_cast<long>(i));
                } else ++i;
            }
            std::sort(in.begin(), in.end(), [](const MidiEvent& a, const MidiEvent& b) { return a.offset < b.offset; });
            for (uint32_t i = 0; i < kBlock; ++i) {
                const double ts = static_cast<double>(t + i) / kSr;
                float x = 0;
                for (auto& s : strings) x += s.sample(ts);
                if (extra) x = extra(ts) < 0 ? 0.0f : x;
                mono[i] = x;
                input.push_back(x);
            }
            bl.process(in, mono, kBlock, bpm, 0.0, false, out, str, dry);
            for (const auto& e : out) { outAll.push_back(e); outAt.push_back(t + e.offset); }
            for (uint32_t i = 0; i < kBlock; ++i) { strOut.push_back(str[i]); dryOut.push_back(dry[i]); }
            t += kBlock;
            if (perBlock) perBlock(*this);
        }
    }
    std::vector<std::pair<double, int>> noteOns(double from = 0, double to = 1e9) const {
        std::vector<std::pair<double, int>> v;
        for (std::size_t i = 0; i < outAll.size(); ++i) {
            const double ts = static_cast<double>(outAt[i]) / kSr;
            if (outAll[i].type == MidiEvent::NoteOn && ts >= from && ts < to) v.emplace_back(ts, outAll[i].note);
        }
        return v;
    }
};

static void testHarmony() {
    std::printf("harmony\n");
    auto name = [](std::initializer_list<int> n, int tonic, int scale, int ext) {
        std::vector<int> v(n);
        return std::string(bd::buildChord(v.data(), static_cast<int>(v.size()), tonic, scale, ext).name);
    };
    CHECK(name({40, 47, 52}, 0, 0, 1) == "Em", "E5 in C major → %s", name({40, 47, 52}, 0, 0, 1).c_str());
    CHECK(name({45, 52}, 4, 1, 1) == "Am", "A5 in E minor → %s", name({45, 52}, 4, 1, 1).c_str());
    CHECK(name({43}, 0, 0, 2) == "G7", "G in C major, 7th → %s", name({43}, 0, 0, 2).c_str());
    CHECK(name({48, 52, 55}, 0, 0, 3) == "Cmaj7(9)", "C in C, 9th → %s", name({48, 52, 55}, 0, 0, 3).c_str());
    CHECK(name({40, 47, 52, 56, 59, 64}, 9, 1, 2) == "E7", "E major in A minor, 7th → %s", name({40, 47, 52, 56, 59, 64}, 9, 1, 2).c_str());
    CHECK(name({40, 47}, -1, 0, 1) == "E5", "E5, no key → %s", name({40, 47}, -1, 0, 1).c_str());
    CHECK(name({45, 52, 57, 60}, 0, 0, 4) == "Am7(9)(11)", "Am in C, 11th → %s", name({45, 52, 57, 60}, 0, 0, 4).c_str());
    CHECK(name({41, 48, 53, 57}, 0, 0, 4) == "Fmaj7(9)(13)", "F in C, 11th → 13th on major: %s", name({41, 48, 53, 57}, 0, 0, 4).c_str());
    CHECK(name({40, 43, 47}, 0, 0, 0) == "Em", "as played → %s", name({40, 43, 47}, 0, 0, 0).c_str());
}

static void testKey() {
    std::printf("key\n");
    bd::KeyTracker k;
    k.reset();
    // C – G – Am – F, one bar each at 100 bpm, 24 s
    const int prog[4][4] = {{48, 52, 55, 60}, {43, 47, 50, 55}, {45, 48, 52, 57}, {41, 45, 48, 53}};
    std::array<uint8_t, 128> on{};
    for (double t = 0; t < 24.0; t += kBlock / kSr) {
        on.fill(0);
        const int c = static_cast<int>(t / 2.4) % 4;
        for (int n : prog[c]) on[static_cast<std::size_t>(n)] = 1;
        k.update(on, kBlock / kSr);
    }
    CHECK(k.key() == 0, "C–G–Am–F → key %d (want 0, C major)", k.key());
    // A minor: Am – Dm – E – Am
    k.reset();
    const int prog2[4][3] = {{45, 48, 52}, {50, 53, 57}, {40, 44, 47}, {45, 48, 52}};
    for (double t = 0; t < 24.0; t += kBlock / kSr) {
        on.fill(0);
        const int c = static_cast<int>(t / 2.4) % 4;
        for (int n : prog2[c]) on[static_cast<std::size_t>(n)] = 1;
        k.update(on, kBlock / kSr);
    }
    CHECK(k.key() == 21, "Am–Dm–E–Am → key %d (want 21, A minor)", k.key());
}

static void testGroove() {
    std::printf("groove\n");
    for (double bpm : {84.0, 100.0, 132.0}) {
        bd::Groove g; g.reset();
        const double beat = 60.0 / bpm;
        for (int k = 0; k < 24; ++k) g.onset(1.0 + k * beat / 2, k % 2 ? -26.0f : -20.0f, 120.0);
        CHECK(g.locked(), "8ths at %.0f bpm: not locked", bpm);
        const double got = 60.0 / g.period();
        CHECK(std::fabs(got / bpm - 1) < 0.01, "8ths at %.0f bpm → %.2f bpm", bpm, got);
        // phase: a beat lands on the strong onsets (k even)
        const double b = g.beatAt(1.0 + 10 * beat / 2);
        CHECK(std::fabs(b - std::round(b)) < 0.03, "beat phase at %.0f bpm: %.3f", bpm, b - std::round(b));
    }
    for (double ratio : {0.6, 0.667}) {   // swung 8ths at 100 bpm
        bd::Groove g; g.reset();
        const double beat = 0.6;
        for (int k = 0; k < 12; ++k) { g.onset(1.0 + k * beat, -20.0f, 120.0); g.onset(1.0 + k * beat + beat * ratio, -26.0f, 120.0); }
        CHECK(g.locked() && std::fabs(60.0 / g.period() - 100.0) < 1.5, "swung (%.3f) 100 bpm → %.2f", ratio, 60.0 / g.period());
        CHECK(std::fabs(g.swing() - ratio) < 0.03f, "swing %.3f → %.3f", ratio, g.swing());
        const double b = g.beatAt(1.0 + 7 * beat);
        CHECK(std::fabs(b - std::round(b)) < 0.03, "swung beat phase %.3f", b - std::round(b));
    }
    {   // 16ths at 100 with accents on the beat
        bd::Groove g; g.reset();
        for (int k = 0; k < 48; ++k) g.onset(1.0 + k * 0.15, k % 4 == 0 ? -18.0f : -27.0f, 120.0);
        CHECK(g.locked() && std::fabs(60.0 / g.period() - 100.0) < 1.0, "16ths at 100 → %.2f", 60.0 / g.period());
    }
}

static void testPll() {
    std::printf("bend PLLs\n");
    // A2 bent up 200 cents between 0.4 and 0.7 s, inside an E2 / D3 chord
    Pluck a{bd::midiHz(45), 0.0, 0.1}; a.bend_from = 0.4; a.bend_to = 0.7; a.bend_cents = 200;
    Pluck e{bd::midiHz(40), 0.0, 0.1}, d{bd::midiHz(50), 0.0, 0.1};
    bd::NotePll p; p.start(45, kSr, 0);
    float at35 = 0, at90 = 0, mid = 0;
    for (int64_t i = 0; i < static_cast<int64_t>(0.9 * kSr); ++i) {
        const double t = static_cast<double>(i) / kSr;
        p.tick(a.sample(t) + e.sample(t) + d.sample(t), i);
        if (i == static_cast<int64_t>(0.35 * kSr)) at35 = p.bendCents();
        if (i == static_cast<int64_t>(0.89 * kSr)) at90 = p.bendCents();
        if (i == static_cast<int64_t>(0.65 * kSr)) mid = p.bendCents();
    }
    CHECK(p.locked, "PLL never locked");
    CHECK(std::fabs(at35) < 4, "before the bend: %.1f cents", at35);
    std::printf("  A2 in E2/D3: %.1f cents mid-bend (true %.0f, ~95 ms behind), %.1f after (true 200)\n", mid, 200 * (0.65 - 0.4 - 0.095) / 0.3, at90);
    CHECK(std::fabs(at90 - 200) < 8, "after a 200-cent bend: %.1f cents", at90);
}

static double harmonicCorr(const float* tab, const Pluck& ref, int H, bool oddOnly = false, int skip = 0) {
    // magnitude spectrum of the table vs the reference string's harmonic amplitudes (at capture age ~0.12 s)
    double xy = 0, xx = 0, yy = 0;
    for (int h = 1; h <= H; ++h) {
        if ((oddOnly && h % 2 == 0) || h == skip) continue;
        double re = 0, im = 0;
        for (int j = 0; j < bd::Strings::M; ++j) { re += tab[j] * std::cos(2 * M_PI * h * j / bd::Strings::M); im += tab[j] * std::sin(2 * M_PI * h * j / bd::Strings::M); }
        const double m = std::sqrt(re * re + im * im);
        const double r = std::fabs(std::sin(M_PI * h * 0.2) / h * std::exp(-0.12 * (0.8 + 0.15 * h)));
        xy += m * r; xx += m * m; yy += r * r;
    }
    (void)ref;
    return xy / std::sqrt(xx * yy);
}

static void testCapture() {
    std::printf("string capture\n");
    {   // one string alone: the whole spectrum
        BloomSettings s; s.trigger = 1; s.voice = 1;
        Sim sim(s);
        sim.strum(0.05, {45});
        sim.run(0.5);
        const float* t = sim.bl.strings().toneTable(45);
        CHECK(t != nullptr, "A2 not captured");
        if (t) { const double c = harmonicCorr(t, sim.strings[0], 12); std::printf("  lone A2 spectrum r = %.4f\n", c); CHECK(c > 0.99, "lone A2: r = %.4f", c); }
    }
    {   // inside an E major chord: B2's even harmonics are shared with E2 / E3 (a fifth), and its 5th sits 5.5 Hz from
        // G#3's 3rd (the equal-tempered major third is 14 cents wide): the rest are its own
        BloomSettings s; s.trigger = 1; s.voice = 1;
        Sim sim(s);
        sim.strum(0.05, {40, 47, 52, 56}, 0.09, 0.01);
        sim.run(0.5);
        const float* tB2 = sim.bl.strings().toneTable(47);
        CHECK(tB2 && sim.bl.strings().toneTable(40), "chord tones not captured");
        if (tB2) { const double c = harmonicCorr(tB2, sim.strings[1], 9, true, 5); std::printf("  B2 in E major, odd harmonics r = %.4f\n", c); CHECK(c > 0.97, "B2 odd harmonics r = %.4f", c); }
    }
}

static void testBloomTrigger() {
    std::printf("bloom trigger\n");
    BloomSettings s; s.trigger = 0; s.bloom_s = 0.6f; s.voice = 2; s.feel = false; s.rate = 3; s.touch = 0.0f; s.motion = 0.5f;
    Sim sim(s);
    sim.strum(0.1, {45, 52, 57, 60, 64});  // A minor
    sim.run(0.65);
    CHECK(sim.noteOns().empty(), "arpeggio before the bloom time (%zu notes)", sim.noteOns().size());
    sim.run(0.35);  // to 1.0 s
    CHECK(sim.bl.live().active, "no bloom 0.85 s after the strum (ring %.1f dB)", sim.bl.live().ring_db);
    sim.run(1.0);   // to 2.0 s
    auto ons = sim.noteOns(0.75, 2.0);
    CHECK(ons.size() >= 6, "only %zu arpeggio notes in 1.25 s", ons.size());
    for (std::size_t i = 1; i < ons.size(); ++i) {
        const double d = ons[i].first - ons[i - 1].first;
        CHECK(std::fabs(d - 0.125) < 0.003, "step %zu: %.4f s (want 0.125 at 120 bpm 1/16)", i, d);
        if (std::fabs(d - 0.125) >= 0.003) break;
    }
    for (auto& [ts, n] : ons) { const int pc = n % 12; CHECK(pc == 9 || pc == 0 || pc == 4, "note %d is not in A minor", n); }
    double se = 0; for (std::size_t i = static_cast<std::size_t>(1.0 * kSr); i < static_cast<std::size_t>(2.0 * kSr); ++i) se += sim.strOut[i] * sim.strOut[i];
    CHECK(se > 1e-3, "Strings silent while arpeggiating (energy %.2e)", se);
    CHECK(std::fabs(sim.dryOut[static_cast<std::size_t>(1.9 * kSr)] - 0.5f) < 0.02f, "dry not ducked: %.3f", sim.dryOut[static_cast<std::size_t>(1.9 * kSr)]);
    // pick again: it gets out of the way at once
    sim.strum(2.0, {43, 50, 55, 59});  // G
    sim.run(0.03);
    CHECK(!sim.bl.live().active, "still arpeggiating after a new attack");
    CHECK(sim.dryOut.back() > 0.97f, "dry not restored 30 ms after the attack: %.3f", sim.dryOut.back());
    sim.run(1.2);
    CHECK(sim.bl.live().active, "no second bloom on the G chord");
    std::printf("  chord now %s\n", sim.bl.live().chord);
    CHECK(std::string(sim.bl.live().chord).rfind("G", 0) == 0, "chord after G strum: %s", sim.bl.live().chord);
}

static void testChoke() {
    std::printf("fretting-hand stop\n");
    BloomSettings s; s.trigger = 1; s.feel = false;
    Sim sim(s);
    sim.strum(0.1, {40, 47, 52, 55, 59, 64}, 0.09, 0.008, 1.5);
    sim.run(1.4);
    CHECK(sim.bl.live().active, "Always mode not arpeggiating");
    sim.run(0.25);
    CHECK(!sim.bl.live().active, "still arpeggiating 150 ms after the strings were muted");
}

static void testPalmMuteStaccato() {
    std::printf("palm mute → staccato\n");
    BloomSettings s; s.trigger = 1; s.feel = false; s.voice = 0; s.gate = 0.8f; s.choke = 1.0f; s.touch = 0.0f;
    auto noteLen = [&](bool muted) {
        Sim sim(s);
        if (muted) {
            // chug: E5 that dies in 60 ms, the notes stay held for the arpeggio memory
            Pluck p{bd::midiHz(40), 0.1, 0.12}; p.stop_at = 0.16; sim.strings.push_back(p);
            Pluck q{bd::midiHz(47), 0.1, 0.12}; q.stop_at = 0.16; sim.strings.push_back(q);
            sim.note(0.112, 40, true); sim.note(0.112, 47, true);
        } else sim.strum(0.1, {40, 47});
        sim.run(1.2);
        double on = -1, len = 0; int cnt = 0;
        for (std::size_t i = 0; i < sim.outAll.size(); ++i) {
            const double ts = static_cast<double>(sim.outAt[i]) / kSr;
            if (ts < 0.5) continue;
            if (sim.outAll[i].type == MidiEvent::NoteOn) on = ts;
            else if (sim.outAll[i].type == MidiEvent::NoteOff && on >= 0) { len += ts - on; ++cnt; on = -1; }
        }
        return cnt ? len / cnt : -1.0;
    };
    const double open = noteLen(false), muted = noteLen(true);
    std::printf("  open %.3f s, palm-muted %.3f s\n", open, muted);
    CHECK(open > 0 && muted > 0 && muted < 0.4 * open, "palm mute did not shorten the notes (%.3f vs %.3f)", muted, open);
}

static void testFeel() {
    std::printf("feel clock\n");
    BloomSettings s; s.trigger = 0; s.feel = true; s.rate = 3; s.motion = 0.5f; s.touch = 0.0f; s.bloom_s = 0.5f;
    Sim sim(s);
    sim.bpm = 120;  // rig tempo says 120; the player strums 8ths at 96
    const double beat = 60.0 / 96.0;
    for (int k = 0; k < 16; ++k) sim.strum(0.2 + k * beat / 2, {45, 52, 57, 60, 64}, k % 2 ? 0.05 : 0.09, 0.004);
    const double last = 0.2 + 15 * beat / 2;
    sim.run(last + 1.6);
    auto ons = sim.noteOns(last + 0.4, last + 1.6);
    CHECK(ons.size() >= 5, "only %zu notes after the bloom", ons.size());
    double worst = 0;
    for (std::size_t i = 1; i < ons.size(); ++i) worst = std::max(worst, std::fabs((ons[i].first - ons[i - 1].first) - beat / 4));
    std::printf("  learned %.2f bpm, worst step error %.1f ms\n", sim.bl.live().bpm, worst * 1000);
    CHECK(std::fabs(sim.bl.live().bpm - 96.0f) < 1.0f, "learned tempo %.2f (want 96)", sim.bl.live().bpm);
    CHECK(worst < 0.004, "steps off the 96 bpm 16th grid by %.1f ms", worst * 1000);
    // on the player's grid: each arpeggio note lands on a 16th of the strummed 8ths
    double phaseErr = 0;
    for (auto& [ts, n] : ons) { const double k = (ts - 0.2) / (beat / 4); phaseErr = std::max(phaseErr, std::fabs(k - std::round(k)) * beat / 4); }
    std::printf("  worst grid offset %.1f ms (swing %.3f)\n", phaseErr * 1000, sim.bl.live().swing);
    if (phaseErr >= 0.012) for (auto& [ts, n] : ons) std::printf("    %.4f  k=%.3f\n", ts, (ts - 0.2) / (beat / 4));
    CHECK(phaseErr < 0.012, "arpeggio off the player's grid by %.1f ms", phaseErr * 1000);
}

// Keyed: the same riff played twice gives the same arpeggio, note for note and step for step, whatever the pattern,
// even played softer the second time and with tracker ghost notes thrown in.
static std::vector<std::pair<double, int>> riffPass(Sim& sim, double t0, double amp, bool ghosts) {
    const int chords[4][4] = {{45, 52, 57, 60}, {41, 48, 53, 57}, {48, 52, 55, 60}, {43, 50, 55, 59}};  // Am F C G
    for (int c = 0; c < 4; ++c) {
        const double at = t0 + c * 0.75;
        sim.strum(at, {chords[c][0], chords[c][1], chords[c][2], chords[c][3]}, amp);
        if (ghosts) { sim.note(at + 0.25, 63, true, 0.5f); sim.note(at + 0.4, 63, false); }   // a stray D#4 mid-chord
    }
    sim.run(t0 + 3.0 - static_cast<double>(sim.t) / kSr);
    std::vector<std::pair<double, int>> v;
    for (auto& [ts, n] : sim.noteOns(t0, t0 + 3.0)) v.emplace_back(ts - t0, n);
    return v;
}
static void testKeyed() {
    std::printf("keyed: same riff → same arpeggio\n");
    const char* names[] = {"Up", "Down", "Up/Down", "Converge", "Walk", "Melody", "Pedal", "Random"};
    for (int pat = 0; pat < 8; ++pat) {
        BloomSettings s; s.trigger = 1; s.feel = false; s.pattern = pat; s.key = 0; s.scale = 0; s.extend = 2; s.voice = 0;
        Sim sim(s);
        const auto a = riffPass(sim, 0.2, 0.09, false);
        const auto b = riffPass(sim, 3.4, 0.05, true);   // softer, with ghosts
        bool same = a.size() == b.size() && !a.empty();
        double worst = 0;
        for (std::size_t i = 0; same && i < a.size(); ++i) {
            if (a[i].second != b[i].second) same = false;
            worst = std::max(worst, std::fabs(a[i].first - b[i].first));
        }
        std::printf("  %-9s %zu notes, %s, worst timing %.1f ms\n", names[pat], a.size(), same ? "identical" : "DIFFERENT", worst * 1000);
        CHECK(same && worst < 0.002, "%s: second pass differs (%zu vs %zu notes)", names[pat], a.size(), b.size());
        if (!same) {
            for (std::size_t i = 0; i < std::max(a.size(), b.size()); ++i)
                std::printf("    %2zu  %s  |  %s\n", i, i < a.size() ? (std::to_string(a[i].first).substr(0, 6) + " " + std::to_string(a[i].second)).c_str() : "-",
                            i < b.size() ? (std::to_string(b[i].first).substr(0, 6) + " " + std::to_string(b[i].second)).c_str() : "-");
        }
    }
}
static void testRetrigger() {
    std::printf("retrigger\n");
    for (int rt = 0; rt < 2; ++rt) {
        BloomSettings s; s.trigger = 1; s.feel = false; s.pattern = 0; s.key = 9; s.scale = 1; s.voice = 0; s.retrig = rt;
        Sim sim(s);
        sim.strum(0.2, {45, 52, 57, 60});
        sim.strum(0.2 + 0.125 * 3 + 0.03, {45, 52, 57, 60});   // the same chord struck again mid-figure
        sim.run(1.0);
        auto ons = sim.noteOns(0.2, 1.0);
        // Up from A2: A2 C3 E3 A3 …; Pick restarts the figure at the re-strike (its first step, A2, is under the pick, so
        // C3 is the first note heard), Chord plays on
        int after = -1;
        for (auto& [ts, n] : ons) if (ts > 0.2 + 0.125 * 3 + 0.03) { after = n; break; }
        std::printf("  %s: first note after the re-strike %d\n", rt ? "Pick " : "Chord", after);
        CHECK(rt ? after == 48 : (after != 45 && after != 48), "retrigger %s: %d", rt ? "Pick" : "Chord", after);
    }
}

static void testFreezeBend() {
    std::printf("freeze + bend follow\n");
    BloomSettings s; s.trigger = 3; s.feel = false; s.voice = 2; s.bend = 1.0f;
    Sim sim(s);
    Pluck a{bd::midiHz(57), 0.1, 0.1}; a.bend_from = 1.0; a.bend_to = 1.25; a.bend_cents = 100;
    sim.strings.push_back(a); sim.note(0.112, 57, true);
    sim.strum(0.1, {45, 52});
    sim.run(0.5);
    BloomSettings e = s; e.engage = true; sim.bl.setSettings(e);
    sim.run(1.2);  // to 1.7 s: bend done at 1.25
    CHECK(sim.bl.live().active, "Freeze not arpeggiating while engaged");
    float lastBend = 0;
    for (std::size_t i = 0; i < sim.outAll.size(); ++i) if (sim.outAll[i].type == MidiEvent::PitchBend) lastBend = sim.outAll[i].bend;
    std::printf("  pitch bend sent %.3f st\n", lastBend);
    CHECK(std::fabs(lastBend - 1.0f) < 0.12f, "bend follow: %.3f st (want 1.0)", lastBend);
    e.engage = false; sim.bl.setSettings(e);
    sim.run(0.05);
    CHECK(!sim.bl.live().active, "Freeze still running after release");
}

static void testOneNote() {
    std::printf("one note → chord on its degree\n");
    BloomSettings s; s.trigger = 1; s.feel = false; s.key = 8; s.scale = 0;  // G major (key index 8 = G with Auto at 0)
    s.key = 7; s.extend = 2;
    Sim sim(s);
    sim.strum(0.1, {45});  // A in G major → Am7
    sim.run(0.8);
    std::printf("  A in G major → %s\n", sim.bl.live().chord);
    CHECK(std::string(sim.bl.live().chord) == "Am7", "A in G major → %s (want Am7)", sim.bl.live().chord);
}

static void testHandoff() {
    std::printf("knob-edit handoff (Strings voices sounding)\n");
    BloomSettings s; s.trigger = 1; s.feel = false; s.voice = 2;
    auto sim = std::make_unique<Sim>(s);
    sim->strum(0.1, {45, 52, 57, 60, 64});
    sim->run(1.0);
    CHECK(sim->bl.live().active, "not arpeggiating before the handoff");
    // what BloomComponent::adoptRuntimeStateFrom does: a freshly prepared engine takes the playing one's state, and
    // the old unit is destroyed while the new one plays on
    auto other = std::make_unique<Bloom>();
    other->prepare(kSr);
    std::swap(*other, sim->bl);   // the playing state moves into another object …
    std::swap(sim->bl, *other);   // … and back, and the object it passed through is destroyed
    other.reset();
    double e = 0;
    sim->run(0.5, [&](Sim& x) { for (std::size_t i = x.strOut.size() - kBlock; i < x.strOut.size(); ++i) e += x.strOut[i] * x.strOut[i]; });
    CHECK(sim->bl.live().active && e > 1e-4, "silent after the handoff (energy %.2e)", e);
}

int main() {
    testHandoff();
    testHarmony();
    testKey();
    testGroove();
    testPll();
    testCapture();
    testBloomTrigger();
    testChoke();
    testPalmMuteStaccato();
    testFeel();
    testKeyed();
    testRetrigger();
    testFreezeBend();
    testOneNote();
    if (g_fail) { std::printf("[FAIL] bloom: %d check(s) failed\n", g_fail); return 1; }
    std::printf("[PASS] bloom\n");
    return 0;
}
