#include "Library.hpp"
#include "../platform/Paths.hpp"
#include "../generated/ComponentCatalog.hpp"
#include "../midi/MidiComponents.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace rig::library {

namespace {

int64_t fileMtimeSec(const fs::path& p) {
    std::error_code ec;
    const auto ftime = fs::last_write_time(p, ec);
    if (ec) return 0;
    const auto sctp = std::chrono::time_point_cast<std::chrono::seconds>(
        ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    return static_cast<int64_t>(sctp.time_since_epoch().count());
}

std::string jsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': break;
            case '\t': o += ' '; break;
            default:
                if (c < 0x20) o += ' ';
                else o += static_cast<char>(c);
        }
    }
    return o;
}

std::string xmlEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            default: o += c;
        }
    }
    return o;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}


float paramOf(const preset::PresetComponentNode& n, const char* id, float def = 0.5f) {
    for (const auto& p : n.params)
        if (p.id == id) return p.float_val;
    return def;
}

std::string midiInstrumentFamily(int sound) { return rig::instrumentSoundFamily(sound); }

bool isOn(const preset::PresetComponentNode& n) { return paramOf(n, "Pwr", 1.0f) >= 0.5f; }

void flatten(const preset::PresetComponentNode& n, std::vector<const preset::PresetComponentNode*>& out) {
    for (const auto& c : n.children) {
        out.push_back(&c);
        flatten(c, out);
    }
}

bool isAmp(int32_t t) {
    switch (t) {
        case 16000: case 30000: case 34000: case 38000: case 49000: case 54000: case 56000: case 62000:
        case 79000: case 80000: case 81000: case 87000: case 91000: case 92000: case 96000: case 101000:
        case 122000: case 134000: case 150000: case 151000: case 152000: case 162000: case 163000:
        case 172000: case 176000: case 177000: case 300000: return true;
        default: return false;
    }
}

// 0 clean, 1 crunch, 2 high gain, 3 bass
int ampGainClass(const preset::PresetComponentNode& a) {
    const float g = [&] {
        switch (a.table_id) {
            case 34000: return std::max(paramOf(a, "V1"), paramOf(a, "V2", 0.0f));
            case 92000: return std::max(paramOf(a, "V1"), paramOf(a, "V2", 0.0f));
            case 91000: case 56000: case 101000: return paramOf(a, "V1");
            case 134000: return paramOf(a, "L13", 0.0f) >= 0.5f ? paramOf(a, "L1") : paramOf(a, "R0");
            case 122000: return paramOf(a, "O2", 0.0f) >= 0.5f ? paramOf(a, "P1") : paramOf(a, "P0");
            case 163000: return paramOf(a, "channel", 0.0f) >= 0.5f ? paramOf(a, "overdrivePreamp") : paramOf(a, "normalPreamp");
            case 81000: case 80000: return paramOf(a, "g");
            case 16000: case 96000: return paramOf(a, "Gn");
            case 87000: return std::max(paramOf(a, "nv", 0.0f), paramOf(a, "bv", 0.0f));
            case 30000: return paramOf(a, "Vol");
            case 62000: return std::max(paramOf(a, "VB", 0.0f), paramOf(a, "VN", 0.0f));
            case 79000: return std::max(paramOf(a, "vb", 0.0f), paramOf(a, "vn", 0.0f));
            case 38000: return std::max(paramOf(a, "Vol", 0.0f), paramOf(a, "Br", 0.0f));
            default: return paramOf(a, "gain", paramOf(a, "volume", 0.5f));
        }
    }();
    switch (a.table_id) {
        case 54000: case 177000: case 150000: return 3;
        case 134000: return paramOf(a, "L13", 0.0f) >= 0.5f ? (g > 0.2f ? 2 : 1) : (g > 0.55f || paramOf(a, "C16", 0.0f) >= 0.5f ? 1 : 0);
        case 122000: case 163000: {
            const bool od = (a.table_id == 122000) ? paramOf(a, "O2", 0.0f) >= 0.5f : paramOf(a, "channel", 0.0f) >= 0.5f;
            return od ? (g > 0.25f ? 2 : 1) : (g > 0.6f ? 1 : 0);
        }
        case 81000: return paramOf(a, "c", 0.0f) >= 0.5f ? 2 : (g > 0.3f ? 1 : 0);
        case 16000: case 96000: {
            const int m = static_cast<int>(std::lround(paramOf(a, "M", 3.0f)));
            return m >= 2 ? 2 : (m == 1 ? (g > 0.5f ? 2 : 1) : 0);
        }
        case 151000: return g > 0.35f ? 2 : 1;
        case 162000: case 300000: return (paramOf(a, "gainMode", 1.0f) >= 1.5f && g > 0.45f) ? 2 : (g > 0.25f ? 1 : 0);
        case 101000: return paramOf(a, "Hg", 0.0f) >= 0.5f ? (g > 0.35f ? 2 : 1) : (g > 0.3f ? 1 : 0);
        case 56000: return g > 0.6f ? 2 : (g > 0.22f ? 1 : 0);
        case 91000: return g > 0.45f ? 2 : (g > 0.2f ? 1 : 0);
        case 34000: case 92000: case 80000: case 62000: return g > 0.75f ? 2 : (g > 0.35f ? 1 : 0);
        case 38000: case 176000: case 152000: case 79000: return g > 0.6f ? 1 : 0;
        default: return g > 0.85f ? 1 : 0;
    }
}

