#pragma once
// Circuit-engine units: a library netlist run as a rack component (docs/CIRCUIT_ENGINE.md §2.5, §8).
//
// Selection (until each unit is reviewed and the default flips):
//   Every bound unit runs on the circuit engine (clean cut, no fallback), except ids still pending validation.
//   HEADROOM_LEGACY_MODELS=1 (tools only: circuit_ab) renders the old behavioural models for A/B reports.
// The unit's controls come from the netlist; legacy .rig / imported-rack parameter ids reach them through each
// control's "params" aliases, so existing presets keep working.
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "../core/IComponent.hpp"

namespace rig::ce {

// Process-wide default; HEADROOM_CIRCUIT_NOISE=0/1 overrides the initial value.
bool circuitNoiseEnabled() noexcept;
void setCircuitNoiseEnabled(bool enabled) noexcept;
// Control thread only, on a detached component.
bool circuitNoiseNeedsRefresh(const IComponent& unit) noexcept;
bool refreshCircuitNoise(IComponent& unit);

struct UnitBinding {
    int32_t tableId;
    const char* netlistId;
    const char* post; // "" or a post section ("fender-twin-power")
    bool circuitOnly = false; // no legacy model exists: always the circuit (nothing to flag or flip)
};
const std::vector<UnitBinding>& unitBindings();

// Library netlist text by id (embedded; HEADROOM_CIRCUIT_DIR=<dir> loads <dir>/<file> instead, for editing).
std::string libraryNetlist(std::string_view id);
std::vector<std::string> libraryIds();

bool hasCircuitFor(int32_t tableId) noexcept;
bool circuitEngineEnabledFor(int32_t tableId) noexcept;
// nullptr unless the flag enables this table id.
std::unique_ptr<IComponent> makeCircuitUnit(const catalog::ComponentSchema& schema);
// Always builds the circuit unit when one exists (tests, A/B tools).
std::unique_ptr<IComponent> makeCircuitUnitAlways(const catalog::ComponentSchema& schema);

} // namespace rig::ce
