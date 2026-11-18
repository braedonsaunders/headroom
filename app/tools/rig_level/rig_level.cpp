// rig_level: levels rigs OFFLINE, once, into their own level knobs (the way a preset designer masters a library).
//
// The engine never measures or re-levels a rig while it plays. The shipped library (presets/Showcase, presets/Library)
// is matched here instead: each rig is rendered with the calibration DI (bass rigs: the bass DI) and its K-weighted
// loudness compared with the library target (RackGraph::calibrateLoudness); the correction is folded into the rig's
// own pure post-gain level knobs (core/LevelFold.hpp: a cabinet's Volume, an EQ's Output, an instrument's Level, ...)
// and the rig is rendered again to verify. A second pass refines what the first left (knob rounding, units that are
// only nearly linear). With --write the knob values are written into the .rig files as ordinary parameter values
// (a text edit of those parameters only; every other byte stays).
//
// --add-level-eq TOL: a rig the folding leaves more than TOL dB off (no level knob, or none with enough travel) gets
// one visible unit at the end of its chain whose job is level: an EQ Parametric (26000) with both bands at 0 dB
// (it passes the signal bit for bit) and the whole correction on its Output (+-18 dB); the rig's other knobs keep
// their values. Used for the shipped library (tools/library/build_library.py runs it after writing the rigs), so a
// rebuild reproduces the levelled files. Measurements use the fixed noise seed, so a rerun gives the same knobs.
//
//   rig_level [--write] [--add-level-eq TOL] [--root REPO] [--tsv out.tsv] <rig file | folder>...
//
// Output: one TSV line per rig: path, measured correction, folded, verified residual, knobs (unit.param from→to),
// and what ended the knob search. Needs an isolated HEADROOM_DATA_DIR (amp trims are measured into it).
#include "platform/Environment.hpp"
#include "../../src/core/RackGraph.hpp"
#include "../../src/core/LevelFold.hpp"
#include "../../src/platform/Paths.hpp"
#include "../../src/midi/MidiComponents.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
constexpr double kSr = 48000.0;

// A flat EQ Parametric: every band at 0 dB (bit-exact pass-through), Output 0 dB; its Output is the rig's level.
constexpr const char* kLevelEq =
    "  <component id=\"26000\">\n"
    "    <component-audio version=\"1\">\n"
    "      <parameters num-parameters=\"8\">\n"
    "        <parameter id=\"Vol\" value=\"0.5\"/>\n"
    "        <parameter id=\"F1\" value=\"0.5\"/>\n"
    "        <parameter id=\"G1\" value=\"0.5\"/>\n"
    "        <parameter id=\"Q1\" value=\"0.5\"/>\n"
    "        <parameter id=\"F2\" value=\"0.5\"/>\n"
    "        <parameter id=\"G2\" value=\"0.5\"/>\n"
    "        <parameter id=\"Pwr\" value=\"1\"/>\n"
    "        <parameter id=\"Q2\" value=\"0.5\"/>\n"
    "      </parameters>\n"
    "    </component-audio>\n"
    "  </component>\n";

std::string readText(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

float measure(const rig::preset::RackPreset& p) {
    rig::RackGraph g;
    g.buildFromPreset(p, kSr, 512);
    return g.calibrateLoudness();
}

std::string fmtMoves(const std::map<std::pair<std::size_t, std::string>, rig::levelfold::Move>& moves) {
    std::string s;
    char b[200];
    for (const auto& [k, m] : moves) {
        std::snprintf(b, sizeof b, "%s%s.%s(%s) %.4g->%.4g (%+.1f dB)", s.empty() ? "" : "; ", m.unit.c_str(), m.param.c_str(),
                      m.label.c_str(), static_cast<double>(m.from), static_cast<double>(m.to), m.db);
        s += b;
    }
    return s;
}
struct Levelled {
    std::map<std::pair<std::size_t, std::string>, rig::levelfold::Move> moves; // final value per knob
    double applied = 0.0, residual = 0.0;
    std::string stop;
};
// Folds the measured correction c0 into a copy of `orig`'s knobs, re-measuring and refining (knob rounding, units that
// are only nearly linear) up to three times.
Levelled levelOne(const rig::preset::RackPreset& orig, float c0) {
    Levelled lv;
    rig::preset::RackPreset p = orig;
    lv.residual = c0;
    for (int pass = 0; pass < 3 && std::abs(lv.residual) >= 0.05; ++pass) {
        const auto r = rig::levelfold::foldLevel(p, lv.residual);
        lv.stop = r.stop;
        if (r.moves.empty()) break;
        for (const auto& m : r.moves) {
            auto key = std::make_pair(m.preorder, m.param);
            auto it = lv.moves.find(key);
            if (it == lv.moves.end()) lv.moves[key] = m;
            else { it->second.to = m.to; it->second.db += m.db; }
        }
        lv.applied += r.applied_db;
        const float v = measure(p);
        if (!std::isfinite(v)) break;
        lv.residual = v;
    }
    return lv;
}
} // namespace

