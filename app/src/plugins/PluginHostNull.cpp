// Placeholder plugin host for platforms without a hosting backend yet (Windows, Linux). See PluginHost.hpp.
//
// Scaffolding until PluginHost.cpp is split into a shared core plus AU / VST3 backends (docs/PORTING.md).
// Until then, this file reproduces the core's behaviour for plugins that can't be instantiated, i.e. the
// macOS host's "not installed" path, for every plugin:
//   * The same table-id ranges: persisted imports 910000..949999, session-only 950000..989999. plugins.json
//     (same storePath() rules) is read so ids written elsewhere resolve identically, and it's rewritten only
//     when an import is removed.
//   * remapPreset() resolves each plugin node by identity (plugin_id / name / vendor) to a local id and
//     registers a session-only "missing" entry when needed.
//   * createComponent() returns a pass-through unit that holds every parameter (numeric and string,
//     including the base64 "state") and exports it back unchanged, so presets round-trip byte for byte.
//   * scanInstalled() lists VST3 bundles from platform::vst3Dirs() (supported=false, no AU); importPlugin()
//     refuses with a clear error. /api/plugins* returns the same JSON shapes as on macOS.
#include "PluginHost.hpp"
#include "../library/Library.hpp"
#include "../platform/Paths.hpp"

#include <json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>

namespace rig::plugins {

namespace fs = std::filesystem;
using catalog::ComponentSchema;
using catalog::ParamKind;
using catalog::ParamSchema;
using nlohmann::json;

namespace {

constexpr std::size_t kSlots = 4096;
constexpr const char* kNoHosting = "plugin hosting isn't available on this platform yet";

std::string jsonEsc(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", c);
                    o += b;
                } else {
                    o += static_cast<char>(c);
                }
        }
    }
    return o;
}

std::string num(double v) {
    if (!std::isfinite(v)) v = 0.0;
    char b[40];
    std::snprintf(b, sizeof b, "%.6g", v);
    return b;
}

// Parameter description as captured at import time on a machine that could host the plugin.
struct ParamMeta {
    uint32_t au_id = 0;
    std::string id, name, unit;
    float min = 0.0f, max = 1.0f, def = 0.0f;
    bool log = false, indexed = false, boolean = false, expert = false;
    std::vector<std::string> labels;
    float toHost(float native) const noexcept {
        if (!log) return native;
        const float t = std::log(std::max(native, min) / min) / std::log(max / min);
        return std::clamp(t, 0.0f, 1.0f);
    }
};

struct Entry {
    PluginInfo info;
    std::atomic<bool> persisted{false};
    std::vector<ParamMeta> meta;
    std::vector<ParamSchema> params;
    std::string ref_name, disp_name;
    ComponentSchema schema{};

    // Same catalog view as the macOS host builds for a plugin that isn't installed.
    void finalize(int32_t tid) {
        info.table_id = tid;
        ref_name = info.name.empty() ? std::string("Plugin") : info.name;
        disp_name = info.vendor.empty() ? "Plugin not installed" : info.vendor + " · not installed";
        params.clear();
        for (const auto& m : meta) {
            ParamSchema p{};
            p.id = m.id;
            p.display_name = m.name;
            p.kind = m.boolean ? ParamKind::Bool : m.indexed ? ParamKind::Int : ParamKind::Float;
            p.default_value = m.toHost(m.def);
            p.min_value = m.log ? 0.0f : m.min;
            p.max_value = m.log ? 1.0f : m.max;
            p.role = m.log ? "log" : "";
            p.unit = m.log ? std::string_view() : std::string_view(m.unit);
            params.push_back(p);
        }
        params.push_back(ParamSchema{"Pwr", "On/Off", ParamKind::Float, 1.0f, 0.0f, 1.0f, "", ""});
        schema = ComponentSchema{tid, ref_name, disp_name, "HostedPlugin", catalog::EngineFamily::ModernDSP, -1, false,
                                 std::span<const ParamSchema>(params.data(), params.size())};
    }
    const ParamMeta* metaFor(std::string_view id) const noexcept {
        for (const auto& m : meta)
            if (m.id == id) return &m;
        return nullptr;
    }
};

