// Level folding (core/LevelFold.hpp): levels live in the rigs' own knobs, never in a hidden engine gain.
//  * every knob in the table is a pure output gain, and its dB map matches what the unit's DSP actually does;
//  * the Output knobs wired after being stored-only (SSL channel EQ, Lexicon 224XL / 480L, Shimmer / Multi-Mode
//    delays) move the level, and at their default position null the unit's old output (< -100 dB); the CE-1's
//    Volume moves the level too but is its input Level pot (it drives a preamp), so it is not a level knob;
//  * the knob search: nearest the output first, linear units walked past, amps / drives end it, bypassed units
//    skipped, split lanes moved together, nothing forced past a knob's travel (the rest is reported);
//  * a fold moves the rendered rig by exactly the folded amount (the master section adds no gain of its own);
//  * the .rig text edit changes only that parameter (and adds it when it was at its default);
//  * a folded knob round-trips through save / load.
#undef NDEBUG
#include "platform/Environment.hpp"
#include "../src/core/RackGraph.hpp"
#include "../src/core/LevelFold.hpp"
#include "../src/library/Library.hpp"
#include "../src/preset/PresetLoader.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>
#include "test_compat.hpp"

namespace {
int g_failures = 0;
void check(bool ok, const std::string& what) {
    std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what.c_str());
    if (!ok) ++g_failures;
}
constexpr double kSr = 48000.0;
namespace lf = rig::levelfold;

std::vector<float> noise(std::size_t n) {
    std::mt19937 rng(7);
    std::normal_distribution<float> nd(0.0f, 0.08f);
    std::vector<float> x(n);
    for (auto& v : x) v = nd(rng);
    return x;
}

// Output of a unit for input x (mono in, left out), from a fresh prepare. param == nullptr: every knob untouched.
std::vector<float> renderUnit(int32_t table_id, const char* param, float value, const std::vector<float>& x) {
    auto c = rig::createComponentByTableId(table_id);
    if (!c) return {};
    if (param) c->setParameter(param, value);
    c->prepare(kSr, 256);
    c->reset();
    std::vector<float> out(x.size()), r(256);
    for (std::size_t p = 0; p + 256 <= x.size(); p += 256) {
        c->processControlRate();
        const float* ins[2] = {x.data() + p, x.data() + p};
        float* outs[2] = {out.data() + p, r.data()};
        rig::AudioBlock b{};
        b.in = ins; b.out = outs; b.num_channels = 2; b.num_frames = 256; b.sample_rate = kSr;
        c->processAudio(b);
    }
    return out;
}

std::string rigXml(const std::vector<std::pair<int, std::string>>& units) {
    std::string s = R"(<?xml version="1.0" encoding="UTF-8"?>
<headroom-preset version="1"><headroom-info><soundinfo><properties><name>t</name></properties></soundinfo></headroom-info>
<headroom-rig><rack><non-fix-components>
)";
    for (const auto& [t, params] : units)
        s += "  <component id=\"" + std::to_string(t) + "\">\n    <component-audio version=\"1\">\n      <parameters num-parameters=\"2\">\n"
             "        <parameter id=\"Pwr\" value=\"1\"/>\n" + params + "      </parameters>\n    </component-audio>\n  </component>\n";
    return s + "</non-fix-components></rack></headroom-rig>\n<headroom-library version=\"1\" id=\"keep-me\"></headroom-library>\n</headroom-preset>\n";
}
std::string par(const char* id, float v) {
    char b[96];
    std::snprintf(b, sizeof b, "        <parameter id=\"%s\" value=\"%g\"/>\n", id, static_cast<double>(v));
    return b;
}
rig::preset::RackPreset parse(const std::string& xml) {
    rig::preset::RackPreset p;
    std::string err;
    const bool ok = rig::preset::parsePresetData(xml, "test.rig", p, &err);
    if (!ok) std::printf("parse error: %s\n", err.c_str());
    return p;
}
float paramOf(const rig::preset::RackPreset& p, std::size_t top, const char* id) { return lf::paramValue(p.root_rack.children.at(top), id); }

