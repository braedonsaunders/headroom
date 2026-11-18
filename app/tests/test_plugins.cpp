// Hosted Audio Unit tests: scan, import, render, parameters, state and preset round trips, missing-plugin
// placeholders, instruments. Uses Apple's built-in AUs (always present on macOS) and, when installed,
// third-party AUs found on this Mac.
#undef NDEBUG
#include "../src/core/RackGraph.hpp"
#include "../src/library/Library.hpp"
#include "../src/midi/MidiEvent.hpp"
#include "../src/plugins/PluginHost.hpp"
#include "../src/preset/PresetLoader.hpp"
#include <AudioToolbox/AudioToolbox.h>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>

namespace fs = std::filesystem;
using namespace rig;

namespace {

std::string auUid(const char* t, const char* s, const char* m) {
    auto cc = [](const char* x) { return (uint32_t(uint8_t(x[0])) << 24) | (uint32_t(uint8_t(x[1])) << 16) | (uint32_t(uint8_t(x[2])) << 8) | uint8_t(x[3]); };
    char b[64];
    std::snprintf(b, sizeof b, "AU:%08x:%08x:%08x", cc(t), cc(s), cc(m));
    return b;
}

std::vector<float> sine(double sr, float hz, float secs, float amp = 0.3f) {
    std::vector<float> v(static_cast<std::size_t>(sr * secs));
    for (std::size_t i = 0; i < v.size(); ++i) v[i] = amp * std::sin(2.0 * M_PI * hz * double(i) / sr);
    return v;
}

double rms(const std::vector<float>& v, std::size_t from = 0) {
    double e = 0.0;
    for (std::size_t i = from; i < v.size(); ++i) e += double(v[i]) * v[i];
    return std::sqrt(e / double(std::max<std::size_t>(1, v.size() - from)));
}

// Renders a mono signal through a unit in 512-frame blocks (as the rack does); returns the left output.
std::vector<float> render(IComponent& c, const std::vector<float>& in, std::vector<float>* right = nullptr, std::vector<midi::MidiEvent>* first_block_midi = nullptr) {
    std::vector<float> L(in.size()), R(in.size());
    std::vector<midi::MidiEvent> bus;
    for (std::size_t pos = 0; pos < in.size(); pos += 512) {
        const uint32_t n = static_cast<uint32_t>(std::min<std::size_t>(512, in.size() - pos));
        const float* ins[2] = {in.data() + pos, in.data() + pos};
        float* outs[2] = {L.data() + pos, R.data() + pos};
        bus.clear();
        if (pos == 0 && first_block_midi) bus = *first_block_midi;
        AudioBlock b{};
        b.in = ins;
        b.out = outs;
        b.num_channels = 2;
        b.num_frames = n;
        b.sample_rate = 48000.0;
        b.midi = &bus;
        c.processAudio(b);
    }
    for (float v : L) assert(std::isfinite(v));
    if (right) *right = R;
    return L;
}

std::string exportedString(const IComponent& c, const char* id) {
    for (const auto& e : c.exportParams())
        if (e.id == id && e.is_string) return e.str;
    return {};
}

// Applies exported params to a fresh unit exactly like RackGraph::buildNode does with a loaded preset.
void applyExported(IComponent& c, const std::vector<ExportedParam>& ps) {
    for (const auto& e : ps) {
        if (e.is_string) c.setStringParameter(e.id, e.str);
        else c.setParameter(e.id, e.num);
    }
}

} // namespace

