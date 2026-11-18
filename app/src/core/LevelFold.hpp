#pragma once
// Level folding: put a level change (dB) into a rig's OWN level knobs, as ordinary knob positions.
//
// The engine never measures or re-levels a rig at run time (no loudness gain, no glide). Levels are set where a
// player would set them: on the units' knobs. This file is the single place that knows which knobs are a pure
// output gain (moving them changes the level and nothing else) and how far each one reaches. It is used
//   * offline, by tools/rig_level, to level the shipped library once (the measured correction goes into knobs);
//   * once per user rig, by the server, to migrate the gain the retired loudness cache gave it (no jump);
//   * once per DAW project, by the plug-in, for states saved with the old loudness gain.
//
// Knob choice (findLevelKnobs): walk the rig from its output backwards. A unit with a pure post-gain level knob
// (a cabinet's Volume / Level, an EQ's Output, a reverb's or delay's Output, a compressor's make-up Output, an
// instrument's Level) is taken; bypassed units are skipped; units that pass level through linearly (reverbs, delays, modulation, loopers, MIDI units) are walked
// past; anything else (an amp, a drive, a compressor, a gate) ends the walk: a knob in front of it would change
// the tone, not just the level. Amp masters are never touched. A split is walked lane by lane: each lane must
// give a knob, and the same change is put on every lane's knob (they sum after the split), so the rig as a whole
// moves by exactly that much. Knobs nearer the output are used first; when one runs out of travel the rest goes
// to the next one back (foldLevel). Whatever no knob can take is reported, never forced.
#include "../preset/PresetLoader.hpp"
#include "../generated/ComponentCatalog.hpp"
#include "../dsp/OutputTrim.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace rig::levelfold {

// ───────────── pure post-gain level knobs ─────────────
// db(v): the unit's output gain in dB at raw (stored) value v in [0, 1], exactly as the DSP applies it.
struct KnobSpec {
    int32_t table_id;
    const char* param;
    const char* label;
    double (*db)(double v);
};

namespace detail {
inline double lin20(double g) { return 20.0 * std::log10(std::max(g, 1e-9)); }
// Amp-Matched Cab (156000) Volume: dsp::ampMatchedCabVolumeDb (ConvolutionEngine.hpp), +-12 dB around 0.5.
inline double cab156(double v) { const double u = std::clamp(v, 0.0, 1.0) - 0.5; return 6.8 * u + 68.8 * u * u * u; }
inline double cab88(double v) { return lin20(0.7 + 0.9 * std::clamp(v, 0.0, 1.0)); }           // Classic Amp-Matched Cab Volume
inline double cabRoom(double v) { return lin20(0.6 + 0.8 * std::clamp(v, 0.0, 1.0)); }          // Studio Cab Room / IR loader Volume
inline double pm12(double v) { return 24.0 * (std::clamp(v, 0.0, 1.0) - 0.5); }                 // Cabinet Level, tube comp Output
inline double pm18(double v) { return -18.0 + 36.0 * std::clamp(v, 0.0, 1.0); }                 // studio EQ Output (StudioEQ.hpp)
inline double pm4(double v) { return -4.0 + 8.0 * std::clamp(v, 0.0, 1.0); }                    // graphic EQ Output
inline double inst(double v) { const double c = std::clamp(v, 0.0, 1.0); return lin20(1.6 * c * c); }   // Instrument Level
inline double synth(double v) { const double c = std::clamp(v, 0.0, 1.0); return lin20(1.4 * c * c); }  // Poly Synth Level
// dbx 160VU (125000, circuit unit) Output: the netlist's output pot after the VCA, measured in steps of 1/40 relative
// to 0.5 (pure gain: the waveform scales exactly). tests/test_level_fold.cpp checks it against the unit.
inline double dbx160(double v) {
    static constexpr double t[41] = {-19.670, -18.332, -17.057, -15.843, -14.681, -13.568, -12.497, -11.463, -10.464, -9.496, -8.554,
                                     -7.635,  -6.737,  -5.858,  -4.993,  -4.142,  -3.301,  -2.468,  -1.642,  -0.820,  0.000,  0.820,
                                     1.642,   2.468,   3.301,   4.142,   4.993,   5.858,   6.737,   7.635,   8.554,   9.496,  10.464,
                                     11.464,  12.497,  13.568,  14.681,  15.842,  17.055,  18.321,  19.643};
    const double x = std::clamp(v, 0.0, 1.0) * 40.0;
    const int i = std::min(39, static_cast<int>(x));
    return t[i] + (t[i + 1] - t[i]) * (x - i);
}
} // namespace detail

