#pragma once
#include <cstdint>
#include <vector>

namespace rig::midi {

struct MidiEvent {
    enum Type : uint8_t { NoteOn, NoteOff, PitchBend } type = NoteOn;
    int note = 60;
    float velocity = 0.8f; // 0..1
    float bend = 0.0f;     // semitones (PitchBend)
    uint32_t offset = 0;   // sample offset within the host block
};

using MidiBuffer = std::vector<MidiEvent>;

} // namespace rig::midi