int main(int argc, char** argv) {
    bool write = false;
    double add_eq_tol = -1.0; // < 0: never add a unit
    std::string tsv_path, root_arg;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--write") write = true;
        else if (a == "--tsv" && i + 1 < argc) tsv_path = argv[++i];
        else if (a == "--add-level-eq" && i + 1 < argc) add_eq_tol = std::atof(argv[++i]);
        else if (a == "--root" && i + 1 < argc) root_arg = argv[++i];
        else if (fs::is_directory(a)) {
            for (const auto& e : fs::recursive_directory_iterator(a))
                if (e.is_regular_file() && e.path().extension() == ".rig") files.push_back(e.path().string());
        } else files.push_back(a);
    }
    if (files.empty()) {
        std::fprintf(stderr, "usage: rig_level [--write] [--add-level-eq TOL] [--root REPO] [--tsv out.tsv] <rig file | folder>...\n");
        return 2;
    }
    const char* dd = std::getenv("HEADROOM_DATA_DIR");
    if (!dd || !*dd) {
        std::fprintf(stderr, "refusing to run without an isolated HEADROOM_DATA_DIR (amp trims are measured into it)\n");
        return 2;
    }
    if (!std::getenv("HEADROOM_NOISE_FIXED_SEED")) rig::platform::setEnv("HEADROOM_NOISE_FIXED_SEED", "1"); // circuits' noise: the same every run, so the knobs are too
    std::sort(files.begin(), files.end());
    // Content roots as the app sets them (RealtimeAudioServer::start): <repo>/app/presets/... -> <repo> (or --root)
    fs::path root = root_arg.empty() ? fs::absolute(files.front()) : fs::path(root_arg);
    while (root_arg.empty() && !root.empty() && root != root.root_path() &&
           !(fs::exists(root / "app" / "src") && fs::exists(root / "app" / "presets")))
        root = root.parent_path();
    rig::platform::setAppRoot(root.string());
    // Pitch -> MIDI tracks with the neural model in the app (RealtimeAudioServer::start), so a hybrid rig is measured
    // with it too (HEADROOM_P2M_MODEL=<file> overrides, "-" keeps the classic tracker).
    if (const char* m = std::getenv("HEADROOM_P2M_MODEL")) {
        if (std::string(m) != "-") rig::setNeuralGuitarModelPath(m);
    } else if (fs::exists(root / "app" / "resources" / "models" / "guitar_midi.hgnn")) {
        rig::setNeuralGuitarModelPath((root / "app" / "resources" / "models" / "guitar_midi.hgnn").string());
    }

    FILE* tsv = tsv_path.empty() ? nullptr : std::fopen(tsv_path.c_str(), "a");
    int failures = 0;
    for (const auto& path : files) {
        rig::preset::RackPreset orig;
        std::string err;
        if (!rig::preset::loadPreset(path, orig, &err)) {
            std::fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str());
            ++failures;
            continue;
        }
        const float c0 = measure(orig);
        std::string line;
        char b[512];
        if (!std::isfinite(c0)) {
            std::snprintf(b, sizeof b, "%s\tsilent\t\t\t\t\n", path.c_str());
            line = b;
        } else {
            Levelled lv = levelOne(orig, c0);
            bool added_eq = false;
            std::string xml;
            if (add_eq_tol >= 0.0 && std::abs(lv.residual) > add_eq_tol) {
                // No knob (with travel enough): a flat EQ Parametric at the end of the chain carries the whole
                // correction; the rig's other knobs keep their values.
                xml = readText(path);
                const auto at = xml.find("</non-fix-components>");
                rig::preset::RackPreset p2;
                if (at != std::string::npos && xml.find("</non-fix-components>", at + 1) == std::string::npos) {
                    xml.insert(at, kLevelEq);
                    if (rig::preset::parsePresetData(xml, path, p2, &err)) {
                        lv = levelOne(p2, c0);
                        added_eq = true;
                    }
                }
            }
            std::snprintf(b, sizeof b, "%s\t%+.1f\t%+.1f\t%+.1f\t", path.c_str(), static_cast<double>(c0), lv.applied, lv.residual);
            line = b + std::string(added_eq ? "added EQ Parametric (level); " : "") + fmtMoves(lv.moves) + "\t" + lv.stop + "\n";
            if (write && (added_eq || !lv.moves.empty())) {
                std::vector<rig::levelfold::Move> list;
                for (const auto& [k, m] : lv.moves) list.push_back(m);
                bool ok = true;
                if (added_eq) {
                    for (const auto& m : list) ok = ok && rig::levelfold::setParamInRigXml(xml, m.preorder, m.param, m.to);
                    const std::string tmp = path + ".level-tmp";
                    { std::ofstream o(tmp, std::ios::binary | std::ios::trunc); o << xml; ok = ok && static_cast<bool>(o); }
                    std::error_code ec;
                    if (ok) fs::rename(tmp, path, ec);
                    ok = ok && !ec;
                    if (!ok) err = "cannot write the rig with its level EQ";
                } else {
                    ok = rig::levelfold::applyMovesToFile(path, list, std::string(), &err);
                }
                if (!ok) {
                    std::fprintf(stderr, "%s: write failed: %s\n", path.c_str(), err.c_str());
                    ++failures;
                }
            }
        }
        std::fputs(line.c_str(), stdout);
        std::fflush(stdout);
        if (tsv) { std::fputs(line.c_str(), tsv); std::fflush(tsv); }
    }
    if (tsv) std::fclose(tsv);
    return failures ? 1 : 0;
}