bool refHas(const catalog::ComponentSchema* s, std::initializer_list<const char*> keys) {
    if (!s) return false;
    const std::string r = lower(std::string(s->reference_name)) + " " + lower(std::string(s->display_name));
    for (auto k : keys)
        if (r.find(lower(k)) != std::string::npos) return true;
    return false;
}

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string xmlUnescape(std::string s) {
    static const std::pair<const char*, const char*> kEnt[] = {{"&quot;", "\""}, {"&apos;", "'"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&amp;", "&"}};
    for (const auto& [from, to] : kEnt)
        for (std::size_t p = 0; (p = s.find(from, p)) != std::string::npos; p += std::strlen(to)) s.replace(p, std::strlen(from), to);
    return s;
}

// attr="value" inside one start tag (`tag` = the text between '<' and '>').
std::string tagAttr(const std::string& tag, const std::string& key) {
    const std::string k = " " + key + "=\"";
    const auto p = tag.find(k);
    if (p == std::string::npos) return {};
    const auto e = tag.find('"', p + k.size());
    return e == std::string::npos ? std::string() : xmlUnescape(tag.substr(p + k.size(), e - p - k.size()));
}

// Shipped-library metadata: <headroom-library category=".." order=".." ...> with <description>, <tags><tag>,
// <inspired-by artist song album part year/> and <sources><source url title/></sources>. Returns the category.
std::string parseLibraryBlock(const std::string& raw, LibraryEntry& e) {
    const auto b = raw.find("<headroom-library");
    if (b == std::string::npos) return {};
    const auto end = raw.find("</headroom-library>", b);
    const std::string blk = raw.substr(b, end == std::string::npos ? std::string::npos : end - b);
    const std::string head = blk.substr(0, blk.find('>'));
    e.lib_id = tagAttr(head, "id");
    e.style = tagAttr(head, "style");
    e.instrument = tagAttr(head, "instrument");
    if (const std::string o = tagAttr(head, "order"); !o.empty()) e.order = std::atoi(o.c_str());
    if (const auto d = blk.find("<description>"); d != std::string::npos) {
        const auto de = blk.find("</description>", d);
        if (de != std::string::npos) e.comment = xmlUnescape(blk.substr(d + 13, de - d - 13));
    }
    std::vector<std::string> tags;
    for (std::size_t p = 0; (p = blk.find("<tag>", p)) != std::string::npos;) {
        const auto q = blk.find("</tag>", p);
        if (q == std::string::npos) break;
        tags.push_back(xmlUnescape(blk.substr(p + 5, q - p - 5)));
        p = q;
    }
    if (!tags.empty()) e.tags = tags;
    if (const auto i = blk.find("<inspired-by"); i != std::string::npos) {
        const std::string t = blk.substr(i, blk.find('>', i) - i);
        e.inspired_artist = tagAttr(t, "artist");
        e.inspired_song = tagAttr(t, "song");
        e.inspired_album = tagAttr(t, "album");
        e.inspired_part = tagAttr(t, "part");
        e.inspired_year = tagAttr(t, "year");
    }
    for (std::size_t p = 0; (p = blk.find("<source ", p)) != std::string::npos; ++p) {
        const std::string t = blk.substr(p, blk.find('>', p) - p);
        if (const std::string url = tagAttr(t, "url"); !url.empty()) e.sources.emplace_back(url, tagAttr(t, "title"));
    }
    return tagAttr(head, "category");
}

} // namespace

const std::vector<std::string>& PresetLibrary::categoryOrder() {
    static const std::vector<std::string> k = {"Featured", "Clean", "Crunch", "High Gain", "Bass", "Ambient & FX", "Acoustic & Studio",
                                               "Artist Tones", "Guitar + MIDI", "MIDI"};
    return k;
}

std::string appSupportDir() {
    std::string base = platform::appDataDir(); // ~/Library/Application Support/Headroom on macOS
    std::error_code ec;
    fs::create_directories(base, ec);
    return base;
}

std::string sanitizeFileName(const std::string& name) {
    std::string o;
    for (char c : name) {
        if (c == '/' || c == ':' || c == '\\' || static_cast<unsigned char>(c) < 0x20) o += '_';
        else o += c;
    }
    while (!o.empty() && (o.back() == ' ' || o.back() == '.')) o.pop_back();
    if (o.empty()) o = "Untitled";
    return o;
}

std::string PresetLibrary::classify(const preset::RackPreset& p, std::string& amp_name, std::vector<std::string>& tags) {
    std::vector<const preset::PresetComponentNode*> nodes;
    flatten(p.root_rack, nodes);
    const preset::PresetComponentNode* amp = nullptr;
    int drive_boost = 0;
    bool heavy_pedal = false, has_delay = false, has_reverb = false, has_mod = false, has_fuzz = false, has_split = false,
         has_pitch = false, has_wah = false, has_cab = false, has_dyn = false, has_eq = false, has_creative = false;
    int active = 0;
    for (const auto* n : nodes) {
        const auto* s = catalog::findByTableId(n->table_id);
        if (n->table_id == 7000 || n->table_id == 50000 || n->table_id == 135000 || n->table_id == 138000) has_split = true;
        if (!isOn(*n)) continue;
        if (!s) continue;
        ++active;
        if (isAmp(n->table_id)) {
            if (!amp) amp = n;
            continue;
        }
        if (refHas(s, {"Cabinet", "Studio Cab Room", "Studio Mic Room", "Cab Bay"})) has_cab = true;
        else if (refHas(s, {"Fuzz", "Big Muff", "Percolator"})) has_fuzz = true;
        if (refHas(s, {"Dime", "Metal Zone", "HM-2", "Heavy Metal", "Big Muff", "DS-1", "Distortion", "RAT"}) &&
            paramOf(*n, "D", paramOf(*n, "G", paramOf(*n, "distortion", 0.6f))) > 0.35f)
            heavy_pedal = true;
        if (refHas(s, {"Screamer", "TS808", "TS9", "Treble Booster", "Rangemaster", "Micro Amp", "SansAmp", "Jackhammer", "Tube Driver", "Surveyor", "IVP"}) && !amp)
            drive_boost = 1;
        if (refHas(s, {"Delay", "Echo", "Memory Man"})) has_delay = true;
        if (refHas(s, {"Verb", "Reverb", "Lexicon", "224", "480", "EMT 140", "Plate", "Spring", "Cosmic"})) has_reverb = true;
        if (refHas(s, {"Chorus", "Dimension", "Flanger", "Mistress", "Phaser", "Phase 90", "Small Stone", "Ensemble", "Rotary", "Leslie", "Tremolo", "Vibrato", "Uni-Vibe", "Wow & Flutter"}))
            has_mod = true;
        if (refHas(s, {"Pitch", "Whammy", "Octaver", "OC-2", "Octave Shimmer", "Transpose", "Micro Synthesizer", "Shimmer", "Sympathetic"})) has_pitch = true;
        if (refHas(s, {"Wah", "Cry Baby", "Clyde McCoy"})) has_wah = true;
        if (refHas(s, {"Comp", "1176", "LA-2A", "160VU", "Limiter", "Distressor", "Dynamics", "Transient", "Fairchild"})) has_dyn = true;
        if (refHas(s, {"EQ", "Filter", "Mu-Tron"})) has_eq = true;
        if (refHas(s, {"Stutter", "Trance Gate", "Granul", "Ring Mod", "Crusher", "Wavefolder", "Degrader", "Sequencer", "Formant", "Noise", "Slicer"}))
            has_creative = true;
    }
    // Guitar -> MIDI rigs: instruments driven by a Pitch -> MIDI unit get their own sections.
    int instruments = 0;
    bool has_nam = false, poly_detect = false, mono_detect = false, has_arp = false, has_harmony = false;
    std::vector<std::string> families;
    for (const auto* n : nodes) {
        if (!isOn(*n)) continue;
        if (n->table_id == 900005) has_nam = true;
        if (n->table_id == 900001) (paramOf(*n, "mode", 0.0f) < 0.5f ? poly_detect : mono_detect) = true;
        if (n->table_id == 900002) {
            if (paramOf(*n, "arp", 0.0f) >= 0.5f) has_arp = true;
            if (paramOf(*n, "harmony", 0.0f) >= 0.5f) has_harmony = true;
        }
        if (n->table_id == 900003 || n->table_id == 900004) {
            ++instruments;
            const std::string fam = n->table_id == 900004 ? "Synth"
                                                         : midiInstrumentFamily(static_cast<int>(std::lround(paramOf(*n, "sound", 0.0f))));
            if (!fam.empty() && std::find(families.begin(), families.end(), fam) == families.end()) families.push_back(fam);
        }
    }
    std::string cat;
    amp_name.clear();
    const std::string lname = lower(p.info.name);
    if (instruments > 0) {
        if (amp) {
            const auto* s = catalog::findByTableId(amp->table_id);
            amp_name = s ? std::string(s->reference_name) : "";
        } else if (has_nam) {
            amp_name = "Neural Amp Modeler";
        }
        const bool guitar = amp || has_nam;
        if (instruments > 1) tags.push_back("Multi-instrument");
        for (const auto& f : families) tags.push_back(f);
        if (poly_detect) tags.push_back("Poly");
        if (mono_detect) tags.push_back("Mono");
        if (has_arp) tags.push_back("Arpeggiator");
        if (has_harmony) tags.push_back("Harmony");
        if (has_delay) tags.push_back("Delay");
        if (has_reverb) tags.push_back("Reverb");
        if (has_split) tags.push_back("Parallel");
        return guitar ? "Guitar + MIDI" : "MIDI";
    }
    if (amp) {
        const auto* s = catalog::findByTableId(amp->table_id);
        amp_name = s ? std::string(s->reference_name) : "";
        int cls = ampGainClass(*amp);
        if (cls != 3) {
            cls = std::min(2, cls + drive_boost);
            if (heavy_pedal || has_fuzz) cls = std::max(cls, 2);
        }
        static const char* kCats[] = {"Clean", "Crunch", "High Gain", "Bass"};
        cat = kCats[cls];
    } else {
        if (has_creative || has_pitch || ((has_delay || has_reverb) && has_mod)) cat = "Ambient & FX";
        else if (heavy_pedal || has_fuzz || drive_boost) cat = has_cab ? "Crunch" : "Ambient & FX";
        else if (has_dyn || has_eq) cat = "Acoustic & Studio";
        else cat = "Ambient & FX";
        tags.push_back("No amp");
    }
    if (lname.find("bass") != std::string::npos && cat != "Bass" && !amp) cat = "Bass";
    if (lname.find("acoustic") != std::string::npos || lname.find("vocal") != std::string::npos) {
        if (!amp) cat = "Acoustic & Studio";
    }
    if (lname.find("lead") != std::string::npos || lname.find("solo") != std::string::npos) tags.push_back("Lead");
    else if (cat == "High Gain" && !has_delay) tags.push_back("Rhythm");
    if (has_delay) tags.push_back("Delay");
    if (has_reverb) tags.push_back("Reverb");
    if (has_mod) tags.push_back("Modulation");
    if (has_fuzz) tags.push_back("Fuzz");
    if (has_pitch) tags.push_back("Pitch");
    if (has_wah) tags.push_back("Wah");
    if (has_creative) tags.push_back("Creative");
    if (has_split) tags.push_back("Parallel");
    (void)active;
    return cat;
}

void PresetLibrary::scan(const std::string& user_dir, const std::vector<FeaturedSpec>& featured,
                         const std::string& showcase_dir, const std::string& library_dir) {
    std::vector<LibraryEntry> out;
    user_dir_ = user_dir;
    std::error_code ec;
    fs::create_directories(user_dir, ec);

    std::set<std::string> featured_files;
    for (const auto& f : featured) {
        preset::RackPreset p;
        // Featured rigs are Headroom showcase files.
        if (showcase_dir.empty()) continue;
        const std::string path = showcase_dir + "/" + f.file;
        if (!preset::loadPreset(path, p, nullptr)) continue;
        LibraryEntry e;
        e.name = f.title;
        e.author = p.info.author;
        e.path = path;
        e.featured = true;
        e.comment = p.info.comment;
        e.based_on = p.info.based_on;
        e.based_on_path = p.info.based_on_path;
        e.mtime = fileMtimeSec(path);
        std::vector<std::string> tags;
        e.category = classify(p, e.amp, tags);
        e.tags = tags;
        std::vector<const preset::PresetComponentNode*> nodes;
        flatten(p.root_rack, nodes);
        e.units = static_cast<int>(nodes.size());
        out.push_back(std::move(e));
        featured_files.insert(fs::path(path).lexically_normal().string());
    }

    // Headroom's shipped library (presets/Library/<Category>/*.rig), right after Featured: category from the rig's
    // <headroom-library> metadata (else its folder, else the chain), then the curated order, then name.
    {
        std::string lib_dir = library_dir;
        if (lib_dir.empty())
            if (const char* env = std::getenv("HEADROOM_LIBRARY_DIR")) lib_dir = env;
        if (lib_dir.empty() && !showcase_dir.empty()) lib_dir = (fs::path(showcase_dir).parent_path() / "Library").string();
        std::vector<LibraryEntry> lib;
        if (!lib_dir.empty() && fs::is_directory(lib_dir, ec)) {
            const auto& order = categoryOrder();
            for (auto it = fs::recursive_directory_iterator(lib_dir, fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) break;
                if (!it->is_regular_file(ec) || it->path().extension() != ".rig") continue;
                const std::string path = it->path().lexically_normal().string();
                if (featured_files.count(path)) continue;
                const std::string raw = readFile(path);
                preset::RackPreset p;
                if (!preset::parsePresetData(raw, path, p, nullptr)) continue;
                LibraryEntry e;
                e.name = p.info.name.empty() ? it->path().stem().string() : p.info.name;
                e.author = p.info.author;
                e.path = path;
                e.comment = p.info.comment;
                e.mtime = fileMtimeSec(it->path());
                e.library = true;
                std::vector<std::string> tags;
                const std::string inferred = classify(p, e.amp, tags);
                e.tags = tags;
                std::string cat = parseLibraryBlock(raw, e);
                if (std::find(order.begin(), order.end(), cat) == order.end() || cat == "Featured") {
                    const std::string folder = it->path().parent_path().filename().string();
                    cat = std::find(order.begin(), order.end(), folder) != order.end() && folder != "Featured" ? folder : inferred;
                }
                e.category = cat;
                std::vector<const preset::PresetComponentNode*> nodes;
                flatten(p.root_rack, nodes);
                e.units = static_cast<int>(nodes.size());
                lib.push_back(std::move(e));
            }
            auto rank = [&](const std::string& c) { return std::find(order.begin(), order.end(), c) - order.begin(); };
            std::sort(lib.begin(), lib.end(), [&](const LibraryEntry& a, const LibraryEntry& b) {
                if (rank(a.category) != rank(b.category)) return rank(a.category) < rank(b.category);
                if (a.order != b.order) return a.order < b.order;
                return lower(a.name) < lower(b.name);
            });
        }
        for (auto& e : lib) out.push_back(std::move(e));
    }

    // Headroom's own showcase rigs come right after the featured rigs, in file-name order
    // (files are numbered so the showcase reads as a curated set).
    if (!showcase_dir.empty()) {
        std::vector<fs::path> files;
        for (const auto& it : fs::directory_iterator(showcase_dir, ec))
            if (it.is_regular_file() && it.path().extension() == ".rig") files.push_back(it.path());
        std::sort(files.begin(), files.end());
        for (const auto& f : files) {
            if (featured_files.count(f.lexically_normal().string())) continue; // already listed as featured
            preset::RackPreset p;
            if (!preset::loadPreset(f.string(), p, nullptr)) continue;
            LibraryEntry e;
            e.name = p.info.name.empty() ? f.stem().string() : p.info.name;
            e.author = p.info.author;
            e.path = f.string();
            e.comment = p.info.comment;
            e.based_on = p.info.based_on;
            e.based_on_path = p.info.based_on_path;
            e.mtime = fileMtimeSec(f);
            e.showcase = true;
            std::vector<std::string> tags;
            e.category = classify(p, e.amp, tags);
            e.tags = tags;
            std::vector<const preset::PresetComponentNode*> nodes;
            flatten(p.root_rack, nodes);
            e.units = static_cast<int>(nodes.size());
            out.push_back(std::move(e));
        }
    }

    {
        std::lock_guard<std::mutex> lk(mtx_);
        entries_ = std::move(out);
    }
    rebuildUserEntries();
}

void PresetLibrary::rebuildUserEntries() {
    std::vector<LibraryEntry> users;
    std::error_code ec;
    for (const auto& it : fs::directory_iterator(user_dir_, ec)) {
        if (!it.is_regular_file() || it.path().extension() != ".rig") continue;
        preset::RackPreset p;
        if (!preset::loadPreset(it.path().string(), p, nullptr)) continue;
        LibraryEntry e;
        e.name = p.info.name.empty() ? it.path().stem().string() : p.info.name;
        e.author = p.info.author;
        e.path = it.path().string();
        e.comment = p.info.comment;
        e.based_on = p.info.based_on;
        e.based_on_path = p.info.based_on_path;
        e.mtime = fileMtimeSec(it.path());
        e.user = true;
        std::vector<std::string> tags;
        std::string inferred = classify(p, e.amp, tags);
        e.tags = tags;
        e.tags.insert(e.tags.begin(), inferred);
        e.category = "User";
        std::vector<const preset::PresetComponentNode*> nodes;
        flatten(p.root_rack, nodes);
        e.units = static_cast<int>(nodes.size());
        users.push_back(std::move(e));
    }
    std::sort(users.begin(), users.end(), [](const LibraryEntry& a, const LibraryEntry& b) { return lower(a.name) < lower(b.name); });
    std::lock_guard<std::mutex> lk(mtx_);
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [](const LibraryEntry& e) { return e.user; }), entries_.end());
    for (auto& u : users) entries_.push_back(std::move(u));
    for (std::size_t i = 0; i < entries_.size(); ++i) entries_[i].idx = static_cast<int>(i);
}

