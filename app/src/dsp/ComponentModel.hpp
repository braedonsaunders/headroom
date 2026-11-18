#pragma once
// Exact model export for the component detail page, plus user-editable component overrides.
//
//  * Units implementing `Introspectable` (tube amps, drive pedals) describe the structure and the very
//    values their DSP runs with — stage specs, cathode networks, tone-stack R/C, power section — read
//    from the configured objects, not from marketing text.
//  * Every other unit is described from its implementation kind (ComponentFactory), with values
//    computed by the same formulas the factory implementation uses.
//  * Curves (tone-stack magnitude, stage transfers, harmonic spectrum, impulse/frequency response,
//    measured compression curve) are computed on a private, freshly built instance — never the
//    live graph.
//  * Overrides: the string parameter "__model" carries JSON ({"amp":{...}} / {"drive":{...}}) that
//    the amp/pedal applies when it builds its voicing. It is saved in presets like any parameter.
//    User components ("My Components") persist in
//    ~/Library/Application Support/Headroom/User Components.json.
#include "../core/IComponent.hpp"
#include <functional>
#include <string>

namespace rig::model {

inline constexpr const char* kModelParam = "__model";       // JSON overrides (string parameter)
inline constexpr const char* kAnalysisParam = "__analysis"; // "1" on private analysis instances

class Introspectable {
public:
    virtual ~Introspectable() = default;
    // JSON object text: kind-specific structure plus a "blocks" signal chain for the schematic.
    virtual std::string modelJson() const = 0;
    // JSON object text with static curves for the current settings (may be "{}").
    virtual std::string curvesJson() const { return "{}"; }
};

// Name stored in a unit's "__model" overrides ("" when the unit is stock).
std::string customName(const IComponent& c);

// Computes (and caches) anything expensive an override set needs (e.g. amp level trims) without
// touching a live unit, so applying it to a running slot afterwards is instant.
void prewarm(int32_t table_id, const std::string& model_json, double sr);

// HTTP API for /api/component/* and /api/usercomponents*. `with_slot(slot, fn)` runs fn with the live
// unit of that rack slot (nullptr if none) in a control-thread transaction. Mutating callers
// receive a prepared candidate; rejected edits do not publish it. fn returns true on modification.
using SlotFn = std::function<bool(IComponent*)>;
using WithSlot = std::function<void(long slot, const SlotFn&)>;
bool handlesPath(const std::string& path);
std::string handleApi(const std::string& path, const std::function<std::string(const std::string&)>& query,
                      const WithSlot& with_slot, double sample_rate, bool* rack_modified);

// Exposed for tests.
std::string describe(int32_t table_id, const std::string& params_json, const std::string& model_json, double sr);
std::string curves(int32_t table_id, const std::string& params_json, const std::string& model_json, double sr,
                   const std::string& what, float level = 0.25f, float freq = 330.0f);
std::string userComponentsPath();

} // namespace rig::model
