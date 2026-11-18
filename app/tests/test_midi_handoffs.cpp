// Allocation guard around the MIDI units' audio-thread state handoff (adoptRuntimeStateFrom):
// any heap allocation or free while it runs fails the test, same pattern as test_effect_handoffs.
#undef NDEBUG
#include "../src/core/IComponent.hpp"
#include "test_compat.hpp"
#include "../src/midi/MidiEvent.hpp"
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>
#include <vector>

namespace { thread_local bool inHandoff = false; }
void* operator new(std::size_t size) {
    assert(!inHandoff);
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { assert(!inHandoff); std::free(p); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
void* operator new(std::size_t size, std::align_val_t a) {
    assert(!inHandoff);
    if (void* p = rig_test::alignedAlloc(static_cast<std::size_t>(a), size)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p, std::align_val_t) noexcept {
    if constexpr (rig_test::kAlignedNeedsOwnFree) rig_test::alignedFree(p); // _aligned_malloc block (Windows)
    else ::operator delete(p);
}
void* operator new[](std::size_t size, std::align_val_t a) { return ::operator new(size, a); }
void operator delete[](void* p, std::align_val_t a) noexcept { ::operator delete(p, a); }
void operator delete(void* p, std::size_t, std::align_val_t a) noexcept { ::operator delete(p, a); }
void operator delete[](void* p, std::size_t, std::align_val_t a) noexcept { ::operator delete(p, a); }

static constexpr double kSr = 48000.0;
static constexpr uint32_t kN = 256;

static double render(rig::IComponent& c, std::vector<rig::midi::MidiEvent>& midi, double hz, double& ph) {
    std::vector<float> in(kN), L(kN), R(kN);
    for (uint32_t i = 0; i < kN; ++i) { in[i] = hz > 0 ? 0.3f * std::sin(ph) : 0.0f; ph += 2 * M_PI * hz / kSr; }
    const float* ins[2] = {in.data(), in.data()};
    float* outs[2] = {L.data(), R.data()};
    rig::AudioBlock b; b.in = ins; b.out = outs; b.num_channels = 2; b.num_frames = kN; b.sample_rate = kSr; b.midi = &midi;
    c.processControlRate();
    c.processAudio(b);
    double e = 0; for (float v : L) e += double(v) * v;
    return std::sqrt(e / kN);
}

// Builds `next` with a changed knob, plays `prev` for a while, then adopts under the guard.
static void check(int32_t tid, const char* knob, float a, float b, std::vector<std::pair<const char*, float>> common,
                  double hz, bool send_note) {
    auto prev = rig::createComponentByTableId(tid), next = rig::createComponentByTableId(tid);
    assert(prev && next);
    for (auto& [k, v] : common) { prev->setParameter(k, v); next->setParameter(k, v); }
    prev->setParameter(knob, a); next->setParameter(knob, b);
    prev->prepare(kSr, kN); next->prepare(kSr, kN);
    next->processControlRate();
    std::vector<rig::midi::MidiEvent> midi;
    midi.reserve(256);
    double ph = 0;
    for (int i = 0; i < 40; ++i) {
        midi.clear();
        if (send_note && i == 0) midi.push_back({rig::midi::MidiEvent::NoteOn, 64, 0.9f, 0.0f, 0});
        render(*prev, midi, hz, ph);
    }
    inHandoff = true;
    next->adoptRuntimeStateFrom(*prev);
    inHandoff = false;
    midi.clear();
    const double rms = render(*next, midi, hz, ph);
    std::cout << "  table " << tid << " adopt ok, post-handoff rms " << rms << "\n";
}

int main() {
    check(900001, "sens", 0.5f, 0.7f, {{"mode", 1.0f}}, 196.0, false);                 // Pitch -> MIDI
    check(900002, "transpose", 0.0f, 2.0f, {{"arp", 1.0f}, {"hold", 1.0f}}, 0.0, true); // MIDI FX (arp running)
    check(900003, "tone", 0.5f, 0.8f, {{"sound", 0.0f}}, 0.0, true);                    // Instrument, FM keys
    check(900003, "tone", 0.5f, 0.8f, {{"sound", 30.0f}}, 0.0, true);                   // Instrument, GM via AU
    check(900004, "cutoff", 0.4f, 0.6f, {}, 0.0, true);                                 // Poly Synth
    check(900006, "engage", 0.0f, 1.0f, {{"trigger", 1.0f}, {"voice", 2.0f}}, 164.8, true); // Bloom (arpeggiating)
    std::cout << "[PASS] MIDI unit handoffs: no heap allocation or free during adoptRuntimeStateFrom.\n";
    return 0;
}