std::vector<LibraryEntry> PresetLibrary::entries() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return entries_;
}

bool PresetLibrary::entry(int idx, LibraryEntry& out) const {
    std::lock_guard<std::mutex> lk(mtx_);
    if (idx < 0 || idx >= static_cast<int>(entries_.size())) return false;
    out = entries_[static_cast<std::size_t>(idx)];
    return true;
}

std::string PresetLibrary::json() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::ostringstream o;
    o << "{\"presets\":[";
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const auto& e = entries_[i];
        if (i) o << ",";
        o << "{\"idx\":" << e.idx << ",\"name\":\"" << jsonEscape(e.name) << "\",\"author\":\"" << jsonEscape(e.author)
          << "\",\"category\":\"" << jsonEscape(e.category) << "\",\"amp\":\"" << jsonEscape(e.amp) << "\",\"tags\":[";
        for (std::size_t t = 0; t < e.tags.size(); ++t) o << (t ? "," : "") << "\"" << jsonEscape(e.tags[t]) << "\"";
        o << "],\"source\":\"" << (e.user ? "user" : e.showcase ? "showcase" : e.library ? "library" : "factory") << "\",\"featured\":" << (e.featured ? "true" : "false");
        if (e.library) {
            o << ",\"lib_id\":\"" << jsonEscape(e.lib_id) << "\",\"order\":" << e.order << ",\"style\":\"" << jsonEscape(e.style)
              << "\",\"instrument\":\"" << jsonEscape(e.instrument) << "\"";
            if (!e.inspired_artist.empty() || !e.inspired_song.empty())
                o << ",\"inspired_by\":{\"artist\":\"" << jsonEscape(e.inspired_artist) << "\",\"song\":\"" << jsonEscape(e.inspired_song)
                  << "\",\"album\":\"" << jsonEscape(e.inspired_album) << "\",\"part\":\"" << jsonEscape(e.inspired_part)
                  << "\",\"year\":\"" << jsonEscape(e.inspired_year) << "\"}";
            o << ",\"sources\":[";
            for (std::size_t s = 0; s < e.sources.size(); ++s)
                o << (s ? "," : "") << "{\"url\":\"" << jsonEscape(e.sources[s].first) << "\",\"title\":\"" << jsonEscape(e.sources[s].second) << "\"}";
            o << "]";
        }
        o
          << ",\"units\":" << e.units << ",\"comment\":\"" << jsonEscape(e.comment) << "\""
          << ",\"based_on\":\"" << jsonEscape(e.based_on) << "\""
          << ",\"based_on_path\":\"" << jsonEscape(e.based_on_path) << "\""
          << ",\"mtime\":" << e.mtime
          << ",\"path\":\"" << jsonEscape(e.path) << "\"}";
    }
    // Categories in browse order, listing those Headroom's own rigs fill.
    std::set<std::string> own;
    for (const auto& e : entries_)
        if (!e.user) own.insert(e.category);
    o << "],\"categories\":[";
    bool first = true;
    for (const auto& c : categoryOrder()) {
        if (c != "Featured" && !own.count(c) && !(own.empty() && c != "Artist Tones")) continue;
        o << (first ? "" : ",") << "\"" << jsonEscape(c) << "\"";
        first = false;
    }
    o << "],\"count\":" << entries_.size() << "}";
    return o.str();
}