// Rendered level (dB) of a whole rack, master section included, for a steady chord.
double rackLevelDb(const rig::preset::RackPreset& p) {
    rig::RackGraph g;
    g.buildFromPreset(p, kSr, 512);
    const std::size_t n = static_cast<std::size_t>(kSr * 1.5);
    std::vector<float> in(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kSr;
        in[i] = static_cast<float>(0.03 * (std::sin(2 * M_PI * 110 * t) + std::sin(2 * M_PI * 165 * t) + std::sin(2 * M_PI * 220 * t)));
    }
    std::vector<float> l(256), r(256);
    double e = 0;
    std::size_t cnt = 0;
    for (std::size_t pos = 0; pos + 256 <= n; pos += 256) {
        const float* ins[2] = {in.data() + pos, in.data() + pos};
        float* outs[2] = {l.data(), r.data()};
        rig::AudioBlock b{};
        b.in = ins; b.out = outs; b.num_channels = 2; b.num_frames = 256; b.sample_rate = kSr;
        g.process(b);
        if (pos > n / 3)
            for (std::size_t i = 0; i < 256; ++i) { e += 0.5 * (double(l[i]) * l[i] + double(r[i]) * r[i]); ++cnt; }
    }
    return 10.0 * std::log10(std::max(1e-30, e / double(cnt)));
}
} // namespace