inline const std::vector<KnobSpec>& knobSpecs() {
    static const std::vector<KnobSpec> k = {
        {156000, "MV", "Volume", detail::cab156},    // Amp-Matched Cab
        {88000, "MV", "Volume", detail::cab88},      // Classic Amp-Matched Cab
        {119000, "v", "Volume", detail::cabRoom},    // Studio Cab Room
        {168000, "v", "Volume", detail::cabRoom},    // Cabinet IR loader
        {900100, "level", "Level", detail::pm12},    // Cabinet
        {25000, "Vol", "Output", detail::pm18},      // EQ Shelving
        {26000, "Vol", "Output", detail::pm18},      // EQ Parametric
        {32000, "V", "Output", detail::pm4},         // EQ Graphic
        {159000, "P6", "Output", detail::pm12},      // Tube Saturation Compressor (make-up, after the gain cell)
        {125000, "O2", "Output", detail::dbx160},    // dbx 160VU (output pot after the VCA)
        // Output gains after everything else in the unit: one law shared with the DSP (dsp/OutputTrim.hpp)
        {121000, "O14", "Output", dsp::trimlaw::ssl},           // SSL 4000 E channel EQ (after the card)
        {154000, "O22", "Output", dsp::trimlaw::lexicon},       // Lexicon 224XL (after the mix)
        {155000, "O45", "Output", dsp::trimlaw::lexicon},       // Lexicon 480L (after the mix)
        {157000, "output", "Output", dsp::trimlaw::shimmer},    // Dual-Pitch Shimmer Delay (after the mix)
        {158000, "output", "Output", dsp::trimlaw::multiMode},  // Studio Multi-Mode Delay (after the mix)
        {900003, "level", "Level", detail::inst},    // Instrument (pure only at Mix 100 %: see knobFor)
        {900004, "level", "Level", detail::synth},   // Poly Synth
    };
    return k;
}

// Units that pass level through linearly (scaling their input scales their output): walked past.
// Includes the modulation circuits (CE-1, Leslie, Small Stone, Electric Mistress, Tremulus) at line level, and the
// SSL channel EQ circuit. (The SSL, the Lexicons and the Shimmer / Multi-Mode delays have an Output knob in the table
// above, which is taken first.) The CE-1's Volume is its input Level pot (VR11) in front of a +33.6 dB preamp that
// saturates when turned up: not a level knob; the circuit has no output pot.
// Not in the table because their output stage colours the sound as it moves (measured: the waveform no longer just
// scales): LA-2A Gain (124000), 1176 Output (123000), Culture Vulture Volume (173000). Amp masters are never used.
inline bool passesLevelThrough(int32_t t) {
    switch (t) {
        case 13000: case 20000: case 27000: case 28000: case 29000: case 31000: case 33000: case 36000: case 37000:
        case 53000: case 74000:
        case 95000: case 98000: case 107000: case 114000: case 116000: case 118000: case 121000: case 126000: case 127000:
        case 132000: case 142000: case 148000: case 154000: case 155000: case 157000: case 158000: case 164000:
        case 900001: case 900002: case 900200:
            return true;
        default:
            return false;
    }
}

