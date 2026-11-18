#pragma once
// Speaker cabinet & microphone catalog:
//
//   matched:<Amp>       Amp-Matched Cab (156000) — 26 matched amp cabinets, two close mics (A, B) + room
//   studio:<Folder>     Studio Cab Room (119000) — 29 studio cabinets, 5–13 named mics each, cap / cap edge / cone
//   capture:<Vendor/F>  Cabinet IR Loader    — 31 cabinet captures + any user .wav captures in ~/Library/Application Support/Headroom/IRs
//
// Ids are stable strings, so presets keep their choice across sessions. <Amp> and <Folder> are the IR file and
// folder names of the local cabinet content, not display names (the UI shows `label`).
#include "../core/IComponent.hpp"
#include "CabinetIds.hpp"
#include "ConvolutionEngine.hpp"
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace rig::dsp::cab {

// Physical motional speaker impedance profile for bi-directional amp-to-speaker coupling. Built-in cabinets use the
// profile class in their cab_ids table entry; anything else (captures, user IRs) is matched on its speaker format.
inline SpeakerImpedanceProfile speakerProfileFor(cab_ids::Profile p) noexcept {
    using P = cab_ids::Profile;
    switch (p) {
        case P::Bass8x10: return {true, 62.0f, 1.60f, 1.25f};
        case P::Bass15: return {true, 58.0f, 1.45f, 1.30f};
        case P::BassAluminum: return {true, 70.0f, 1.50f, 0.92f};
        case P::FourByTen: return {true, 95.0f, 1.30f, 0.85f};
        case P::OneByTwelve: return {true, 106.0f, 1.15f, 0.82f};
        case P::AlnicoTwoByTwelve: return {true, 102.0f, 1.25f, 0.90f};
        case P::TwoByTwelve: return {true, 108.0f, 1.20f, 0.85f};
        case P::Greenback: return {true, 86.0f, 1.50f, 1.05f};
        case P::Modern: return {true, 80.0f, 1.65f, 1.15f};
        case P::BritishV30: return {true, 84.0f, 1.55f, 1.08f};
        case P::None: break;
    }
    // Unidentified cabinet: no profile, so the amp keeps its own voiced speaker load rather than
    // being re-tuned to a generic guess.
    return {false, 88.0f, 1.45f, 1.00f};
}
inline SpeakerImpedanceProfile speakerProfileForCab(std::string_view id_or_name, std::string_view speakers = {}) noexcept {
    if (const auto* def = cab_ids::find(id_or_name)) return speakerProfileFor(def->profile);
    using P = cab_ids::Profile;
    auto has = [&](std::string_view tok) noexcept { return id_or_name.find(tok) != std::string_view::npos; };
    if (speakers == "8x10" || has("8x10")) return speakerProfileFor(P::Bass8x10);
    if (speakers == "1x15" || speakers == "2x15" || has("1x15") || has("2x15")) return speakerProfileFor(P::Bass15);
    if (speakers == "4x10" || has("4x10")) return speakerProfileFor(P::FourByTen);
    if (speakers == "1x12" || has("1x12")) return speakerProfileFor(P::OneByTwelve);
    if (speakers == "2x12" || has("2x12") || has("Jazz")) return speakerProfileFor(P::TwoByTwelve);
    if (has("Green") || has("green")) return speakerProfileFor(P::Greenback);
    if (has("Modern")) return speakerProfileFor(P::Modern);
    return speakerProfileFor(P::None);
}

// A stored cabinet id in its current form (ids are stored as they are; the hook stays for future renames).
inline std::string canonicalCabId(std::string id) { return id; }

struct MicInfo {
    std::string id;     // token, e.g. "dyn57", "con47V1", "A" (matched), "ir" (capture)
    std::string label;  // "Shure SM57"
    std::string family; // Dynamic | Condenser | Ribbon | Matched | Captured
    std::string note;   // one-line character description
    std::vector<std::string> positions; // tokens: cap, capedge, cone, room, fixed
};

struct CabInfo {
    std::string id;      // "studio:marshall-1960tv-4x12"
    std::string kind;    // matched | studio | capture
    std::string name;    // folder / matched model / relative wav path
    std::string label;   // "Orange PPC412 4x12 V30"
    std::string group;   // "Matched" | "Studio" | vendor (captures)
    std::string speakers;// "4x12", "2x12", … (for drawing)
    std::vector<MicInfo> mics;
    bool room = false;   // a room/ambience impulse exists
    std::string pair;    // matched ↔ studio counterpart id (best match; empty if none)
    int matched_index = -1;
    int studio_index = -1; // index into cab_ids::kStudio
};

inline std::string positionLabel(const std::string& p) {
    if (p == "cap") return "Cap · on-axis";
    if (p == "capedge") return "Cap edge";
    if (p == "cone") return "Cone · off-axis";
    if (p == "room") return "Room · far";
    if (p == "fixed") return "Fixed";
    return p;
}

// Studio microphone models and character notes.
inline MicInfo micInfoFor(const std::string& tok) {
    struct Row { const char* base; const char* label; const char* note; };
    static const Row rows[] = {
        {"dyn57", "Shure SM57", "Shure SM57 cardioid dynamic: forward mids, tight lows"},
        {"dyn421", "Sennheiser MD 421-II", "Sennheiser MD 421 large dynamic: full, punchy low mids"},
        {"dyn441", "Sennheiser MD 441-U", "Sennheiser MD 441 supercardioid dynamic: smooth, detailed top"},
        {"dyn20", "Electro-Voice RE20", "Electro-Voice RE20 broadcast dynamic: even, round lows"},
        {"dyn40", "Heil PR 40", "Heil PR 40 large-diaphragm dynamic: bright, extended"},
        {"dyn112", "AKG D112", "AKG D112 bass dynamic: deep sub lows, scooped mids"},
        {"dyn6", "Audix D6", "Audix D6 bass dynamic: sub punch, pre-scooped attack"},
        {"con87", "Neumann U87 Ai", "Neumann U87 large-diaphragm condenser: open, airy top"},
        {"con47", "Neumann U47 FET", "Neumann U47 FET condenser: warm, authoritative low mids"},
        {"con67", "Neumann U67", "Neumann U67 tube condenser: rich, smooth midrange"},
        {"con89", "Neumann U89i", "Neumann U89i condenser: tight, modern transient detail"},
        {"con54", "Neumann KM 54", "Neumann KM 54 small tube condenser: crisp, focused"},
        {"con149", "Neumann M 149", "Neumann M 149 tube condenser: wide, three-dimensional"},
        {"con30", "Earthworks M30", "Earthworks M30 omni measurement condenser: flat, uncoloured"},
        {"rib121", "Royer R-121", "Royer R-121 figure-8 ribbon: warm proximity lows, smooth highs"},
        {"rib160", "Beyerdynamic M 160", "Beyerdynamic M 160 hypercardioid ribbon: focused, natural"},
    };
    MicInfo m;
    m.id = tok;
    std::string base = tok, suffix;
    if (!base.empty() && base.back() == '+') { suffix = " +"; base.pop_back(); }
    if (base.size() > 2 && (base[base.size() - 2] == 'V' || base[base.size() - 2] == 'v') &&
        std::isdigit(static_cast<unsigned char>(base.back()))) {
        suffix = std::string(" V") + base.back() + suffix;
        base.resize(base.size() - 2);
    }
    for (const auto& r : rows) {
        if (base == r.base) {
            m.label = std::string(r.label) + suffix;
            m.note = r.note;
            break;
        }
    }
    const std::string fam = tok.substr(0, std::min<std::size_t>(3, tok.size()));
    m.family = fam == "dyn" ? "Dynamic" : fam == "con" ? "Condenser" : fam == "rib" ? "Ribbon" : "Mic";
    if (m.label.empty()) {
        std::string up = tok;
        for (auto& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        m.label = up;
    }
    return m;
}

namespace detail {
struct MatchedRow { const char* name; const char* label; const char* speakers; const char* studio; };
inline const MatchedRow* matchedRow(const std::string& n) {
    static const MatchedRow rows[] = {
        {"vox-ac30-2x12", "Vox AC30 2x12 Alnico Silver", "2x12", "vox-2x12-silver"},
        {"ampeg-svt-810e", "Ampeg SVT-810E 8x10", "8x10", "ampeg-svt-8x10"},
        {"orange-ppc412", "Orange PPC412 4x12 V30", "4x12", "orange-4x12-v30"},
        {"marshall-1960a-variac", "Marshall 1960A 4x12 Variac Greenback", "4x12", "marshall-68-4x12-greenback"},
        {"mesa-recto-4x12", "Mesa/Boogie Recto 4x12 V30", "4x12", "mesa-recto-4x12-v30"},
        {"hiwatt-se4123", "Hiwatt SE4123 4x12 Fane", "4x12", "hiwatt-4x12-fane"},
        {"marshall-1960b", "Marshall 1960B 4x12 G12H-30", "4x12", "marshall-76-4x12-g12h30"},
        {"soldano-4x12", "Soldano 4x12 Eminence Legend", "4x12", "soldano-4x12-slant"},
        {"roland-jc120", "Roland JC-120 2x12 Stereo", "2x12", "roland-jc120-2x12"},
        {"marshall-1960a-g12t75", "Marshall 1960A 4x12 G12T-75", "4x12", "marshall-82-4x12-g12t75"},
        {"marshall-1960a-jcm800", "Marshall 1960A JCM800 4x12", "4x12", "marshall-76-4x12-g12h30"},
        {"marshall-1960tv", "Marshall 1960TV 4x12 Greenback", "4x12", "marshall-68-4x12-greenback"},
        {"mesa-oversized-4x12", "Mesa/Boogie Oversized 4x12 V30", "4x12", "mesa-oversized-4x12-a"},
        {"fender-twin-2x12", "Fender '65 Twin 2x12 Jensen C12K", "2x12", "fender-twin-2x12-c12k"},
        {"fender-tweed-deluxe", "Fender '57 Tweed Deluxe 1x12", "1x12", "fender-tweed-1x12-p12q"},
        {"fender-bassman-4x10", "Fender '59 Bassman 4x10 Jensen P10R", "4x10", "fender-bassman-4x10-p10r"},
        {"bogner-uberkab", "Bogner Uberkab 4x12 V30+T75", "4x12", "bogner-uberkab-4x12-v30"},
        {"peavey-5150-4x12", "Peavey 5150 4x12 Sheffield 1200", "4x12", "peavey-5150-4x12-sheffield"},
        {"supro-1624t", "Supro 1624T 1x12 DT12", "1x12", "fender-tweed-1x12-p12q"},
        {"ampeg-b15n", "Ampeg B-15N 1x15 Vintage", "1x15", "ampeg-b15-1x15"},
        {"friedman-be-4x12", "Friedman BE 4x12 Greenback+V30", "4x12", "marshall-1960tv-4x12"},
        {"bogner-ecstasy-4x12", "Bogner Ecstasy 4x12 Celestion V30", "4x12", "bogner-412st-4x12"},
        {"soldano-slo-4x12", "Soldano SLO 4x12 Celestion V30", "4x12", "bogner-uberkab-4x12-t75"},
        {"vox-ac15-2x12", "Vox AC15 2x12 Celestion Alnico Blue", "2x12", "vox-2x12-blue"},
        {"fender-deluxe-reverb", "Fender '65 Deluxe 1x12 Oxford", "2x12", "fender-twin-2x12-c12k"},
        {"ampeg-svt-410hlf", "Ampeg SVT-410HLF 4x10", "4x10", "hartke-4x10-aluminum"},
    };
    for (const auto& r : rows)
        if (n == r.name) return &r;
    return nullptr;
}
inline std::string studioLabel(const std::string& folder, std::string& speakers) {
    static const std::map<std::string, std::string> nice = {
        {"vox-2x12-silver", "2x12 Vox Alnico Silver"},
        {"vox-2x12-blue", "2x12 Vox Alnico Blue"},
        {"orange-4x12-v30", "4x12 Orange PPC412 V30"},
        {"mesa-recto-4x12-v30", "4x12 Mesa Recto V30"},
        {"hiwatt-4x12-fane", "4x12 Hiwatt SE4123 Fane"},
        {"marshall-68-4x12-greenback", "4x12 Marshall '68 Greenback"},
        {"marshall-71-4x12-blackback", "4x12 Marshall '71 Blackback"},
        {"marshall-76-4x12-g12h30", "4x12 Marshall '76 G12H-30"},
        {"marshall-82-4x12-g12t75", "4x12 Marshall '82 G12T-75"},
        {"marshall-84-4x12-g12-65", "4x12 Marshall '84 G12-65"},
        {"soldano-4x12-straight", "4x12 Soldano Straight V30"},
        {"soldano-4x12-slant", "4x12 Soldano Slant Legend"},
        {"roland-jc120-2x12", "2x12 Roland JC-120"},
        {"fender-twin-2x12-c12k", "2x12 Fender Twin Jensen C12K"},
        {"fender-showman-2x15", "2x15 Fender Dual Showman JBL"},
        {"fender-bassman-4x10-p10r", "4x10 Fender '59 Bassman P10R"},
        {"fender-tweed-1x12-p12q", "1x12 Fender '57 Tweed P12Q"},
        {"bogner-uberkab-4x12-v30", "4x12 Bogner Uberkab V30"},
        {"bogner-uberkab-4x12-t75", "4x12 Bogner Uberkab T75"},
        {"peavey-5150-4x12-sheffield", "4x12 Peavey 5150 Sheffield"},
        {"ampeg-svt-8x10", "8x10 Ampeg SVT-810E"},
        {"hartke-4x10-aluminum", "4x10 Hartke XL Aluminum"},
        {"ampeg-b15-1x15", "1x15 Ampeg B-15 Portaflex"},
        {"mesa-lonestar-2x12", "2x12 Mesa Lonestar C90"},
        {"marshall-1936-2x12", "2x12 Marshall 1936 G12T-75"},
        {"marshall-1960tv-4x12", "4x12 Marshall 1960TV Greenback"},
        {"bogner-412st-4x12", "4x12 Bogner 412ST V30"},
        {"mesa-oversized-4x12-a", "4x12 Mesa Oversized V30 A"},
        {"mesa-oversized-4x12-b", "4x12 Mesa Oversized V30 B"}};
    for (const char* sp : {"8x10", "4x12", "2x12", "1x12", "4x10", "2x15", "1x15"})
        if (folder.find(sp) != std::string::npos) { speakers = sp; break; }
    if (auto it = nice.find(folder); it != nice.end()) return it->second;
    return folder;
}
} // namespace detail

class Catalog {
public:
    static const Catalog& get() {
        static Catalog c;
        return c;
    }
    const std::vector<CabInfo>& cabs() const noexcept { return cabs_; }
    const CabInfo* find(const std::string& id) const {
        for (const auto& c : cabs_) if (c.id == id) return &c;
        return nullptr;
    }
    int indexOf(const std::string& id) const {
        for (std::size_t i = 0; i < cabs_.size(); ++i) if (cabs_[i].id == id) return static_cast<int>(i);
        return -1;
    }
    static const MicInfo* findMic(const CabInfo& c, const std::string& id) {
        for (const auto& m : c.mics) if (m.id == id) return &m;
        return nullptr;
    }
    // Best default mic for a cabinet: a Shure SM57, then Sennheiser MD 421, then the first mic.
    static const MicInfo* defaultMic(const CabInfo& c) {
        for (const char* pref : {"dyn57", "dyn57V1", "dyn421", "dyn421V1", "A", "ir"})
            if (const auto* m = findMic(c, pref)) return m;
        return c.mics.empty() ? nullptr : &c.mics.front();
    }
    static std::string defaultPosition(const MicInfo& m) {
        for (const char* pref : {"capedge", "fixed", "cap"})
            if (std::find(m.positions.begin(), m.positions.end(), pref) != m.positions.end()) return pref;
        return m.positions.empty() ? std::string() : m.positions.front();
    }
    // Nearest mic of the same model (ignoring V1/V2/+ variants), then the same family, then the default.
    static const MicInfo* closestMic(const CabInfo& c, const std::string& want) {
        if (const auto* m = findMic(c, want)) return m;
        auto core = [](std::string t) {
            while (!t.empty() && !std::isdigit(static_cast<unsigned char>(t.back()))) t.pop_back();
            if (t.size() > 2 && (t[t.size() - 2] == 'V' || t[t.size() - 2] == 'v')) t.resize(t.size() - 2);
            return t;
        };
        const std::string k = core(want);
        for (const auto& m : c.mics) if (!k.empty() && core(m.id) == k) return &m;
        const std::string fam = want.substr(0, std::min<std::size_t>(3, want.size()));
        for (const auto& m : c.mics) if (m.id.rfind(fam, 0) == 0) return &m;
        return defaultMic(c);
    }

    // Relative path of one close-mic impulse, or "" if the selection is not valid.
    static std::string micFile(const CabInfo& c, const MicInfo& m, const std::string& pos) {
        if (std::find(m.positions.begin(), m.positions.end(), pos) == m.positions.end()) return {};
        if (c.kind == "matched") return CabinetIRLibrary::matched(c.matched_index, m.id == "B" ? 'B' : 'A', false);
        if (c.kind == "capture") return "Cabinet IR Loader/" + c.name;
        return CabinetIRLibrary::roomCabFile(c.name, m.id, pos);
    }
    // Room/ambience impulses for a cabinet (matched cabs have one per mic, studio cabs one per cabinet).
    static std::vector<std::string> roomFiles(const CabInfo& c) {
        if (!c.room) return {};
        if (c.kind == "matched") return {CabinetIRLibrary::matched(c.matched_index, 'A', true), CabinetIRLibrary::matched(c.matched_index, 'B', true)};
        if (c.kind == "studio") return {"studio/" + c.name + "/" + c.name + "_room.ir"};
        return {};
    }

    std::size_t micCount() const {
        std::size_t n = 0;
        for (const auto& c : cabs_) if (c.kind == "studio") n += c.mics.size();
        return n;
    }

private:
    Catalog() {
        // 1. All 26 Matched cabinets
        for (std::size_t i = 0; i < cab_ids::kMatched.size(); ++i) {
            const std::string n = cab_ids::kMatched[i].id;
            CabInfo c;
            c.kind = "matched";
            c.id = "matched:" + n;
            c.name = n;
            c.group = "Matched";
            c.matched_index = static_cast<int>(i);
            const auto* row = detail::matchedRow(n);
            c.label = row ? row->label : n;
            c.speakers = row ? row->speakers : "4x12";
            if (row) c.pair = std::string("studio:") + row->studio;
            c.mics.push_back({"A", "Shure SM57 (Close A)", "Matched", "Close dynamic mic on speaker cap edge", {"fixed"}});
            c.mics.push_back({"B", "Royer R-121 (Close B)", "Matched", "Complementary ribbon mic for body and warmth", {"fixed"}});
            c.room = true;
            cabs_.push_back(std::move(c));
        }
        // 2. All 29 Studio cabinets
        for (std::size_t i = 0; i < cab_ids::kStudio.size(); ++i) {
            const std::string folder = cab_ids::kStudio[i].id;
            const auto& info = CabinetIRLibrary::roomCabInfo(folder);
            if (info.mics.empty()) continue;
            CabInfo c;
            c.kind = "studio";
            c.id = "studio:" + folder;
            c.name = folder;
            c.group = "Studio";
            c.studio_index = static_cast<int>(i);
            c.label = detail::studioLabel(folder, c.speakers);
            if (c.speakers.empty()) c.speakers = "4x12";
            c.room = info.room;
            for (const auto& tok : info.mics) {
                MicInfo m = micInfoFor(tok);
                m.positions = info.positions.at(tok);
                c.mics.push_back(std::move(m));
            }
            cabs_.push_back(std::move(c));
        }
        // Back-links studio -> matched.
        for (auto& c : cabs_) {
            if (c.kind != "matched" || c.pair.empty()) continue;
            for (auto& s : cabs_) if (s.id == c.pair && s.pair.empty()) s.pair = c.id;
        }
        for (auto& c : cabs_) if (c.kind == "matched" && !c.pair.empty() && !find(c.pair)) c.pair.clear();
        // 3. Built-in & User IR captures.
        for (const auto& f : CabinetIRLibrary::listFiles("Cabinet IR Loader", ".wav")) {
            CabInfo c;
            c.kind = "capture";
            c.id = "capture:" + f;
            c.name = f;
            const auto sl = f.find('/');
            c.group = sl == std::string::npos ? "IR capture" : f.substr(0, sl);
            std::string stem = sl == std::string::npos ? f : f.substr(sl + 1);
            if (auto dot = stem.rfind('.'); dot != std::string::npos) stem.resize(dot);
            c.label = stem;
            c.speakers = stem.find("212") != std::string::npos || stem.find("2x12") != std::string::npos ? "2x12"
                       : stem.find("4x10") != std::string::npos || stem.find("410") != std::string::npos ? "4x10"
                       : stem.find("8x10") != std::string::npos ? "8x10"
                       : stem.find("1x15") != std::string::npos ? "1x15"
                       : stem.find("1x12") != std::string::npos ? "1x12" : "4x12";
            c.mics.push_back({"ir", "Captured mic", "Captured", "Mic and position are baked into this capture", {"fixed"}});
            cabs_.push_back(std::move(c));
        }
    }
    std::vector<CabInfo> cabs_;
};

} // namespace rig::dsp::cab
