// Bloom on a real DI: Pitch → MIDI (neural, as the app tracks) → Bloom, with a timeline of what Bloom heard and played.
//   bloom_probe <di.wav> <out.wav> [param=value ...]     (Bloom params by id, raw values; e.g. trigger=1 voice=2)
// Prints the chord / key / bloom state changes and every arpeggio note; writes Bloom's audio output (dry × duck +
// Strings) as a stereo WAV: left = Bloom out, right = the dry DI. The DI is played like the app plays it (mono, 0.35 FS
// peak) unless HEADROOM_RAW_DI=1. Set HEADROOM_P2M_MODEL to pick the tracker weights (default: app/resources/models).
#include "../src/audio/WavIO.hpp"
#include "../src/core/IComponent.hpp"
#include "../src/midi/MidiComponents.hpp"
#include "../src/midi/MidiEvent.hpp"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: bloom_probe <di.wav> <out.wav> [param=value ...]\n"); return 2; }
    const char* model = std::getenv("HEADROOM_P2M_MODEL");
    rig::setNeuralGuitarModelPath(model ? model : "resources/models/guitar_midi.hgnn");
    rig::audio::WavData w;
    std::string err;
    if (!rig::audio::readWavFile(argv[1], w, &err)) { std::fprintf(stderr, "read: %s\n", err.c_str()); return 1; }
    const double sr = w.sample_rate;
    std::vector<float> x(w.left.size());
    float pk = 0;
    for (std::size_t i = 0; i < x.size(); ++i) { x[i] = w.right.empty() ? w.left[i] : 0.5f * (w.left[i] + w.right[i]); pk = std::max(pk, std::fabs(x[i])); }
    if (!std::getenv("HEADROOM_RAW_DI") && pk > 0) for (auto& v : x) v *= 0.35f / pk;
    auto p2m = rig::createComponentByTableId(900001), bl = rig::createComponentByTableId(900006);
    if (!p2m || !bl) { std::fprintf(stderr, "units missing\n"); return 1; }
    p2m->setParameter("mode", 0.0f); p2m->setParameter("sens", 0.55f); p2m->setParameter("low", 28.0f); p2m->setParameter("high", 96.0f);
    for (int a = 3; a < argc; ++a) {
        const char* eq = std::strchr(argv[a], '=');
        if (!eq) continue;
        bl->setParameter(std::string(argv[a], static_cast<std::size_t>(eq - argv[a])), std::strtof(eq + 1, nullptr));
    }
    const uint32_t N = 128;
    p2m->prepare(sr, N); bl->prepare(sr, N);
    bl->setSlotIndex(1);
    std::vector<float> outL, a(N), b(N), c(N), d(N);
    std::vector<rig::midi::MidiEvent> midi;
    midi.reserve(512);
    std::string lastChord, lastState;
    int notes = 0;
    double cpu = 0;
    for (std::size_t pos = 0; pos + N <= x.size(); pos += N) {
        for (uint32_t i = 0; i < N; ++i) a[i] = x[pos + i];
        const float* in[2] = {a.data(), a.data()};
        float* o1[2] = {b.data(), c.data()};
        midi.clear();
        rig::AudioBlock blk; blk.in = in; blk.out = o1; blk.num_channels = 2; blk.num_frames = N; blk.sample_rate = sr; blk.midi = &midi; blk.bpm = 120;
        p2m->processControlRate(); p2m->processAudio(blk);
        const float* in2[2] = {b.data(), c.data()};
        float* o2[2] = {d.data(), c.data()};
        rig::AudioBlock bk2 = blk; bk2.in = in2; bk2.out = o2;
        const auto t0 = std::chrono::steady_clock::now();
        bl->processControlRate(); bl->processAudio(bk2);
        cpu += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        for (uint32_t i = 0; i < N; ++i) outL.push_back(d[i]);
        const double t = static_cast<double>(pos) / sr;
        for (const auto& e : midi) if (e.type == rig::midi::MidiEvent::NoteOn) { ++notes; if (std::getenv("BLOOM_NOTES")) std::printf("%8.3f  note %d v%.2f\n", t + e.offset / sr, e.note, e.velocity); }
        if (pos % (N * 16) == 0) {
            // parse the live JSON for this unit (slot 1)
            const std::string j = rig::liveMidiJson();
            const auto at = j.find("\"bloom\":{\"1\":{");
            if (at == std::string::npos) continue;
            auto field = [&](const char* k) { const auto q = j.find(std::string("\"") + k + "\":", at); if (q == std::string::npos) return std::string(); auto e = j.find_first_of(",}", q); return j.substr(q + std::strlen(k) + 3, e - q - std::strlen(k) - 3); };
            const std::string state = field("active") + " " + field("chord") + " key " + field("key") + " bpm " + field("bpm") + " feel " + field("feel");
            if (state != lastState) { std::printf("%8.3f  active %s\n", t, state.c_str()); lastState = state; }
        }
    }
    std::vector<float> R(x.begin(), x.begin() + static_cast<long>(outL.size()));
    rig::audio::writeWavFile16(argv[2], static_cast<uint32_t>(sr), outL, R);
    const double dur = static_cast<double>(outL.size()) / sr;
    std::printf("%d arpeggio notes in %.1f s; Bloom CPU %.2f %% of a core (wall clock, this machine)\n", notes, dur, 100.0 * cpu / dur);
    return 0;
}
