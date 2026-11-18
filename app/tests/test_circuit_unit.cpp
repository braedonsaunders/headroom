// Circuit-engine units in the rack (docs/CIRCUIT_ENGINE.md §2.5, §8): factory flag, legacy parameter aliases,
// part overrides from the component page, state hand-off and the model / curves JSON.
#include "platform/Environment.hpp"
#include "circuit/CircuitOversampler.hpp"
#include "circuit/CircuitUnit.hpp"
#include "circuit/Codegen.hpp"
#include "circuit/Engine.hpp"
#include "circuit/Netlist.hpp"
#include "core/RackGraph.hpp"
#include "dsp/ComponentModel.hpp"
#include "json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

using namespace rig;
using json = nlohmann::json;

static int failures = 0;
#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);        \
            std::fprintf(stderr, __VA_ARGS__);                               \
            std::fprintf(stderr, "\n");                                      \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

static std::vector<float> render(IComponent& c, const std::vector<float>& in, uint32_t block = 256) {
    std::vector<float> outL(in.size()), outR(in.size());
    for (std::size_t o = 0; o < in.size(); o += block) {
        const uint32_t n = static_cast<uint32_t>(std::min<std::size_t>(block, in.size() - o));
        const float* ins[2] = {in.data() + o, in.data() + o};
        float* outs[2] = {outL.data() + o, outR.data() + o};
        AudioBlock b;
        b.in = ins;
        b.out = outs;
        b.num_channels = 2;
        b.num_frames = n;
        b.sample_rate = 48000;
        c.processControlRate();
        c.processAudio(b);
    }
    return outL;
}
static double rms(const std::vector<float>& x, std::size_t from) {
    double s = 0;
    for (std::size_t i = from; i < x.size(); ++i) s += double(x[i]) * x[i];
    return std::sqrt(s / double(x.size() - from));
}
static std::vector<float> sine(double f, double a, int n) {
    std::vector<float> v(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) v[static_cast<std::size_t>(i)] = static_cast<float>(a * std::sin(2 * M_PI * f * i / 48000.0));
    return v;
}

