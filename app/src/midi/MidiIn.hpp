#pragma once
// Hardware MIDI in for controllers: every MIDI source on the system (foot controllers, expression pedals, keyboards'
// knobs), reconnected when devices come and go. Control-change messages only; they go to the engine's controller map
// (MidiControllerMap) and its targets (macros, scenes, the looper, tap tempo, Bloom's Engage / Motion …).
// The callback runs on the MIDI system's thread (CoreMIDI's on the Mac), never on the audio thread.
// macOS: CoreMIDI (MidiOut.mm). Elsewhere: a stub for now (MidiOutPortable.cpp).
#include <functional>
#include <memory>

namespace rig::midi {

class MidiIn {
public:
    using CcFn = std::function<void(int cc, int value, int channel)>;  // channel 1..16
    MidiIn();
    ~MidiIn();
    bool start(CcFn fn);   // safe to call once; false where there is no MIDI input
    int sources() const;   // connected sources (excluding Headroom's own MIDI Out)
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace rig::midi