struct Registry {
    std::array<std::atomic<Entry*>, kSlots> imported{};
    std::array<std::atomic<Entry*>, kSlots> transient{};
    std::mutex m;
    std::map<std::string, int32_t> by_uid;
    std::vector<std::unique_ptr<Entry>> owned; // entries (and their schemas) live for the whole process
    int32_t next_imported = kImportedBase;
    int32_t next_transient = kTransientBase;
    std::string store_path;
    bool loaded = false;
};
Registry& reg() {
    static Registry* r = new Registry(); // intentionally immortal, like the macOS host
    return *r;
}

Entry* lookup(int32_t tid) noexcept {
    auto& r = reg();
    if (tid >= kImportedBase && tid < kImportedBase + static_cast<int32_t>(kSlots))
        return r.imported[static_cast<std::size_t>(tid - kImportedBase)].load(std::memory_order_acquire);
    if (tid >= kTransientBase && tid < kTransientBase + static_cast<int32_t>(kSlots))
        return r.transient[static_cast<std::size_t>(tid - kTransientBase)].load(std::memory_order_acquire);
    return nullptr;
}

// Caller holds reg().m.
void publish(std::unique_ptr<Entry> e, int32_t tid) {
    auto& r = reg();
    e->finalize(tid);
    Entry* raw = e.get();
    r.owned.push_back(std::move(e));
    if (tid >= kImportedBase && tid < kImportedBase + static_cast<int32_t>(kSlots))
        r.imported[static_cast<std::size_t>(tid - kImportedBase)].store(raw, std::memory_order_release);
    else if (tid >= kTransientBase && tid < kTransientBase + static_cast<int32_t>(kSlots))
        r.transient[static_cast<std::size_t>(tid - kTransientBase)].store(raw, std::memory_order_release);
    r.by_uid[raw->info.uid] = tid;
}

std::string storePath() {
    auto& r = reg();
    if (!r.store_path.empty()) return r.store_path;
    if (const char* p = std::getenv("HEADROOM_PLUGINS_FILE")) return p;
    return library::appSupportDir() + "/plugins.json";
}

json metaToJson(const ParamMeta& m) {
    json j = {{"au_id", m.au_id}, {"name", m.name}, {"unit", m.unit}, {"min", m.min}, {"max", m.max}, {"def", m.def}};
    if (m.log) j["log"] = true;
    if (m.indexed) j["indexed"] = true;
    if (m.boolean) j["bool"] = true;
    if (m.expert) j["expert"] = true;
    if (!m.labels.empty()) j["labels"] = m.labels;
    return j;
}

// Caller holds reg().m. Same document as the macOS host writes.
void saveLocked() {
    auto& r = reg();
    json plugins = json::array(), retired = json::object();
    for (const auto& e : r.owned) {
        const int32_t tid = e->info.table_id;
        if (tid < kImportedBase || tid >= kTransientBase) continue;
        if (lookup(tid) != e.get()) continue;
        if (!e->persisted.load()) {
            retired[e->info.uid] = tid;
            continue;
        }
        json params = json::array();
        for (const auto& m : e->meta) params.push_back(metaToJson(m));
        plugins.push_back({{"table_id", tid}, {"uid", e->info.uid}, {"format", e->info.format}, {"name", e->info.name},
                           {"vendor", e->info.vendor}, {"kind", e->info.kind}, {"version", e->info.version}, {"v3", e->info.v3},
                           {"params", params}});
    }
    for (const auto& kv : r.by_uid) // retired ids that were never loaded as entries stay reserved
        if (kv.second >= kImportedBase && kv.second < kTransientBase && !lookup(kv.second) && !retired.contains(kv.first))
            retired[kv.first] = kv.second;
    const json doc = {{"version", 1}, {"next_id", r.next_imported}, {"plugins", plugins}, {"retired", retired}};
    const std::string path = storePath();
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream o(tmp, std::ios::trunc);
        o << doc.dump(1);
        if (!o) return;
    }
    fs::rename(tmp, path, ec);
}

