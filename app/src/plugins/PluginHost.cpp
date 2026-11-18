// Third-party Audio Unit hosting for the rack. See PluginHost.hpp for the model.
//
// Threading:
//   * Scanning, importing, instantiation, AudioUnitInitialize, state get/set and parameter changes run on
//     control threads (HTTP / worker / preset loading). Rack inserts pre-instantiate before the rack lock.
//   * processAudio() runs on the audio thread: AudioUnitRender with an input render callback that reads
//     the host's input pointers. No allocation, no locks, no Core Foundation on that path.
//   * Instances are owned through shared_ptr<AUHandle> so an open editor window keeps its AudioUnit alive
//     after the rack unit is removed; units are disposed off the rack lock.
#include "PluginHost.hpp"
#include "../platform/Paths.hpp"
#include "../library/Library.hpp"
#include "../midi/MidiEvent.hpp"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <json.hpp>

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
#include <sstream>
#include <dispatch/dispatch.h>
#include <pthread.h>

namespace rig::plugins {

namespace fs = std::filesystem;
using catalog::ComponentSchema;
using catalog::ParamKind;
using catalog::ParamSchema;
using nlohmann::json;

namespace {

constexpr std::size_t kSlots = 4096;      // ids per range
constexpr std::size_t kMaxParams = 160;   // knobs exposed per plugin (the editor covers the rest)

// ───────────────────────────── small helpers ─────────────────────────────
std::string cfString(CFStringRef s) {
    if (!s) return {};
    char buf[512] = {0};
    if (CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8)) return buf;
    return {};
}

std::string hex8(uint32_t v) {
    char b[9];
    std::snprintf(b, sizeof b, "%08x", v);
    return b;
}

std::string makeUid(const AudioComponentDescription& d) {
    return "AU:" + hex8(d.componentType) + ":" + hex8(d.componentSubType) + ":" + hex8(d.componentManufacturer);
}

bool parseUid(const std::string& uid, AudioComponentDescription& d) {
    unsigned a = 0, b = 0, c = 0;
    if (uid.size() < 3 || uid.compare(0, 3, "AU:") != 0) return false;
    if (std::sscanf(uid.c_str() + 3, "%8x:%8x:%8x", &a, &b, &c) != 3) return false;
    d = AudioComponentDescription{a, b, c, 0, 0};
    return true;
}

std::string kindOf(OSType t) {
    if (t == kAudioUnitType_MusicDevice) return "instrument";
    if (t == kAudioUnitType_MusicEffect) return "music_effect";
    return "effect";
}

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

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string b64encode(const uint8_t* p, std::size_t n) {
    std::string o;
    o.reserve((n + 2) / 3 * 4);
    for (std::size_t i = 0; i < n; i += 3) {
        const uint32_t v = (uint32_t(p[i]) << 16) | (i + 1 < n ? uint32_t(p[i + 1]) << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        o += kB64[(v >> 18) & 63];
        o += kB64[(v >> 12) & 63];
        o += i + 1 < n ? kB64[(v >> 6) & 63] : '=';
        o += i + 2 < n ? kB64[v & 63] : '=';
    }
    return o;
}

std::vector<uint8_t> b64decode(const std::string& s) {
    std::vector<uint8_t> o;
    o.reserve(s.size() * 3 / 4);
    uint32_t acc = 0;
    int bits = 0;
    for (char ch : s) {
        int v;
        if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
        else if (ch == '+') v = 62;
        else if (ch == '/') v = 63;
        else continue; // padding / whitespace
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            o.push_back(static_cast<uint8_t>((acc >> bits) & 0xff));
        }
    }
    return o;
}

// ───────────────────────────── plugin descriptions ─────────────────────────────
struct ParamMeta {
    AudioUnitParameterID au_id = 0;
    std::string id, name, unit;
    float min = 0.0f, max = 1.0f, def = 0.0f;
    bool log = false, indexed = false, boolean = false, expert = false;
    std::vector<std::string> labels;

    float toHost(float native) const noexcept {
        if (!log) return native;
        const float t = std::log(std::max(native, min) / min) / std::log(max / min);
        return std::clamp(t, 0.0f, 1.0f);
    }
    float toNative(float host) const noexcept {
        if (!log) return std::clamp(host, min, max);
        return min * std::pow(max / min, std::clamp(host, 0.0f, 1.0f));
    }
};

struct Entry {
    PluginInfo info;
    AudioComponentDescription desc{};
    bool missing = false;               // not installed: units are pass-through placeholders
    std::atomic<bool> persisted{false}; // shown in the Components rack
    std::vector<ParamMeta> meta;
    std::vector<ParamSchema> params;
    std::string ref_name, disp_name;
    ComponentSchema schema{};

