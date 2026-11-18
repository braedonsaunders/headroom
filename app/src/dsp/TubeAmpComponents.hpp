#pragma once
#include "../core/IComponent.hpp"
#include <memory>
#include <string>

namespace rig {

// Bump whenever amp/pedal DSP changes in a way that alters levels (invalidates on-disk caches).
inline constexpr const char* kEngineDspVersion = "dsp8-circuit1";

bool isTubeAmpTable(int32_t table_id) noexcept;
bool isDrivePedalTable(int32_t table_id) noexcept;
std::unique_ptr<IComponent> makeTubeAmp(const catalog::ComponentSchema& schema);
std::unique_ptr<IComponent> makeDrivePedal(const catalog::ComponentSchema& schema);
// Human-readable signal path for the UI ("5-stage triode preamp · Marshall tone stack · ...").
std::string engineDescription(int32_t table_id);
// Oversampling factor used by the analog-modelled engines (0 = not one of them).
int engineOversampling(int32_t table_id, double host_sr);
// Computes and caches the level trims a user-modified amp ("__model" overrides) needs, off the audio path.
void prewarmTubeAmpModel(int32_t table_id, const std::string& model_json, double sample_rate);

} // namespace rig