void loadLocked() {
    auto& r = reg();
    if (r.loaded) return;
    r.loaded = true;
    std::ifstream f(storePath());
    if (!f) return;
    json doc;
    try {
        doc = json::parse(f);
    } catch (...) {
        std::fprintf(stderr, "[plugins] %s is not valid JSON; ignoring it\n", storePath().c_str());
        return;
    }
    if (doc.contains("next_id") && doc["next_id"].is_number_integer()) r.next_imported = std::max(r.next_imported, doc["next_id"].get<int32_t>());
    if (doc.contains("plugins") && doc["plugins"].is_array()) {
        for (const auto& p : doc["plugins"]) {
            try {
                auto e = std::make_unique<Entry>();
                const int32_t tid = p.at("table_id").get<int32_t>();
                if (tid < kImportedBase || tid >= kImportedBase + static_cast<int32_t>(kSlots) || lookup(tid)) continue;
                e->info.uid = p.at("uid").get<std::string>();
                e->info.format = p.value("format", std::string("AU"));
                e->info.name = p.value("name", std::string());
                e->info.vendor = p.value("vendor", std::string());
                e->info.kind = p.value("kind", std::string("effect"));
                e->info.version = p.value("version", 0u);
                e->info.v3 = p.value("v3", false);
                e->info.supported = false;
                if (p.contains("params")) {
                    for (const auto& pm : p["params"]) {
                        ParamMeta m;
                        m.au_id = pm.value("au_id", 0u);
                        m.id = std::to_string(m.au_id);
                        m.name = pm.value("name", m.id);
                        m.unit = pm.value("unit", std::string());
                        m.min = pm.value("min", 0.0f);
                        m.max = pm.value("max", 1.0f);
                        m.def = pm.value("def", m.min);
                        m.log = pm.value("log", false) && m.min > 0.0f && m.max > m.min;
                        m.indexed = pm.value("indexed", false);
                        m.boolean = pm.value("bool", false);
                        m.expert = pm.value("expert", false);
                        if (pm.contains("labels")) m.labels = pm["labels"].get<std::vector<std::string>>();
                        e->meta.push_back(std::move(m));
                    }
                }
                e->persisted.store(true);
                r.next_imported = std::max(r.next_imported, tid + 1);
                publish(std::move(e), tid);
            } catch (...) {
            }
        }
    }
    if (doc.contains("retired") && doc["retired"].is_object()) {
        for (auto it = doc["retired"].begin(); it != doc["retired"].end(); ++it) {
            if (!it.value().is_number_integer()) continue;
            const int32_t tid = it.value().get<int32_t>();
            r.next_imported = std::max(r.next_imported, tid + 1);
            if (!r.by_uid.count(it.key())) r.by_uid[it.key()] = tid;
        }
    }
}

void ensureLoaded() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::lock_guard<std::mutex> lk(reg().m);
        loadLocked();
    });
}

// Pass-through rack unit that keeps whatever the preset carried.
class PlaceholderComponent final : public IComponent {
public:
    explicit PlaceholderComponent(const Entry* e) : e_(e) {}

    const Entry& entry() const noexcept { return *e_; }

    int32_t tableId() const noexcept override { return e_->schema.table_id; }
    std::string_view referenceName() const noexcept override { return e_->schema.reference_name; }
    std::string_view displayName() const noexcept override { return e_->schema.display_name; }
    catalog::EngineFamily family() const noexcept override { return e_->schema.family; }
    const catalog::ComponentSchema& schema() const noexcept override { return e_->schema; }

    void prepare(double, uint32_t) override {}
    void reset() override {}

    void setParameter(std::string_view id, float value) override {
        if (id == "Pwr" || id == "on_off") {
            bypassed_ = value < 0.5f;
            return;
        }
        hold(id, false, value, {});
    }
    void setStringParameter(std::string_view id, std::string_view value) override { hold(id, true, 0.0f, value); }
    float getParameter(std::string_view id) const override {
        if (id == "Pwr" || id == "on_off") return bypassed_ ? 0.0f : 1.0f;
        for (const auto& h : held_)
            if (h.id == id && !h.is_string) return h.num;
        const ParamMeta* m = e_->metaFor(id);
        return m ? m->toHost(m->def) : 0.0f;
    }
    std::string getStringParameter(std::string_view id) const override {
        for (const auto& h : held_)
            if (h.id == id && h.is_string) return h.str;
        return {};
    }
    std::vector<ExportedParam> exportParams() const override {
        std::vector<ExportedParam> out;
        auto str = [&](const char* id, std::string v) { out.push_back({id, true, 0.0f, std::move(v)}); };
        bool has_id = false;
        for (const auto& h : held_) has_id = has_id || h.id == "plugin_id";
        if (!has_id) {
            str("plugin_id", e_->info.uid);
            str("plugin_name", e_->info.name);
            str("plugin_vendor", e_->info.vendor);
            str("plugin_format", e_->info.format);
        }
        for (const auto& h : held_)
            if (h.id != "Pwr") out.push_back(h);
        out.push_back({"Pwr", false, bypassed_ ? 0.0f : 1.0f, {}});
        return out;
    }