int main() {
    rig::platform::setEnv("HEADROOM_NOISE_FIXED_SEED", "1");

    // ── every knob in the table: a pure output gain whose dB map matches the unit's DSP ──
    {
        const auto x = noise(static_cast<std::size_t>(kSr * 1.0));
        for (const auto& k : lf::knobSpecs()) {
            if (k.table_id == 900003 || k.table_id == 900004) continue; // instruments: MIDI-driven (Level = 1.6 / 1.4 x level^2 in their sources)
            for (const float v : {0.2f, 0.8f}) {
                const auto a = renderUnit(k.table_id, k.param, 0.5f, x), b = renderUnit(k.table_id, k.param, v, x);
                if (a.empty() || b.empty()) { check(false, std::to_string(k.table_id) + " can be created"); continue; }
                // from 0.25 s (glides settled): b = g * a sample by sample, g = the map's difference
                double ea = 0, eb = 0, ab = 0;
                for (std::size_t i = x.size() / 4; i < x.size(); ++i) { ea += double(a[i]) * a[i]; eb += double(b[i]) * b[i]; ab += double(a[i]) * b[i]; }
                const double meas = 10.0 * std::log10(eb / ea), want = k.db(v) - k.db(0.5);
                const double corr = ab / std::sqrt(ea * eb);
                char msg[200];
                std::snprintf(msg, sizeof msg, "%d %s %s at %.1f: %+.2f dB (map %+.2f), correlation %.5f", k.table_id, k.param, k.label,
                              static_cast<double>(v), meas, want, corr);
                check(std::abs(meas - want) < 0.1 && corr > 0.9999, msg);
            }
        }
    }

    // ── the Output knobs that used to be stored only: alive, and silent at their default ──
    {
        const auto x = noise(static_cast<std::size_t>(kSr * 1.0));
        struct Wired { int32_t t; const char* param; std::vector<float> defaults; };
        const Wired wired[] = {
            {121000, "O14", {0.5f}},                // SSL 4000 E channel EQ
            {154000, "O22", {0.5f}},                // Lexicon 224XL
            {155000, "O45", {0.5f}},                // Lexicon 480L
            {157000, "output", {0.752405f, 0.7524f}}, // Shimmer Delay: the catalogue default and its stored rounding
            {158000, "output", {0.722405f, 0.7224f}}, // Multi-Mode Delay
        };
        for (const auto& w : wired) {
            const auto* spec = [&]() -> const lf::KnobSpec* {
                for (const auto& k : lf::knobSpecs()) if (k.table_id == w.t && std::string(k.param) == w.param) return &k;
                return nullptr;
            }();
            check(spec != nullptr, std::to_string(w.t) + " " + w.param + " is a level knob in the table");
            const auto untouched = renderUnit(w.t, nullptr, 0.0f, x);
            double e0 = 0;
            for (std::size_t i = x.size() / 4; i < x.size(); ++i) e0 += double(untouched[i]) * untouched[i];
            for (const float d : w.defaults) {
                const auto at = renderUnit(w.t, w.param, d, x);
                double en = 0;
                for (std::size_t i = 0; i < x.size(); ++i) en += double(at[i] - untouched[i]) * (at[i] - untouched[i]);
                const double null_db = 10.0 * std::log10(std::max(en, 1e-300) / std::max(e0, 1e-300));
                char msg[200];
                std::snprintf(msg, sizeof msg, "%d %s at its default %.6g nulls the untouched unit: %.1f dB", w.t, w.param, static_cast<double>(d), null_db);
                check(null_db < -100.0, msg);
            }
            for (const float v : {0.0f, 1.0f}) {
                const auto at = renderUnit(w.t, w.param, v, x);
                double e = 0;
                for (std::size_t i = x.size() / 4; i < x.size(); ++i) e += double(at[i]) * at[i];
                const double meas = 10.0 * std::log10(e / e0), want = spec ? spec->db(v) - spec->db(w.defaults[0]) : 0.0;
                char msg[200];
                std::snprintf(msg, sizeof msg, "%d %s at %.0f moves the output %+.2f dB (map %+.2f)", w.t, w.param, static_cast<double>(v), meas, want);
                check(std::abs(meas) > 6.0 && std::abs(meas - want) < 0.1, msg);
            }
        }
        // CE-1 Volume: VR11, the input Level pot ahead of IC-1 (+33.6 dB). It moves the level, but up high it drives
        // the preamp (the waveform no longer just scales), so it is walked past, never used as a level knob.
        const auto a = renderUnit(29000, "Vol", 0.5f, x), b = renderUnit(29000, "Vol", 0.2f, x), c = renderUnit(29000, "Vol", 1.0f, x);
        double ea = 0, eb = 0, ec = 0, ac = 0;
        for (std::size_t i = x.size() / 4; i < x.size(); ++i) {
            ea += double(a[i]) * a[i]; eb += double(b[i]) * b[i]; ec += double(c[i]) * c[i]; ac += double(a[i]) * c[i];
        }
        char msg[200];
        std::snprintf(msg, sizeof msg, "CE-1 Volume (input Level) is alive: 0.2 %+.1f dB, 1.0 %+.1f dB (correlation %.4f at 1.0)",
                      10.0 * std::log10(eb / ea), 10.0 * std::log10(ec / ea), ac / std::sqrt(ea * ec));
        check(10.0 * std::log10(eb / ea) < -6.0 && 10.0 * std::log10(ec / ea) > 6.0, msg);
        bool listed = false;
        for (const auto& k : lf::knobSpecs()) listed |= k.table_id == 29000;
        check(!listed && lf::passesLevelThrough(29000), "...and is not a level knob (the CE-1 is walked past)");
    }

    // ── knob search and folding ──
    const std::string amp = par("Gn", 0.5f);
    {
        // amp -> cab (MV 0.5) -> plate reverb: the cab's Volume takes it all
        auto p = parse(rigXml({{34000, ""}, {156000, par("MV", 0.5f)}, {127000, ""}}));
        const auto r = lf::foldLevel(p, 6.0);
        check(r.moves.size() == 1 && r.moves[0].table_id == 156000 && std::abs(r.applied_db - 6.0) < 0.01 &&
                  std::abs(lf::detail::cab156(paramOf(p, 1, "MV")) - 6.0) < 0.01,
              "+6 dB goes into the Amp-Matched Cab Volume behind the reverb (MV " + std::to_string(paramOf(p, 1, "MV")) + ")");
        check(r.stop.find("Plexi") != std::string::npos || r.stop.find("1959") != std::string::npos, "the search ends at the amp (" + r.stop + ")");
    }
    {
        // amp -> cab (MV 0.5) -> graphic EQ (Output 0.5): nearest the output first, the rest one knob back
        auto p = parse(rigXml({{34000, ""}, {156000, par("MV", 0.5f)}, {32000, par("V", 0.5f)}}));
        const auto r = lf::foldLevel(p, 10.0);
        check(r.moves.size() == 2 && r.moves[0].table_id == 32000 && std::abs(paramOf(p, 2, "V") - 1.0f) < 1e-6f &&
                  std::abs(lf::detail::cab156(paramOf(p, 1, "MV")) - 6.0) < 0.01 && std::abs(r.applied_db - 10.0) < 0.01,
              "+10 dB: graphic EQ Output +4 dB (its travel), the cab Volume +6 dB");
    }
    {
        // amp -> cab -> Lexicon 480L: the reverb's Output, nearest the output, takes it; the cab is left alone
        auto p = parse(rigXml({{34000, ""}, {156000, par("MV", 0.5f)}, {155000, ""}}));
        const auto r = lf::foldLevel(p, -5.0);
        check(r.moves.size() == 1 && r.moves[0].table_id == 155000 && std::abs(r.applied_db + 5.0) < 0.01 &&
                  std::abs(paramOf(p, 1, "MV") - 0.5f) < 1e-6f,
              "-5 dB goes into the Lexicon 480L Output (O45 " + std::to_string(paramOf(p, 2, "O45")) + ")");
        // ... and a Multi-Mode Delay at its stored default (0.7224): +12 dB of travel, the rest one knob back
        auto q = parse(rigXml({{34000, ""}, {156000, par("MV", 0.5f)}, {158000, par("output", 0.7224f)}}));
        const auto rq = lf::foldLevel(q, 14.0);
        check(rq.moves.size() == 2 && rq.moves[0].table_id == 158000 && std::abs(paramOf(q, 2, "output") - 1.0f) < 1e-6f &&
                  std::abs(rq.applied_db - 14.0) < 0.02,
              "+14 dB: the Multi-Mode Delay Output to the top (+12 dB), the cab Volume the rest");
    }
    {
        // a knob at the end of its travel: nothing forced, the rest reported
        auto p = parse(rigXml({{34000, ""}, {156000, par("MV", 1.0f)}}));
        const auto r = lf::foldLevel(p, 3.0);
        check(r.moves.empty() && std::abs(r.residual() - 3.0) < 1e-9, "a cab Volume already at max takes nothing; 3 dB reported");
        const auto r2 = lf::foldLevel(p, -3.0);
        check(r2.moves.size() == 1 && std::abs(r2.applied_db + 3.0) < 0.01, "...and still takes a cut");
    }
    {
        // bypassed units are skipped; an amp with no cab after it gives no knob (its master is never touched)
        auto p = parse(rigXml({{56000, par("V2", 0.5f)}, {156000, par("MV", 0.5f) + "        <parameter id=\"Pwr\" value=\"0\"/>\n"}}));
        p.root_rack.children[1].params.erase(p.root_rack.children[1].params.begin()); // drop the leading Pwr=1
        const auto r = lf::foldLevel(p, -4.0);
        check(r.moves.empty() && !r.stop.empty(), "a bypassed cab is skipped and the amp master is not used (stop: " + r.stop + ")");
    }
    {
        // split: lane A amp -> cab, lane B pitch -> instrument (Mix 100 %): both lanes move together
        auto p = parse(rigXml({{7000, ""}, {34000, ""}, {156000, par("MV", 0.5f)}, {8000, ""}, {900001, ""},
                               {900003, par("level", 0.7f) + par("mix", 1.0f)}, {9000, ""}, {33000, ""}}));
        const auto r = lf::foldLevel(p, -3.0);
        bool cab = false, inst = false;
        for (const auto& m : r.moves) { cab |= m.table_id == 156000 && std::abs(m.db + 3.0) < 0.02; inst |= m.table_id == 900003 && std::abs(m.db + 3.0) < 0.02; }
        check(cab && inst && std::abs(r.applied_db + 3.0) < 0.02, "a split moves the knob of each lane by the same -3 dB");
        auto q = parse(rigXml({{7000, ""}, {34000, ""}, {8000, ""}, {900001, ""}, {900003, par("level", 0.7f) + par("mix", 1.0f)}, {9000, ""}}));
        check(lf::foldLevel(q, -3.0).moves.empty(), "a lane without a level knob blocks the split (nothing moved, reported)");
        auto m = parse(rigXml({{900001, ""}, {900003, par("level", 0.7f) + par("mix", 0.6f)}}));
        check(lf::foldLevel(m, -3.0).moves.empty(), "an Instrument with the guitar mixed in is not a pure level knob");
    }

    // ── migration of a gain measured while the Output knobs were dead (liveTrimDb / foldLegacyGain) ──
    {
        namespace tl = rig::dsp::trimlaw;
        // serial: cab -> SSL 0.695 -> Multi-Mode 0.762405 -> bypassed 480L at 0.53: SSL + delay count, the bypassed one not
        auto p = parse(rigXml({{156000, par("MV", 0.5f)}, {121000, par("O14", 0.695f)}, {158000, par("output", 0.762405f)},
                               {155000, par("O45", 0.53f) + "        <parameter id=\"Pwr\" value=\"0\"/>\n"}}));
        p.root_rack.children[3].params.erase(p.root_rack.children[3].params.begin()); // drop the leading Pwr=1
        const auto lt = lf::liveTrimDb(p);
        check(std::abs(lt.db - (tl::ssl(0.695f) + tl::multiMode(0.762405f))) < 1e-4 && lt.restored.empty(),
              "the now-live Outputs add their dB along the chain; a bypassed one adds nothing");
        auto q = p;
        const auto mf = lf::foldLegacyGain(q, -6.0);
        check(std::abs(mf.fold.requested_db - (-6.0 - lt.db)) < 1e-9 && std::abs(mf.change_db) < 0.02,
              "foldLegacyGain folds the old gain minus that, and the rig's level ends where it was (change " + std::to_string(mf.change_db) + " dB)");
        // splits: the same knob in both lanes counts once; a knob in one lane only is set back to its default
        auto b = parse(rigXml({{7000, ""}, {155000, par("O45", 0.53f)}, {8000, ""}, {155000, par("O45", 0.53f)}, {9000, ""}}));
        const auto lb = lf::liveTrimDb(b);
        check(std::abs(lb.db - tl::lexicon(0.53f)) < 1e-4 && lb.restored.empty(), "a split whose lanes add the same counts it once");
        // in front of an amp the knob would now drive it harder: set back to 0 dB, not compensated
        auto a = parse(rigXml({{121000, par("O14", 0.695f)}, {34000, ""}, {156000, par("MV", 0.5f)}}));
        const auto la = lf::liveTrimDb(a);
        check(la.db == 0.0 && la.restored.size() == 1 && paramOf(a, 0, "O14") == 0.5f, "an Output in front of an amp is set back to its default");
        auto u = parse(rigXml({{7000, ""}, {158000, par("output", 0.762405f)}, {8000, ""}, {32000, par("V", 0.5f)}, {9000, ""}}));
        const auto lu = lf::liveTrimDb(u);
        check(lu.db == 0.0 && lu.restored.size() == 1 && lu.restored[0].preorder == 1 &&
                  tl::multiMode(paramOf(u, 1, "output")) == 0.0,
              "a lane-only Output is set back to its default (0 dB: the lanes keep their balance)");
    }

    // ── a fold moves the rendered rig by exactly that much ──
    {
        const auto base = parse(rigXml({{25000, par("Vol", 0.5f)}, {156000, par("MV", 0.45f)}, {127000, ""}}));
        auto folded = base;
        const auto r = lf::foldLevel(folded, -5.0);
        const double d = rackLevelDb(folded) - rackLevelDb(base);
        char msg[160];
        std::snprintf(msg, sizeof msg, "folding -5 dB moves the rendered rig by %+.2f dB (applied %+.2f)", d, r.applied_db);
        check(std::abs(d - r.applied_db) < 0.05 && std::abs(r.applied_db + 5.0) < 0.01, msg);
        // the same through the rack for each newly wired Output (the rig's last unit)
        for (const int32_t t : {121000, 154000, 155000, 157000, 158000}) {
            const auto b2 = parse(rigXml({{156000, par("MV", 0.5f)}, {t, ""}}));
            auto f2 = b2;
            const auto r2 = lf::foldLevel(f2, 4.0);
            const double d2 = rackLevelDb(f2) - rackLevelDb(b2);
            std::snprintf(msg, sizeof msg, "%d: folding +4 dB into its Output moves the rendered rig by %+.2f dB (applied %+.2f)", t, d2, r2.applied_db);
            check(r2.moves.size() == 1 && r2.moves[0].table_id == t && std::abs(d2 - r2.applied_db) < 0.05 && std::abs(r2.applied_db - 4.0) < 0.02, msg);
        }
    }

    // ── the .rig text edit, and save / load ──
    {
        const std::string xml = rigXml({{34000, ""}, {156000, par("MV", 0.5f)}, {32000, ""}});
        std::string a = xml;
        check(lf::setParamInRigXml(a, 1, "MV", 0.6123f) && a.find("<parameter id=\"MV\" value=\"0.6123\"/>") != std::string::npos,
              "an existing parameter is rewritten in place");
        std::string rest_a = a, rest_x = xml;
        rest_a.replace(rest_a.find("0.6123"), 6, "0.5");
        check(rest_a == rest_x, "...and nothing else in the file changes (the <headroom-library> block stays)");
        std::string b = xml;
        check(lf::setParamInRigXml(b, 2, "V", 0.25f), "a parameter at its default is added");
        const auto pb = parse(b);
        check(std::abs(paramOf(pb, 2, "V") - 0.25f) < 1e-6f && b.find("num-parameters=\"3\"") != std::string::npos, "...and reads back (count bumped)");
        check(!lf::setParamInRigXml(b, 7, "V", 0.25f), "a unit that isn't there is refused");

        // save / load: the folded knob is an ordinary parameter of the rig
        const auto dir = std::filesystem::temp_directory_path() / ("headroom-levelfold-" + std::to_string(rig_test::processId()));
        std::filesystem::create_directories(dir);
        auto p = parse(xml);
        lf::foldLevel(p, -2.5);
        rig::RackGraph g;
        g.buildFromPreset(p, kSr, 512);
        auto saved = g.toPreset("Saved", "me");
        const std::string path = (dir / "Saved.rig").string();
        check(rig::library::writeUserPreset(path, saved, nullptr), "the rig saves");
        rig::preset::RackPreset back;
        check(std::abs(paramOf(p, 2, "V") - 0.5f) > 0.01f, "the fold moved the graphic EQ Output (nearest the output)");
        check(rig::preset::loadPreset(path, back, nullptr) && std::abs(paramOf(back, 2, "V") - paramOf(p, 2, "V")) < 1e-6f &&
                  std::abs(paramOf(back, 1, "MV") - paramOf(p, 1, "MV")) < 1e-6f,
              "the folded EQ Output (" + std::to_string(paramOf(p, 2, "V")) + ") round-trips through save / load");
        check(std::abs(rackLevelDb(back) - rackLevelDb(p)) < 0.01, "...and so does the rig's level");
        std::filesystem::remove_all(dir);
    }

    std::printf("%s\n", g_failures ? "FAILED" : "all level-fold checks passed");
    return g_failures ? 1 : 0;
}