int PresetLibrary::saveUser(const preset::RackPreset& p, std::string* err) {
    // fs::path join: matches the paths directory iteration hands back (backslash separators on Windows).
    const std::string path = (fs::path(user_dir_) / (sanitizeFileName(p.info.name) + ".rig")).string();
    if (!writeUserPreset(path, p, err)) return -1;
    rebuildUserEntries();
    std::lock_guard<std::mutex> lk(mtx_);
    for (const auto& e : entries_)
        if (e.user && e.path == path) return e.idx;
    return -1;
}

bool PresetLibrary::deleteUser(int idx) {
    LibraryEntry e;
    if (!entry(idx, e) || !e.user) return false;
    std::error_code ec;
    fs::remove(e.path, ec);
    rebuildUserEntries();
    return !ec;
}

int PresetLibrary::renameUser(int idx, const std::string& new_name, std::string* err) {
    if (new_name.empty()) {
        if (err) *err = "new name required";
        return -1;
    }
    LibraryEntry e;
    if (!entry(idx, e) || !e.user) {
        if (err) *err = "not a user rig";
        return -1;
    }
    preset::RackPreset p;
    if (!preset::loadPreset(e.path, p, err)) return -1;
    p.info.name = new_name;
    const std::string new_path = (fs::path(user_dir_) / (sanitizeFileName(new_name) + ".rig")).string();
    if (!writeUserPreset(new_path, p, err)) return -1;
    if (new_path != e.path) {
        std::error_code ec;
        fs::remove(e.path, ec);
    }
    rebuildUserEntries();
    std::lock_guard<std::mutex> lk(mtx_);
    for (const auto& ent : entries_)
        if (ent.user && ent.path == new_path) return ent.idx;
    return -1;
}