inline const preset::PresetParam* findParam(const preset::PresetComponentNode& n, std::string_view id) {
    for (const auto& p : n.params) if (p.id == id) return &p;
    return nullptr;
}
inline float paramValue(const preset::PresetComponentNode& n, std::string_view id) {
    if (const auto* p = findParam(n, id)) return p->float_val;
    if (const auto* s = catalog::findByTableId(n.table_id))
        for (const auto& sp : s->params) if (sp.id == id) return sp.default_value;
    return 0.5f;
}
inline bool isBypassed(const preset::PresetComponentNode& n) {
    if (const auto* p = findParam(n, "Pwr")) return p->float_val < 0.5f;
    return false;
}
inline const KnobSpec* knobFor(const preset::PresetComponentNode& n) {
    for (const auto& k : knobSpecs()) {
        if (k.table_id != n.table_id) continue;
        if (n.table_id == 900003 && paramValue(n, "mix") < 0.999f) return nullptr; // Level scales the instrument, not the dry thru
        return &k;
    }
    return nullptr;
}

// ───────────── finding the knobs ─────────────
struct KnobRef {
    preset::PresetComponentNode* node = nullptr;
    std::size_t preorder = 0; // index of the unit in document order (the N-th <component> of the .rig file)
    const KnobSpec* spec = nullptr;
};
using Stage = std::vector<KnobRef>; // one knob per parallel lane: moving all of them by x dB moves the rig by x dB
struct Path {
    std::vector<Stage> stages; // from the output backwards
    std::string stop;          // the unit that ended the walk ("" = reached the input)
};

namespace detail {
struct Flat { preset::PresetComponentNode* node; std::size_t preorder; };
inline void flatten(preset::PresetComponentNode& n, std::vector<Flat>& out, std::size_t& counter) {
    out.push_back({&n, counter++});
    for (auto& c : n.children) flatten(c, out, counter);
}
inline std::string unitName(int32_t t) {
    if (const auto* s = catalog::findByTableId(t)) return std::string(s->reference_name);
    return std::to_string(t);
}
// Walks seq[begin, end) backwards. Splits are flat siblings: 7000 (lane A) ... 8000 (lane B) ... 9000 (mix).
inline Path walk(const std::vector<Flat>& seq, std::size_t begin, std::size_t end) {
    Path path;
    std::size_t i = end;
    while (i > begin) {
        --i;
        auto& n = *seq[i].node;
        if (!n.children.empty()) { path.stop = unitName(n.table_id) + " (container)"; return path; }
        if (n.table_id == 9000) { // split mix: find this split's lane B (8000) and start (7000)
            int depth = 0;
            std::size_t b = end, a = end;
            for (std::size_t j = i; j > begin;) {
                --j;
                const int32_t t = seq[j].node->table_id;
                if (t == 9000) ++depth;
                else if (t == 8000 && depth == 0 && b == end) b = j;
                else if (t == 7000) {
                    if (depth == 0) { a = j; break; }
                    --depth;
                }
            }
            if (a == end || b == end) { path.stop = "an unbalanced split"; return path; }
            const Path la = walk(seq, a + 1, b), lb = walk(seq, b + 1, i);
            const std::size_t k = std::min(la.stages.size(), lb.stages.size());
            for (std::size_t s = 0; s < k; ++s) {
                Stage st = la.stages[s];
                st.insert(st.end(), lb.stages[s].begin(), lb.stages[s].end());
                path.stages.push_back(std::move(st));
            }
            if (k == 0) {
                path.stop = la.stages.empty() ? ("split lane A: " + (la.stop.empty() ? std::string("no level knob") : la.stop))
                                              : ("split lane B: " + (lb.stop.empty() ? std::string("no level knob") : lb.stop));
                return path;
            }
            if (!la.stop.empty() || !lb.stop.empty() || la.stages.size() != lb.stages.size()) {
                path.stop = !la.stop.empty() ? la.stop : lb.stop;
                return path; // knobs before a lane's stop would not be pure gain
            }
            i = a;
            continue;
        }
        if (n.table_id == 7000 || n.table_id == 8000) { path.stop = "an unbalanced split"; return path; }
        if (isBypassed(n)) continue;
        if (const KnobSpec* k = knobFor(n)) {
            path.stages.push_back({KnobRef{&n, seq[i].preorder, k}});
            continue;
        }
        if (passesLevelThrough(n.table_id)) continue;
        path.stop = unitName(n.table_id);
        return path;
    }
    return path;
}
} // namespace detail

