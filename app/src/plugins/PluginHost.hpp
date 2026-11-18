#pragma once
// Third-party plugin hosting: installed Audio Units (effects, music effects, instruments) imported once
// into the component rack and used like native units.
//
//   * Imports persist in ~/Library/Application Support/Headroom/plugins.json, each with a stable table id
//     in 910000..949999 (never reused). Plugins a preset needs but that were never imported get
//     session-only ids in 950000..989999 — presets resolve plugins by identity, not by id.
//   * Schemas (parameter list, ranges, units, value strings) are captured at import time and live for
//     the whole process, so catalog::findByTableId() can hand them out like the built-in units.
//   * Each unit stores its identity (plugin_id / plugin_name / plugin_vendor) and its full AU state
//     (kAudioUnitProperty_ClassInfo, base64 binary plist) as string parameters, so it travels through
//     every preset path (user .rig files, background re-calibration snapshots, lane moves).
//   * A preset whose plugin is not installed loads a pass-through placeholder that keeps the state and
//     writes it back unchanged.
#include "../core/IComponent.hpp"
#include "../preset/PresetLoader.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rig::plugins {

inline constexpr int32_t kImportedBase = kPluginTableBase;   // 910000 (persisted imports)
inline constexpr int32_t kTransientBase = 950000;            // session-only (preset-referenced) plugins

inline bool isPluginTable(int32_t t) noexcept { return isPluginTableId(t); }

struct PluginInfo {
    std::string uid;      // "AU:<type hex>:<subtype hex>:<manufacturer hex>"
    std::string format;   // "AU" (VST3 entries are listed but not hostable yet)
    std::string name;     // "AUDelay"
    std::string vendor;   // "Apple"
    std::string kind;     // "effect" | "music_effect" | "instrument"
    std::string path;     // bundle path when known (VST3 listing)
    uint32_t version = 0;
    bool v3 = false;      // AUv3 (app extension)
    bool supported = true;
    int32_t table_id = 0; // 0 = not imported
};

// Installed plugins (no instantiation; fast). AU first, then VST3 bundles (listed, supported=false).
std::vector<PluginInfo> scanInstalled();
// Imported plugins (persisted), sorted by name.
std::vector<PluginInfo> imported();

// Imports (or refreshes the parameter list of) an installed plugin. Instantiates it once, off the audio
// thread. Returns the table id, or 0 with `err` set.
int32_t importPlugin(const std::string& uid, std::string* err = nullptr);
// Removes an import from the Components rack. Rigs already using it keep working this session; its id is
// retired so it is never reassigned to another plugin.
bool removePlugin(int32_t table_id);

// Lock-free schema lookup (also reached through catalog::findByTableId).
const catalog::ComponentSchema* findSchema(int32_t table_id) noexcept;
std::vector<const catalog::ComponentSchema*> importedSchemas();
bool isInstrumentTable(int32_t table_id) noexcept;
std::string engineDescription(int32_t table_id);

// Creates a hosted unit (or a pass-through placeholder when the plugin can't be instantiated).
std::unique_ptr<IComponent> createComponent(int32_t table_id);
// Instantiates + prepares a unit ahead of a rack insert so the (possibly slow) instantiation happens
// before the caller takes the rack lock; the next createComponent(table_id) picks it up.
void preinstantiate(int32_t table_id, double sample_rate, uint32_t max_block_size);

// Rewrites plugin table ids in a loaded preset to this machine's ids (matching by plugin identity),
// registering session-only entries for plugins that were never imported or aren't installed.
void remapPreset(preset::RackPreset& preset);

// JSON fragments for the engine API.
std::string paramJson(int32_t table_id, const catalog::ParamSchema& p, float host_value);
std::string slotJson(const IComponent& c); // "plugin":{...}
int instanceIdOf(const IComponent& c);     // -1 if not a hosted plugin
// Handles every /api/plugins* route. `q` reads a query parameter. `instance` is the hosted instance at
// ?slot= (resolved by the server under its rack lock) for /api/plugins/editor.
std::string handleApi(const std::string& path, const std::function<std::string(const char*)>& q, int instance);

// ───────────── plugin editors (native windows, implemented by the app) ─────────────
struct EditorTarget {
    std::shared_ptr<void> keepalive; // keeps the AudioUnit alive while its window is open
    void* audio_unit = nullptr;      // AudioUnit
    std::string title;
    bool v3 = false;
};
using EditorOpenFn = bool (*)(int instance_id, std::string* err);
using EditorCloseFn = void (*)(int instance_id);
void setEditorHooks(EditorOpenFn open, EditorCloseFn close);
bool openEditor(int instance_id, std::string* err = nullptr);
bool editorTarget(int instance_id, EditorTarget& out);
void setEditorOpen(int instance_id, bool open);

// Tests: use a different plugins.json (default: ~/Library/Application Support/Headroom/plugins.json).
void setStorePathForTesting(const std::string& path);

} // namespace rig::plugins