int PresetLibrary::duplicateUser(int idx, std::string* err) {
    LibraryEntry e;
    if (!entry(idx, e) || !e.user) {
        if (err) *err = "not a user rig";
        return -1;
    }
    preset::RackPreset p;
    if (!preset::loadPreset(e.path, p, err)) return -1;
    std::string base_name = p.info.name.empty() ? e.name : p.info.name;
    std::string copy_name = base_name + " Copy";
    int n = 2;
    while (fs::exists(user_dir_ + "/" + sanitizeFileName(copy_name) + ".rig")) {
        copy_name = base_name + " Copy " + std::to_string(n++);
    }
    p.info.name = copy_name;
    return saveUser(p, err);
}

void PresetLibrary::assignFrom(const PresetLibrary& other) {
    if (&other == this) return;
    std::vector<LibraryEntry> e;
    std::string u;
    {
        std::lock_guard<std::mutex> lk(other.mtx_);
        e = other.entries_;
        u = other.user_dir_;
    }
    {
        std::lock_guard<std::mutex> lk(mtx_);
        entries_ = std::move(e);
        user_dir_ = std::move(u);
    }
    rebuildUserEntries();
}

// ───────────────────────────── user preset writer ─────────────────────────────
static void writeComponent(std::ostream& o, const preset::PresetComponentNode& n, int depth) {
    const std::string pad(static_cast<std::size_t>(depth * 2), ' ');
    o << pad << "<component id=\"" << n.table_id << "\">\n" << pad << "  <component-audio version=\"1\">\n" << pad
      << "    <parameters num-parameters=\"" << n.params.size() << "\">\n";
    for (const auto& p : n.params) {
        o << pad << "      <parameter id=\"" << xmlEscape(p.id) << "\"";
        if (p.type == "string" || p.type == "filename") o << " type=\"string\" value=\"" << xmlEscape(p.str_val) << "\"/>\n";
        else {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.7g", static_cast<double>(p.float_val));
            o << " value=\"" << buf << "\"/>\n";
        }
    }
    o << pad << "    </parameters>\n" << pad << "  </component-audio>\n";
    if (!n.children.empty()) {
        o << pad << "  <component-container>\n";
        for (const auto& c : n.children) writeComponent(o, c, depth + 2);
        o << pad << "  </component-container>\n";
    }
    o << pad << "</component>\n";
}

