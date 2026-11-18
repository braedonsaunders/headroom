#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>

namespace rig::preset {

struct PresetTag {
    std::string mode;
    std::string id;
    std::string value;
};

struct PresetDatabaseInfo {
    std::string name;
    std::string author;
    std::string comment;
    std::string uuid;
    std::string based_on;      // Lineage: source Library preset name (empty if built from scratch)
    std::string based_on_path; // Lineage: source Library preset path or identifier
    std::string instrument;    // Shipped library: <headroom-library instrument="guitar|bass|any"> (empty if absent)
    std::vector<PresetTag> tags;
};

struct PresetParam {
    std::string id;
    std::string type; // "float", "int", "bool", "string", "filename"
    std::string name;
    float float_val = 0.0f;
    int32_t int_val = 0;
    bool bool_val = false;
    std::string str_val;
};

struct ModulationLink {
    int32_t mod_table_id = 0;
    int32_t mod_index = 0;
    std::string target_param_id;
    float intensity = 0.0f;
};

struct PresetComponentNode {
    int32_t slot_index = 0;
    int32_t table_id = 0;
    int32_t instance_index = 0;
    std::vector<PresetParam> params;
    std::vector<ModulationLink> modulations;
    std::vector<PresetComponentNode> children; // Nested sub-chains (Split A/B, CrossOver A/B, SplitMS A/B, Container)
};

struct MacroTarget {
    int32_t page = 0;
    int32_t column = 0;
    std::string control_name;
    int32_t target_table_id = 0;
    int32_t target_index = 0;
    std::string target_param_id;
    float min_val = 0.0f;
    float max_val = 1.0f;
};

struct RackPreset {
    std::string source_path;
    PresetDatabaseInfo info;
    PresetComponentNode root_rack; // table_id 100
    std::vector<MacroTarget> macros;
    std::string extra_xml; // Headroom blocks (e.g. <headroom-synth>) carried verbatim
    // Rig tempo in BPM (0 = not stored -> 120). .rig: <headroom-tempo bpm="..."/>
    double tempo_bpm = 0.0;
};

/**
 * Loads a Headroom .rig rack preset file (`<headroom-preset>`).
 */
bool loadPreset(const std::string& path, RackPreset& out_preset, std::string* err_msg = nullptr);
/** Same parser over bytes already in memory (e.g. plugin state); `path` only labels errors/source_path. */
bool parsePresetData(std::string_view data, const std::string& path, RackPreset& out_preset, std::string* err_msg = nullptr);

} // namespace rig::preset
