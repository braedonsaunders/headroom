// Placeholder plugin host (src/plugins/PluginHostNull.cpp, platforms without AU/VST3 hosting yet): presets that
// reference plugins load as pass-through units and write identity, state and params back unchanged; the scan
// lists no AUs; imports are refused with a clear error; /api/plugins* stays well-formed JSON.
#undef NDEBUG
#include "../src/core/RackGraph.hpp"
#include "../src/library/Library.hpp"
#include "../src/plugins/PluginHost.hpp"
#include "../src/preset/PresetLoader.hpp"
#include <json.hpp>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>

namespace fs = std::filesystem;
using namespace rig;

namespace {

std::vector<float> sine(double sr, float hz, float secs) {
    std::vector<float> v(static_cast<std::size_t>(sr * secs));
    for (std::size_t i = 0; i < v.size(); ++i) v[i] = 0.3f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * hz * double(i) / sr));
    return v;
}

std::vector<float> render(IComponent& c, const std::vector<float>& in) {
    std::vector<float> L(in.size()), R(in.size());
    for (std::size_t pos = 0; pos < in.size(); pos += 512) {
        const uint32_t n = static_cast<uint32_t>(std::min<std::size_t>(512, in.size() - pos));
        const float* ins[2] = {in.data() + pos, in.data() + pos};
        float* outs[2] = {L.data() + pos, R.data() + pos};
        AudioBlock b{};
        b.in = ins;
        b.out = outs;
        b.num_channels = 2;
        b.num_frames = n;
        b.sample_rate = 48000.0;
        c.processAudio(b);
    }
    return L;
}

bool validJson(const std::string& s) {
    try {
        (void)nlohmann::json::parse(s);
        return true;
    } catch (...) {
        return false;
    }
}

preset::PresetComponentNode pluginNode(int32_t tid, const char* uid, const char* name, const char* vendor, const char* state, float p7) {
    preset::PresetComponentNode n;
    n.table_id = tid;
    auto sp = [&](const char* id, const char* v) { preset::PresetParam q; q.id = id; q.type = "string"; q.str_val = v; n.params.push_back(q); };
    sp("plugin_id", uid);
    sp("plugin_name", name);
    sp("plugin_vendor", vendor);
    sp("state", state);
    preset::PresetParam f; f.id = "7"; f.type = "float"; f.float_val = p7; n.params.push_back(f);
    return n;
}

} // namespace