    void applyModulationDelta(std::string_view, float) override {}
    void clearModulationDeltas() override {}
    void processControlRate() override {}
    void processAudio(AudioBlock& b) override { b.copyInToOut(); }
    bool monoPreserving() const noexcept override { return true; } // series pipelining (RackGraph): a copy

private:
    void hold(std::string_view id, bool is_string, float v, std::string_view s) {
        for (auto& h : held_) {
            if (h.id == id) {
                h.is_string = is_string;
                h.num = v;
                h.str = std::string(s);
                return;
            }
        }
        held_.push_back({std::string(id), is_string, v, std::string(s)});
    }

    const Entry* e_;
    bool bypassed_ = false;
    std::vector<ExportedParam> held_;
};

int32_t allocTransientLocked() {
    auto& r = reg();
    if (r.next_transient >= kTransientBase + static_cast<int32_t>(kSlots)) return 0;
    return r.next_transient++;
}

int32_t resolve(const std::string& uid, const std::string& name, const std::string& vendor, int32_t hint) {
    ensureLoaded();
    auto& r = reg();
    std::lock_guard<std::mutex> lk(r.m);
    if (uid.empty()) {
        if (lookup(hint)) return hint;
    } else if (auto it = r.by_uid.find(uid); it != r.by_uid.end() && lookup(it->second)) {
        return it->second;
    }
    auto e = std::make_unique<Entry>();
    e->info.uid = uid.empty() ? "unknown:" + std::to_string(hint) : uid;
    e->info.format = uid.rfind("VST3:", 0) == 0 ? "VST3" : "AU";
    e->info.name = name.empty() ? "Missing plugin" : name;
    e->info.vendor = vendor;
    e->info.kind = "effect";
    e->info.supported = false;
    if (auto it = r.by_uid.find(e->info.uid); it != r.by_uid.end() && lookup(it->second)) return it->second;
    const int32_t tid = allocTransientLocked();
    if (!tid) return hint;
    publish(std::move(e), tid);
    return tid;
}

std::string infoJson(const PluginInfo& p, bool installed_flag, std::size_t nparams) {
    std::string o = "{\"uid\":\"" + jsonEsc(p.uid) + "\",\"format\":\"" + jsonEsc(p.format) + "\",\"name\":\"" + jsonEsc(p.name) +
                    "\",\"vendor\":\"" + jsonEsc(p.vendor) + "\",\"kind\":\"" + jsonEsc(p.kind) + "\",\"version\":\"";
    o += std::to_string((p.version >> 16) & 0xffff) + "." + std::to_string((p.version >> 8) & 0xff) + "." + std::to_string(p.version & 0xff);
    o += "\",\"v3\":" + std::string(p.v3 ? "true" : "false") + ",\"supported\":" + (p.supported ? "true" : "false") +
         ",\"installed\":" + (installed_flag ? "true" : "false") + ",\"table_id\":" + (p.table_id ? std::to_string(p.table_id) : "null");
    if (!p.path.empty()) o += ",\"path\":\"" + jsonEsc(p.path) + "\"";
    if (nparams) o += ",\"params\":" + std::to_string(nparams);
    return o + "}";
}

} // namespace

// ───────────────────────────── public API ─────────────────────────────
void setStorePathForTesting(const std::string& path) {
    std::lock_guard<std::mutex> lk(reg().m);
    reg().store_path = path;
}

std::vector<PluginInfo> scanInstalled() {
    ensureLoaded();
    std::vector<PluginInfo> out;
    for (const auto& dir : platform::vst3Dirs()) {
        std::error_code ec;
        for (const auto& it : fs::directory_iterator(dir, ec)) {
            if (it.path().extension() != ".vst3") continue;
            if (it.path().stem() == "Headroom") continue; // never list Headroom's own plugin build
            PluginInfo p;
            p.uid = "VST3:" + it.path().string();
            p.format = "VST3";
            p.name = it.path().stem().string();
            p.kind = "effect";
            p.path = it.path().string();
            p.supported = false;
            out.push_back(std::move(p));
        }
    }
    return out;
}