    // Builds the catalog views. `meta` and the strings must not change afterwards.
    void finalize(int32_t tid) {
        info.table_id = tid;
        ref_name = info.name.empty() ? std::string("Plugin") : info.name;
        const std::string what = info.kind == "instrument" ? "instrument" : info.kind == "music_effect" ? "music effect" : "effect";
        disp_name = missing ? (info.vendor.empty() ? "Plugin not installed" : info.vendor + " · not installed")
                            : (info.vendor.empty() ? "" : info.vendor + " · ") + std::string(info.v3 ? "AUv3 " : "Audio Unit ") + what;
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

// ───────────────────────────── registry ─────────────────────────────
struct Registry {
    std::array<std::atomic<Entry*>, kSlots> imported{};
    std::array<std::atomic<Entry*>, kSlots> transient{};
    std::mutex m;                          // guards everything below (never taken on the audio thread)
    std::map<std::string, int32_t> by_uid; // every registered entry (imported, removed, session-only)
    std::vector<std::unique_ptr<Entry>> owned; // entries live for the whole process (schemas are referenced)
    int32_t next_imported = kImportedBase;
    int32_t next_transient = kTransientBase;
    std::string store_path;
    bool loaded = false;
};
Registry& reg() {
    static Registry* r = new Registry(); // intentionally immortal: schemas outlive static destruction order
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

// Caller holds reg().m.
void saveLocked() {
    auto& r = reg();
    json plugins = json::array(), retired = json::object();
    for (const auto& e : r.owned) {
        const int32_t tid = e->info.table_id;
        if (tid < kImportedBase || tid >= kTransientBase) continue;
        if (lookup(tid) != e.get()) continue; // superseded by a refresh
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

bool installed(const AudioComponentDescription& d) {
    AudioComponentDescription q = d;
    return AudioComponentFindNext(nullptr, &q) != nullptr;
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
                if (!parseUid(e->info.uid, e->desc)) continue;
                e->missing = !installed(e->desc);
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
            // Keep the id reserved for this plugin: a later re-import gets it back.
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

// ───────────────────────────── AU introspection ─────────────────────────────
std::string unitLabel(const AudioUnitParameterInfo& pi) {
    switch (pi.unit) {
        case kAudioUnitParameterUnit_Percent:
        case kAudioUnitParameterUnit_EqualPowerCrossfade: return "%"; // Apple dry/wet mixes (0..100)
        case kAudioUnitParameterUnit_Seconds: return "s";
        case kAudioUnitParameterUnit_SampleFrames: return "smp";
        case kAudioUnitParameterUnit_Phase: return "°";
        case kAudioUnitParameterUnit_Rate: return "x";
        case kAudioUnitParameterUnit_Hertz: return "Hz";
        case kAudioUnitParameterUnit_Cents: return "ct";
        case kAudioUnitParameterUnit_RelativeSemiTones: return "st";
        case kAudioUnitParameterUnit_Decibels: return "dB";
        case kAudioUnitParameterUnit_Degrees: return "°";
        case kAudioUnitParameterUnit_Meters: return "m";
        case kAudioUnitParameterUnit_AbsoluteCents: return "ct";
        case kAudioUnitParameterUnit_Octaves: return "oct";
        case kAudioUnitParameterUnit_BPM: return "bpm";
        case kAudioUnitParameterUnit_Beats: return "beats";
        case kAudioUnitParameterUnit_Milliseconds: return "ms";
        case kAudioUnitParameterUnit_Ratio: return ":1";
        case kAudioUnitParameterUnit_CustomUnit: return cfString(pi.unitName);
        default: return "";
    }
}

std::vector<ParamMeta> readParams(AudioUnit u) {
    std::vector<ParamMeta> out, expert;
    UInt32 sz = 0;
    if (AudioUnitGetPropertyInfo(u, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, &sz, nullptr) != noErr || sz == 0) return out;
    std::vector<AudioUnitParameterID> ids(sz / sizeof(AudioUnitParameterID));
    if (AudioUnitGetProperty(u, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, ids.data(), &sz) != noErr) return out;
    ids.resize(sz / sizeof(AudioUnitParameterID));
    for (const auto id : ids) {
        AudioUnitParameterInfo pi{};
        UInt32 isz = sizeof(pi);
        if (AudioUnitGetProperty(u, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global, id, &pi, &isz) != noErr) continue;
        ParamMeta m;
        if ((pi.flags & kAudioUnitParameterFlag_HasCFNameString) && pi.cfNameString) {
            m.name = cfString(pi.cfNameString);
            if (pi.flags & kAudioUnitParameterFlag_CFNameRelease) CFRelease(pi.cfNameString);
        }
        if (m.name.empty()) m.name = std::string(pi.name, strnlen(pi.name, sizeof(pi.name)));
        const bool writable = (pi.flags & kAudioUnitParameterFlag_IsWritable) != 0;
        if (!writable || !(pi.maxValue > pi.minValue) || !std::isfinite(pi.minValue) || !std::isfinite(pi.maxValue)) {
            if (pi.unit == kAudioUnitParameterUnit_CustomUnit && pi.unitName) CFRelease(pi.unitName);
            continue; // meters and other read-only values
        }
        m.au_id = id;
        m.id = std::to_string(id);
        m.unit = unitLabel(pi);
        if (pi.unit == kAudioUnitParameterUnit_CustomUnit && pi.unitName) CFRelease(pi.unitName);
        m.min = pi.minValue;
        m.max = pi.maxValue;
        m.def = std::clamp(pi.defaultValue, pi.minValue, pi.maxValue);
        m.boolean = pi.unit == kAudioUnitParameterUnit_Boolean;
        m.indexed = pi.unit == kAudioUnitParameterUnit_Indexed || pi.unit == kAudioUnitParameterUnit_MIDINoteNumber;
        m.expert = (pi.flags & kAudioUnitParameterFlag_ExpertMode) != 0;
        m.log = !m.boolean && !m.indexed && m.min > 0.0f && m.max / m.min >= 20.0f &&
                ((pi.flags & (kAudioUnitParameterFlag_DisplayLogarithmic | kAudioUnitParameterFlag_DisplayExponential)) ||
                 pi.unit == kAudioUnitParameterUnit_Hertz);
        if (pi.unit == kAudioUnitParameterUnit_Indexed) {
            CFArrayRef arr = nullptr;
            UInt32 asz = sizeof(arr);
            if (AudioUnitGetProperty(u, kAudioUnitProperty_ParameterValueStrings, kAudioUnitScope_Global, id, &arr, &asz) == noErr && arr) {
                for (CFIndex i = 0; i < CFArrayGetCount(arr); ++i)
                    m.labels.push_back(cfString(static_cast<CFStringRef>(CFArrayGetValueAtIndex(arr, i))));
                CFRelease(arr);
            }
            if (m.labels.size() != static_cast<std::size_t>(std::lround(m.max - m.min)) + 1) m.labels.clear();
        }
        (m.expert ? expert : out).push_back(std::move(m));
    }
    for (auto& m : expert) out.push_back(std::move(m));
    if (out.size() > kMaxParams) out.resize(kMaxParams);
    return out;
}

AudioStreamBasicDescription floatFormat(double sr, UInt32 ch) {
    AudioStreamBasicDescription f{};
    f.mSampleRate = sr;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = static_cast<AudioFormatFlags>(kAudioFormatFlagsNativeFloatPacked) | static_cast<AudioFormatFlags>(kAudioFormatFlagIsNonInterleaved);
    f.mBytesPerPacket = f.mBytesPerFrame = sizeof(float);
    f.mFramesPerPacket = 1;
    f.mChannelsPerFrame = ch;
    f.mBitsPerChannel = 32;
    return f;
}

UInt32 elementCount(AudioUnit u, AudioUnitScope scope) {
    UInt32 n = 0, sz = sizeof(n);
    if (AudioUnitGetProperty(u, kAudioUnitProperty_ElementCount, scope, 0, &n, &sz) != noErr) return scope == kAudioUnitScope_Input ? 1 : 1;
    return n;
}

// Tries the channel layouts a guitar rack wants, best first. Leaves the unit initialized on success.
bool configureAndInitialize(AudioUnit u, bool has_input, double sr, UInt32 max_frames, UInt32& in_ch, UInt32& out_ch) {
    AudioUnitSetProperty(u, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &max_frames, sizeof(max_frames));
    static const UInt32 kFx[][2] = {{2, 2}, {1, 2}, {1, 1}, {2, 1}};
    static const UInt32 kGen[][2] = {{0, 2}, {0, 1}};
    const auto* cands = has_input ? kFx : kGen;
    const int nc = has_input ? 4 : 2;
    for (int i = 0; i < nc; ++i) {
        const UInt32 ic = cands[i][0], oc = cands[i][1];
        if (ic) {
            const auto fi = floatFormat(sr, ic);
            if (AudioUnitSetProperty(u, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &fi, sizeof(fi)) != noErr) continue;
        }
        const auto fo = floatFormat(sr, oc);
        if (AudioUnitSetProperty(u, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &fo, sizeof(fo)) != noErr) continue;
        if (AudioUnitInitialize(u) == noErr) {
            in_ch = ic;
            out_ch = oc;
            return true;
        }
    }
    return false;
}

// AUv2 plugins (JUCE, iPlug, … — most of them) expect to be created, initialized and disposed on the main
// thread, as Logic does; disposing a JUCE AU on a background thread deadlocks or kills the process. When
// the app's run loop is available (Headroom.app installs editor hooks) that work is marshalled to the main
// thread; command-line tools and tests run it inline. AUv3 units are out-of-process and stay off-main.
std::atomic<bool> g_main_dispatch{false};

void runOnMain(bool v3, const std::function<void()>& fn) {
    if (v3 || !g_main_dispatch.load() || pthread_main_np()) {
        fn();
        return;
    }
    dispatch_sync_f(dispatch_get_main_queue(), const_cast<std::function<void()>*>(&fn),
                    [](void* ctx) { (*static_cast<std::function<void()>*>(ctx))(); });
}

// Instantiates the AU once to capture its description and parameter list.
std::unique_ptr<Entry> describeAU(const AudioComponentDescription& want, std::string* err) {
    AudioComponentDescription q = want;
    AudioComponent comp = AudioComponentFindNext(nullptr, &q);
    if (!comp) {
        if (err) *err = "not installed";
        return nullptr;
    }
    auto e = std::make_unique<Entry>();
    AudioComponentGetDescription(comp, &e->desc);
    CFStringRef name = nullptr;
    AudioComponentCopyName(comp, &name);
    const std::string full = cfString(name);
    if (name) CFRelease(name);
    const auto colon = full.find(": ");
    e->info.vendor = colon == std::string::npos ? std::string() : full.substr(0, colon);
    e->info.name = colon == std::string::npos ? full : full.substr(colon + 2);
    e->info.uid = makeUid(e->desc);
    e->info.format = "AU";
    e->info.kind = kindOf(e->desc.componentType);
    AudioComponentGetVersion(comp, &e->info.version);
    e->info.v3 = (e->desc.componentFlags & kAudioComponentFlag_IsV3AudioUnit) != 0;
    const char* failure = nullptr;
    runOnMain(e->info.v3, [&] {
        AudioUnit u = nullptr;
        if (AudioComponentInstanceNew(comp, &u) != noErr || !u) {
            failure = "could not instantiate";
            return;
        }
        UInt32 ic = 0, oc = 0;
        const bool has_input = e->desc.componentType != kAudioUnitType_MusicDevice && elementCount(u, kAudioUnitScope_Input) > 0;
        if (!configureAndInitialize(u, has_input, 48000.0, 512, ic, oc)) {
            AudioComponentInstanceDispose(u);
            failure = "no usable mono/stereo configuration";
            return;
        }
        e->meta = readParams(u);
        AudioUnitUninitialize(u);
        AudioComponentInstanceDispose(u);
    });
    if (failure) {
        if (err) *err = failure;
        return nullptr;
    }
    return e;
}

// ───────────────────────────── instances & editors ─────────────────────────────
struct AUHandle {
    AudioUnit unit = nullptr;
    int id = 0;
    std::string title;
    bool v3 = false;
    ~AUHandle() {
        if (unit) {
            AudioUnitUninitialize(unit);
            AudioComponentInstanceDispose(unit);
        }
    }
};

// Drops a reference without blocking; the AudioUnit is disposed on the main thread when that applies.
void releaseHandle(std::shared_ptr<AUHandle>&& h) {
    if (!h) return;
    if (h->v3 || !g_main_dispatch.load() || pthread_main_np()) {
        h.reset();
        return;
    }
    dispatch_async_f(dispatch_get_main_queue(), new std::shared_ptr<AUHandle>(std::move(h)),
                     [](void* ctx) { delete static_cast<std::shared_ptr<AUHandle>*>(ctx); });
}

std::mutex g_inst_m;
std::map<int, std::weak_ptr<AUHandle>> g_insts;
std::set<int> g_editors_open;
std::atomic<int> g_next_inst{1};
std::atomic<EditorOpenFn> g_open_hook{nullptr};
std::atomic<EditorCloseFn> g_close_hook{nullptr};

std::string formatNative(const ParamMeta& m, float v) {
    char b[64];
    if (m.boolean) return v >= 0.5f ? "On" : "Off";
    if (m.indexed) {
        const long i = std::lround(v - m.min);
        if (i >= 0 && static_cast<std::size_t>(i) < m.labels.size()) return m.labels[static_cast<std::size_t>(i)];
        std::snprintf(b, sizeof b, "%ld", std::lround(v));
        return b;
    }
    const std::string& u = m.unit;
    if (u == "Hz") {
        if (std::abs(v) >= 1000.0f) std::snprintf(b, sizeof b, "%.2f kHz", v / 1000.0f);
        else std::snprintf(b, sizeof b, std::abs(v) >= 100.0f ? "%.0f Hz" : "%.1f Hz", v);
        return b;
    }
    if (u == "%") { std::snprintf(b, sizeof b, "%.0f%%", v); return b; }
    if (u == "dB") { std::snprintf(b, sizeof b, "%.1f dB", v); return b; }
    if (u == "ms") { std::snprintf(b, sizeof b, std::abs(v) >= 100.0f ? "%.0f ms" : "%.1f ms", v); return b; }
    if (u == "s") { std::snprintf(b, sizeof b, std::abs(v) < 1.0f ? "%.0f ms" : "%.2f s", std::abs(v) < 1.0f ? v * 1000.0f : v); return b; }
    const float span = m.max - m.min;
    const char* f = span <= 2.0f ? "%.2f" : span <= 20.0f ? "%.1f" : "%.0f";
    std::snprintf(b, sizeof b, f, v);
    std::string s = b;
    if (!u.empty()) s += (u == ":1" ? "" : " ") + u;
    return s;
}

// ───────────────────────────── the rack unit ─────────────────────────────
class PluginComponent final : public IComponent {
public:
    explicit PluginComponent(const Entry* e) : e_(e) {
        if (!e_->missing) instantiate();
    }

    ~PluginComponent() override {
        if (!au_) return;
        AudioUnit u = au_->unit;
        // An open editor may keep the unit alive: detach every callback that points at this object.
        if (has_input_) {
            AURenderCallbackStruct cb{nullptr, nullptr};
            AudioUnitSetProperty(u, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof(cb));
        }
        HostCallbackInfo hci{};
        AudioUnitSetProperty(u, kAudioUnitProperty_HostCallbacks, kAudioUnitScope_Global, 0, &hci, sizeof(hci));
        const int id = au_->id;
        {
            std::lock_guard<std::mutex> lk(g_inst_m);
            g_insts.erase(id);
        }
        if (auto close = g_close_hook.load()) close(id);
        // Never block here (we may hold the rack lock): disposal happens on the main thread, or right
        // away outside the app. An open editor keeps its own reference until its window closes.
        releaseHandle(std::move(au_));
    }

    const Entry& entry() const noexcept { return *e_; }
    bool placeholder() const noexcept { return !au_; }
    int instanceId() const noexcept { return au_ ? au_->id : -1; }
    double latencySec() const noexcept { return latency_; }
    double tailSec() const noexcept { return tail_; }
    UInt32 inChannels() const noexcept { return in_ch_; }
    UInt32 outChannels() const noexcept { return out_ch_; }
    uint32_t renderErrors() const noexcept { return errors_.load(std::memory_order_relaxed); }
    bool initialized() const noexcept { return initialized_; }
    void* audioUnit() const noexcept { return au_ ? au_->unit : nullptr; }

    int32_t tableId() const noexcept override { return e_->schema.table_id; }
    std::string_view referenceName() const noexcept override { return e_->schema.reference_name; }
    std::string_view displayName() const noexcept override { return e_->schema.display_name; }
    catalog::EngineFamily family() const noexcept override { return e_->schema.family; }
    const catalog::ComponentSchema& schema() const noexcept override { return e_->schema; }

    void prepare(double sample_rate, uint32_t max_block_size) override {
        const double sr = sample_rate > 1000.0 ? sample_rate : 48000.0;
        const uint32_t mbs = std::clamp<uint32_t>(std::max<uint32_t>(max_block_size, 512), 512, static_cast<uint32_t>(kMaxBlockSize));
        if (prepared_ && sr == sr_ && mbs == mbs_) return;
        sr_ = sr;
        mbs_ = mbs;
        prepared_ = true;
        mono_.assign(mbs_, 0.0f);
        spare_.assign(mbs_, 0.0f);
        if (!au_) return;
        AudioUnit u = au_->unit;
        runOnMain(e_->info.v3, [&] {
            if (initialized_) AudioUnitUninitialize(u);
            initialized_ = configureAndInitialize(u, has_input_, sr_, mbs_, in_ch_, out_ch_);
        });
        if (!initialized_) {
            std::fprintf(stderr, "[plugins] %s: no usable channel configuration at %.0f Hz\n", e_->info.name.c_str(), sr_);
            return;
        }
        Float64 v = 0.0;
        UInt32 sz = sizeof(v);
        latency_ = AudioUnitGetProperty(u, kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0, &v, &sz) == noErr ? v : 0.0;
        sz = sizeof(v);
        tail_ = AudioUnitGetProperty(u, kAudioUnitProperty_TailTime, kAudioUnitScope_Global, 0, &v, &sz) == noErr ? v : 0.0;
        if (e_->info.kind == "instrument") {
            // Pitch bend range 12 semitones (RPN 0), matching the Pitch → MIDI detector's bends.
            MusicDeviceMIDIEvent(u, 0xB0, 101, 0, 0);
            MusicDeviceMIDIEvent(u, 0xB0, 100, 0, 0);
            MusicDeviceMIDIEvent(u, 0xB0, 6, 12, 0);
        }
        ts_ = AudioTimeStamp{};
        ts_.mFlags = kAudioTimeStampSampleTimeValid;
        if (!pending_state_.empty()) {
            applyState(pending_state_);
            pending_state_.clear();
        }
    }

    void reset() override {
        if (au_ && initialized_) AudioUnitReset(au_->unit, kAudioUnitScope_Global, 0);
    }

    void setParameter(std::string_view id, float value) override {
        if (id == "Pwr" || id == "on_off") {
            bypassed_ = value < 0.5f;
            return;
        }
        if (!au_) {
            hold(id, false, value, {});
            return;
        }
        const ParamMeta* m = e_->metaFor(id);
        if (!m) return;
        AudioUnitSetParameter(au_->unit, m->au_id, kAudioUnitScope_Global, 0, m->toNative(value), 0);
        AudioUnitParameter p{au_->unit, m->au_id, kAudioUnitScope_Global, 0};
        AUParameterListenerNotify(nullptr, nullptr, &p); // keeps an open editor in sync
    }

    void setStringParameter(std::string_view id, std::string_view value) override {
        if (!au_) {
            hold(id, true, 0.0f, value);
            return;
        }
        if (id != "state" || value.empty()) return;
        if (initialized_) applyState(std::string(value));
        else pending_state_ = std::string(value);
    }

    float getParameter(std::string_view id) const override {
        if (id == "Pwr" || id == "on_off") return bypassed_ ? 0.0f : 1.0f;
        const ParamMeta* m = e_->metaFor(id);
        if (!au_) {
            for (const auto& h : held_)
                if (h.id == id && !h.is_string) return h.num;
            return m ? m->toHost(m->def) : 0.0f;
        }
        if (!m) return 0.0f;
        AudioUnitParameterValue v = m->def;
        AudioUnitGetParameter(au_->unit, m->au_id, kAudioUnitScope_Global, 0, &v);
        return m->toHost(v);
    }

    std::string getStringParameter(std::string_view id) const override {
        if (!au_) {
            for (const auto& h : held_)
                if (h.id == id && h.is_string) return h.str;
            return {};
        }
        if (id == "state") return stateB64();
        if (id == "plugin_id") return e_->info.uid;
        if (id == "plugin_name") return e_->info.name;
        if (id == "plugin_vendor") return e_->info.vendor;
        return {};
    }

    std::vector<ExportedParam> exportParams() const override {
        std::vector<ExportedParam> out;
        auto str = [&](const char* id, std::string v) { out.push_back({id, true, 0.0f, std::move(v)}); };
        if (!au_) {
            // Placeholder: write back exactly what the preset carried (state included).
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
        str("plugin_id", e_->info.uid);
        str("plugin_name", e_->info.name);
        str("plugin_vendor", e_->info.vendor);
        str("plugin_format", e_->info.format);
        if (std::string s = stateB64(); !s.empty()) str("state", std::move(s));
        for (const auto& m : e_->meta) out.push_back({m.id, false, getParameter(m.id), {}});
        out.push_back({"Pwr", false, bypassed_ ? 0.0f : 1.0f, {}});
        return out;
    }

    void applyModulationDelta(std::string_view, float) override {}
    void clearModulationDeltas() override {}
    void processControlRate() override {}

    // Audio thread.
    void processAudio(AudioBlock& b) override {
        const uint32_t n = b.num_frames;
        if (!initialized_ || n == 0 || !b.out || !b.out[0]) {
            b.copyInToOut();
            return;
        }
        AudioUnit u = au_->unit;
        bpm_ = b.bpm > 0.0 ? b.bpm : 120.0;
        const bool stereo_host = b.num_channels > 1 && b.out[1];
        float* outL = b.out[0];
        float* outR = stereo_host ? b.out[1] : spare_.data();
        const float* inL = b.in && b.in[0] ? b.in[0] : nullptr;
        const float* inR = (b.in && b.num_channels > 1 && b.in[1]) ? b.in[1] : inL;
        if (b.midi && wants_midi_) { // instruments and music effects read the lane's MIDI bus
            for (const auto& ev : *b.midi) sendMidi(u, ev);
        }
        uint32_t done = 0;
        while (done < n) {
            const uint32_t k = std::min<uint32_t>(n - done, mbs_);
            if (has_input_) {
                if (!inL) {
                    std::memset(mono_.data(), 0, k * sizeof(float));
                    cur_in_[0] = cur_in_[1] = mono_.data();
                } else if (in_ch_ == 1) {
                    for (uint32_t i = 0; i < k; ++i) mono_[i] = 0.5f * (inL[done + i] + inR[done + i]);
                    cur_in_[0] = cur_in_[1] = mono_.data();
                } else {
                    cur_in_[0] = inL + done;
                    cur_in_[1] = inR + done;
                }
                cur_frames_ = k;
            }
            alignas(16) uint8_t mem[sizeof(AudioBufferList) + sizeof(AudioBuffer)];
            auto* abl = reinterpret_cast<AudioBufferList*>(mem);
            abl->mNumberBuffers = out_ch_;
            abl->mBuffers[0] = {1, static_cast<UInt32>(k * sizeof(float)), outL + done};
            if (out_ch_ > 1) abl->mBuffers[1] = {1, static_cast<UInt32>(k * sizeof(float)), outR + (stereo_host ? done : 0)};
            AudioUnitRenderActionFlags flags = 0;
            const OSStatus st = AudioUnitRender(u, &flags, &ts_, 0, k, abl);
            if (st != noErr) {
                errors_.fetch_add(1, std::memory_order_relaxed);
                for (uint32_t i = 0; i < k; ++i) {
                    outL[done + i] = inL ? inL[done + i] : 0.0f;
                    if (stereo_host) outR[done + i] = inR ? inR[done + i] : 0.0f;
                }
            } else {
                // Units may hand back their own buffers instead of rendering into ours.
                if (abl->mBuffers[0].mData && abl->mBuffers[0].mData != outL + done)
                    std::memcpy(outL + done, abl->mBuffers[0].mData, k * sizeof(float));
                if (out_ch_ > 1 && stereo_host && abl->mBuffers[1].mData && abl->mBuffers[1].mData != outR + done)
                    std::memcpy(outR + done, abl->mBuffers[1].mData, k * sizeof(float));
                if (out_ch_ == 1 && stereo_host) std::memcpy(outR + done, outL + done, k * sizeof(float));
            }
            ts_.mSampleTime += k;
            done += k;
        }
        // Speaker safety: a misbehaving plugin must never push NaN/Inf into the rest of the rig.
        float acc = 0.0f;
        for (uint32_t i = 0; i < n; ++i) acc += outL[i] * 0.0f + (stereo_host ? outR[i] * 0.0f : 0.0f);
        if (!std::isfinite(acc)) {
            std::memset(outL, 0, n * sizeof(float));
            if (stereo_host) std::memset(outR, 0, n * sizeof(float));
            errors_.fetch_add(1, std::memory_order_relaxed);
        }
    }

private:
    void instantiate() {
        AudioComponentDescription q = e_->desc;
        AudioComponent comp = AudioComponentFindNext(nullptr, &q);
        AudioUnit u = nullptr;
        OSStatus st = -1;
        if (comp) runOnMain(e_->info.v3, [&] { st = AudioComponentInstanceNew(comp, &u); });
        if (!comp || st != noErr || !u) {
            std::fprintf(stderr, "[plugins] could not instantiate %s (%s); using a pass-through placeholder\n", e_->info.name.c_str(), e_->info.uid.c_str());
            return;
        }
        au_ = std::make_shared<AUHandle>();
        au_->unit = u;
        au_->id = g_next_inst.fetch_add(1);
        au_->title = e_->info.name;
        au_->v3 = e_->info.v3;
        {
            std::lock_guard<std::mutex> lk(g_inst_m);
            g_insts[au_->id] = au_;
        }
        has_input_ = e_->desc.componentType != kAudioUnitType_MusicDevice && elementCount(u, kAudioUnitScope_Input) > 0;
        wants_midi_ = e_->desc.componentType == kAudioUnitType_MusicDevice || e_->desc.componentType == kAudioUnitType_MusicEffect;
        if (has_input_) {
            AURenderCallbackStruct cb{&PluginComponent::inputProc, this};
            AudioUnitSetProperty(u, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof(cb));
        }
        HostCallbackInfo hci{};
        hci.hostUserData = this;
        hci.beatAndTempoProc = &PluginComponent::beatAndTempo;
        AudioUnitSetProperty(u, kAudioUnitProperty_HostCallbacks, kAudioUnitScope_Global, 0, &hci, sizeof(hci));
    }

    static OSStatus inputProc(void* ref, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 frames, AudioBufferList* io) {
        auto* self = static_cast<PluginComponent*>(ref);
        const UInt32 have = std::min<UInt32>(frames, self->cur_frames_);
        for (UInt32 c = 0; c < io->mNumberBuffers; ++c) {
            const float* src = self->cur_in_[std::min<UInt32>(c, 1)];
            auto& buf = io->mBuffers[c];
            if (!buf.mData) {
                if (have == frames && src) {
                    buf.mData = const_cast<float*>(src);
                    buf.mDataByteSize = frames * sizeof(float);
                    continue;
                }
                return kAudioUnitErr_InvalidParameter;
            }
            auto* dst = static_cast<float*>(buf.mData);
            if (src) std::memcpy(dst, src, have * sizeof(float));
            else std::memset(dst, 0, have * sizeof(float));
            if (frames > have) std::memset(dst + have, 0, (frames - have) * sizeof(float));
            buf.mDataByteSize = frames * sizeof(float);
        }
        return noErr;
    }

    static OSStatus beatAndTempo(void* ud, Float64* beat, Float64* tempo) {
        auto* self = static_cast<PluginComponent*>(ud);
        if (!self) return kAudioUnitErr_CannotDoInCurrentContext;
        if (tempo) *tempo = self->bpm_;
        if (beat) *beat = self->ts_.mSampleTime / std::max(1.0, self->sr_) * self->bpm_ / 60.0;
        return noErr;
    }

    static void sendMidi(AudioUnit u, const midi::MidiEvent& e) {
        if (e.type == midi::MidiEvent::NoteOn)
            MusicDeviceMIDIEvent(u, 0x90, static_cast<UInt32>(e.note & 127), static_cast<UInt32>(std::clamp(static_cast<int>(e.velocity * 127.0f), 1, 127)), e.offset);
        else if (e.type == midi::MidiEvent::NoteOff)
            MusicDeviceMIDIEvent(u, 0x80, static_cast<UInt32>(e.note & 127), 0, e.offset);
        else {
            const int v = std::clamp(8192 + static_cast<int>(e.bend / 12.0f * 8191.0f), 0, 16383);
            MusicDeviceMIDIEvent(u, 0xE0, static_cast<UInt32>(v & 127), static_cast<UInt32>((v >> 7) & 127), e.offset);
        }
    }

    std::string stateB64() const {
        if (!au_) return {};
        CFPropertyListRef pl = nullptr;
        UInt32 sz = sizeof(pl);
        if (AudioUnitGetProperty(au_->unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &pl, &sz) != noErr || !pl) return {};
        std::string out;
        if (CFDataRef d = CFPropertyListCreateData(nullptr, pl, kCFPropertyListBinaryFormat_v1_0, 0, nullptr)) {
            out = b64encode(CFDataGetBytePtr(d), static_cast<std::size_t>(CFDataGetLength(d)));
            CFRelease(d);
        }
        CFRelease(pl);
        return out;
    }

    bool applyState(const std::string& b64) {
        const auto bytes = b64decode(b64);
        if (bytes.empty() || !au_) return false;
        CFDataRef d = CFDataCreate(nullptr, bytes.data(), static_cast<CFIndex>(bytes.size()));
        if (!d) return false;
        CFPropertyListRef pl = CFPropertyListCreateWithData(nullptr, d, kCFPropertyListImmutable, nullptr, nullptr);
        CFRelease(d);
        if (!pl) return false;
        const OSStatus st = AudioUnitSetProperty(au_->unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &pl, sizeof(pl));
        CFRelease(pl);
        if (st != noErr) {
            std::fprintf(stderr, "[plugins] %s rejected its saved state (%d)\n", e_->info.name.c_str(), static_cast<int>(st));
            return false;
        }
        AudioUnitParameter any{au_->unit, kAUParameterListener_AnyParameter, kAudioUnitScope_Global, 0};
        AUParameterListenerNotify(nullptr, nullptr, &any);
        return true;
    }

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
    std::shared_ptr<AUHandle> au_;
    bool has_input_ = true;
    bool wants_midi_ = false;
    bool initialized_ = false;
    bool prepared_ = false;
    UInt32 in_ch_ = 2, out_ch_ = 2;
    double sr_ = 48000.0;
    uint32_t mbs_ = 512;
    double latency_ = 0.0, tail_ = 0.0;
    double bpm_ = 120.0;
    std::vector<float> mono_, spare_;
    const float* cur_in_[2] = {nullptr, nullptr};
    uint32_t cur_frames_ = 0;
    AudioTimeStamp ts_{};
    std::atomic<uint32_t> errors_{0};
    std::string pending_state_;
    std::vector<ExportedParam> held_; // placeholder storage (missing plugin)
};

std::mutex g_pool_m;
std::map<int32_t, std::unique_ptr<IComponent>> g_pool;

int32_t allocTransientLocked() {
    auto& r = reg();
    if (r.next_transient >= kTransientBase + static_cast<int32_t>(kSlots)) return 0;
    return r.next_transient++;
}

// Resolves a preset's plugin identity to a local table id, registering session-only entries as needed.
int32_t resolve(const std::string& uid, const std::string& name, const std::string& vendor, int32_t hint) {
    ensureLoaded();
    auto& r = reg();
    {
        std::lock_guard<std::mutex> lk(r.m);
        if (uid.empty()) {
            if (lookup(hint)) return hint;
        } else if (auto it = r.by_uid.find(uid); it != r.by_uid.end() && lookup(it->second)) {
            return it->second;
        }
    }
    std::unique_ptr<Entry> e;
    AudioComponentDescription d{};
    if (parseUid(uid, d)) e = describeAU(d, nullptr);
    if (!e) {
        e = std::make_unique<Entry>();
        e->missing = true;
        e->info.uid = uid.empty() ? "unknown:" + std::to_string(hint) : uid;
        e->info.format = uid.rfind("VST3:", 0) == 0 ? "VST3" : "AU";
        e->info.name = name.empty() ? "Missing plugin" : name;
        e->info.vendor = vendor;
        e->info.kind = "effect";
        e->desc = d;
    }
    std::lock_guard<std::mutex> lk(r.m);
    if (auto it = r.by_uid.find(e->info.uid); it != r.by_uid.end() && lookup(it->second)) return it->second; // raced
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
    for (OSType t : {kAudioUnitType_Effect, kAudioUnitType_MusicEffect, kAudioUnitType_MusicDevice}) {
        AudioComponentDescription d{t, 0, 0, 0, 0};
        AudioComponent c = nullptr;
        while ((c = AudioComponentFindNext(c, &d))) {
            AudioComponentDescription cd{};
            AudioComponentGetDescription(c, &cd);
            // Never offer Headroom's own AU/plugin build inside Headroom: loading a second copy of the engine
            // in-process collides on process-wide registrations (NAM config parsers) and makes no sense.
            if (cd.componentManufacturer == 'Hdrm') continue;
            PluginInfo p;
            CFStringRef name = nullptr;
            AudioComponentCopyName(c, &name);
            const std::string full = cfString(name);
            if (name) CFRelease(name);
            const auto colon = full.find(": ");
            p.vendor = colon == std::string::npos ? std::string() : full.substr(0, colon);
            p.name = colon == std::string::npos ? full : full.substr(colon + 2);
            p.uid = makeUid(cd);
            p.format = "AU";
            p.kind = kindOf(cd.componentType);
            AudioComponentGetVersion(c, &p.version);
            p.v3 = (cd.componentFlags & kAudioComponentFlag_IsV3AudioUnit) != 0;
            {
                std::lock_guard<std::mutex> lk(reg().m);
                if (auto it = reg().by_uid.find(p.uid); it != reg().by_uid.end()) {
                    const Entry* e = lookup(it->second);
                    if (e && e->persisted.load()) p.table_id = it->second;
                }
            }
            out.push_back(std::move(p));
        }
    }
    // VST3 bundles: listed so the import dialog can show them; hosting them is not implemented yet.
    const std::vector<std::string> dirs = platform::vst3Dirs();
    for (const auto& dir : dirs) {
        std::error_code ec;
        for (const auto& it : fs::directory_iterator(dir, ec)) {
            if (it.path().extension() != ".vst3") continue;
            if (it.path().stem() == "Headroom") continue;  // never list Headroom's own plugin build
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

int32_t importPlugin(const std::string& uid, std::string* err) {
    ensureLoaded();
    AudioComponentDescription d{};
    if (!parseUid(uid, d)) {
        if (err) *err = uid.rfind("VST3:", 0) == 0 ? "VST3 hosting is not available yet" : "unknown plugin id";
        return 0;
    }
    auto e = describeAU(d, err);
    if (!e) return 0;
    auto& r = reg();
    std::lock_guard<std::mutex> lk(r.m);
    int32_t tid = 0;
    if (auto it = r.by_uid.find(uid); it != r.by_uid.end() && it->second >= kImportedBase && it->second < kTransientBase) tid = it->second; // refresh / re-import
    if (!tid) {
        if (r.next_imported >= kImportedBase + static_cast<int32_t>(kSlots)) {
            if (err) *err = "too many imported plugins";
            return 0;
        }
        tid = r.next_imported++;
    }
    e->persisted.store(true);
    if (Entry* old = lookup(tid)) old->persisted.store(false);
    publish(std::move(e), tid);
    saveLocked();
    return tid;
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
    const Entry* e = lookup(table_id);
    if (!e) return "Hosted plugin";
    if (e->missing) return "Plugin not installed on this Mac — passing audio through";
    std::string v = std::to_string((e->info.version >> 16) & 0xffff) + "." + std::to_string((e->info.version >> 8) & 0xff) + "." +
                    std::to_string(e->info.version & 0xff);
    return std::string(e->info.v3 ? "AUv3" : "Audio Unit") + " · " + (e->info.vendor.empty() ? "" : e->info.vendor + " · ") + "v" + v;
}

std::unique_ptr<IComponent> createComponent(int32_t table_id) {
    {
        std::lock_guard<std::mutex> lk(g_pool_m);
        if (auto it = g_pool.find(table_id); it != g_pool.end()) {
            auto c = std::move(it->second);
            g_pool.erase(it);
            return c;
        }
    }
    const Entry* e = lookup(table_id);
    if (!e) return nullptr;
    return std::make_unique<PluginComponent>(e);
}

void preinstantiate(int32_t table_id, double sample_rate, uint32_t max_block_size) {
    const Entry* e = lookup(table_id);
    if (!e || e->missing) return;
    auto c = std::make_unique<PluginComponent>(e);
    c->prepare(sample_rate, max_block_size);
    std::unique_ptr<IComponent> old;
    std::lock_guard<std::mutex> lk(g_pool_m);
    old = std::move(g_pool[table_id]);
    g_pool[table_id] = std::move(c);
}

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
    float mn = p.min_value, mx = p.max_value > p.min_value ? p.max_value : p.min_value + 1.0f;
    const float norm = std::clamp((host - mn) / (mx - mn), 0.0f, 1.0f);
    const float def_norm = std::clamp((p.default_value - mn) / (mx - mn), 0.0f, 1.0f);
    std::string kind = "continuous", fmt;
    if (m) {
        if (m->boolean) kind = "bool";
        else if (m->indexed) kind = "stepped";
        fmt = formatNative(*m, m->toNative(host));
    } else {
        fmt = num(host);
    }
    std::string o = "{\"id\":\"" + jsonEsc(std::string(p.id)) + "\",\"name\":\"" + jsonEsc(std::string(p.display_name)) + "\",\"unit\":\"" +
                    jsonEsc(std::string(p.unit)) + "\",\"min\":" + num(mn) + ",\"max\":" + num(mx) + ",\"val\":" + num(host) +
                    ",\"norm\":" + num(norm) + ",\"default\":" + num(def_norm) + ",\"kind\":\"" + kind + "\",\"fmt\":\"" + jsonEsc(fmt) + "\"";
    if (m && !m->labels.empty()) {
        o += ",\"labels\":[";
        for (std::size_t i = 0; i < m->labels.size(); ++i) o += (i ? ",\"" : "\"") + jsonEsc(m->labels[i]) + "\"";
        o += "]";
    }
    if (m && m->log) o += ",\"scale\":\"log\"";
    return o + "}";
}

int instanceIdOf(const IComponent& c) {
    const auto* pc = dynamic_cast<const PluginComponent*>(&c);
    return pc ? pc->instanceId() : -1;
}

std::string slotJson(const IComponent& c) {
    const auto* pc = dynamic_cast<const PluginComponent*>(&c);
    if (!pc) return "\"plugin\":null";
    const auto& e = pc->entry();
    const int id = pc->instanceId();
    bool open = false;
    {
        std::lock_guard<std::mutex> lk(g_inst_m);
        open = g_editors_open.count(id) > 0;
    }
    std::string o = "\"plugin\":{\"instance\":" + std::to_string(id) + ",\"uid\":\"" + jsonEsc(e.info.uid) + "\",\"format\":\"" + jsonEsc(e.info.format) +
                    "\",\"name\":\"" + jsonEsc(e.info.name) + "\",\"vendor\":\"" + jsonEsc(e.info.vendor) + "\",\"kind\":\"" + jsonEsc(e.info.kind) +
                    "\",\"missing\":" + (pc->placeholder() ? "true" : "false") + ",\"ready\":" + (pc->initialized() ? "true" : "false") +
                    ",\"editor\":" + (pc->placeholder() ? "false" : "true") + ",\"editor_open\":" + (open ? "true" : "false") +
                    ",\"imported\":" + (e.persisted.load() ? "true" : "false") + ",\"in_channels\":" + std::to_string(pc->inChannels()) +
                    ",\"out_channels\":" + std::to_string(pc->outChannels()) + ",\"latency_ms\":" + num(pc->latencySec() * 1000.0) +
                    ",\"tail_s\":" + num(pc->tailSec()) + ",\"render_errors\":" + std::to_string(pc->renderErrors()) + "}";
    return o;
}

void setEditorHooks(EditorOpenFn open, EditorCloseFn close) {
    g_open_hook.store(open);
    g_main_dispatch.store(open != nullptr); // the app's main run loop is available for AUv2 lifecycle work
    g_close_hook.store(close);
}

bool openEditor(int instance_id, std::string* err) {
    auto fn = g_open_hook.load();
    if (!fn) {
        if (err) *err = "plugin editors open in the Headroom app window";
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(g_inst_m);
        auto it = g_insts.find(instance_id);
        if (it == g_insts.end() || it->second.expired()) {
            if (err) *err = "no such plugin instance";
            return false;
        }
    }
    return fn(instance_id, err);
}

bool editorTarget(int instance_id, EditorTarget& out) {
    std::lock_guard<std::mutex> lk(g_inst_m);
    auto it = g_insts.find(instance_id);
    if (it == g_insts.end()) return false;
    auto h = it->second.lock();
    if (!h) return false;
    out.audio_unit = h->unit;
    out.title = h->title;
    out.v3 = h->v3;
    out.keepalive = h;
    return true;
}

void setEditorOpen(int instance_id, bool open) {
    std::lock_guard<std::mutex> lk(g_inst_m);
    if (open) g_editors_open.insert(instance_id);
    else g_editors_open.erase(instance_id);
}

std::string handleApi(const std::string& path, const std::function<std::string(const char*)>& q, int instance) {
    if (path == "/api/plugins") {
        const auto avail = scanInstalled();
        std::string o = "{\"store\":\"" + jsonEsc(storePath()) + "\",\"imported\":[";
        bool first = true;
        for (const auto& p : imported()) {
            const Entry* e = lookup(p.table_id);
            o += (first ? "" : ",") + infoJson(p, e && !e->missing, e ? e->meta.size() : 0);
            first = false;
        }
        o += "],\"available\":[";
        for (std::size_t i = 0; i < avail.size(); ++i) o += (i ? "," : "") + infoJson(avail[i], true, 0);
        o += "],\"vst3\":{\"supported\":false,\"note\":\"VST3 bundles are listed but cannot be hosted yet\"}}";
        return o;
    }
    if (path == "/api/plugins/import") {
        std::string list = q("uid");
        std::string o = "{\"results\":[";
        bool any_ok = false, first = true;
        std::size_t pos = 0;
        while (pos <= list.size()) {
            const std::size_t comma = list.find(',', pos);
            const std::string uid = list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            pos = comma == std::string::npos ? list.size() + 1 : comma + 1;
            if (uid.empty()) continue;
            std::string err;
            const int32_t tid = importPlugin(uid, &err);
            any_ok = any_ok || tid != 0;
            o += std::string(first ? "" : ",") + "{\"uid\":\"" + jsonEsc(uid) + "\",\"table_id\":" + (tid ? std::to_string(tid) : "null") +
                 (tid ? "" : ",\"error\":\"" + jsonEsc(err) + "\"") + "}";
            first = false;
        }
        return o + "],\"ok\":" + (any_ok ? "true" : "false") + "}";
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
        std::string err;
        const bool ok = id >= 0 && openEditor(id, &err);
        if (id < 0) err = "that slot is not a hosted plugin";
        return std::string("{\"ok\":") + (ok ? "true" : "false") + ",\"instance\":" + std::to_string(id) + (ok ? "" : ",\"error\":\"" + jsonEsc(err) + "\"") + "}";
    }
    return "{\"error\":\"not_found\"}";
}

} // namespace rig::plugins