bool writeUserPreset(const std::string& path, const preset::RackPreset& p, std::string* err) {
    std::ofstream o(path, std::ios::binary | std::ios::trunc);
    if (!o) {
        if (err) *err = "cannot write " + path;
        return false;
    }
    writeUserPresetTo(o, p);
    return static_cast<bool>(o);
}

std::string userPresetXml(const preset::RackPreset& p) {
    std::ostringstream o;
    writeUserPresetTo(o, p);
    return o.str();
}

void writeUserPresetTo(std::ostream& o, const preset::RackPreset& p) {
    o << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<headroom-preset version=\"1\">\n";
    o << "<headroom-info><soundinfo><properties><name>" << xmlEscape(p.info.name) << "</name><author>"
      << xmlEscape(p.info.author) << "</author>";
    if (!p.info.based_on.empty()) o << "<based-on>" << xmlEscape(p.info.based_on) << "</based-on>";
    if (!p.info.based_on_path.empty()) o << "<based-on-path>" << xmlEscape(p.info.based_on_path) << "</based-on-path>";
    o << "</properties></soundinfo></headroom-info>\n";
    o << "<headroom-rig>\n<metadata preset-name=\"" << xmlEscape(p.info.name) << "\" preset-author=\"" << xmlEscape(p.info.author)
      << "\" preset-comment=\"" << xmlEscape(p.info.comment) << "\""
      << " based-on=\"" << xmlEscape(p.info.based_on) << "\""
      << " based-on-path=\"" << xmlEscape(p.info.based_on_path) << "\"/>\n<rack>\n<non-fix-components>\n";
    for (const auto& c : p.root_rack.children) writeComponent(o, c, 1);
    o << "</non-fix-components>\n</rack>\n<realtimecontroldb>\n<remotecontroldb>\n<component-event-listeners>\n";
    for (const auto& m : p.macros) {
        o << "  <connection-data><target controller-slot-id=\"" << m.column << "\" component-id=\"" << m.target_table_id
          << "\" target-number=\"" << m.target_index << "\" target-name=\"" << xmlEscape(m.target_param_id) << "\" min-value=\""
          << m.min_val << "\" max-value=\"" << m.max_val << "\" controller-name=\"" << xmlEscape(m.control_name) << "\"/></connection-data>\n";
    }
    o << "</component-event-listeners>\n</remotecontroldb>\n</realtimecontroldb>\n</headroom-rig>\n";
    if (!p.info.based_on.empty() || !p.info.based_on_path.empty()) {
        o << "<headroom-lineage based-on=\"" << xmlEscape(p.info.based_on) << "\" based-on-path=\"" << xmlEscape(p.info.based_on_path) << "\"/>\n";
    }
    if (p.tempo_bpm > 0.0) {
        char tb[48];
        std::snprintf(tb, sizeof(tb), "<headroom-tempo bpm=\"%.3f\"/>\n", p.tempo_bpm);
        o << tb;
    }
    if (!p.extra_xml.empty()) o << p.extra_xml << "\n";
    o << "</headroom-preset>\n";
}