// Every pure post-gain level knob on the rig's output path, nearest the output first. Pointers are into `p`.
inline Path findLevelKnobs(preset::RackPreset& p) {
    std::vector<detail::Flat> seq;
    std::size_t counter = 0;
    for (auto& c : p.root_rack.children) {
        const std::size_t before = seq.size();
        detail::flatten(c, seq, counter);
        // flatten() lists a container's children after it; the walk treats a container as a stop, so keep only
        // the top-level node in the walk order (its children keep their document-order indices).
        seq.resize(before + 1);
    }
    return detail::walk(seq, 0, seq.size());
}

// ───────────── folding a change into the knobs ─────────────
struct Move {
    std::size_t preorder = 0;
    int32_t table_id = 0;
    std::string unit, param, label;
    float from = 0, to = 0;
    double db = 0; // the knob's own change (dB)
};
struct FoldResult {
    double requested_db = 0, applied_db = 0;
    std::vector<Move> moves;
    std::string stop; // what ended the knob search (diagnostics)
    double residual() const { return requested_db - applied_db; }
};

// Raw value of the knob that gives `target_db` (monotonic db(): bisection), clamped to the knob's travel.
inline double valueForDb(const KnobSpec& k, double target_db) {
    double lo = 0.0, hi = 1.0;
    if (target_db <= k.db(lo)) return lo;
    if (target_db >= k.db(hi)) return hi;
    for (int it = 0; it < 60; ++it) {
        const double mid = 0.5 * (lo + hi);
        (k.db(mid) < target_db ? lo : hi) = mid;
    }
    return 0.5 * (lo + hi);
}
// Stored values are rounded to 1e-4 (a plain knob position in the file); the dB that rounding gives is what counts.
inline float roundKnob(double v) { return static_cast<float>(std::round(std::clamp(v, 0.0, 1.0) * 1e4) / 1e4); }

inline void setParam(preset::PresetComponentNode& n, std::string_view id, float v) {
    for (auto& p : n.params)
        if (p.id == id) { p.float_val = v; return; }
    preset::PresetParam np;
    np.id = std::string(id);
    np.type = "float";
    np.float_val = v;
    n.params.push_back(std::move(np));
}

// Puts `db` into the rig's level knobs (changes `p`). Knobs nearest the output first; each stage moves all its
// lanes' knobs by the same amount, limited by the knob in that stage with the least travel left.
inline FoldResult foldLevel(preset::RackPreset& p, double db) {
    FoldResult r;
    r.requested_db = db;
    Path path = findLevelKnobs(p);
    r.stop = path.stop;
    double left = db;
    for (auto& stage : path.stages) {
        if (std::abs(left) < 0.005) break;
        // travel this stage can give in the requested direction (min over its lanes)
        double reach = left;
        for (const auto& ref : stage) {
            const double cur = ref.spec->db(paramValue(*ref.node, ref.spec->param));
            const double lim = (left > 0 ? ref.spec->db(1.0) : ref.spec->db(0.0)) - cur;
            reach = left > 0 ? std::min(reach, std::max(0.0, lim)) : std::max(reach, std::min(0.0, lim));
        }
        if (std::abs(reach) < 0.005) continue;
        double moved = reach;
        for (auto& ref : stage) {
            Move m;
            m.preorder = ref.preorder;
            m.table_id = ref.node->table_id;
            m.unit = detail::unitName(ref.node->table_id);
            m.param = ref.spec->param;
            m.label = ref.spec->label;
            m.from = paramValue(*ref.node, ref.spec->param);
            const double cur = ref.spec->db(m.from);
            m.to = roundKnob(valueForDb(*ref.spec, cur + reach));
            m.db = ref.spec->db(m.to) - cur;
            moved = std::abs(m.db) < std::abs(moved) ? m.db : moved; // the stage moves the rig by its smallest move
            setParam(*ref.node, ref.spec->param, m.to);
            r.moves.push_back(std::move(m));
        }
        r.applied_db += moved;
        left = db - r.applied_db;
    }
    return r;
}