std::vector<PluginInfo> imported() {
    ensureLoaded();
    std::vector<PluginInfo> out;
    std::lock_guard<std::mutex> lk(reg().m);
    for (const auto& kv : reg().by_uid) {
        const Entry* e = lookup(kv.second);
        if (e && e->persisted.load() && e->info.uid == kv.first) out.push_back(e->info);
    }
    std::sort(out.begin(), out.end(), [](const PluginInfo& a, const PluginInfo& b) { return a.name < b.name; });
    return out;
}

int32_t importPlugin(const std::string&, std::string* err) {
    ensureLoaded();
    if (err) *err = kNoHosting;
    return 0;
}

bool removePlugin(int32_t table_id) {
    ensureLoaded();
    std::lock_guard<std::mutex> lk(reg().m);
    Entry* e = lookup(table_id);
    if (!e || !e->persisted.load()) return false;
    e->persisted.store(false);
    saveLocked();
    return true;
}

const catalog::ComponentSchema* findSchema(int32_t table_id) noexcept {
    if (!isPluginTable(table_id)) return nullptr;
    ensureLoaded();
    const Entry* e = lookup(table_id);
    return e ? &e->schema : nullptr;
}

std::vector<const catalog::ComponentSchema*> importedSchemas() {
    ensureLoaded();
    std::vector<const catalog::ComponentSchema*> out;
    for (const auto& p : imported())
        if (const Entry* e = lookup(p.table_id)) out.push_back(&e->schema);
    return out;
}

bool isInstrumentTable(int32_t table_id) noexcept {
    const Entry* e = isPluginTable(table_id) ? lookup(table_id) : nullptr;
    return e && e->info.kind == "instrument";
}

std::string engineDescription(int32_t table_id) {
    if (!lookup(table_id)) return "Hosted plugin";
    return "Plugin hosting isn't available on this platform yet — passing audio through";
}

std::unique_ptr<IComponent> createComponent(int32_t table_id) {
    const Entry* e = lookup(table_id);
    if (!e) return nullptr;
    return std::make_unique<PlaceholderComponent>(e);
}

void preinstantiate(int32_t, double, uint32_t) {} // nothing to instantiate

void remapPreset(preset::RackPreset& preset) {
    std::map<int32_t, std::set<int32_t>> mapping;
    std::function<void(preset::PresetComponentNode&)> walk = [&](preset::PresetComponentNode& n) {
        if (isPluginTable(n.table_id)) {
            std::string uid, name, vendor;
            for (const auto& p : n.params) {
                if (p.id == "plugin_id") uid = p.str_val;
                else if (p.id == "plugin_name") name = p.str_val;
                else if (p.id == "plugin_vendor") vendor = p.str_val;
            }
            const int32_t t = resolve(uid, name, vendor, n.table_id);
            mapping[n.table_id].insert(t);
            n.table_id = t;
        }
        for (auto& c : n.children) walk(c);
    };
    walk(preset.root_rack);
    for (auto& m : preset.macros) {
        auto it = mapping.find(m.target_table_id);
        if (it != mapping.end() && it->second.size() == 1) m.target_table_id = *it->second.begin();
    }
}

std::string paramJson(int32_t table_id, const catalog::ParamSchema& p, float host) {
    const Entry* e = lookup(table_id);
    const ParamMeta* m = e ? e->metaFor(p.id) : nullptr;
    const float mn = p.min_value, mx = p.max_value > p.min_value ? p.max_value : p.min_value + 1.0f;
    const float norm = std::clamp((host - mn) / (mx - mn), 0.0f, 1.0f);
    const float def_norm = std::clamp((p.default_value - mn) / (mx - mn), 0.0f, 1.0f);
    std::string kind = "continuous";
    if (m && m->boolean) kind = "bool";
    else if (m && m->indexed) kind = "stepped";
    std::string o = "{\"id\":\"" + jsonEsc(std::string(p.id)) + "\",\"name\":\"" + jsonEsc(std::string(p.display_name)) + "\",\"unit\":\"" +
                    jsonEsc(std::string(p.unit)) + "\",\"min\":" + num(mn) + ",\"max\":" + num(mx) + ",\"val\":" + num(host) +
                    ",\"norm\":" + num(norm) + ",\"default\":" + num(def_norm) + ",\"kind\":\"" + kind + "\",\"fmt\":\"" + jsonEsc(num(host)) + "\"";
    if (m && !m->labels.empty()) {
        o += ",\"labels\":[";
        for (std::size_t i = 0; i < m->labels.size(); ++i) o += (i ? ",\"" : "\"") + jsonEsc(m->labels[i]) + "\"";
        o += "]";
    }
    if (m && m->log) o += ",\"scale\":\"log\"";
    return o + "}";
}