// ───────────────────────────── component presets ─────────────────────────────
void ComponentPresetBank::scan() {
    by_table_.clear();
    // Seed clean default INIT presets from the component schemas
    for (const auto& c : catalog::kAllComponents) {
        ComponentPreset init_cp;
        init_cp.name = "Init";
        for (const auto& p : c.params) {
            if (p.kind != catalog::ParamKind::String) {
                // Amp-Matched Cab's (156000) X-Fade blends in the room: start on the close capture (see ComponentFactory).
                const float def = (c.table_id == 156000 && p.id == "c") ? 0.0f : p.default_value;
                init_cp.params.emplace_back(std::string(p.id), def);
            }
        }
        if (!init_cp.params.empty()) {
            by_table_[c.table_id].push_back(std::move(init_cp));
        }
    }
}

const std::vector<ComponentPreset>* ComponentPresetBank::presetsFor(int32_t t) const {
    auto it = by_table_.find(t);
    return it == by_table_.end() ? nullptr : &it->second;
}

const ComponentPreset* ComponentPresetBank::initFor(int32_t t) const {
    const auto* v = presetsFor(t);
    if (!v || v->empty()) return nullptr;
    for (const auto& p : *v)
        if (lower(p.name) == "init" || lower(p.name) == "default") return &p;
    return nullptr;
}

} // namespace rig::library
