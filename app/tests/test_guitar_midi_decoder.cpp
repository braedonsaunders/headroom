// Neural guitar → MIDI decoder: onset confirmation + octave/fifth ghost suppression, on synthetic per-frame
// probabilities fed straight into the decoder (NeuralGuitarMidi::decodeFrame) with the shipped model's decoder
// settings. Pins: a strong low note's octave harmonic must not emit a ghost (as an onset blip or as a creeping
// sustain), while a really played octave still emits in its onset frame; and decoding never allocates.
// Needs app/resources/models/guitar_midi.hgnn (exits 77 = skipped without it).
#undef NDEBUG
#include "../src/midi/NeuralGuitarMidi.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

#ifndef RIG_SOURCE_DIR
#define RIG_SOURCE_DIR "."
#endif

namespace { thread_local bool inDecode = false; }
void* operator new(std::size_t size) {
    assert(!inDecode);
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using rig::midi::NeuralGuitarMidi;

namespace {
constexpr int kLow = 45;  // A2: parent; ghosts at +12 (A3), +19 (E4)

struct Frame { std::vector<float> on, fr; };

// Runs `frames` through a fresh decoder; returns note-ons as (pitch, frame).
std::vector<std::pair<int, int>> run(NeuralGuitarMidi& d, const std::vector<Frame>& frames) {
    d.reset();
    std::vector<NeuralGuitarMidi::Note> out; out.reserve(256);
    std::vector<std::pair<int, int>> ons;
    for (const auto& f : frames) {
        out.clear();
        inDecode = true;
        d.decodeFrame(f.on.data(), f.fr.data(), out);
        inDecode = false;
        for (const auto& n : out) if (n.on) ons.emplace_back(n.pitch, n.frame);
    }
    return ons;
}
int onsetOf(const std::vector<std::pair<int, int>>& ons, int pitch) {
    for (auto [p, f] : ons) if (p == pitch) return f;
    return -1;
}

// 60 frames of silence-level probabilities, then a strong low note struck at frame 10 and ringing.
std::vector<Frame> base(const NeuralGuitarMidi& d) {
    const int P = d.numPitches(), lo = kLow - d.minPitch();
    std::vector<Frame> fs(60, Frame{std::vector<float>(P, 0.01f), std::vector<float>(P, 0.01f)});
    fs[10].on[lo] = 0.97f; fs[11].on[lo] = 0.6f;
    for (int t = 10; t < 60; ++t) fs[t].fr[lo] = 0.97f;
    return fs;
}
}  // namespace

int main() {
    NeuralGuitarMidi d; std::string err;
    if (!d.load(std::string(RIG_SOURCE_DIR) + "/resources/models/guitar_midi.hgnn", &err)) {
        std::fprintf(stderr, "guitar_midi_decoder: model not found (%s), skipping\n", err.c_str());
        return 77;
    }
    const int m = d.minPitch(), oct = kLow + 12 - m, fifth = kLow + 19 - m;

    // 1. Onset-head ghost: the octave's onset prob crosses threshold with the parent's attack, but its sustain prob
    //    never follows (the harmonic belongs to the parent).
    auto blip = base(d);
    blip[10].on[oct] = 0.8f; blip[11].on[oct] = 0.5f;
    for (int t = 10; t < 60; ++t) blip[t].fr[oct] = 0.15f;
    {
        auto ons = run(d, blip);
        assert(onsetOf(ons, kLow) == 10);       // the parent itself: no added latency
        assert(onsetOf(ons, kLow + 12) < 0);    // ghost suppressed
    }

    // 2. Sustain-head ghost: the octave's sustain prob creeps up while the parent rings, with no attack of its own.
    auto creep = base(d);
    for (int t = 20; t < 60; ++t) creep[t].fr[fifth] = std::min(0.9f, 0.1f + 0.04f * (t - 20));
    {
        auto ons = run(d, creep);
        assert(onsetOf(ons, kLow) == 10);
        assert(onsetOf(ons, kLow + 19) < 0);
    }

    // 3. Really played octave in the same strum: own attack and own sustain → emitted in its onset frame.
    auto real = base(d);
    real[10].on[oct] = 0.9f; real[11].on[oct] = 0.5f;
    for (int t = 10; t < 60; ++t) real[t].fr[oct] = 0.9f;
    {
        auto ons = run(d, real);
        assert(onsetOf(ons, kLow) == 10);
        assert(onsetOf(ons, kLow + 12) == 10);
    }

    // 4. Real octave whose sustain lags the attack by a frame: held, then confirmed one frame later.
    auto lag = base(d);
    lag[30].on[oct] = 0.9f; lag[31].on[oct] = 0.6f;
    lag[30].fr[oct] = 0.2f;
    for (int t = 31; t < 60; ++t) lag[t].fr[oct] = 0.9f;
    {
        auto ons = run(d, lag);
        assert(onsetOf(ons, kLow + 12) == 31);
    }

    // 5. Real octave hammered on (legato: no onset peak, but an attack trace and a fast sustain rise).
    auto ham = base(d);
    ham[30].on[oct] = 0.3f;
    for (int t = 30; t < 60; ++t) ham[t].fr[oct] = 0.85f;
    {
        auto ons = run(d, ham);
        assert(onsetOf(ons, kLow + 12) == 30);
    }

    // 6. With suppression disabled the same blip/creep inputs do produce ghosts (the cases above are meaningful).
    d.setConfirm(0.0f, 2); d.setGhost(0.0f, 0.0f, 1.0f);
    assert(onsetOf(run(d, blip), kLow + 12) == 10);
    assert(onsetOf(run(d, creep), kLow + 19) >= 0);

    std::puts("guitar_midi_decoder: ok");
    return 0;
}