int instanceIdOf(const IComponent&) { return -1; }

std::string slotJson(const IComponent& c) {
    const auto* pc = dynamic_cast<const PlaceholderComponent*>(&c);
    if (!pc) return "\"plugin\":null";
    const auto& e = pc->entry();
    return "\"plugin\":{\"instance\":-1,\"uid\":\"" + jsonEsc(e.info.uid) + "\",\"format\":\"" + jsonEsc(e.info.format) + "\",\"name\":\"" +
           jsonEsc(e.info.name) + "\",\"vendor\":\"" + jsonEsc(e.info.vendor) + "\",\"kind\":\"" + jsonEsc(e.info.kind) +
           "\",\"missing\":true,\"ready\":false,\"editor\":false,\"editor_open\":false,\"imported\":" + (e.persisted.load() ? "true" : "false") +
           ",\"in_channels\":2,\"out_channels\":2,\"latency_ms\":0,\"tail_s\":0,\"render_errors\":0}";
}

void setEditorHooks(EditorOpenFn, EditorCloseFn) {}
bool openEditor(int, std::string* err) {
    if (err) *err = kNoHosting;
    return false;
}
bool editorTarget(int, EditorTarget&) { return false; }
void setEditorOpen(int, bool) {}

std::string handleApi(const std::string& path, const std::function<std::string(const char*)>& q, int instance) {
    if (path == "/api/plugins") {
        const auto avail = scanInstalled();
        std::string o = "{\"store\":\"" + jsonEsc(storePath()) + "\",\"imported\":[";
        bool first = true;
        for (const auto& p : imported()) {
            const Entry* e = lookup(p.table_id);
            o += (first ? "" : ",") + infoJson(p, false, e ? e->meta.size() : 0);
            first = false;
        }
        o += "],\"available\":[";
        for (std::size_t i = 0; i < avail.size(); ++i) o += (i ? "," : "") + infoJson(avail[i], true, 0);
        o += "],\"vst3\":{\"supported\":false,\"note\":\"VST3 bundles are listed but cannot be hosted yet\"},"
             "\"hosting\":false,\"note\":\"" + std::string(kNoHosting) + "\"}";
        return o;
    }
    if (path == "/api/plugins/import") {
        const std::string list = q("uid");
        std::string o = "{\"results\":[";
        bool first = true;
        std::size_t pos = 0;
        while (pos <= list.size()) {
            const std::size_t comma = list.find(',', pos);
            const std::string uid = list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            pos = comma == std::string::npos ? list.size() + 1 : comma + 1;
            if (uid.empty()) continue;
            std::string err;
            importPlugin(uid, &err);
            o += std::string(first ? "" : ",") + "{\"uid\":\"" + jsonEsc(uid) + "\",\"table_id\":null,\"error\":\"" + jsonEsc(err) + "\"}";
            first = false;
        }
        return o + "],\"ok\":false}";
    }
    if (path == "/api/plugins/remove") {
        int32_t tid = 0;
        try { tid = std::stoi(q("table_id")); } catch (...) {}
        return std::string("{\"ok\":") + (removePlugin(tid) ? "true" : "false") + "}";
    }
    if (path == "/api/plugins/editor") {
        int id = instance;
        if (id < 0) {
            try { id = std::stoi(q("instance")); } catch (...) {}
        }
        const std::string err = id < 0 ? "that slot is not a hosted plugin" : kNoHosting;
        return "{\"ok\":false,\"instance\":" + std::to_string(id) + ",\"error\":\"" + jsonEsc(err) + "\"}";
    }
    return "{\"error\":\"not_found\"}";
}

} // namespace rig::plugins
