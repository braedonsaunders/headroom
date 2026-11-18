#pragma once
// "Cabinet" rack unit (table 900100): any studio or matched cabinet after any amp, with the microphones
// and positions for that cabinet, an optional second mic and a room mic.
// See CabinetCatalog.hpp for the cabinet library and docs/ENGINE_API.md ("Cabinet").
#include "../core/IComponent.hpp"
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace rig {

inline constexpr int32_t kCabinetUnitTable = 900100;

std::unique_ptr<IComponent> makeCabinetUnit(const catalog::ComponentSchema& schema);

namespace cabinet {
// Selects `matched:<model>` (Amp-Matched Cab 156000 index order) on a Cabinet unit with capture A only, as
// 156000 at its defaults (Cab A/B = A, X-Fade 0), so an auto-inserted cabinet sounds identical to it.
bool selectMatched(IComponent& unit, int matched_index);
// Selects by stable ids ("studio:marshall-1960tv-4x12", "dyn57", "capedge"; mic_b "" = off). Control thread.
bool select(IComponent& unit, const std::string& cab_id, const std::string& mic_a, const std::string& pos_a,
            const std::string& mic_b, const std::string& pos_b);
// The whole installed catalog (cabinets → mics → positions), for /api/cabinets.
std::string catalogJson();
// `"cabinet":{…}` fragment for a Cabinet unit's slot in /api/rack (selection, labels, status).
std::string slotJson(const IComponent& unit);
bool isCabinetUnit(const IComponent& c) noexcept;

// The Cabinet-unit equivalent of a rack cabinet unit (156000 Amp-Matched Cab, 88000 Classic Amp-Matched Cab,
// 119000 Studio Cab Room, 168000 Cabinet IR Loader), so a rig's existing cabinet can be swapped for one
// with mic choice. Amp-Matched Cab (156000) converts sample-exactly; the others carry over their first two
// active channels (cabinet, mic, position, balance, room). Returns false for other units.
struct Equivalent {
    std::string cab, mic_a, pos_a, mic_b, pos_b;
    std::vector<std::pair<std::string, float>> params; // blend, room, level, …
};
bool equivalentOf(const IComponent& unit, Equivalent& out);
bool apply(IComponent& unit, const Equivalent& eq);
} // namespace cabinet
} // namespace rig

namespace rig::catalog {
// Headroom studio units that are not in the main component table and not MIDI units (currently the Cabinet).
const ComponentSchema* findStudioComponent(int32_t table_id) noexcept;
std::span<const ComponentSchema> studioComponents() noexcept;
} // namespace rig::catalog