int main() {
    rig::platform::setEnv("HEADROOM_CIRCUIT_NOISE", "1"); // exercise circuit noise despite the user default
    rig::platform::setEnv("HEADROOM_NOISE_FIXED_SEED", "1"); // reproducible noise streams (CircuitUnit, docs §3.9)
    // Embedded library == JSON sources (regenerate with tools/circuit/embed_library.py).
    {
        int checked = 0;
        for (const auto& entry : std::filesystem::directory_iterator(HEADROOM_CIRCUIT_LIBRARY)) {
            const std::string name = entry.path().filename().string();
            if (name.size() < 13 || name.substr(name.size() - 13) != ".circuit.json") continue;
            std::ifstream f(entry.path());
            std::stringstream s;
            s << f.rdbuf();
            const auto j = json::parse(s.str(), nullptr, false);
            CHECK(!j.is_discarded() && s.str() == ce::libraryNetlist(j.value("id", "")), "embedded netlist %s matches its JSON file", name.c_str());
            ++checked;
        }
        CHECK(checked >= 3, "library files found (%d)", checked);
    }
    // Authoring lint: every library netlist is free of unit errors and mains-node collisions.
    for (const auto& id : ce::libraryIds()) {
        if (id.rfind("test-", 0) == 0) continue; // engine fixtures (deliberately minimal), not instruments
        const std::string text = ce::libraryNetlist(id);
        ce::Netlist n;
        std::string err;
        if (!ce::parseNetlistText(text, n, err)) continue;
        for (const auto& w : ce::lintNetlist(json::parse(text, nullptr, false), n)) CHECK(false, "lint %s: %s", id.c_str(), w.c_str());
    }

    // Clean cut: a bound, validated unit is the circuit by default. The tool-only legacy hook gives the old model.
    rig::platform::setEnv("HEADROOM_LEGACY_MODELS", "1");
    {
        auto c = createComponentByTableId(15000);
        auto* in = dynamic_cast<model::Introspectable*>(c.get());
        CHECK(in && json::parse(in->modelJson()).value("kind", "") != "circuit", "legacy hook: behavioural TS808");
    }
    rig::platform::unsetEnv("HEADROOM_LEGACY_MODELS");
    auto ts = createComponentByTableId(15000);
    {
        auto* in = dynamic_cast<model::Introspectable*>(ts.get());
        CHECK(in, "circuit unit is introspectable");
        const json m = json::parse(in->modelJson());
        CHECK(m.value("kind", "") == "circuit", "default: circuit TS808");
    }
    ts->prepare(48000, 512);
    const auto x = sine(220, 0.2, 48000);
    ts->setParameter("Dr", 0.1f);
    const auto lo = render(*ts, x);
    auto ts2 = createComponentByTableId(15000);
    ts2->prepare(48000, 512);
    ts2->setParameter("Dr", 1.0f);
    const auto hi = render(*ts2, x);
    std::printf("  TS808 circuit at 0.2 V: rms drive 0.1 = %.4f, drive 1.0 = %.4f (both clipping)\n", rms(lo, 24000), rms(hi, 24000));
    CHECK(std::isfinite(rms(hi, 0)) && rms(lo, 24000) > 1e-3, "circuit TS808 produces signal");
    {
        // Below clipping the Drive pot sets the gain: 1 + (51k + Rdrive)/4.7k, ~19 dB between 0.1 and 1.0.
        const auto small = sine(1000, 0.002, 48000);
        auto a = createComponentByTableId(15000), b = createComponentByTableId(15000);
        a->prepare(48000, 512);
        b->prepare(48000, 512);
        a->setParameter("Dr", 0.1f);
        b->setParameter("Dr", 1.0f);
        const double ra = rms(render(*a, small), 24000), rb = rms(render(*b, small), 24000);
        std::printf("  small-signal drive 0.1 -> 1.0: %+.1f dB\n", 20 * std::log10(rb / ra));
        CHECK(rb > 3.0 * ra, "legacy id Dr reaches the Drive pot");
    }
    // Level (Vol) is an output pot: -> big level change.
    auto ts3 = createComponentByTableId(15000);
    ts3->prepare(48000, 512);
    ts3->setParameter("Vol", 1.0f);
    const auto loud = render(*ts3, x);
    CHECK(rms(loud, 24000) > 3.0 * rms(lo, 24000) || rms(loud, 24000) > 2.0 * rms(hi, 24000), "Vol reaches the Level pot");

    // Part override from the component page: R7 51k -> 5.1k (much less gain).
    auto ts4 = createComponentByTableId(15000);
    ts4->setStringParameter(model::kModelParam, R"({"name":"Low gain","circuit":{"parts":{"R7":{"value":5100},"DRIVE":{"value":"50k"}}}})");
    ts4->prepare(48000, 512);
    ts4->setParameter("Dr", 1.0f);
    const auto mod = render(*ts4, x);
    {
        const json m = json::parse(dynamic_cast<model::Introspectable*>(ts4.get())->modelJson());
        double r7 = 0;
        for (const auto& p : m["circuit"]["parts"])
            if (p["ref"] == "R7") r7 = p["value"].get<double>();
        CHECK(std::abs(r7 - 5100) < 1e-6, "override visible in the model (R7 = %.1f)", r7);
        CHECK(m["custom"].get<bool>(), "custom flag");
    }
    std::printf("  R7=5.1k, Drive pot 50k at full drive: rms %.4f (stock %.4f)\n", rms(mod, 24000), rms(hi, 24000));
    CHECK(std::abs(rms(mod, 24000) - rms(hi, 24000)) > 0.05 * rms(hi, 24000), "part override changes the sound");

    // Hand-off: a prepared copy adopting the running unit continues without a click.
    {
        auto a = createComponentByTableId(15000);
        a->prepare(48000, 512);
        const auto pre = render(*a, sine(220, 0.2, 12000));
        auto b = createComponentByTableId(15000);
        b->prepare(48000, 512);
        b->setParameter("Dr", 0.8f);
        b->adoptRuntimeStateFrom(*a);
        std::vector<float> tail(4800);
        for (int i = 0; i < 4800; ++i) tail[static_cast<std::size_t>(i)] = static_cast<float>(0.2 * std::sin(2 * M_PI * 220 * (i + 12000) / 48000.0));
        const auto post = render(*b, tail);
        double jump = std::abs(post[0] - pre.back()), typical = 0;
        for (std::size_t i = pre.size() - 200; i < pre.size(); ++i) typical = std::max(typical, double(std::abs(pre[i] - pre[i - 1])));
        std::printf("  hand-off: first-sample step %.4f (typical step %.4f)\n", jump, typical);
        CHECK(jump < 3.0 * typical + 1e-3, "no click at the hand-off");
    }

    // Curves JSON: probes and the DC operating point.
    {
        const json cj = json::parse(dynamic_cast<model::Introspectable*>(ts.get())->curvesJson());
        CHECK(cj.contains("probes") && cj["probes"]["traces"].contains("out1"), "probe traces present");
        CHECK(cj.contains("dc") && std::abs(cj["dc"]["inp1"].get<double>() - 4.5) < 0.01, "DC bias 4.5 V at the op-amp input");
    }

    // A whole user circuit imported from SPICE text through __model: a pot-controlled RC low-pass + diode clipper.
    {
        const std::string deck = "* user clipper\n*@io in out\n*@pot RV1 100k in mid mid A drive\nC1 mid out 100n\nR2 out 0 100k\n"
                                 ".model DX D(IS=2.52n N=1.752 RS=0.568)\nD1 out 0 DX\nD2 0 out DX\n";
        json ov = {{"name", "My clipper"}, {"circuit", {{"spice", deck}}}};
        auto u = createComponentByTableId(15000);
        u->setStringParameter(model::kModelParam, ov.dump());
        u->prepare(48000, 512);
        const json m = json::parse(dynamic_cast<model::Introspectable*>(u.get())->modelJson());
        CHECK(!m.contains("error"), "imported circuit compiles: %s", m.value("error", "").c_str());
        CHECK(m["circuit"]["parts"].size() == 5, "imported parts (%zu)", m["circuit"]["parts"].size());
        CHECK(m["circuit"]["controls"][0]["param"] == "Dr", "imported pot answers to the host's Drive knob");
        const auto y = render(*u, sine(440, 1.0, 9600));
        CHECK(rms(y, 4800) > 0.05 && rms(y, 4800) < 1.0, "imported circuit plays (rms %.3f)", rms(y, 4800));
        // A broken edit is rejected and the previous circuit keeps running.
        u->setStringParameter(model::kModelParam, json({{"circuit", {{"spice", "* bad\n*@io in out\nR1 in out 1k\nE1 out 0 floating 0 2\n"}}}}).dump());
        const json m2 = json::parse(dynamic_cast<model::Introspectable*>(u.get())->modelJson());
        CHECK(m2.contains("error") && m2["circuit"]["parts"].size() == 5, "bad circuit rejected, previous kept (%s)", m2.value("error", "").c_str());
    }

    // A stereo circuit fed true stereo (io.inputR + io.outputR) through __model: L and R drive their own inputs, one
    // instance renders both (CircuitUnit::processStereoCircuit).
    {
        const json net = json::parse(R"({"schema":"headroom.circuit.v1","id":"user-stereo-in","io":{"input":"in","inputR":"inr","output":"a","outputR":"b"},
            "parts":[{"ref":"R1","type":"R","nodes":["in","a"],"value":"1k"},{"ref":"R2","type":"R","nodes":["a","0"],"value":"1k"},
                     {"ref":"R3","type":"R","nodes":["inr","b"],"value":"3k"},{"ref":"R4","type":"R","nodes":["b","0"],"value":"1k"}]})");
        auto u = createComponentByTableId(15000);
        u->setStringParameter(model::kModelParam, json({{"circuit", {{"netlist", net}}}}).dump());
        u->prepare(48000, 512);
        const json m = json::parse(dynamic_cast<model::Introspectable*>(u.get())->modelJson());
        CHECK(!m.contains("error"), "stereo-input circuit compiles: %s", m.value("error", "").c_str());
        const int n = 19200;
        const auto inL = sine(440, 0.5, n), inR = sine(1000, 0.4, n);
        std::vector<float> outL(n), outR(n);
        for (int o = 0; o < n; o += 256) {
            const float* ins[2] = {inL.data() + o, inR.data() + o};
            float* outs[2] = {outL.data() + o, outR.data() + o};
            AudioBlock b;
            b.in = ins;
            b.out = outs;
            b.num_channels = 2;
            b.num_frames = 256;
            b.sample_rate = 48000;
            u->processControlRate();
            u->processAudio(b);
        }
        auto tone = [&](const std::vector<float>& x, double f) { // amplitude at f over the second half
            double re = 0, im = 0;
            for (int i = n / 2; i < n; ++i) re += x[i] * std::cos(2 * M_PI * f * i / 48000.0), im += x[i] * std::sin(2 * M_PI * f * i / 48000.0);
            return 2 * std::hypot(re, im) / (n / 2);
        };
        const double l440 = tone(outL, 440), l1k = tone(outL, 1000), r1k = tone(outR, 1000), r440 = tone(outR, 440);
        std::printf("  stereo-input unit: L 440 Hz %.4f (0.25), leak 1 kHz %.1e; R 1 kHz %.4f (0.1), leak 440 Hz %.1e\n", l440, l1k, r1k, r440);
        CHECK(std::abs(l440 - 0.25) < 0.003 && std::abs(r1k - 0.1) < 0.002 && l1k < 1e-4 && r440 < 1e-4, "stereo-input unit feeds L and R separately");
    }
    // Twin (30000): the complete AB763 circuit.
    {
        auto tw = createComponentByTableId(30000);
        CHECK(json::parse(dynamic_cast<model::Introspectable*>(tw.get())->modelJson()).value("kind", "") == "circuit", "Twin circuit");
        tw->prepare(48000, 512);
        tw->setParameter("Vol", 0.6f);
        const auto y = render(*tw, sine(110, 0.1, 24000));
        std::printf("  Twin (complete circuit): rms %.4f\n", rms(y, 12000));
        CHECK(std::isfinite(rms(y, 0)) && rms(y, 12000) > 1e-3, "Twin circuit produces signal");
        tw->setParameter("Br", 1.0f); // switch: > 0.5 = on
        const auto yb = render(*tw, sine(3000, 0.05, 24000));
        auto tw2 = createComponentByTableId(30000);
        tw2->prepare(48000, 512);
        tw2->setParameter("Vol", 0.6f);
        tw2->setParameter("Br", 0.0f);
        const auto yn = render(*tw2, sine(3000, 0.05, 24000));
        std::printf("  Twin bright on/off at 3 kHz: %.4f / %.4f\n", rms(yb, 12000), rms(yn, 12000));
        CHECK(rms(yb, 12000) > 1.1 * rms(yn, 12000), "Bright switch adds treble");
    }
    // Every bound unit: builds through the factory, plays, no Newton failures, and the netlist's control aliases cover
    // every legacy parameter of the catalogue entry (CASSt / Pwr are generic base-class parameters).
    for (const auto& b : ce::unitBindings()) {
        if (!ce::circuitEngineEnabledFor(b.tableId)) { // pending validation: not live, gated in test_circuit_engine
            std::printf("  unit %d (%s): pending validation, skipped\n", b.tableId, b.netlistId);
            continue;
        }
        const auto* schema = catalog::findByTableId(b.tableId);
        CHECK(schema != nullptr, "unit %d has no catalogue entry", b.tableId);
        if (!schema) continue;
        auto u = ce::makeCircuitUnitAlways(*schema);
        CHECK(u != nullptr, "unit %d: no circuit unit", b.tableId);
        if (!u) continue;
        u->prepare(48000, 256);
        const auto y = render(*u, sine(220, 0.1, 9600));
        bool finite = true;
        for (float v : y) finite &= std::isfinite(v);
        const double r = rms(y, 4800);
        auto* in = dynamic_cast<model::Introspectable*>(u.get());
        const auto m = in ? json::parse(in->modelJson(), nullptr, false) : json();
        std::uint64_t fails = 0;
        if (m.is_object() && m.contains("circuit") && m["circuit"].contains("diagnostics")) fails = m["circuit"]["diagnostics"].value("failures", 0ull);
        std::printf("  unit %d (%s): rms %.4f, Newton failures %llu\n", b.tableId, b.netlistId, r, static_cast<unsigned long long>(fails));
        CHECK(finite && r > 1e-5 && r < 10, "unit %d plays (rms %.3g)", b.tableId, r);
        CHECK(fails == 0, "unit %d: Newton failures", b.tableId);
        ce::Netlist n;
        std::string err;
        ce::parseNetlistText(ce::libraryNetlist(b.netlistId), n, err);
        for (const auto& ps : schema->params) {
            const std::string id(ps.id);
            if (id == "CASSt" || id == "Pwr") continue;
            bool mapped = false;
            for (const auto& c : n.controls)
                for (const auto& a : c.params) mapped |= a == id;
            CHECK(mapped, "unit %d: legacy parameter %s has no control alias", b.tableId, id.c_str());
        }
    }
    // Quality: "hq" (the whole circuit at 2x) is the default; "standard" (power stage only at 2x) is opt-in.
    {
        auto u = ce::makeCircuitUnitAlways(*catalog::findByTableId(30000));
        u->prepare(48000, 256);
        auto* in = dynamic_cast<model::Introspectable*>(u.get());
        const auto m0 = json::parse(in->modelJson());
        u->setStringParameter(model::kModelParam, R"({"quality":"standard"})");
        const auto mj = json::parse(in->modelJson());
        const auto y = render(*u, sine(220, 0.1, 9600));
        std::printf("  Twin quality: default %s (multirate %d), standard -> multirate %d, rms %.4f\n", m0["quality"].get<std::string>().c_str(),
                    int(m0["multirate"].get<bool>()), int(mj["multirate"].get<bool>()), rms(y, 4800));
        CHECK(m0["quality"] == "hq" && !m0["multirate"].get<bool>(), "default quality is hq: the whole circuit oversampled");
        CHECK(mj["quality"] == "standard" && mj["multirate"].get<bool>(), "quality standard runs the multirate split");
        CHECK(rms(y, 4800) > 1e-4, "standard Twin plays");
    }
    // The HQ switch follows the rack at once (RackGraph::syncCabFollow, run before every plan publish): an amp with
    // no active cab after it is re-prepared at quality "hq"; adding the cab back returns it to the multirate split.
    {
        RackGraph g;
        g.prepare(48000, 256);
        CHECK(g.addComponentSlot(30000), "rack: Twin inserted");
        auto quality = [&](std::size_t slot) {
            for (const auto& v : g.activeNodeViews())
                if (v.slot_idx == slot)
                    if (auto* in = dynamic_cast<model::Introspectable*>(v.component))
                        return json::parse(in->modelJson())["quality"].get<std::string>();
            return std::string("?");
        };
        auto comp = [&](std::size_t slot) -> IComponent* {
            for (const auto& v : g.activeNodeViews())
                if (v.slot_idx == slot) return v.component;
            return nullptr;
        };
        const std::size_t nslots = g.activeNodeViews().size();
        g.syncCabFollow();
        const std::string q0 = quality(0);
        g.setSlotBypassed(1, true); // the auto-matched cab
        const bool rep1 = g.syncCabFollow();
        const std::string q1 = quality(0);
        // A published plan shares the amp: the switch must land on a detached copy.
        auto plan = g.renderSnapshot();
        IComponent* shared = comp(0);
        g.setSlotBypassed(1, false);
        const bool rep2 = g.syncCabFollow();
        const std::string q2 = quality(0);
        const bool detached = comp(0) != shared;
        // A knob edit detaches a fresh copy: it keeps the cab state (no silent fall-back to standard).
        g.setSlotBypassed(1, true);
        g.syncCabFollow();
        g.setComponentNormalizedParameter(0, "Vol", 0.4f);
        const std::string q3 = quality(0);
        const bool rep4 = g.syncCabFollow();
        std::printf("  rack HQ switch: %zu slots, cab %s, bypassed %s (re-prepared %d), back %s (%d, detached %d), "
                    "knob edit %s (re-prepared %d)\n", nslots, q0.c_str(), q1.c_str(), int(rep1), q2.c_str(), int(rep2),
                    int(detached), q3.c_str(), int(rep4));
        CHECK(nslots == 2 && q0 == "hq", "rack: amp + matched cab runs hq (the default)");
        CHECK(!rep1 && q1 == "hq", "rack: bypassing the cab keeps hq with no re-prepare");
        CHECK(!rep2 && q2 == "hq", "rack: restoring the cab keeps hq with no re-prepare");
        CHECK(q3 == "hq" && !rep4, "rack: a knob edit keeps hq without another re-prepare");
        (void)detached;
    }
    // An amp whose netlist ends in speaker + cab (docs/CIRCUIT_ENGINE.md §10.21): the model JSON says so, and the rack
    // (syncCabFollow) bypasses the cabinet after it once, leaves one the user turns back on, and turns it back on when the
    // netlist goes back to a plain speaker load.
    {
        json nj = json::parse(ce::libraryNetlist("marshall-1959-plexi"));
        json parts = json::array();
        for (const auto& p : nj["parts"])
            if (!(p.value("type", "") == "sub" && p.value("of", "").rfind("lib:spk-", 0) == 0)) parts.push_back(p);
        parts.push_back({{"ref", "SPK"}, {"type", "speaker"}, {"nodes", {"spk", "0", "cone"}}, {"model", "CELESTION_G12M25_16"}, {"series", 2}, {"parallel", 2}});
        parts.push_back({{"ref", "CAB"}, {"type", "sub"}, {"of", "lib:cab-1960ax-g12m25-sm57"}, {"nodes", {"cone", "mic"}}});
        nj["parts"] = parts;
        nj["io"]["output"] = "mic";
        const std::string spkcab = json({{"circuit", {{"netlist", nj}}}}).dump();
        RackGraph g;
        g.prepare(48000, 256);
        CHECK(g.addComponentSlot(34000), "rack: Plexi inserted");
        auto view = [&](std::size_t slot) -> IComponent* {
            for (const auto& v : g.activeNodeViews())
                if (v.slot_idx == slot) return v.component;
            return nullptr;
        };
        const std::size_t nslots = g.activeNodeViews().size();
        const bool plain0 = view(0) && !view(0)->includesCabinet();
        view(0)->setStringParameter(model::kModelParam, spkcab);
        auto* in = dynamic_cast<model::Introspectable*>(view(0));
        const auto mj = in ? json::parse(in->modelJson(), nullptr, false) : json();
        const bool flag = view(0)->includesCabinet() && mj.is_object() && mj.value("ends_in_cab", false) && !mj.contains("error");
        if (!flag) std::printf("    model: kind %s, error %s\n", mj.value("kind", std::string("?")).c_str(), mj.value("error", std::string("-")).c_str());
        if (const char* dump = std::getenv("HEADROOM_DUMP_SPKCAB_MODEL")) std::ofstream(dump) << in->modelJson(); // UI fixture
        g.syncCabFollow();
        const bool muted = g.isSlotBypassed(1);
        g.setSlotBypassed(1, false); // the user wants both
        g.syncCabFollow();
        const bool userKept = !g.isSlotBypassed(1);
        view(0)->setStringParameter(model::kModelParam, "");
        g.syncCabFollow();
        view(0)->setStringParameter(model::kModelParam, spkcab);
        g.syncCabFollow();
        const bool mutedAgain = g.isSlotBypassed(1);
        view(0)->setStringParameter(model::kModelParam, "");
        g.syncCabFollow();
        const bool restored = !g.isSlotBypassed(1) && !view(0)->includesCabinet();
        const auto y = render(*view(0), sine(220, 0.1, 4800));
        std::printf("  rack speaker + cab: %zu slots, plain %d, flag %d, cab bypassed %d, user override kept %d, again %d, restored %d\n", nslots,
                    int(plain0), int(flag), int(muted), int(userKept), int(mutedAgain), int(restored));
        CHECK(nslots == 2 && plain0 && flag, "speaker + cab netlist: includesCabinet and model JSON ends_in_cab");
        CHECK(muted && userKept && mutedAgain && restored, "rack: the cab after a speaker + cab amp is bypassed once and restored");
        CHECK(rms(y, 2400) > 1e-5, "Plexi plays after the round trip");
    }
    // Unit-to-unit variation ("serial"): 0 is the nominal unit, bit for bit; any other serial is a different but
    // repeatable unit whose level stays within a realistic spread of the nominal.
    {
        auto mk = [&](const char* model) {
            auto u = ce::makeCircuitUnitAlways(*catalog::findByTableId(30000));
            u->prepare(48000, 256);
            if (model) u->setStringParameter(model::kModelParam, model);
            return render(*u, sine(220, 0.1, 9600));
        };
        const auto y0 = mk(nullptr), ys0 = mk(R"({"serial":0})"), y7a = mk(R"({"serial":7})"), y7b = mk(R"({"serial":7})"), y8 = mk(R"({"serial":8})");
        double d0 = 0, d7 = 0, d78 = 0;
        for (std::size_t i = 0; i < y0.size(); ++i) {
            d0 = std::max(d0, std::abs(double(y0[i]) - ys0[i]));
            d7 = std::max(d7, std::abs(double(y7a[i]) - y7b[i]));
            d78 = std::max(d78, std::abs(double(y7a[i]) - y8[i]));
        }
        const double lvl = 20 * std::log10(rms(y7a, 4800) / rms(y0, 4800));
        std::printf("  unit variation: serial 0 vs none %.1e, serial 7 twice %.1e, serial 7 vs 8 %.2e, serial 7 level %+.2f dB\n", d0, d7, d78, lvl);
        CHECK(d0 == 0.0, "serial 0 is the nominal unit, bit for bit");
        CHECK(d7 == 0.0, "a serial recalls the same unit");
        CHECK(d78 > 1e-6, "different serials are different units");
        CHECK(std::abs(lvl) < 3.0, "a varied unit stays within a realistic level spread (%.2f dB)", lvl);
    }
    // Topology edits from the circuit editor ("circuit.topology": rename, remove, wire, add; docs/CIRCUIT_ENGINE.md §6).
    {
        auto modelOf = [](IComponent& u) { return json::parse(dynamic_cast<model::Introspectable&>(u).modelJson()); };
        auto tone = [](const std::vector<float>& x, double f) { // amplitude at f over the second half
            const std::size_t n = x.size();
            double re = 0, im = 0;
            for (std::size_t i = n / 2; i < n; ++i) re += x[i] * std::cos(2 * M_PI * f * double(i) / 48000.0), im += x[i] * std::sin(2 * M_PI * f * double(i) / 48000.0);
            return 2 * std::hypot(re, im) / double(n - n / 2);
        };
        // An added part changes the response as expected: a 1k / 1k divider (gain 1/2) gets a third 1k across its
        // output (gain 1/3); deleting the original shunt leaves 1k / 1k again (1/2).
        {
            const json net = json::parse(R"({"schema":"headroom.circuit.v1","id":"user-div","io":{"input":"in","output":"out"},
                "parts":[{"ref":"R1","type":"R","nodes":["in","out"],"value":"1k"},{"ref":"R2","type":"R","nodes":["out","0"],"value":"1k"}]})");
            auto gainWith = [&](const json& topo, json* mOut) {
                auto u = createComponentByTableId(15000);
                u->prepare(48000, 256);
                json c = {{"netlist", net}};
                if (!topo.is_null()) c["topology"] = topo;
                u->setStringParameter(model::kModelParam, json({{"circuit", c}}).dump());
                if (mOut) *mOut = modelOf(*u);
                return tone(render(*u, sine(440, 0.5, 9600)), 440) / 0.5;
            };
            json m1, m2, m3;
            const double g0 = gainWith(json(), nullptr);
            const double g1 = gainWith(json::parse(R"({"add":{"R3":{"type":"R","nodes":["out","0"],"value":"1k"}}})"), &m1);
            const double g2 = gainWith(json::parse(R"({"remove":["R2"],"add":{"R3":{"type":"R","nodes":["out","0"],"value":"1k"}}})"), &m2);
            // the added part's value is edited like any other (circuit.parts.R3.value, applied after the topology)
            auto u = createComponentByTableId(15000);
            u->prepare(48000, 256);
            u->setStringParameter(model::kModelParam, json({{"circuit", {{"netlist", net}, {"topology", json::parse(R"({"add":{"R3":{"type":"R","nodes":["out","0"],"value":"1k"}}})")},
                                                                         {"parts", {{"R3", {{"value", 500.0}}}}}}}}).dump());
            const double g3 = tone(render(*u, sine(440, 0.5, 9600)), 440) / 0.5;
            std::printf("  topology: divider gain %.4f (0.5), + R3 %.4f (1/3), R3 replacing R2 %.4f (0.5), R3 = 500R %.4f (0.25)\n", g0, g1, g2, g3);
            CHECK(std::abs(g0 - 0.5) < 2e-3, "user divider plays at 1/2 (%.4f)", g0);
            CHECK(!m1.contains("error") && std::abs(g1 - 1.0 / 3.0) < 2e-3, "an added shunt makes it 1/3 (%.4f; %s)", g1, m1.value("error", "").c_str());
            CHECK(m1["circuit"]["parts"].size() == 3 && m1.contains("base_netlist") && m1["base_netlist"]["parts"].size() == 2,
                  "the model lists the added part and the netlist it was added to");
            CHECK(!m2.contains("error") && std::abs(g2 - 0.5) < 2e-3 && m2["circuit"]["parts"].size() == 2, "a deleted part is gone and the circuit compiles (%.4f)", g2);
            CHECK(std::abs(g3 - 0.25) < 2e-3, "an added part's value is editable (%.4f)", g3);
        }
        // A library circuit: renaming nets changes nothing (bit for bit); deleting one clipping diode compiles, plays,
        // and makes the clipping asymmetric (even harmonics); rewiring a pin moves it.
        {
            auto ts = [&](const char* topo, json* mOut = nullptr, IComponent** keep = nullptr) {
                auto u = createComponentByTableId(15000);
                u->prepare(48000, 256);
                if (topo) u->setStringParameter(model::kModelParam, json({{"circuit", {{"topology", json::parse(topo)}}}}).dump());
                u->setParameter("Dr", 0.8f); // after the edit, so both units glide to it alike
                if (mOut) *mOut = modelOf(*u);
                auto y = render(*u, sine(220, 0.3, 19200));
                if (keep) *keep = u.release();
                return y;
            };
            json m0, mr, md;
            const auto y0 = ts(nullptr, &m0);
            const std::size_t nBase = m0["circuit"]["parts"].size(), nDel = nBase - 1;
            const auto yr = ts(R"({"rename":{"out1":"clip","t1":"tone_in"}})", &mr);
            double dr = 0;
            for (std::size_t i = 0; i < y0.size(); ++i) dr = std::max(dr, std::abs(double(y0[i]) - yr[i]));
            bool renamed = false;
            for (const auto& p : mr["circuit"]["parts"]) renamed |= p["ref"] == "R8" && p["nodes"][0] == "clip" && p["nodes"][1] == "tone_in";
            IComponent* raw = nullptr;
            const auto yd = ts(R"({"remove":["D2"]})", &md, &raw);
            std::unique_ptr<IComponent> ud(raw);
            const double h2_0 = tone(y0, 440) / tone(y0, 220), h2_d = tone(yd, 440) / tone(yd, 220);
            std::printf("  topology TS808: rename max diff %.1e; D2 deleted: H2/H1 %.4f -> %.4f\n", dr, h2_0, h2_d);
            CHECK(dr == 0.0 && renamed && !mr.contains("error"), "renaming nets changes nothing, bit for bit (%.1e)", dr);
            CHECK(!md.contains("error") && md["circuit"]["parts"].size() == nDel, "a deleted diode compiles (%s)", md.value("error", "").c_str());
            CHECK(rms(yd, 9600) > 1e-3 && h2_d > 3 * h2_0 + 1e-3, "one diode clips asymmetrically (H2/H1 %.4f vs %.4f)", h2_d, h2_0);
            // A bad edit is rejected with the engine's message; the previous circuit keeps playing and the stored model
            // string stays the one it came from.
            const std::string good = ud->getStringParameter(model::kModelParam);
            const auto before = render(*ud, sine(220, 0.3, 4800));
            ud->setStringParameter(model::kModelParam, json({{"circuit", {{"topology", json::parse(R"({"remove":["D2"],"wire":{"IC1A":["nowhere","inv1","out1","vcc","0"]}})")}}}}).dump());
            const json mb = modelOf(*ud);
            const auto after = render(*ud, sine(220, 0.3, 4800));
            std::printf("  topology bad edit: \"%s\"; kept %zu parts, rms %.4f -> %.4f\n", mb.value("error", "").c_str(), mb["circuit"]["parts"].size(), rms(before, 0), rms(after, 0));
            CHECK(mb.contains("error") && mb["error"].get<std::string>().find("nowhere") != std::string::npos, "a floating op-amp input is rejected and named");
            CHECK(mb["circuit"]["parts"].size() == nDel && ud->getStringParameter(model::kModelParam) == good, "the previous circuit and its model string are kept");
            CHECK(std::abs(rms(after, 0) - rms(before, 0)) < 0.05 * rms(before, 0), "the previous circuit keeps playing");
            // malformed edits name what is wrong and change nothing either
            for (const char* bad : {R"({"remove":["NOPE"]})", R"({"wire":{"R8":["a"]}})", R"({"add":{"R8":{"type":"R","nodes":["a","0"],"value":"1k"}}})",
                                    R"({"add":{"RX":{"type":"R","nodes":["out1","0"]}}})", R"({"rename":{"vr":"0"}})", R"({"shuffle":{}})"}) {
                ud->setStringParameter(model::kModelParam, json({{"circuit", {{"topology", json::parse(bad)}}}}).dump());
                const json mx = modelOf(*ud);
                CHECK(mx.contains("error") && mx["circuit"]["parts"].size() == nDel && ud->getStringParameter(model::kModelParam) == good,
                      "bad edit %s rejected (%s)", bad, mx.value("error", "").c_str());
            }
            // rewire: R13 (the output load) moved from out to t1 leaves out loaded only by the next stage
            json mw;
            ts(R"({"wire":{"R13":["t1","0"]}})", &mw);
            bool moved = false;
            for (const auto& p : mw["circuit"]["parts"]) moved |= p["ref"] == "R13" && p["nodes"][0] == "t1";
            CHECK(!mw.contains("error") && moved, "a rewired pin moves (%s)", mw.value("error", "").c_str());
        }
        // Every bound unit takes an edit: the topology path round-trips the netlist exactly (an empty edit renders bit
        // for bit like the factory unit) and passes the editor's gate (compile, DC, smoke render, cost cap).
        int units = 0, exact = 0;
        for (const auto& b : ce::unitBindings()) {
            if (!ce::circuitEngineEnabledFor(b.tableId)) continue;
            const auto* schema = catalog::findByTableId(b.tableId);
            if (!schema) continue;
            auto mk = [&](bool edited, json* mOut) {
                auto u = ce::makeCircuitUnitAlways(*schema);
                u->prepare(48000, 256);
                if (edited) u->setStringParameter(model::kModelParam, R"({"circuit":{"topology":{"rename":{}}}})");
                if (mOut) *mOut = modelOf(*u);
                return render(*u, sine(220, 0.1, 2400));
            };
            json me;
            const auto ya = mk(false, nullptr), yb = mk(true, &me);
            double d = 0;
            for (std::size_t i = 0; i < ya.size(); ++i) d = std::max(d, std::abs(double(ya[i]) - yb[i]));
            ++units;
            exact += d == 0.0 && !me.contains("error");
            CHECK(!me.contains("error"), "unit %d (%s) accepts a circuit edit: %s", b.tableId, b.netlistId, me.value("error", "").c_str());
            CHECK(d == 0.0, "unit %d (%s): the edit path round-trips the netlist (max diff %.2e)", b.tableId, b.netlistId, d);
        }
        std::printf("  topology round trip: %d / %d units editable and bit-exact\n", exact, units);
    }
    // MXR Dyna Comp (48000) absolute level at guitar level (1.0 FS = 1 V): Output at full is make-up gain over the
    // input, against the real pedal's measured static curves (Kröning, Dempwolf, Zölzer, DAFx-11 Fig. 6, Output max,
    // read off the plot: Sensitivity 0 is +14 dB linear below its knee at about -23 dBV, Sensitivity 100 lifts -50 dBV
    // to about -12 dBV, both ceilings about -10 dBV). The SPICE gate cannot catch a level error (both solvers share
    // the netlist), and the two Showcase rigs once bypassed this unit for a level that was their Output knob, not the
    // circuit. The audio-taper Output pot puts unity near 2 o'clock (V ~0.72), as players report.
    {
        auto levelDbv = [](float sens, float out, double rmsV) {
            auto u = createComponentByTableId(48000);
            u->prepare(48000, 256);
            u->setParameter("S", sens);
            u->setParameter("V", out);
            const auto y = render(*u, sine(1000, rmsV * std::sqrt(2.0), 72000)); // 1.5 s: the loop attacks in ms
            return 20.0 * std::log10(rms(y, y.size() - 24000));
        };
        struct Pt { float sens; double inDbv, measuredDbv; };
        for (const Pt& p : {Pt{0.0f, -50.0, -36.0}, Pt{0.0f, -27.0, -13.0}, Pt{1.0f, -50.0, -12.0}, Pt{1.0f, -27.0, -10.3}}) {
            const double o = levelDbv(p.sens, 1.0f, std::pow(10.0, p.inDbv / 20.0));
            std::printf("  Dyna Comp Output 1, Sensitivity %.0f: %.1f dBV in -> %.1f dBV out (pedal measured %.1f)\n", p.sens, p.inDbv, o, p.measuredDbv);
            CHECK(std::abs(o - p.measuredDbv) <= 3.0, "Dyna Comp at Sensitivity %.0f, %.0f dBV: %.1f dBV out vs %.1f measured", p.sens, p.inDbv, o, p.measuredDbv);
        }
        const double half = levelDbv(0.5f, 0.5f, 0.045), full = levelDbv(0.5f, 1.0f, 0.045);
        std::printf("  Dyna Comp at -27 dBV, Sensitivity 0.5: Output 0.5 %+.1f dB, Output 1 %+.1f dB over the input\n", half + 26.9, full + 26.9);
        CHECK(full + 26.9 > 10.0 && full - half > 15.0, "Dyna Comp Output: full %+.1f dB over the input, noon %.1f dB below full", full + 26.9, full - half);
    }
    // Live readouts for the faces (netlist "readouts", docs/CIRCUIT_ENGINE.md §2.7): the GR meters read the circuit and
    // track the gain the unit actually takes off (output level of a 1 kHz tone against a quiet reference tone, by
    // lock-in so the noise floor cannot bias it); LFO lamps report the oscillator's phase, or "stopped" while it is off.
    {
        auto lockin = [](const std::vector<float>& y, std::size_t from) {
            double sn = 0, cs = 0;
            for (std::size_t i = from; i < y.size(); ++i) {
                sn += y[i] * std::sin(2 * M_PI * 1000.0 * double(i) / 48000.0);
                cs += y[i] * std::cos(2 * M_PI * 1000.0 * double(i) / 48000.0);
            }
            return 2.0 * std::hypot(sn, cs) / double(y.size() - from);
        };
        struct Case {
            int tid;
            const char* name;
            std::vector<std::pair<const char*, float>> on, off; // the unit as set, and with its gain reduction disabled
            std::vector<double> amps;
            double tol;
        };
        // Tolerances: the LA-2A's meter is its own photocell bridge (the hardware reads GR through a second cell and a
        // resistor bridge, not the gain cell itself); the 1176 reads its averaged control voltage while the cell follows
        // the attack ripple, so attack / ratio settings move the reading by up to ~1 dB, as a meter amplifier would.
        // The Fairchild reads its control voltage through a table measured on the circuit: 0.5 dB.
        const std::vector<Case> cases = {
            {124000, "LA-2A (T4B meter bridge)", {}, {{"P1", 0.0f}}, {0.1, 0.5, 1.0}, 1.5},
            {123000, "1176 4:1 (FET cell)", {{"R1", 1.0f}}, {{"R1", 0.0f}}, {0.5, 2.0}, 1.2},
            {123000, "1176 20:1", {{"R1", 4.0f}}, {{"R1", 0.0f}}, {0.75}, 1.2},
            {123000, "1176 all buttons", {{"R1", 5.0f}}, {{"R1", 0.0f}}, {1.0}, 1.2},
            {133000, "SSL 4000E dynamics (2151)", {{"G7", 0.0f}}, {{"G7", 0.0f}, {"C1", 0.0f}}, {1.0, 3.0}, 0.3},
            {133000, "SSL 4000E expander (2151)", {{"G7", 0.5f}, {"C1", 0.0f}}, {{"G7", 0.0f}, {"C1", 0.0f}}, {0.02}, 0.3},
            {130000, "SSL 4000G bus comp 10:1 (202X)", {{"T0", 0.88f}, {"R4", 2.0f}, {"M1", 0.0f}}, {{"T0", 0.88f}, {"R4", 2.0f}, {"M1", 0.0f}, {"compin", 0.0f}}, {0.3, 1.0}, 0.3},
            {130000, "SSL 4000G bus comp 2:1 (202X)", {{"T0", 0.6f}, {"R4", 0.0f}, {"M1", 0.0f}}, {{"T0", 0.6f}, {"R4", 0.0f}, {"M1", 0.0f}, {"compin", 0.0f}}, {1.0, 3.0}, 0.3},
            {125000, "dbx 160 4:1 (VCA 200)", {}, {{"C1", 0.0f}}, {0.5, 1.0}, 0.3},
            {125000, "dbx 160 inf:1 (VCA 200)", {{"C1", 1.0f}}, {{"C1", 0.0f}}, {1.0}, 0.3},
            {24000, "Fairchild 670 factory (6386 bias)", {{"Th", 0.83f}, {"Rt", 0.64f}, {"In", 0.75f}, {"Rl", 0.0f}}, {{"Th", 0.0f}, {"Rt", 0.64f}, {"In", 0.75f}, {"Rl", 0.0f}}, {0.3, 0.6}, 0.5},
        };
        for (const auto& k : cases) {
            auto run = [&](double amp, const std::vector<std::pair<const char*, float>>& params, float& gr) {
                auto u = createComponentByTableId(k.tid);
                u->prepare(48000, 256);
                for (const auto& [id, v] : params) u->setParameter(id, v);
                const auto y = render(*u, sine(1000, amp, 72000)); // 1.5 s: the cells settle well within it
                UnitLiveReadout r;
                u->liveReadout(r);
                gr = r.gain_reduction_db;
                return lockin(y, y.size() - 24000) / amp;
            };
            for (double a : k.amps) {
                float gr = -1, grOff = -1;
                const double truth = 20.0 * std::log10(run(a, k.off, grOff) / run(a, k.on, gr));
                std::printf("  %s: amp %.2f  GR %.2f dB, meter reads %.2f dB (%.2f dB with GR off)\n", k.name, a, truth, gr, grOff);
                CHECK(gr >= 0.0f && std::abs(gr - truth) <= k.tol, "%s at %.2f: meter %.2f dB vs %.2f dB taken off", k.name, a, gr, truth);
                CHECK(grOff >= 0.0f && grOff < 0.3f, "%s at %.2f: meter at rest with GR off (%.2f dB)", k.name, a, grOff);
            }
        }
        // Twin vibrato lamp: the roach lamp's light node. Off (the default) -> stopped; on -> a phase that runs.
        for (const float on : {0.0f, 1.0f}) {
            auto tw = createComponentByTableId(30000);
            tw->prepare(48000, 256);
            tw->setParameter("ViOn", on);
            const std::vector<float> in(256, 0.0f); // no signal: with the vibrato off the roach lamp is dark
            std::vector<float> phases;
            for (int blk = 0; blk < 4 * 48000 / 256; ++blk) {
                render(*tw, in);
                UnitLiveReadout r;
                tw->liveReadout(r);
                if (blk >= 48000 / 256 && blk % 20 == 0) phases.push_back(r.lfo_phase);
            }
            int running = 0, stopped = 0;
            for (float p : phases) running += p >= 0.0f && p < 1.0f, stopped += p < -1.5f;
            std::printf("  Twin vibrato %s: %d running, %d stopped of %zu readings\n", on > 0.5f ? "on" : "off", running, stopped, phases.size());
            if (on > 0.5f) CHECK(running >= int(phases.size()) - 2, "Twin vibrato on: the lamp follows the oscillator");
            else CHECK(stopped == int(phases.size()), "Twin vibrato off: the lamp rests");
        }
        // AC15 Vibravox lamp: the oscillator plate p6. Footswitch open (the default) -> stopped, even with a loud
        // signal on the input; closed -> a phase that runs (docs §10.23). Readings from 2 s: the oscillator grows from
        // its 1 mV start kick and passes the 5 V readout floor at ~0.9 s, full swing at ~1.3 s.
        for (const float on : {0.0f, 1.0f}) {
            auto ac = createComponentByTableId(176000);
            ac->prepare(48000, 256);
            ac->setParameter("footswitch", on);
            std::vector<float> in(256);
            std::vector<float> phases;
            for (int blk = 0; blk < 4 * 48000 / 256; ++blk) {
                for (std::size_t i = 0; i < in.size(); ++i)
                    in[i] = 0.5f * static_cast<float>(std::sin(2 * M_PI * 110.0 * double(blk * 256 + int(i)) / 48000.0));
                render(*ac, in);
                UnitLiveReadout r;
                ac->liveReadout(r);
                if (blk >= 2 * 48000 / 256 && blk % 20 == 0) phases.push_back(r.lfo_phase);
            }
            int running = 0, stopped = 0;
            for (float p : phases) running += p >= 0.0f && p < 1.0f, stopped += p < -1.5f;
            std::printf("  AC15 Vibravox %s: %d running, %d stopped of %zu readings\n", on > 0.5f ? "on" : "off", running, stopped, phases.size());
            if (on > 0.5f) CHECK(running >= int(phases.size()) - 2, "AC15 Vibravox on: the lamp follows the oscillator");
            else CHECK(stopped == int(phases.size()), "AC15 Vibravox off: the lamp rests while playing");
        }
        // AC15 face controls with no legacy parameter (netlist "face"): exported as stepped extra parameters, raw value
        // = option index (the rack maps the UI's norm to round(norm x (n-1))). Input index 1 = the Vib-Trem jack: the
        // vibrato channel alone is ~10 dB below the normal channel, so the level must drop.
        {
            auto ac = createComponentByTableId(176000);
            ac->prepare(48000, 256);
            const auto ex = ac->extraParams();
            auto find = [&](const char* id) { return std::find_if(ex.begin(), ex.end(), [&](const ExtraParam& e) { return e.id == id; }); };
            CHECK(find("input") != ex.end() && find("input")->labels.size() == 3, "AC15 exports its Input selector (3 positions)");
            CHECK(find("footswitch") != ex.end() && find("footswitch")->labels.size() == 2, "AC15 exports its Vib-Trem footswitch");
            auto level = [&](float input) {
                auto u = createComponentByTableId(176000);
                u->prepare(48000, 256);
                u->setParameter("input", input);
                double e = 0;
                for (int blk = 0; blk < 48000 / 256; ++blk) {
                    std::vector<float> x(256);
                    for (std::size_t i = 0; i < x.size(); ++i) x[i] = 0.05f * static_cast<float>(std::sin(2 * M_PI * 220.0 * double(blk * 256 + int(i)) / 48000.0));
                    const auto y = render(*u, x);
                    if (blk >= 24000 / 256)
                        for (float v : y) e += double(v) * v;
                }
                return 10 * std::log10(e + 1e-30);
            };
            const double lN = level(0.0f), lV = level(1.0f), lB = level(2.0f);
            std::printf("  AC15 input: Normal %.1f dB, Vib-Trem %.1f dB, both %.1f dB (relative energy)\n", lN, lV, lB);
            CHECK(lN - lV > 3.0 && std::abs(lB - lN) < 3.0, "AC15 input index reaches the right jack (Normal %.1f, Vib-Trem %.1f, both %.1f)", lN, lV, lB);
        }
        // 5E3 Tweed Deluxe: Input selector (Bright / Normal / Both, face-exported) and the tied Volume wipers. The guitar
        // reaches the channel the selector names; the unused channel's Volume at 0 grounds the shared V2A grid and
        // mutes the amp whichever input is used (authentic; the library rigs that set it to 0 went silent, 2026-11-11).
        {
            auto t = createComponentByTableId(79000);
            t->prepare(48000, 256);
            const auto ex = t->extraParams();
            const auto it = std::find_if(ex.begin(), ex.end(), [](const ExtraParam& e) { return e.id == "input"; });
            CHECK(it != ex.end() && it->labels.size() == 3 && it->def == 0.0f, "5E3 exports its Input selector (3 positions, Bright default)");
            auto level = [&](float input, float vb, float vn) {
                auto u = createComponentByTableId(79000);
                u->setParameter("input", input);
                u->setParameter("vb", vb);
                u->setParameter("vn", vn);
                u->prepare(48000, 256);
                double e = 0;
                for (int blk = 0; blk < 48000 / 256; ++blk) {
                    std::vector<float> x(256);
                    for (std::size_t i = 0; i < x.size(); ++i) x[i] = 0.05f * static_cast<float>(std::sin(2 * M_PI * 220.0 * double(blk * 256 + int(i)) / 48000.0));
                    const auto y = render(*u, x);
                    if (blk >= 24000 / 256)
                        for (float v : y) e += double(v) * v;
                }
                return 10 * std::log10(e + 1e-30);
            };
            const double lB = level(0, 0.5f, 0.5f), lN = level(1, 0.5f, 0.5f), lJ = level(2, 0.5f, 0.5f);
            const double lBmute = level(0, 0.5f, 0.0f), lNsolo = level(1, 0.0f, 0.5f), lBsolo = level(1, 0.5f, 0.0f);
            std::printf("  5E3 input: Bright %.1f, Normal %.1f, both %.1f dB; Bright with Normal Vol 0 %.1f dB; Normal input with "
                        "Bright Vol 0 %.1f dB, with Normal Vol 0 %.1f dB\n", lB, lN, lJ, lBmute, lNsolo, lBsolo);
            CHECK(std::abs(lB - lN) < 12.0 && lJ > lN && lB - lBmute > 40.0 && lN - lNsolo > 40.0 && lN - lBsolo > 40.0,
                  "5E3 input reaches the selected channel, and either Volume at 0 mutes the shared grid");
        }
    }
    // Leslie 122 (31000): the circuit amplifier and its 800 Hz dividing network feed the rotor block (dsp/LeslieRotors.hpp):
    // io.output (the treble driver) drives the horn rotor, io.outputR (the bass driver) the drum; the microphones are the
    // unit's stereo output. The catalogue's 14 rotor parameters drive the block, the circuit keeps its Volume.
    {
        const auto* sc = catalog::findByTableId(31000);
        using Ev = std::vector<std::pair<int, std::pair<const char*, float>>>; // block index -> parameter
        struct LR {
            std::vector<float> l, r;
            std::vector<float> phase; // the unit's live readout after each block
        };
        auto renderLR = [](IComponent& c, const std::vector<float>& in, const Ev& ev = {}, uint32_t block = 64, uint32_t sub = 0) {
            LR o;
            o.l.resize(in.size());
            o.r.resize(in.size());
            int bi = 0;
            for (std::size_t off = 0; off < in.size(); off += block, ++bi) {
                for (const auto& e : ev)
                    if (e.first == bi) c.setParameter(e.second.first, e.second.second);
                const uint32_t n = static_cast<uint32_t>(std::min<std::size_t>(block, in.size() - off));
                c.processControlRate();
                // sub > 0: the host block rendered as consecutive sub-blocks, as RackGraph's pipelining does
                for (uint32_t so = 0; so < n; so += sub ? sub : n) {
                    const uint32_t m = std::min(sub ? sub : n, n - so);
                    const float* ins[2] = {in.data() + off + so, in.data() + off + so};
                    float* outs[2] = {o.l.data() + off + so, o.r.data() + off + so};
                    AudioBlock b;
                    b.in = ins;
                    b.out = outs;
                    b.num_channels = 2;
                    b.num_frames = m;
                    b.sample_rate = 48000;
                    b.mono_input = 1;
                    b.host_offset = so;
                    b.host_frames = n;
                    c.processAudio(b);
                }
                UnitLiveReadout ro;
                c.liveReadout(ro);
                o.phase.push_back(ro.lfo_phase);
            }
            return o;
        };
        auto mk = [&](std::initializer_list<std::pair<const char*, float>> set = {}) {
            auto u = ce::makeCircuitUnitAlways(*sc);
            for (const auto& [id, v] : set) u->setParameter(id, v);
            u->prepare(48000, 256);
            return u;
        };
        auto power = [](const LR& y, std::size_t from) {
            double e = 0;
            for (std::size_t i = from; i < y.l.size(); ++i) e += 0.5 * (double(y.l[i]) * y.l[i] + double(y.r[i]) * y.r[i]);
            return 10 * std::log10(e / double(y.l.size() - from) + 1e-30);
        };
        auto tone = [](const std::vector<float>& y, std::size_t from, double f) { // amplitude at f (Hann-windowed DFT bin)
            double sn = 0, cs = 0, wsum = 0;
            const std::size_t n = y.size() - from;
            for (std::size_t i = 0; i < n; ++i) {
                const double w = 0.5 - 0.5 * std::cos(2 * M_PI * double(i) / double(n));
                sn += w * y[from + i] * std::sin(2 * M_PI * f * double(i) / 48000.0);
                cs += w * y[from + i] * std::cos(2 * M_PI * f * double(i) / 48000.0);
                wsum += w;
            }
            return 2.0 * std::hypot(sn, cs) / wsum;
        };
        // Horn / drum separation through the dividing network: Balance 1 = the horn rotor alone, 0 = the drum alone.
        {
            const auto lo = sine(100, 0.1, 48000), hi = sine(4000, 0.1, 48000);
            auto uh = mk({{"Wt", 1.0f}, {"B", 1.0f}}), ud = mk({{"Wt", 1.0f}, {"B", 0.0f}});
            const double hLo = power(renderLR(*uh, lo), 12000), dLo = power(renderLR(*ud, lo), 12000);
            uh = mk({{"Wt", 1.0f}, {"B", 1.0f}}), ud = mk({{"Wt", 1.0f}, {"B", 0.0f}});
            const double hHi = power(renderLR(*uh, hi), 12000), dHi = power(renderLR(*ud, hi), 12000);
            std::printf("  Leslie split: 100 Hz horn %.1f / drum %.1f dB; 4 kHz horn %.1f / drum %.1f dB\n", hLo, dLo, hHi, dHi);
            CHECK(dLo - hLo > 25.0 && hHi - dHi > 20.0, "Leslie: the 800 Hz network sends the bass to the drum and the treble to the horn");
        }
        // Rotor modulation: at tremolo the horn's Doppler and amplitude modulation put sidebands at 3 kHz +- the horn speed
        // (7.056 Hz), the drum's at 150 Hz +- 5.955 Hz; the two microphones differ.
        {
            auto u = mk({{"Wt", 1.0f}});
            const auto y = renderLR(*u, sine(3000, 0.1, 3 * 48000));
            const double c0 = tone(y.l, 48000, 3000), s1 = tone(y.l, 48000, 3000 + 7.056), s1m = tone(y.l, 48000, 3000 - 7.056);
            const double off = tone(y.l, 48000, 3000 + 3.5);
            auto ud = mk({{"Wt", 1.0f}, {"B", 0.0f}});
            const auto yd = renderLR(*ud, sine(150, 0.1, 3 * 48000));
            const double d0 = tone(yd.l, 48000, 150), d1 = tone(yd.l, 48000, 150 + 5.955), doff = tone(yd.l, 48000, 150 + 3.0);
            double lr = 0, ll = 0, rr = 0;
            for (std::size_t i = 48000; i < y.l.size(); ++i) lr += double(y.l[i]) * y.r[i], ll += double(y.l[i]) * y.l[i], rr += double(y.r[i]) * y.r[i];
            const double corr = lr / std::sqrt(ll * rr);
            std::printf("  Leslie rotors at tremolo: horn sidebands %+.1f / %+.1f dB re carrier (between them %+.1f dB); drum sideband "
                        "%+.1f dB (between %+.1f dB); L/R correlation %.2f\n", 20 * std::log10(s1 / c0), 20 * std::log10(s1m / c0),
                        20 * std::log10(off / c0), 20 * std::log10(d1 / d0), 20 * std::log10(doff / d0), corr);
            CHECK(s1 > 0.1 * c0 && s1m > 0.1 * c0 && off < 0.1 * s1 && d1 > 0.1 * d0 && doff < 0.1 * d1 && corr < 0.9,
                  "Leslie: Doppler / AM sidebands at the rotor speeds, a stereo pair");
        }
        // Speed switch: chorale -> tremolo -> chorale, the horn's speed (from the unit's rotor-phase readout) follows the
        // published time constants: accelerating tau 0.161 s, braking 0.321 s.
        {
            auto u = mk({{"SF", 0.0f}});
            const int n1 = 48000 / 64, n2 = 2 * 48000 / 64;
            const auto y = renderLR(*u, std::vector<float>(3 * 48000, 0.0f), {{n1, {"SF", 1.0f}}, {n2, {"SF", 0.0f}}});
            std::vector<double> hz;
            for (std::size_t k = 1; k < y.phase.size(); ++k) {
                double d = double(y.phase[k]) - y.phase[k - 1];
                if (d < -0.5) d += 1;
                hz.push_back(d * 48000.0 / 64.0);
            }
            auto t63 = [&](int from, double a, double b) {
                for (int k = from; k < int(hz.size()); ++k)
                    if ((hz[std::size_t(k)] - a) / (b - a) >= 1 - std::exp(-1.0)) return double(k - from + 1) * 64.0 / 48000.0;
                return 99.0;
            };
            const double up = t63(n1 - 1, 0.672, 7.056), down = t63(n2 - 1, 7.056, 0.672);
            std::printf("  Leslie horn speed %.3f -> %.3f Hz: 63 %% in %.3f s up (published 0.161), %.3f s down (0.321)\n",
                        hz[std::size_t(n1 - 4)], hz[std::size_t(n2 - 4)], up, down);
            CHECK(std::abs(hz[std::size_t(n1 - 4)] - 0.672) < 0.01 && std::abs(hz[std::size_t(n2 - 4)] - 7.056) < 0.02 &&
                      std::abs(up - 0.161) < 0.02 && std::abs(down - 0.321) < 0.03, "Leslie: horn speeds and acceleration");
        }
        // Level: the defaults play at the level of the earlier behavioural rotary unit (pink noise at -20 dBFS RMS).
        {
            std::vector<float> pink(4 * 48000);
            std::uint32_t seed = 12345;
            float b0 = 0, b1 = 0, b2 = 0;
            for (auto& x : pink) {
                seed = seed * 1664525u + 1013904223u;
                const float w = (float(seed >> 8) / float(1u << 24) - 0.5f) * 3.4641f; // unit variance
                b0 = 0.99765f * b0 + w * 0.0990460f;
                b1 = 0.96300f * b1 + w * 0.2965164f;
                b2 = 0.57000f * b2 + w * 1.0526913f;
                x = b0 + b1 + b2 + w * 0.1848f;
            }
            double e = 0;
            for (float x : pink) e += double(x) * x;
            const float k = 0.1f / float(std::sqrt(e / double(pink.size())));
            for (auto& x : pink) x *= k;
            rig::platform::setEnv("HEADROOM_LEGACY_MODELS", "1");
            auto legacy = createComponentByTableId(31000);
            rig::platform::unsetEnv("HEADROOM_LEGACY_MODELS");
            legacy->prepare(48000, 256);
            auto u = mk();
            const double lL = power(renderLR(*legacy, pink), 48000), lC = power(renderLR(*u, pink), 48000);
            std::printf("  Leslie level at defaults, pink -20 dBFS: behavioural %.2f, circuit + rotors %.2f dBFS (%+.2f dB)\n", lL, lC, lC - lL);
            CHECK(std::abs(lC - lL) < 1.0, "Leslie: default level within 1 dB of the behavioural unit");
        }
        // Every rotor parameter reaches the rotor block. Each is moved after the first block (a speed change needs a running
        // rotor; the accelerations act during a Speed switch, so those runs switch to chorale with it).
        {
            const auto x = sine(440, 0.1, 24000);
            const auto ref = [&](bool sw) {
                auto u = mk();
                return renderLR(*u, x, sw ? Ev{{1, {"SF", 0.0f}}} : Ev{});
            };
            const LR r0 = ref(false), r1 = ref(true);
            for (const char* id : {"SF", "B", "P", "D", "TS", "TF", "TA", "TSp", "BS", "BF", "BA", "BSp", "CTn", "Wt"}) {
                const std::string s(id);
                const bool slow = s == "TS" || s == "BS" || s == "TA" || s == "BA";
                auto u = mk();
                Ev ev = {{1, {id, s == "SF" ? 0.0f : 0.95f}}};
                if (slow && s != "SF") ev.push_back({1, {"SF", 0.0f}});
                const LR y = renderLR(*u, x, ev);
                const LR& base = slow ? r1 : r0;
                double d = 0;
                for (std::size_t i = 12000; i < x.size(); ++i) d = std::max(d, double(std::abs(y.l[i] - base.l[i]) + std::abs(y.r[i] - base.r[i])));
                CHECK(d > 1e-3, "Leslie: parameter %s changes the rotor sound (max diff %.2e)", id, d);
            }
        }
        // Pipelining: a host block rendered as sub-blocks is bit-identical (subBlockSafety), and the output is stereo.
        {
            auto a = mk(), b = mk();
            const auto x = sine(330, 0.2, 24000);
            const LR ya = renderLR(*a, x, {{40, {"SF", 0.0f}}}, 256), yb = renderLR(*b, x, {{40, {"SF", 0.0f}}}, 256, 48);
            double d = 0, side = 0;
            for (std::size_t i = 0; i < x.size(); ++i) d = std::max(d, double(std::abs(ya.l[i] - yb.l[i]) + std::abs(ya.r[i] - yb.r[i]))), side += std::abs(ya.l[i] - ya.r[i]);
            std::printf("  Leslie sub-block render: max diff %.3g (subBlockSafety %d, monoPreserving %d)\n", d, a->subBlockSafety(), int(a->monoPreserving()));
            CHECK(d == 0.0 && a->subBlockSafety() != 2 && !a->monoPreserving() && side > 1.0, "Leslie: sub-blocks bit-exact, a stereo output");
        }
    }
    // Ahead-of-time specialised sparse kernels (docs/CIRCUIT_ENGINE.md §10.44): 100 % hit rate across full-step and
    // LTE substep runtimes at defaults and unseen random knob positions, and bit-identical output to the interpreter.
    if (ce::cg::linKernelCount() > 0 || ce::cg::luKernelCount() > 0) {
        CHECK(ce::cg::linKernelCount() > 0 && ce::cg::luKernelCount() > 0, "codegen: both Lin (%d) and Lu (%d) registries populated",
              ce::cg::linKernelCount(), ce::cg::luKernelCount());
        for (const char* id : {"peavey-5150-134000", "friedman-be100-151000", "bogner-uberschall-81000", "soldano-slo100-122000",
                               "soldano-slo100-mk2-163000", "marshall-jcm800-2203", "marshall-1959-plexi", "marshall-1959rr-91000",
                               "vox-ac30-tb", "ampeg-svt-1969", "mesa-dual-rectifier-16000", "mesa-dual-rectifier-revc-96000",
                               "roland-jc-120", "fairchild-670-24000"}) {
            ce::Netlist n;
            std::string err;
            CHECK(ce::parseNetlistText(ce::libraryNetlist(id), n, err), "codegen parse %s: %s", id, err.c_str());
            int f = std::max(1, n.oversampling);
            while (48000.0 * f < n.minRate - 1.0 && f < ce::CircuitOversampler::kMaxFactor) f *= 2;
            ce::CompileOptions o;
            o.sampleRate = 48000.0 * f;
            o.lteTol = ce::kLiveLteTol;
            o.lteMaxPorts = ce::kLiveLteMaxPorts;
            while ((1 << (o.ladderBaseStage + 1)) <= f) ++o.ladderBaseStage;
            for (const auto& [ref, rate] : n.partRates) {
                int lv = 0;
                while ((f << (lv + 1)) <= rate + 1e-9 && (f << (lv + 1)) <= ce::CircuitOversampler::kMaxFactor) ++lv;
                if (lv > 0) o.partLevels.push_back({ref, lv});
            }
            ce::Circuit c;
            CHECK(c.compile(n, o, err), "codegen compile %s: %s", id, err.c_str());
            int bound = 0, total = 0;
            c.codegenStats(bound, total);
            CHECK(total > 0 && bound == total, "codegen %s defaults: %d/%d kernels bound", id, bound, total);
            std::uint32_t rng = 0x42424242u;
            for (std::size_t ci = 0; ci < n.controls.size(); ++ci) {
                const auto& ctl = n.controls[ci];
                if (ctl.kind != "continuous" || ctl.legacyOnly || (ctl.params.empty() && !ctl.face)) continue;
                rng = rng * 1664525u + 1013904223u;
                c.setControlImmediate(static_cast<int>(ci), 0.05 + 0.90 * (double(rng >> 8) / double(1u << 24)));
            }
            CHECK(c.reset(), "codegen %s unseen knob reset", id);
            int b2 = 0, t2 = 0;
            c.codegenStats(b2, t2);
            CHECK(t2 > 0 && b2 == t2, "codegen %s unseen knobs: %d/%d kernels bound", id, b2, t2);
        }
        ce::Netlist n;
        std::string err;
        ce::parseNetlistText(ce::libraryNetlist("peavey-5150-134000"), n, err);
        ce::CompileOptions o1, o0;
        o1.sampleRate = o0.sampleRate = 192000.0;
        o1.lteTol = o0.lteTol = ce::kLiveLteTol;
        o1.lteMaxPorts = o0.lteMaxPorts = ce::kLiveLteMaxPorts;
        o1.noise = o0.noise = true;
        o1.noiseSeed = o0.noiseSeed = 777;
        o0.codegen = false;
        ce::Circuit c1, c0;
        CHECK(c1.compile(n, o1, err) && c0.compile(n, o0, err), "codegen 5150 A/B compile: %s", err.c_str());
        int b1 = 0, t1 = 0, b0 = 0, t0 = 0;
        c1.codegenStats(b1, t1);
        c0.codegenStats(b0, t0);
        CHECK(t1 > 0 && b1 == t1 && b0 == 0 && t0 == t1, "codegen 5150 A/B stats: %d/%d vs %d/%d", b1, t1, b0, t0);
        const int gIdx = n.controlIndex("pre_lead");
        double maxDiff = 0.0;
        for (int i = 0; i < 2048; ++i) {
            if (i == 512 && gIdx >= 0) {
                c1.setControlTarget(gIdx, 0.85);
                c0.setControlTarget(gIdx, 0.85);
            }
            const double vin = 0.8 * std::sin(2.0 * M_PI * 220.0 * i / 192000.0);
            maxDiff = std::max(maxDiff, std::abs(c1.process(vin) - c0.process(vin)));
        }
        std::printf("  codegen (§10.44): %d Lin + %d Lu kernels, 5150 %d/%d bound, 2048-sample glide max diff %.3g\n",
                    ce::cg::linKernelCount(), ce::cg::luKernelCount(), b1, t1, maxDiff);
        CHECK(maxDiff == 0.0, "codegen 5150 bit-identical to interpreter (max diff %.3e)", maxDiff);
    }
    if (failures) {
        std::fprintf(stderr, "%d circuit unit check(s) failed\n", failures);
        return 1;
    }
    std::printf("all circuit unit checks passed\n");
    return 0;
}