// ───────────── Output knobs that used to be stored only ─────────────
// The SSL channel EQ, Lexicon 224XL / 480L and Shimmer / Multi-Mode delay Output knobs did nothing in earlier builds
// (stored only); they are now a gain on the unit's output (dsp/OutputTrim.hpp). A gain measured by an earlier build
// (the retired loudness cache, a DAW project's preset_gain) was measured with them dead, so a rig migrated with it
// must not also get what those knobs now add. liveTrimDb returns the dB they now add to the rig at their stored
// values, for the knobs on the rig's linear tail (the level knobs findLevelKnobs finds: a gain there moves the whole
// rig and nothing else; a split stage counts when every lane's knob adds the same). Any other such knob not at 0 dB
// (in front of an amp, a drive, a compressor..., where it would now change the drive, or in one split lane only,
// where it would change the lanes' balance) is set back to its default, 0 dB, exactly how the rig sounded; those are
// listed in `restored` and change `p`.
struct LiveTrim {
    double db = 0;               // what the newly live knobs on the linear tail add to the rig (dB)
    std::vector<Move> restored;  // knobs set back to their default
};
namespace detail {
inline void restoreTrimsExcept(preset::PresetComponentNode& n, std::size_t& counter, const std::vector<const preset::PresetComponentNode*>& keep,
                               LiveTrim& out) {
    const std::size_t pre = counter++;
    if (const auto* law = dsp::outputTrimLaw(n.table_id);
        law && !isBypassed(n) && std::find(keep.begin(), keep.end(), &n) == keep.end() && law->db(paramValue(n, law->param)) != 0.0) {
        Move m;
        m.preorder = pre;
        m.table_id = n.table_id;
        m.unit = unitName(n.table_id);
        m.param = law->param;
        m.label = "Output";
        m.from = paramValue(n, law->param);
        m.to = roundKnob(law->def);
        m.db = law->db(m.to) - law->db(m.from);
        setParam(n, law->param, m.to);
        out.restored.push_back(std::move(m));
    }
    for (auto& c : n.children) restoreTrimsExcept(c, counter, keep, out);
}
} // namespace detail

inline LiveTrim liveTrimDb(preset::RackPreset& p) {
    LiveTrim out;
    std::vector<const preset::PresetComponentNode*> keep; // trim knobs whose dB moves the whole rig
    for (const auto& stage : findLevelKnobs(p).stages) {
        bool all_trim = true, any_trim = false;
        for (const auto& r : stage) {
            const bool t = dsp::outputTrimLaw(r.node->table_id) != nullptr;
            all_trim &= t;
            any_trim |= t;
        }
        if (!any_trim) continue;
        const auto dbOf = [](const KnobRef& r) { return r.spec->db(paramValue(*r.node, r.spec->param)); };
        bool same = all_trim;
        for (const auto& r : stage) same = same && std::abs(dbOf(r) - dbOf(stage.front())) < 0.005;
        if (!same) continue; // lanes would move apart: set back below
        out.db += dbOf(stage.front());
        for (const auto& r : stage) keep.push_back(r.node);
    }
    std::size_t counter = 0;
    for (auto& n : p.root_rack.children) detail::restoreTrimsExcept(n, counter, keep, out);
    return out;
}

// A gain measured by an earlier build (loudness cache, DAW preset_gain), folded into the rig's knobs so it plays at
// exactly the level it played then: the gain minus what the newly live Output knobs now add (liveTrimDb). The
// result's moves start with the knobs liveTrimDb set back (if any); requested_db is the dB that was folded.
struct MigrationFold {
    FoldResult fold;
    double live_db = 0;   // what the newly live knobs add at the rig's stored values
    double change_db = 0; // the rig's level after the migration minus its level in the earlier build (0 = no jump)
};
inline MigrationFold foldLegacyGain(preset::RackPreset& p, double legacy_db) {
    MigrationFold m;
    LiveTrim lt = liveTrimDb(p);
    m.live_db = lt.db;
    m.fold = foldLevel(p, legacy_db - lt.db);
    m.fold.moves.insert(m.fold.moves.begin(), lt.restored.begin(), lt.restored.end());
    m.change_db = lt.db + m.fold.applied_db - legacy_db;
    return m;
}