void runPluginTests() {
    const fs::path dir = fs::temp_directory_path() / ("headroom_plugin_test_" + std::to_string(::getpid()));
    fs::create_directories(dir);
    plugins::setStorePathForTesting((dir / "plugins.json").string());

    // ── scan ──
    const auto avail = plugins::scanInstalled();
    std::map<std::string, plugins::PluginInfo> by_uid;
    int effects = 0, instruments = 0, third_party = 0;
    for (const auto& p : avail) {
        by_uid[p.uid] = p;
        if (p.format == "AU") {
            if (p.kind == "instrument") ++instruments;
            else ++effects;
            if (p.vendor != "Apple") ++third_party;
        }
    }
    assert(by_uid.count(auUid("aufx", "dely", "appl")));
    assert(by_uid.count(auUid("aumu", "dls ", "appl")));
    std::cout << "[PASS] Plugin scan: " << effects << " AU effects, " << instruments << " AU instruments (" << third_party << " third-party).\n";

    // ── import Apple's effects; ids are stable and sequential from 910000 ──
    struct Want { const char* sub; const char* name; };
    const Want wants[] = {{"dely", "AUDelay"}, {"rvb2", "AUReverb2"}, {"dist", "AUDistortion"}, {"nbeq", "AUNBandEQ"}, {"lpas", "AULowpass"}, {"lmtr", "AUPeakLimiter"}};
    std::map<std::string, int32_t> tid;
    int32_t expect = plugins::kImportedBase;
    for (const auto& w : wants) {
        std::string err;
        const int32_t t = plugins::importPlugin(auUid("aufx", w.sub, "appl"), &err);
        assert(t == expect && "imports get sequential stable ids");
        ++expect;
        tid[w.name] = t;
        const auto* sc = catalog::findByTableId(t);
        assert(sc && sc->reference_name == w.name);
        assert(sc->params.size() > 1);
        assert(std::string(implementationKind(t)) == "plugin");
    }
    // Re-importing refreshes in place and keeps the id.
    assert(plugins::importPlugin(auUid("aufx", "dely", "appl")) == tid["AUDelay"]);
    {
        std::ifstream f(dir / "plugins.json");
        const std::string js((std::istreambuf_iterator<char>(f)), {});
        assert(js.find(auUid("aufx", "rvb2", "appl")) != std::string::npos && js.find("\"table_id\": 910001") != std::string::npos);
    }
    // Remove + re-import gives the same id back; the id is never handed to another plugin.
    assert(plugins::removePlugin(tid["AUPeakLimiter"]));
    assert(plugins::importedSchemas().size() == 5);
    assert(plugins::importPlugin(auUid("aufx", "lmtr", "appl")) == tid["AUPeakLimiter"]);
    assert(plugins::importedSchemas().size() == 6);
    std::cout << "[PASS] Plugin import: 6 Apple AUs as table ids " << plugins::kImportedBase << "–" << expect - 1
              << ", persisted to plugins.json, re-import/remove keep ids stable.\n";

    // ── host + render every imported effect ──
    const auto sig = sine(48000.0, 220.0f, 1.0f);
    for (const auto& w : wants) {
        auto c = createComponentByTableId(tid[w.name]);
        assert(c);
        c->prepare(48000.0, 512);
        c->setParameter("Pwr", 1.0f);
        std::vector<float> R;
        const auto L = render(*c, sig, &R);
        const double out = rms(L, 4800);
        assert(out > 1e-4 && "plugin produced output");
        for (float v : R) assert(std::isfinite(v));
        const std::string js = plugins::slotJson(*c);
        assert(js.find("\"ready\":true") != std::string::npos && js.find("\"missing\":false") != std::string::npos);
        const auto lat = js.find("\"latency_ms\":"), ch = js.find("\"in_channels\":");
        std::printf("       %-14s rms in %.3f → out %.3f   %s ch, %s\n", w.name, rms(sig, 4800), out,
                    js.substr(ch + 14, 1).c_str(), js.substr(lat, js.find(',', lat) - lat).c_str());
    }
    {   // AULowpass at 200 Hz must remove most of a 5 kHz tone (proves we render through the AU)
        auto c = createComponentByTableId(tid["AULowpass"]);
        c->prepare(48000.0, 512);
        const auto* sc = catalog::findByTableId(tid["AULowpass"]);
        const auto& cut = sc->params[0]; // cutoff: logarithmic, host space 0..1
        assert(cut.role == "log" && cut.max_value == 1.0f);
        c->setParameter(cut.id, 0.0f);
        const auto hi = sine(48000.0, 5000.0f, 0.5f);
        const auto L = render(*c, hi);
        assert(rms(L, 4800) < 0.05 * rms(hi, 4800));
        c->setParameter(cut.id, 0.37f);
        assert(std::abs(c->getParameter(cut.id) - 0.37f) < 1e-3f);
        const std::string pj = plugins::paramJson(tid["AULowpass"], cut, 0.37f);
        assert(pj.find("Hz") != std::string::npos && pj.find("\"scale\":\"log\"") != std::string::npos);
    }
    std::cout << "[PASS] Plugin render: all 6 effects render 48 kHz / 512-frame blocks via AudioUnitRender; AULowpass filters audibly.\n";

    // ── parameters + ClassInfo state round trip ──
    {
        auto a = createComponentByTableId(tid["AUDelay"]);
        a->prepare(48000.0, 512);
        a->setParameter("0", 37.0f);  // dry/wet %
        a->setParameter("1", 0.77f);  // delay time s
        a->setParameter("2", -12.0f); // feedback %
        assert(std::abs(a->getParameter("0") - 37.0f) < 1e-3f && std::abs(a->getParameter("1") - 0.77f) < 1e-3f);
        const std::string state = exportedString(*a, "state");
        assert(state.size() > 40);
        auto b = createComponentByTableId(tid["AUDelay"]);
        b->setStringParameter("state", state); // before prepare: applied after initialization
        b->prepare(48000.0, 512);
        assert(std::abs(b->getParameter("0") - 37.0f) < 1e-3f);
        assert(std::abs(b->getParameter("1") - 0.77f) < 1e-3f);
        assert(std::abs(b->getParameter("2") + 12.0f) < 1e-3f);
        auto c = createComponentByTableId(tid["AUDelay"]);
        c->prepare(48000.0, 512);
        applyExported(*c, a->exportParams());
        assert(std::abs(c->getParameter("1") - 0.77f) < 1e-3f);
        // identical state → identical audio
        const auto x = render(*a, sig), y = render(*c, sig);
        double d = 0.0;
        for (std::size_t i = 0; i < x.size(); ++i) d = std::max(d, double(std::abs(x[i] - y[i])));
        assert(d < 1e-5);
    }
    std::cout << "[PASS] Plugin state: kAudioUnitProperty_ClassInfo round-trips (before and after initialization), renders identically.\n";

    // ── user preset (.rig) round trip through the rack: chain + split lane + macro + bypass ──
    {
        RackGraph g;
        preset::RackPreset empty;
        g.buildFromPreset(empty, 48000.0, 512);
        const int s0 = g.insertComponent(tid["AUDistortion"], 0, {});
        const int s1 = g.insertComponent(tid["AUDelay"], 9, {});
        assert(s0 == 0 && s1 == 1);
        g.setComponentNormalizedParameter(1, "1", 0.25f); // delay time 0..2 s → 0.5 s (not quantised)
        const int sid = g.insertSplit(99);
        const int s2 = g.insertIntoLane(sid, 'B', 0, tid["AUReverb2"], {});
        assert(s2 >= 0);
        g.setSlotBypassed(0, true);
        assert(g.assignMacro(3, 1, "0", 0.0f, 1.0f, "Delay Mix", false));
        g.setMacroNormalized(3, 0.6f); // dry/wet → 60 %
        auto views = g.activeNodeViews();
        assert(std::abs(views[1].component->getParameter("1") - 0.5f) < 1e-3f);
        assert(std::abs(views[1].component->getParameter("0") - 60.0f) < 1e-2f);
        const std::string path = (dir / "Plugin Rig.rig").string();
        assert(library::writeUserPreset(path, g.toPreset("Plugin Rig", "test")));
        preset::RackPreset back;
        assert(preset::loadPreset(path, back));
        RackGraph h;
        h.buildFromPreset(back, 48000.0, 512);
        auto hv = h.activeNodeViews();
        assert(hv.size() == 3);
        assert(hv[0].table_id == tid["AUDistortion"] && hv[0].component->isBypassed());
        assert(hv[1].table_id == tid["AUDelay"]);
        assert(std::abs(hv[1].component->getParameter("1") - 0.5f) < 1e-3f);
        assert(std::abs(hv[1].component->getParameter("0") - 60.0f) < 1e-2f);
        assert(hv[2].table_id == tid["AUReverb2"]);
        const auto mv = h.macroViews();
        assert(mv[3].targets.size() == 1 && mv[3].targets[0].slot == 1 && mv[3].targets[0].param == "0");
        assert(h.layoutJson().find("\"split\"") != std::string::npos);
        // A preset from another machine (different local ids) resolves by plugin identity.
        for (auto& n : back.root_rack.children)
            if (n.table_id == tid["AUDelay"]) n.table_id = 913333;
        for (auto& m : back.macros) if (m.target_table_id == tid["AUDelay"]) m.target_table_id = 913333;
        RackGraph k;
        k.buildFromPreset(back, 48000.0, 512);
        assert(k.activeNodeViews()[1].table_id == tid["AUDelay"]);
        assert(k.macroViews()[3].targets.size() == 1);
    }
    std::cout << "[PASS] Plugin presets: .rig save/load keeps chain, split lane, bypass, macro, values; foreign ids resolve by identity.\n";

    // ── a preset whose plugin isn't installed loads as a pass-through placeholder and keeps its data ──
    {
        preset::RackPreset p;
        preset::PresetComponentNode n;
        n.table_id = 912345;
        auto sp = [&](const char* id, const char* v) { preset::PresetParam q; q.id = id; q.type = "string"; q.str_val = v; n.params.push_back(q); };
        sp("plugin_id", "AU:61756678:7a7a7a7a:7a7a7a7a");
        sp("plugin_name", "Imaginary Fuzz");
        sp("plugin_vendor", "Nowhere Audio");
        sp("state", "YnBsaXN0MDDUAQIDBAUGBwhfEA9pbWFnaW5hcnkgc3RhdGU=");
        preset::PresetParam f; f.id = "7"; f.type = "float"; f.float_val = 0.42f; n.params.push_back(f);
        p.root_rack.children.push_back(n);
        RackGraph g;
        g.buildFromPreset(p, 48000.0, 512);
        auto v = g.activeNodeViews();
        assert(v.size() == 1 && plugins::isPluginTable(v[0].table_id));
        assert(v[0].component->referenceName() == "Imaginary Fuzz");
        assert(plugins::slotJson(*v[0].component).find("\"missing\":true") != std::string::npos);
        const auto L = render(*v[0].component, sig);
        for (std::size_t i = 0; i < sig.size(); ++i) assert(L[i] == sig[i]);
        const auto out = g.toPreset("again", "");
        const auto& on = out.root_rack.children[0];
        std::map<std::string, preset::PresetParam> byid;
        for (const auto& q : on.params) byid[q.id] = q;
        assert(byid["plugin_id"].str_val == "AU:61756678:7a7a7a7a:7a7a7a7a");
        assert(byid["state"].str_val == "YnBsaXN0MDDUAQIDBAUGBwhfEA9pbWFnaW5hcnkgc3RhdGU=");
        assert(std::abs(byid["7"].float_val - 0.42f) < 1e-6f);
    }
    std::cout << "[PASS] Missing plugin: preset loads, unit passes audio through bit-exact, identity/state/params written back unchanged.\n";

    // ── AU instrument in a MIDI lane (Apple DLS synth) ──
    {
        const int32_t t = plugins::importPlugin(auUid("aumu", "dls ", "appl"));
        assert(t && plugins::isInstrumentTable(t));
        auto c = createComponentByTableId(t);
        c->prepare(48000.0, 512);
        std::vector<midi::MidiEvent> on = {{midi::MidiEvent::NoteOn, 60, 0.9f, 0.0f, 0}, {midi::MidiEvent::NoteOn, 64, 0.9f, 0.0f, 10}};
        std::vector<float> silence(48000, 0.0f);
        const auto L = render(*c, silence, nullptr, &on);
        assert(rms(L) > 1e-3);
        RackGraph g;
        preset::RackPreset empty;
        g.buildFromPreset(empty, 48000.0, 512);
        const int sid = g.insertSplit(99);
        g.insertIntoLane(sid, 'B', 0, t, {});
        bool detector = false;
        for (const auto& v : g.activeNodeViews()) detector = detector || v.table_id == 900001;
        assert(detector && "instrument plugins get a Pitch → MIDI detector in front of them");
    }
    std::cout << "[PASS] Plugin instrument: DLSMusicDevice plays MIDI from the lane bus; inserting it adds Pitch → MIDI upstream.\n";

    // ── third-party AUs installed on this Mac ──
    int tested = 0;
    for (const auto& p : avail) {
        if (p.format != "AU" || p.vendor == "Apple") continue;
        std::string err;
        const auto t0 = std::chrono::steady_clock::now();
        const int32_t t = plugins::importPlugin(p.uid, &err);
        if (!t) {
            std::printf("       %s (%s): import failed: %s\n", p.name.c_str(), p.vendor.c_str(), err.c_str());
            continue;
        }
        auto c = createComponentByTableId(t);
        c->prepare(48000.0, 512);
        const auto L = render(*c, sig);
        const std::string state = exportedString(*c, "state");
        auto d = createComponentByTableId(t);
        d->prepare(48000.0, 512);
        d->setStringParameter("state", state);
        const std::string state2 = exportedString(*d, "state");
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        const std::string js = plugins::slotJson(*c);
        std::printf("       %s (%s, %s): %zu params, state %zu bytes (reload %s), out rms %.4f, %s, %.0f ms\n", p.name.c_str(), p.vendor.c_str(),
                    p.kind.c_str(), catalog::findByTableId(t)->params.size() - 1, state.size(), state2.empty() ? "empty" : "ok", rms(L, 4800),
                    js.substr(js.find("\"in_channels\""), js.find(",\"tail_s\"") - js.find("\"in_channels\"")).c_str(), ms);
        assert(!state.empty() && !state2.empty());
        ++tested;
    }
    std::cout << "[PASS] Third-party AUs: " << tested << " hosted, rendered and state-reloaded.\n";

    std::error_code ec;
    fs::remove_all(dir, ec);
}