void runPluginTests() {
    const fs::path dir = fs::temp_directory_path() / ("headroom_plugin_null_test_" + std::to_string(std::random_device{}()));
    fs::create_directories(dir);
    // A plugins.json written by a Mac: its persisted ids must resolve to the same tables here.
    {
        std::ofstream o(dir / "plugins.json");
        o << R"({"version":1,"next_id":910003,"plugins":[{"table_id":910001,"uid":"AU:61756678:64656c79:6170706c","format":"AU",
               "name":"AUDelay","vendor":"Apple","kind":"effect","version":65536,"v3":false,
               "params":[{"au_id":0,"name":"dry/wet mix","unit":"%","min":0,"max":100,"def":50},
                         {"au_id":1,"name":"delay time","unit":"s","min":0,"max":2,"def":1}]}],
               "retired":{"AU:61756678:64697374:6170706c":910002}})";
    }
    plugins::setStorePathForTesting((dir / "plugins.json").string());

    // ── scan: no AUs off macOS; VST3 bundles (if any) listed as unsupported ──
    for (const auto& p : plugins::scanInstalled()) {
        assert(p.format == "VST3" && !p.supported);
    }
    // ── imports are refused, clearly ──
    {
        std::string err;
        assert(plugins::importPlugin("AU:61756678:64656c79:6170706c", &err) == 0 && err.find("isn't available") != std::string::npos);
        assert(plugins::importPlugin("VST3:/x/y.vst3", &err) == 0 && !err.empty());
    }
    std::cout << "[PASS] Placeholder host: scan lists no AUs (VST3 unsupported), imports refused with a clear error.\n";

    // ── persisted imports from plugins.json keep their ids and schemas ──
    {
        const auto imp = plugins::imported();
        assert(imp.size() == 1 && imp[0].table_id == 910001 && imp[0].name == "AUDelay");
        const auto* s = plugins::findSchema(910001);
        assert(s && s->params.size() == 3 && s->params[0].id == "0" && s->params[1].id == "1");
        assert(plugins::importedSchemas().size() == 1);
        assert(!plugins::isInstrumentTable(910001));
        assert(plugins::engineDescription(910001).find("passing audio through") != std::string::npos);
    }

    // ── a preset with plugins loads as pass-through placeholders and writes everything back unchanged ──
    const char* kState = "YnBsaXN0MDDUAQIDBAUGBwhfEA9pbWFnaW5hcnkgc3RhdGU=";
    {
        preset::RackPreset p;
        p.root_rack.children.push_back(pluginNode(912345, "AU:61756678:7a7a7a7a:7a7a7a7a", "Imaginary Fuzz", "Nowhere Audio", kState, 0.42f));
        // Same plugin as the persisted import, saved on another machine under a different local id.
        p.root_rack.children.push_back(pluginNode(913333, "AU:61756678:64656c79:6170706c", "AUDelay", "Apple", "c3RhdGUy", 0.25f));
        plugins::remapPreset(p);
        assert(p.root_rack.children[0].table_id >= plugins::kTransientBase);
        assert(p.root_rack.children[1].table_id == 910001);
        RackGraph g;
        g.buildFromPreset(p, 48000.0, 512);
        auto v = g.activeNodeViews();
        assert(v.size() == 2 && plugins::isPluginTable(v[0].table_id) && v[1].table_id == 910001);
        assert(v[0].component->referenceName() == "Imaginary Fuzz");
        const std::string sj = plugins::slotJson(*v[0].component);
        assert(sj.find("\"missing\":true") != std::string::npos && validJson("{" + sj + "}"));
        assert(plugins::instanceIdOf(*v[0].component) == -1);
        const auto sig = sine(48000.0, 220.0f, 0.5f);
        const auto L = render(*v[0].component, sig);
        for (std::size_t i = 0; i < sig.size(); ++i) assert(L[i] == sig[i]);
        const auto out = g.toPreset("again", "");
        assert(out.root_rack.children.size() == 2);
        std::map<std::string, preset::PresetParam> a, b;
        for (const auto& q : out.root_rack.children[0].params) a[q.id] = q;
        for (const auto& q : out.root_rack.children[1].params) b[q.id] = q;
        assert(a["plugin_id"].str_val == "AU:61756678:7a7a7a7a:7a7a7a7a" && a["plugin_name"].str_val == "Imaginary Fuzz");
        assert(a["plugin_vendor"].str_val == "Nowhere Audio" && a["state"].str_val == kState);
        assert(std::abs(a["7"].float_val - 0.42f) < 1e-6f);
        assert(b["plugin_id"].str_val == "AU:61756678:64656c79:6170706c" && b["state"].str_val == "c3RhdGUy");
        assert(std::abs(b["7"].float_val - 0.25f) < 1e-6f);
        // createComponent + exportParams directly, and a second load of the saved preset.
        auto c = plugins::createComponent(v[0].table_id);
        assert(c);
        for (const auto& e : v[0].component->exportParams()) {
            if (e.is_string) c->setStringParameter(e.id, e.str);
            else c->setParameter(e.id, e.num);
        }
        assert(c->getStringParameter("state") == kState && std::abs(c->getParameter("7") - 0.42f) < 1e-6f);
        const std::string path = (dir / "Placeholder Rig.rig").string();
        assert(library::writeUserPreset(path, out));
        preset::RackPreset back;
        assert(preset::loadPreset(path, back));
        RackGraph h;
        h.buildFromPreset(back, 48000.0, 512);
        const auto hv = h.activeNodeViews();
        assert(hv.size() == 2 && hv[0].table_id == v[0].table_id && hv[1].table_id == 910001);
        assert(hv[0].component->getStringParameter("state") == kState);
        // Placeholder parameters render through paramJson as valid JSON.
        const auto* s = plugins::findSchema(910001);
        assert(s && validJson(plugins::paramJson(910001, s->params[0], 50.0f)));
    }
    std::cout << "[PASS] Placeholder plugins: presets load, audio passes through bit-exact, identity/state/params round-trip; "
                 "foreign ids resolve by identity.\n";

    // ── engine API JSON stays well-formed ──
    {
        auto q = [](const char* k) -> std::string { return std::string(k) == "uid" ? "AU:1:2:3,VST3:/a.vst3" : "910001"; };
        const std::string list = plugins::handleApi("/api/plugins", q, -1);
        assert(validJson(list) && list.find("\"imported\":[") != std::string::npos);
        const std::string imp = plugins::handleApi("/api/plugins/import", q, -1);
        assert(validJson(imp) && imp.find("\"ok\":false") != std::string::npos);
        assert(validJson(plugins::handleApi("/api/plugins/editor", q, -1)));
        assert(validJson(plugins::handleApi("/api/plugins/remove", q, -1)));
        assert(plugins::imported().empty()); // removed (and retired) above
        assert(validJson(plugins::handleApi("/api/plugins/nope", q, -1)));
        // The rewritten store keeps both retired ids reserved.
        std::ifstream f(dir / "plugins.json");
        const auto doc = nlohmann::json::parse(f);
        assert(doc["retired"].contains("AU:61756678:64656c79:6170706c") && doc["retired"].contains("AU:61756678:64697374:6170706c"));
    }
    std::cout << "[PASS] Placeholder host API: /api/plugins, import, editor, remove return valid JSON; removal keeps ids retired.\n";

    std::error_code ec;
    fs::remove_all(dir, ec);
}