// ───────────── writing knob values into a .rig file ─────────────
// Sets one parameter of the N-th <component> (document order) of a .rig file's XML and leaves every other byte as
// it is (Library rigs carry <headroom-library> blocks the preset writer would not reproduce). Adds the parameter
// when the unit had it at its default. False when the component or its <parameters> block isn't found.
inline bool setParamInRigXml(std::string& xml, std::size_t preorder, std::string_view param, float value) {
    char vbuf[32];
    std::snprintf(vbuf, sizeof vbuf, "%.4f", static_cast<double>(value));
    std::string v(vbuf);
    while (v.size() > 1 && v.back() == '0') v.pop_back();
    if (!v.empty() && v.back() == '.') v.pop_back();
    const std::string open = "<component id=\"";
    std::size_t pos = xml.find("<non-fix-components>");
    if (pos == std::string::npos) return false;
    for (std::size_t k = 0;; ++k) {
        pos = xml.find(open, pos);
        if (pos == std::string::npos) return false;
        if (k == preorder) break;
        pos += open.size();
    }
    const std::size_t pend = xml.find("</parameters>", pos);
    const std::size_t next = xml.find(open, pos + open.size());
    if (pend == std::string::npos || (next != std::string::npos && next < pend)) return false;
    const std::string key = "<parameter id=\"" + std::string(param) + "\"";
    const std::size_t at = xml.find(key, pos);
    if (at != std::string::npos && at < pend) {
        const std::size_t va = xml.find("value=\"", at);
        const std::size_t ve = va == std::string::npos ? va : xml.find('"', va + 7);
        const std::size_t tag_end = xml.find('>', at);
        if (va == std::string::npos || ve == std::string::npos || va > tag_end) return false;
        xml.replace(va + 7, ve - va - 7, v);
        return true;
    }
    // not stored (default): add it on its own line, indented two spaces more than </parameters>
    std::size_t ls = xml.rfind('\n', pend);
    ls = ls == std::string::npos ? 0 : ls + 1;
    const std::string indent = xml.substr(ls, pend - ls);
    const bool own_line = indent.find_first_not_of(" \t") == std::string::npos;
    if (own_line) xml.insert(ls, indent + "  " + key + " value=\"" + v + "\"/>\n");
    else xml.insert(pend, key + " value=\"" + v + "\"/>");
    // num-parameters="N" of this block, if present
    const std::size_t ps = xml.rfind("<parameters", pend);
    if (ps != std::string::npos && ps > pos) {
        const std::size_t na = xml.find("num-parameters=\"", ps);
        if (na != std::string::npos && na < pend) {
            const std::size_t ne = xml.find('"', na + 16);
            try {
                const int cnt = std::stoi(xml.substr(na + 16, ne - na - 16));
                xml.replace(na + 16, ne - na - 16, std::to_string(cnt + 1));
            } catch (...) {}
        }
    }
    return true;
}

// Applies `moves` to the .rig file at `path` (text edit, temp file + rename). When `backup` is non-empty the
// original bytes are copied there first (never overwritten if it exists).
inline bool applyMovesToFile(const std::string& path, const std::vector<Move>& moves, const std::string& backup, std::string* err) {
    namespace fs = std::filesystem;
    std::string xml;
    {
        std::ifstream f(path, std::ios::binary);
        if (!f) { if (err) *err = "cannot read " + path; return false; }
        std::ostringstream ss;
        ss << f.rdbuf();
        xml = ss.str();
    }
    const std::string original = xml;
    for (const auto& m : moves)
        if (!setParamInRigXml(xml, m.preorder, m.param, m.to)) {
            if (err) *err = "unit " + std::to_string(m.preorder) + " (" + m.unit + ") not found in " + path;
            return false;
        }
    std::error_code ec;
    if (!backup.empty() && !fs::exists(backup, ec)) {
        std::ofstream b(backup, std::ios::binary | std::ios::trunc);
        b << original;
        if (!b) { if (err) *err = "cannot write the backup " + backup; return false; }
    }
    const std::string tmp = path + ".level-tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        o << xml;
        if (!o) { if (err) *err = "cannot write " + tmp; return false; }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        if (err) *err = "cannot replace " + path;
        return false;
    }
    return true;
}

} // namespace rig::levelfold
