#pragma once
// Headroom's bundled showcase rigs (guitar → MIDI instruments, and guitar + instrument hybrids).
// The rigs are generated from code with the regular rack-editing API, then balanced by rendering real
// DI performances through each lane, so they always match the current engine.
#include "Library.hpp"
#include <ostream>
#include <string>
#include <vector>

namespace rig::library {

struct ShowcaseRig {
    std::string file;  // file name, e.g. "01 Concert Grand.rig"
    preset::RackPreset preset;
};

// `bank` supplies factory INIT values for factory units (may be null); `di_dir` holds the DI clips used
// to balance lanes (samples/di). Progress and measurements go to `log` when given.
std::vector<ShowcaseRig> buildShowcase(const ComponentPresetBank* bank, const std::string& di_dir, std::ostream* log = nullptr);

} // namespace rig::library
