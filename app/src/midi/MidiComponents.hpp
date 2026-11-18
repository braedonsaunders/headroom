#pragma once
#include "../core/IComponent.hpp"
#include "MidiEvent.hpp"
#include <memory>
#include <string>
#include <vector>

namespace rig {

using MidiSinkFn = void (*)(void* ctx, const midi::MidiEvent& e);

bool isMidiComponentTable(int32_t table_id) noexcept;
std::unique_ptr<IComponent> makeMidiComponent(const catalog::ComponentSchema& schema);
// Pitch→MIDI units forward their notes here too (e.g. the virtual CoreMIDI output).
void setGlobalMidiSink(MidiSinkFn fn, void* ctx);
// Raw parameter values of a Poly Synth preset (for component presets / templates).
std::vector<std::pair<std::string, float>> polySynthPresetParams(const std::string& preset);
// Instrument unit sound list: index of the first sound whose label contains `needle` (-1 if none),
// the label of a sound, and a broad family for library tags ("Piano", "Brass", "Strings", ...).
int instrumentSoundIndex(const std::string& needle);
std::string instrumentSoundLabel(int index);
std::string instrumentSoundFamily(int index);
// Neural poly guitar→MIDI model file (.hgnn); loaded once and shared by every Pitch → MIDI unit.
void setNeuralGuitarModelPath(const std::string& path);
// {"notes":[[note,vel],...],"pitch":Hz,"confidence":0..1,"level":rms} from the active Pitch → MIDI unit.
std::string liveMidiJson();

} // namespace rig
