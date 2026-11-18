#pragma once
// Virtual CoreMIDI source ("Headroom MIDI Out"): lets a DAW or any synth play from the guitar.
// Events are queued lock-free from the audio thread and sent from a dispatcher thread.
#include "PitchToMidi.hpp"
#include <memory>

namespace rig::midi {

class MidiOut {
public:
    MidiOut();
    ~MidiOut();
    bool start();          // creates the virtual source; safe to call repeatedly
    void push(const MidiEvent& e); // audio thread: lock-free, drops when full
    bool active() const;
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace rig::midi
