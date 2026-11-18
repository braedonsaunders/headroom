#include "platform/Environment.hpp"
#include <cstdlib>
#undef NDEBUG // the regression checks are asserts: keep them live in Release builds too
#include "../src/core/RackGraph.hpp"
#include "../src/dsp/AmpModeling.hpp"
#include "../src/dsp/ConvolutionEngine.hpp"
#include "../src/dsp/TubeAmpComponents.hpp"
#include "../src/midi/MidiEvent.hpp"
#include <array>
#include <cmath>
#include "../src/dsp/DSPPrimitives.hpp"
#include "../src/generated/ComponentCatalog.hpp"
#include "../src/preset/PresetLoader.hpp"
#include "../src/library/Library.hpp"
#include "../src/dsp/ComponentModel.hpp"
#include "../src/dsp/LooperUnit.hpp"
#include "../src/dsp/PitchDoublerUnits.hpp"
#include "../src/dsp/ToneMatchAndCalibration.hpp"
#include "../src/core/RigScenes.hpp"
#include "json.hpp"
#include <cassert>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <numbers>

namespace fs = std::filesystem;

static void testCatalogCompleteness() {
    assert(rig::catalog::kAllComponents.size() == 168); // 166 legacy + 2 circuit-only units (179000, 180000)
    assert(rig::catalog::findByTableId(179000) != nullptr);
    assert(rig::catalog::findByTableId(180000) != nullptr);
    assert(rig::catalog::findByTableId(100) != nullptr);
    assert(rig::catalog::findByTableId(34000) != nullptr);  // Marshall 1959 Super Lead Plexi
    assert(rig::catalog::findByTableId(26000) != nullptr);  // EQ Parametric
    assert(rig::catalog::findByTableId(164000) != nullptr); // Pitch Quantizer & Scale Harmonizer
    assert(rig::catalog::findByTableId(169000) != nullptr); // Ring Modulator & Frequency Shifter

    std::size_t total_params = 0;
    for (const auto& c : rig::catalog::kAllComponents) {
        total_params += c.params.size();
        auto inst = rig::createComponentByTableId(c.table_id);
        assert(inst != nullptr);
        assert(inst->tableId() == c.table_id);
    }
    std::cout << "[PASS] Catalog completeness: 168 components, " << total_params << " parameter schemas.\n";
}

static void testInternalSampleRateFormula() {
    // k=1 (88.2 kHz target): at 44.1 kHz -> 2x (88.2k), at 48 kHz -> 2x (96k), at 96 kHz -> 1x (96k)
    const auto r44 = rig::dsp::InternalRateConfig::forModernDSP(44100.0, 1);
    assert(r44.oversampling_factor == 2 && std::abs(r44.internal_sr - 88200.0) < 1e-3);

    const auto r48 = rig::dsp::InternalRateConfig::forModernDSP(48000.0, 1);
    assert(r48.oversampling_factor == 2 && std::abs(r48.internal_sr - 96000.0) < 1e-3);

    const auto r96 = rig::dsp::InternalRateConfig::forModernDSP(96000.0, 1);
    assert(r96.oversampling_factor == 1 && std::abs(r96.internal_sr - 96000.0) < 1e-3);

    // k=2 (Harmonic Saturator / Ring Modulator: 176.4 kHz target): at 48 kHz -> 4x (192k)
    const auto r176 = rig::dsp::InternalRateConfig::forModernDSP(48000.0, 2);
    assert(r176.oversampling_factor == 4 && std::abs(r176.internal_sr - 192000.0) < 1e-3);

    // k=3 (Wavefolder Distortion: 352.8 kHz target): at 48 kHz -> 8x (384k)
    const auto r352 = rig::dsp::InternalRateConfig::forModernDSP(48000.0, 3);
    assert(r352.oversampling_factor == 8 && std::abs(r352.internal_sr - 384000.0) < 1e-3);

    // Legacy circuit-waveshaper amp: fixed 4x
    const auto rPlexi = rig::dsp::InternalRateConfig::forLegacyCircuitAmp(48000.0);
    assert(rPlexi.oversampling_factor == 4 && std::abs(rPlexi.internal_sr - 192000.0) < 1e-3);

    std::cout << "[PASS] Internal sample rate rules verified (adaptive power-of-two & 4x fixed).\n";
}

static void testNeuralMathAndPilots() {
    // Test the 3 Pilot components: Marshall 1959 Plexi (34000), Studio Parametric EQ (26000), Pitch Quantizer (164000)
    const int32_t pilots[3] = {34000, 26000, 164000};
    for (int32_t tid : pilots) {
        auto comp = rig::createComponentByTableId(tid);
        assert(comp != nullptr);
        comp->prepare(48000.0, 256);
        comp->processControlRate();

        rig::StereoScratchBuffer in_buf{};
        rig::StereoScratchBuffer out_buf{};
        for (uint32_t i = 0; i < 256; ++i) {
            const float s = 0.3f * std::sin(2.0f * std::numbers::pi_v<float> * 220.0f * static_cast<float>(i) / 48000.0f);
            in_buf.left[i] = s;
            in_buf.right[i] = s;
        }
        rig::AudioBlock block{};
        block.in = in_buf.const_ptrs.data();
        block.out = out_buf.ptrs.data();
        block.num_channels = 2;
        block.num_frames = 256;
        block.sample_rate = 48000.0;
        comp->processAudio(block);

        float energy = 0.0f;
        for (uint32_t i = 0; i < 256; ++i) {
            assert(std::isfinite(out_buf.left[i]));
            energy += out_buf.left[i] * out_buf.left[i];
        }
        assert(energy > 1e-4f);
    }
    std::cout << "[PASS] 3 pilot DSP components verified.\n";
}

static void testBundledShowcaseRigsParse() {
    const fs::path root = fs::path(__FILE__).parent_path().parent_path();
    const fs::path showcase_dir = root / "presets" / "Showcase";
    if (!fs::exists(showcase_dir)) {
        std::cout << "[SKIP] Showcase presets dir not present.\n";
        return;
    }

    std::size_t rig_count = 0, rig_ok = 0;
    for (const auto& entry : fs::recursive_directory_iterator(showcase_dir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() == ".rig") {
            ++rig_count;
            rig::preset::RackPreset p;
            if (rig::preset::loadPreset(entry.path().string(), p)) {
                ++rig_ok;
            }
        }
    }
    std::cout << "[PASS] Bundled .rig presets parsed: " << rig_ok << "/" << rig_count << ".\n";
    assert(rig_count > 0 && rig_ok == rig_count);
}

// ───────────── Headroom engine regression tests ─────────────
static double rmsOf(const std::vector<float>& v) {
    double e = 0.0;
    for (float x : v) e += static_cast<double>(x) * x;
    return v.empty() ? 0.0 : std::sqrt(e / static_cast<double>(v.size()));
}

static std::vector<float> runComponent(rig::IComponent& c, const std::vector<float>& sig, double sr, std::vector<rig::midi::MidiEvent>* bus = nullptr) {
    std::vector<float> out(sig.size(), 0.0f), R(256);
    for (std::size_t p = 0; p + 256 <= sig.size(); p += 256) {
        const float* in[2] = {&sig[p], &sig[p]};
        float* o[2] = {&out[p], R.data()};
        rig::AudioBlock b;
        b.in = in;
        b.out = o;
        b.num_channels = 2;
        b.num_frames = 256;
        b.sample_rate = sr;
        if (bus) bus->clear();
        b.midi = bus;
        c.processAudio(b);
    }
    return out;
}

static void testPartitionedConvolverExact() {
    std::vector<float> ir(3000), x(12000);
    uint32_t s = 7;
    auto rnd = [&s]() { s = s * 1664525u + 1013904223u; return static_cast<float>((s >> 8) & 0xFFFF) / 32768.0f - 1.0f; };
    for (std::size_t i = 0; i < ir.size(); ++i) ir[i] = rnd() * std::exp(-static_cast<float>(i) / 600.0f);
    for (auto& v : x) v = rnd();
    rig::dsp::PartitionedIR pir;
    pir.build(ir);
    rig::dsp::PartitionedConvolver conv;
    double err = 0.0, ref = 0.0;
    for (std::size_t n = 0; n < x.size(); ++n) {
        const float y = conv.process(x[n], pir);
        double d = 0.0;
        for (std::size_t k = 0; k < ir.size() && k <= n; ++k) d += ir[k] * x[n - k];
        err += (y - d) * (y - d);
        ref += d * d;
    }
    const double db = 10.0 * std::log10(err / ref);
    std::cout << "[PASS] Partitioned convolver matches direct convolution (" << db << " dB error).\n";
    assert(db < -100.0);
}

static void testAllAmpsAndPedalsStable() {
    const auto sig = rig::amp::makeCalibrationDI(48000.0, 1.5f);
    int n = 0;
    for (const auto& sc : rig::catalog::kAllComponents) {
        if (!rig::isTubeAmpTable(sc.table_id) && !rig::isDrivePedalTable(sc.table_id)) continue;
        auto c = rig::createComponentByTableId(sc.table_id);
        c->prepare(48000.0, 256);
        const auto out = runComponent(*c, sig, 48000.0);
        for (float v : out) assert(std::isfinite(v));
        assert(rmsOf(out) > 1e-4);
        ++n;
    }
    std::cout << "[PASS] " << n << " tube amps & drive pedals render finite, non-silent output.\n";
}

static void testPitchToMidiMono() {
    // A3 (220 Hz) plucked-string-like tone → Pitch → MIDI (mono) must report note 57.
    const double sr = 48000.0;
    std::vector<float> sig(static_cast<std::size_t>(sr * 1.0), 0.0f);
    for (std::size_t i = 4800; i < sig.size(); ++i) {
        const double t = static_cast<double>(i - 4800) / sr;
        double v = 0.0;
        for (int h = 1; h <= 8; ++h) v += std::sin(2.0 * M_PI * 220.0 * h * t) / h;
        sig[i] = static_cast<float>(0.3 * v * std::exp(-t * 1.5));
    }
    auto c = rig::createComponentByTableId(900001);
    c->prepare(sr, 256);
    c->setParameter("mode", 1.0f);
    c->processControlRate();
    std::vector<rig::midi::MidiEvent> bus;
    int found = -1;
    for (std::size_t p = 0; p + 256 <= sig.size() && found < 0; p += 256) {
        const float* in[2] = {&sig[p], &sig[p]};
        std::array<float, 256> L{}, R{};
        float* o[2] = {L.data(), R.data()};
        rig::AudioBlock b;
        b.in = in;
        b.out = o;
        b.num_frames = 256;
        b.midi = &bus;
        bus.clear();
        c->processAudio(b);
        for (const auto& e : bus)
            if (e.type == rig::midi::MidiEvent::NoteOn) found = e.note;
    }
    std::cout << "[PASS] Pitch → MIDI (mono) detected note " << found << " for a 220 Hz tone.\n";
    assert(found == 57);
}

static void testSplitLaneRack() {
    rig::RackGraph g;
    rig::preset::RackPreset empty;
    empty.root_rack.table_id = 100;
    g.buildFromPreset(empty, 48000.0, 256);
    const int sid = g.insertSplit(999);
    g.insertIntoLane(sid, 'A', 9, 56000, {});
    g.insertIntoLane(sid, 'B', 9, 900003, {});
    const std::string layout = g.layoutJson();
    // Amp gets an automatic cabinet in lane A; Instrument gets a Pitch → MIDI in front of it in lane B.
    assert(g.activeNodeViews().size() == 4);
    assert(layout.find("\"type\":\"split\"") != std::string::npos);
    const auto views = g.activeNodeViews();
    assert(views[2].table_id == 900001 && views[3].table_id == 900003);
    std::vector<float> L(256), R(256);
    const auto sig = rig::amp::makeCalibrationDI(48000.0, 1.0f);
    for (std::size_t p = 0; p + 256 <= sig.size(); p += 256) {
        const float* in[2] = {&sig[p], &sig[p]};
        float* o[2] = {L.data(), R.data()};
        rig::AudioBlock b;
        b.in = in;
        b.out = o;
        b.num_frames = 256;
        g.process(b);
        for (float v : L) assert(std::isfinite(v));
    }
    std::cout << "[PASS] Split lanes: amp+auto-cab in A, auto Pitch→MIDI + Instrument in B, renders cleanly.\n";
}

// CrossOver split (50000 / 51000 / 52000): a Linkwitz-Riley 4th-order band split at the split's input, the low band to
// lane A and the high band to lane B, at RackGraph::crossoverHz("f"). Parallel splits still copy the input unfiltered.
static std::string splitRigXml(int32_t a, int32_t b, int32_t m, const std::string& pre, const std::string& lane_b, float xfade, float f,
                               float inv = 0.5f) {
    auto unit = [](int32_t id, const std::string& params) {
        return "<component id=\"" + std::to_string(id) + "\"><component-audio version=\"1\"><parameters num-parameters=\"9\">" + params +
               "<parameter id=\"Pwr\" value=\"1\"/></parameters></component-audio></component>\n";
    };
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<headroom-preset version=\"1\">\n<headroom-rig>\n<rack>\n<non-fix-components>\n" + pre +
           unit(a, "") + unit(b, "") + lane_b +
           unit(m, "<parameter id=\"XFade\" value=\"" + std::to_string(xfade) + "\"/><parameter id=\"f\" value=\"" + std::to_string(f) +
                       "\"/><parameter id=\"iP\" value=\"" + std::to_string(inv) + "\"/>") +
           "</non-fix-components>\n</rack>\n</headroom-rig>\n</headroom-preset>\n";
}
static std::vector<float> renderRig(const std::string& xml, const std::vector<float>& in, bool pipelined, uint32_t block = 256) {
    rig::preset::RackPreset p;
    std::string err;
    const bool ok = rig::preset::parsePresetData(xml, "test.rig", p, &err);
    assert(ok);
    rig::RackGraph g;
    g.buildFromPreset(p, 48000.0, 512);
    g.setInputGainLinear(1.0f);
    g.setPipelining(pipelined);
    std::vector<float> out(in.size()), r(block);
    for (std::size_t pos = 0; pos + block <= in.size(); pos += block) {
        const float* ins[2] = {&in[pos], &in[pos]};
        float* outs[2] = {&out[pos], r.data()};
        rig::AudioBlock b;
        b.in = ins, b.out = outs, b.num_channels = 2, b.num_frames = block;
        g.process(b);
    }
    return out;
}
static double magnitudeAt(const std::vector<float>& h, double hz) { // |DFT| of an impulse response at hz (48 kHz)
    double re = 0.0, im = 0.0;
    const double w = 2.0 * std::numbers::pi * hz / 48000.0;
    for (std::size_t n = 0; n < h.size(); ++n) re += h[n] * std::cos(w * double(n)), im -= h[n] * std::sin(w * double(n));
    return std::hypot(re, im);
}
static void testCrossoverSplit() {
    constexpr std::size_t N = 16384;
    constexpr float kImp = 0.25f;
    std::vector<float> imp(N, 0.0f);
    imp[0] = kImp;
    auto db = [](double x) { return 20.0 * std::log10(std::max(x, 1e-12)); };
    const double probes[] = {25.0, 60.0, 150.0, 400.0, 1000.0, 2500.0, 6000.0, 15000.0};
    for (float f : {0.3f, 0.5f, 0.75f}) {
        const double fc = rig::RackGraph::crossoverHz(f);
        // Both lanes empty, X-Fade at the middle (unity for both): low + high is an allpass, flat in magnitude.
        auto h = renderRig(splitRigXml(50000, 51000, 52000, "", "", 0.5f, f), imp, false);
        for (float& v : h) v /= kImp;
        double worst = 0.0;
        for (double hz : probes) worst = std::max(worst, std::abs(db(magnitudeAt(h, hz))));
        worst = std::max(worst, std::abs(db(magnitudeAt(h, fc))));
        assert(worst < 0.01);
        // Lane A alone (X-Fade 0: lane A at the mix's 1.4 cap, lane B silent) is the low band: -6 dB at fc, flat an
        // octave and a half below, 24 dB per octave above. Lane B alone (X-Fade 1) is the high band, mirrored.
        for (int lane = 0; lane < 2; ++lane) {
            auto b = renderRig(splitRigXml(50000, 51000, 52000, "", "", lane ? 1.0f : 0.0f, f), imp, false);
            for (float& v : b) v /= kImp * 1.4f;
            const double at = db(magnitudeAt(b, fc)), pass = db(magnitudeAt(b, lane ? fc * 3.0 : fc / 3.0)),
                         stop = db(magnitudeAt(b, lane ? fc / 4.0 : fc * 4.0));
            assert(std::abs(at + 6.02) < 0.05);
            assert(std::abs(pass) < 0.3);
            assert(stop < -44.0);
        }
    }
    // Parallel split with empty lanes at X-Fade 0.5, Invert off: each lane is the input unfiltered, so the mix is exactly 2x.
    std::vector<float> sig(N);
    uint32_t r = 7;
    for (auto& v : sig) r = r * 1664525u + 1013904223u, v = 0.2f * (static_cast<float>(r >> 9) * (2.0f / 8388608.0f) - 1.0f);
    const auto par = renderRig(splitRigXml(7000, 8000, 9000, "", "", 0.5f, 0.3f, 0.0f), sig, false);
    for (std::size_t i = 0; i < N; ++i) assert(par[i] == 2.0f * sig[i]);
    // Pipelined into the split (an EQ before it, an EQ in the high lane): byte-identical to sequential at 64 / 256 frames.
    const std::string eq_before = "<component id=\"26000\"><component-audio version=\"1\"><parameters num-parameters=\"1\"><parameter id=\"Pwr\" value=\"1\"/></parameters></component-audio></component>\n";
    const std::string eq_lane = "<component id=\"32000\"><component-audio version=\"1\"><parameters num-parameters=\"1\"><parameter id=\"Pwr\" value=\"1\"/></parameters></component-audio></component>\n";
    for (uint32_t block : {64u, 256u}) {
        const std::string xml = splitRigXml(50000, 51000, 52000, eq_before, eq_lane, 0.5f, 0.45f);
        const auto seq = renderRig(xml, sig, false, block), pipe = renderRig(xml, sig, true, block);
        assert(std::memcmp(seq.data(), pipe.data(), sizeof(float) * N) == 0);
    }
    std::cout << "[PASS] CrossOver split: LR4 bands sum allpass-flat (< 0.01 dB), -6 dB at crossoverHz(f) for f = 0.3 / 0.5 / 0.75, "
                 "parallel split unfiltered, pipelined = sequential.\n";
}

static void testMidiClassification() {
    // A rig of instruments only is "MIDI"; with an amp lane it is "Guitar + MIDI"; tags name the families.
    rig::RackGraph g;
    rig::preset::RackPreset empty;
    empty.root_rack.table_id = 100;
    g.buildFromPreset(empty, 48000.0, 256);
    const int sid = g.insertSplit(999);
    g.insertIntoLane(sid, 'B', 9, 900003, {{"sound", 24.0f}}); // GM Acoustic Grand Piano
    g.insertIntoLane(sid, 'B', 9, 900003, {{"sound", 85.0f}}); // GM Brass Section
    std::string amp;
    std::vector<std::string> tags;
    assert(rig::library::PresetLibrary::classify(g.toPreset("t", ""), amp, tags) == "MIDI");
    auto has = [&](const char* t) { return std::find(tags.begin(), tags.end(), t) != tags.end(); };
    assert(has("Multi-instrument") && has("Piano") && has("Brass"));
    g.insertIntoLane(sid, 'A', 9, 56000, {});
    tags.clear();
    assert(rig::library::PresetLibrary::classify(g.toPreset("t", ""), amp, tags) == "Guitar + MIDI");
    std::cout << "[PASS] MIDI / Guitar + MIDI library classification.\n";
}

static void testNamCapture() {
    auto c = rig::createComponentByTableId(900005);
    c->prepare(48000.0, 256);
    const auto eps = c->extraParams();
    assert(!eps.empty() && !eps[0].labels.empty());
    const auto sig = rig::amp::makeCalibrationDI(48000.0, 1.0f);
    const auto out = runComponent(*c, sig, 48000.0);
    for (float v : out) assert(std::isfinite(v));
    assert(rmsOf(out) > 1e-4);
    std::cout << "[PASS] Neural Amp Modeler capture loads and renders (" << eps[0].labels.size() << " captures available).\n";
}

// Component detail page: the exported model must mirror the DSP, and overrides must round-trip.
static void testComponentModelExport() {
    using nlohmann::json;
    rig::platform::setEnv("HEADROOM_LEGACY_MODELS", "1"); // the behavioural amp's model export (56000 itself is a circuit now)
    const json amp = json::parse(rig::model::describe(56000, "{}", "", 48000.0));
    assert(amp["kind"] == "amp" && amp["oversampling"] == 8);
    const auto& st = amp["channels"][0]["stages"];
    assert(st.size() == 4);
    assert(st[0]["role"] == "triode" && std::abs(st[0]["rk"].get<double>() - 1500.0) < 1e-3);
    assert(st[1]["role"] == "cold_clipper");               // JCM800 V1B: unbypassed cold clipper
    assert(st[3]["role"] == "cathode_follower");
    assert(amp["tone_stack"]["preset"] == "marshall");
    assert(std::abs(amp["tone_stack"]["values"]["R4"].get<double>() - 33e3) < 1.0);
    assert(std::abs(amp["power"]["nfb_beta"].get<double>() - 0.39) < 1e-3);
    assert(amp.contains("loops") && !amp["blocks"].empty());

    // Overrides: stage edits, stage removal, tone-stack swap, open-loop power amp.
    const std::string ov = R"({"name":"Test","amp":{"channels":{"0":{"stages":{"1":{"gain":5,"bias":-0.05},"2":{"off":true}}}},"stack":{"preset":"fender"},"power":{"damping":0}}})";
    const json mod = json::parse(rig::model::describe(56000, "{}", ov, 48000.0));
    const auto& ms = mod["channels"][0]["stages"];
    assert(ms.size() == 3 && ms[1]["role"] == "triode" && std::abs(ms[1]["spec"]["gain"].get<double>() - 5.0) < 1e-4);
    assert(mod["tone_stack"]["preset"] == "fender" && mod["power"]["nfb_beta"].get<double>() == 0.0 && !mod.contains("loops"));
    assert(mod["custom"] == true && mod["custom_name"] == "Test");

    // Round trip through exportParams (what presets store) into a fresh unit.
    auto a = rig::createComponentByTableId(56000);
    a->prepare(48000.0, 256);
    a->setStringParameter(rig::model::kModelParam, ov);
    std::string saved;
    for (const auto& e : a->exportParams())
        if (e.id == rig::model::kModelParam) saved = e.str;
    assert(saved == ov);
    assert(rig::model::customName(*a) == "Test");
    auto b = rig::createComponentByTableId(56000);
    b->prepare(48000.0, 256);
    for (const auto& e : a->exportParams()) {
        if (e.is_string) b->setStringParameter(e.id, e.str);
        else b->setParameter(e.id, e.num);
    }
    const auto sig = rig::amp::makeCalibrationDI(48000.0, 0.5f);
    const auto ya = runComponent(*a, sig, 48000.0), yb = runComponent(*b, sig, 48000.0);
    for (std::size_t i = 0; i < ya.size(); ++i) assert(std::isfinite(ya[i]) && std::abs(ya[i] - yb[i]) < 1e-6f);
    auto stock = rig::createComponentByTableId(56000);
    for (const auto& e : stock->exportParams()) assert(e.id != rig::model::kModelParam); // stock units save no override slot

    // Pedals: topology + diode overrides.
    const json ts = json::parse(rig::model::describe(15000, "{}", R"({"drive":{"kind":"rat","vd":0.35}})", 48000.0));
    assert(ts["kind"] == "drive" && ts["topology"] == "rat" && std::abs(ts["values"]["vd"].get<double>() - 0.35) < 1e-4);

    // Curves are computed offline on private instances.
    const json cv = json::parse(rig::model::curves(56000, "{}", "", 48000.0, "static,harmonics"));
    assert(cv["static"]["stages"]["curves"].size() == 4 && cv["static"]["tonestack"]["db"].size() == 96);
    assert(cv["harmonics"]["h"][0].get<double>() == 0.0 && cv["harmonics"]["thd_pct"].get<double>() > 1.0);
    const json gv = json::parse(rig::model::describe(98000, "{}", "", 48000.0));
    assert(gv["kind"] == "delay" && gv["generic"] == true);
    std::cout << "[PASS] Component model export mirrors the DSP; overrides round-trip through presets.\n";
}

void runPluginTests(); // tests/test_plugins.cpp — hosted Audio Units

static void testMidiStateAdoption() {
    // Knob edits rebuild a unit; the MIDI units must carry live state across (no dropped notes).
    const double sr = 48000.0;
    const uint32_t n = 256;
    std::vector<float> L(n), R(n), in(n, 0.0f);
    auto render = [&](rig::IComponent& c, std::vector<rig::midi::MidiEvent>* midi) {
        const float* ins[2] = {in.data(), in.data()};
        float* outs[2] = {L.data(), R.data()};
        rig::AudioBlock b; b.in = ins; b.out = outs; b.num_channels = 2; b.num_frames = n; b.sample_rate = sr; b.midi = midi;
        c.processControlRate(); c.processAudio(b);
        double e = 0; for (float v : L) e += double(v) * v; return std::sqrt(e / n);
    };
    // Instrument (FM electric piano): a held note keeps sounding after a tone-knob rebuild.
    auto a = rig::createComponentByTableId(900003);
    a->setParameter("sound", 0.0f); a->prepare(sr, n);
    std::vector<rig::midi::MidiEvent> ev{{rig::midi::MidiEvent::NoteOn, 60, 0.9f, 0.0f, 0}};
    render(*a, &ev);
    std::vector<rig::midi::MidiEvent> none;
    for (int i = 0; i < 4; ++i) render(*a, &none);
    auto b = rig::createComponentByTableId(900003);
    b->setParameter("sound", 0.0f); b->setParameter("tone", 0.8f); b->prepare(sr, n);
    b->adoptRuntimeStateFrom(*a);
    const double after = render(*b, &none);
    assert(after > 1e-4 && "instrument voice must survive a rebuild");
    // Pitch -> MIDI: a note being tracked stays on (no NoteOff) across a sensitivity rebuild.
    auto d1 = rig::createComponentByTableId(900001);
    d1->setParameter("mode", 1.0f); d1->prepare(sr, n);
    std::vector<rig::midi::MidiEvent> out;
    double ph = 0;
    auto feed = [&](rig::IComponent& c) {
        for (uint32_t i = 0; i < n; ++i) { in[i] = 0.3f * std::sin(ph); ph += 2 * M_PI * 196.0 / sr; }
        out.clear(); render(c, &out);
    };
    bool on = false;
    for (int i = 0; i < 60; ++i) { feed(*d1); for (auto& e : out) if (e.type == rig::midi::MidiEvent::NoteOn) on = true; }
    assert(on && "detector should be tracking the G3 tone");
    auto d2 = rig::createComponentByTableId(900001);
    d2->setParameter("mode", 1.0f); d2->setParameter("sens", 0.7f); d2->prepare(sr, n);
    d2->adoptRuntimeStateFrom(*d1);
    bool off = false, reon = false;
    for (int i = 0; i < 20; ++i) { feed(*d2); for (auto& e : out) { if (e.type == rig::midi::MidiEvent::NoteOff) off = true; if (e.type == rig::midi::MidiEvent::NoteOn) reon = true; } }
    assert(!off && !reon && "held note must continue through the rebuild");
    std::fill(in.begin(), in.end(), 0.0f);
    std::cout << "[PASS] MIDI units keep live voices/notes across knob-edit rebuilds.\n";
}

static void testProStudioSuite() {
    const double sr = 48000.0;
    const auto sig = rig::amp::makeCalibrationDI(sr, 0.5f);

    // 1. Studio Phrase & Re-Amp Looper (900200 + Legacy 53000 + PhraseLooperEngine)
    {
        auto looper = rig::createComponentByTableId(rig::kLooperUnitTable);
        assert(looper != nullptr);
        looper->prepare(sr, 256);
        looper->setParameter("mode", 1.0f); // Record
        looper->processControlRate();
        const auto rec_out = runComponent(*looper, sig, sr);
        assert(rmsOf(rec_out) > 1e-4);

        looper->setParameter("mode", 2.0f); // Play
        looper->processControlRate();
        std::vector<float> silence(sig.size(), 0.0f);
        const auto play_out = runComponent(*looper, silence, sr);
        assert(rmsOf(play_out) > 1e-4 && "looper must play back recorded phrase over silent input");

        // Test overdub + undo
        looper->setParameter("mode", 3.0f); // Overdub
        looper->processControlRate();
        runComponent(*looper, sig, sr);
        looper->setParameter("undo", 1.0f);
        looper->processControlRate();
        assert(rig::looperSlotJson(*looper).find("\"state\":") != std::string::npos);

        auto legacy = rig::createComponentByTableId(rig::kLegacyLoopStationTable);
        assert(legacy != nullptr);
        legacy->prepare(sr, 256);
        const auto leg_out = runComponent(*legacy, sig, sr);
        assert(rmsOf(leg_out) > 1e-4);
    }

    // 2. Polyphonic Pitch Shifter / Drop-Tuner & Whammy (900201)
    {
        auto pitch = rig::createComponentByTableId(rig::kTransposeUnitTable);
        assert(pitch != nullptr);
        pitch->prepare(sr, 256);
        pitch->setParameter("semi", -2.0f); // Drop D / D standard (-2 st)
        pitch->setParameter("mix", 1.0f);
        pitch->processControlRate();
        const auto out = runComponent(*pitch, sig, sr);
        for (float v : out) assert(std::isfinite(v));
        assert(rmsOf(out) > 1e-4);
    }

    // 3. Stereo Doubler & Amp-in-the-Room Binaural Spatializer (900202)
    {
        auto spatial = rig::createComponentByTableId(rig::kDoublerRoomUnitTable);
        assert(spatial != nullptr);
        spatial->prepare(sr, 256);
        spatial->setParameter("dbl_on", 1.0f);
        spatial->setParameter("width", 0.85f);
        spatial->setParameter("room_on", 1.0f);
        spatial->setParameter("room", 0.45f);
        spatial->setParameter("demic", 0.65f);
        spatial->processControlRate();
        const auto out = runComponent(*spatial, sig, sr);
        for (float v : out) assert(std::isfinite(v));
        assert(rmsOf(out) > 1e-4);
    }

    // 4. AI Reference Tone Match EQ (900203) + Built-in Reference Targets
    {
        auto tm = rig::createComponentByTableId(rig::kToneMatchUnitTable);
        assert(tm != nullptr);
        tm->prepare(sr, 256);
        const auto curve = rig::builtinReferenceTarget(1); // British 800 Crunch
        assert(rig::applyToneMatchCurve(*tm, curve));
        tm->processControlRate();
        const auto out = runComponent(*tm, sig, sr);
        for (float v : out) assert(std::isfinite(v));
        assert(rmsOf(out) > 1e-4);
    }

    // 5. Auto Input Gain Calibrator
    {
        rig::InputCalibrator cal;
        cal.observeBlock(sig.data(), sig.data(), static_cast<uint32_t>(sig.size()), sr);
        const auto rec = cal.computeRecommendation(0.0f);
        assert(rec.ok && std::isfinite(rec.recommended_gain_db) && !rec.pickup_profile.empty());
    }

    // 6. Blended Cabinet IR WAV Baker & User IR Folder
    {
        rig::rescanUserIrs();
        assert(rig::userIrsJson().find("\"folder\":") != std::string::npos);
        auto cab = rig::createComponentByTableId(rig::kCabinetUnitTable);
        cab->prepare(sr, 256);
        cab->processControlRate();
        std::string out_path, out_err;
        const bool baked = rig::bakeUnitIrToWav(*cab, sr, "UnitTest_Baked_Cab", false, out_path, out_err);
        assert(baked && std::filesystem::exists(out_path));
        std::filesystem::remove(out_path);
    }

    // 7. Gapless A/B/C/D Rig Scenes & A/B Compare + MIDI Learn Map
    {
        rig::RackGraph g;
        rig::preset::RackPreset empty;
        empty.root_rack.table_id = 100;
        g.buildFromPreset(empty, sr, 256);
        g.insertComponent(56000, 0, {{"V1", 0.35f}});
        rig::RigSceneManager scenes;
        scenes.initForLoadedPreset(g);
        g.setComponentNormalizedParameter(0, "V1", 0.85f);
        scenes.saveScene(1, g, "Lead Boost");
        assert(scenes.selectScene(0, g));
        assert(std::abs(g.activeNodeViews()[0].component->getParameter("V1") - 0.35f) < 0.05f);
        assert(scenes.selectScene(1, g));
        assert(std::abs(g.activeNodeViews()[0].component->getParameter("V1") - 0.85f) < 0.05f);
        scenes.toggleAbCompare(g);

        rig::MidiControllerMap midi_map;
        midi_map.armLearn("scene:1");
        assert(midi_map.handleIncomingCc(21, 127, 1) == "scene:1");
        assert(midi_map.handleIncomingCc(21, 127, 1) == "scene:1");
    }

    // 8. Interactive Pre-Capture Tone-Stack & Coupled Power-Amp Sag on NAM Capture (900005)
    {
        auto nam = rig::createComponentByTableId(900005);
        nam->prepare(sr, 256);
        nam->setParameter("bass", 0.8f);
        nam->setParameter("treble", 0.75f);
        nam->setParameter("stack_react", 0.9f);
        nam->setParameter("sag", 0.65f);
        nam->processControlRate();
        const auto out = runComponent(*nam, sig, sr);
        for (float v : out) assert(std::isfinite(v));
        assert(rmsOf(out) > 1e-4);
    }

    std::cout << "[PASS] Pro Studio Suite (Looper, Poly Pitch/Whammy, Doubler/Room, ToneMatch, Calibrator, IR Baker, Scenes, MIDI Learn, NAM Sag/Stack).\n";
}

static void testUserRigLineageAndOperations() {
    using nlohmann::json;
    const fs::path tmp_dir = fs::temp_directory_path() / "headroom_test_user_rigs";
    std::error_code ec;
    fs::remove_all(tmp_dir, ec);
    fs::create_directories(tmp_dir, ec);

    rig::library::PresetLibrary lib;
    lib.scan(tmp_dir.string(), {}, "");

    // 1. Save a user rig derived from a Library preset ("JCM800 British Crunch")
    rig::preset::RackPreset p;
    p.info.name = "My Crunch Rig";
    p.info.author = "Tester";
    p.info.based_on = "JCM800 British Crunch";
    p.info.based_on_path = "app/presets/Showcase/00 JCM800 British Crunch.rig";
    p.root_rack.table_id = 100;
    rig::preset::PresetComponentNode amp_node{};
    amp_node.table_id = 56000;
    amp_node.params.push_back({"V1", "float", "Gain", 0.65f, 0, false, ""});
    p.root_rack.children.push_back(amp_node);

    std::string err;
    const int idx1 = lib.saveUser(p, &err);
    assert(idx1 >= 0 && err.empty());

    // Verify round-trip in .rig XML and in LibraryEntry
    rig::library::LibraryEntry e1;
    assert(lib.entry(idx1, e1));
    assert(e1.user);
    assert(e1.name == "My Crunch Rig");
    assert(e1.based_on == "JCM800 British Crunch");
    assert(e1.based_on_path == "app/presets/Showcase/00 JCM800 British Crunch.rig");
    assert(e1.mtime > 0);
    assert(fs::exists(e1.path));

    // 2. Save a from-scratch user rig (empty based_on)
    rig::preset::RackPreset p2 = p;
    p2.info.name = "Scratch Rig";
    p2.info.based_on.clear();
    p2.info.based_on_path.clear();
    const int idx2 = lib.saveUser(p2, &err);
    assert(idx2 >= 0);

    // 3. Verify /api/library JSON exposes based_on, based_on_path, mtime, path, and omits "User" from categories
    const json j = json::parse(lib.json());
    assert(j["presets"].size() == 2);
    for (const auto& cat : j["categories"]) {
        assert(cat.get<std::string>() != "User");
    }
    bool found_crunch = false, found_scratch = false;
    for (const auto& pr : j["presets"]) {
        if (pr["name"] == "My Crunch Rig") {
            found_crunch = true;
            assert(pr["source"] == "user");
            assert(pr["based_on"] == "JCM800 British Crunch");
            assert(pr["based_on_path"] == "app/presets/Showcase/00 JCM800 British Crunch.rig");
            assert(pr["mtime"].get<int64_t>() > 0);
            assert(!pr["path"].get<std::string>().empty());
        } else if (pr["name"] == "Scratch Rig") {
            found_scratch = true;
            assert(pr["source"] == "user");
            assert(pr["based_on"] == "");
            assert(pr["mtime"].get<int64_t>() > 0);
        }
    }
    assert(found_crunch && found_scratch);

    // 4. Test duplicateUser: preserves based_on lineage and creates "<Name> Copy"
    const int dup_idx = lib.duplicateUser(idx1, &err);
    assert(dup_idx >= 0 && err.empty());
    rig::library::LibraryEntry edup;
    assert(lib.entry(dup_idx, edup));
    assert(edup.name == "My Crunch Rig Copy");
    assert(edup.based_on == "JCM800 British Crunch");

    // 5. Test renameUser: renames file and updates internal preset name while preserving lineage
    const int ren_idx = lib.renameUser(dup_idx, "Renamed British Stack", &err);
    assert(ren_idx >= 0 && err.empty());
    rig::library::LibraryEntry eren;
    assert(lib.entry(ren_idx, eren));
    assert(eren.name == "Renamed British Stack");
    assert(eren.based_on == "JCM800 British Crunch");
    assert(!fs::exists(edup.path));
    assert(fs::exists(eren.path));

    // 6. Test deleteUser
    assert(lib.deleteUser(ren_idx));
    assert(lib.entries().size() == 2);

    fs::remove_all(tmp_dir, ec);
    std::cout << "[PASS] User rig lineage (based_on, mtime) and rename/duplicate/delete verified.\n";
}

// Headroom's shipped library (presets/Library): every rig parses, carries its <headroom-library> metadata, lands in
// a known category, and is listed as source "library" ahead of the showcase.
static void testShippedLibrary() {
    using nlohmann::json;
    const fs::path root = fs::path(__FILE__).parent_path().parent_path();
    const fs::path lib_dir = root / "presets" / "Library";
    if (!fs::exists(lib_dir)) {
        std::cout << "[SKIP] Shipped library dir not present.\n";
        return;
    }
    const fs::path tmp_dir = fs::temp_directory_path() / "headroom_test_library_user";
    std::error_code ec;
    fs::remove_all(tmp_dir, ec);
    rig::library::PresetLibrary lib;
    lib.scan(tmp_dir.string(), {}, (root / "presets" / "Showcase").string(), lib_dir.string());
    std::size_t files = 0;
    for (const auto& it : fs::recursive_directory_iterator(lib_dir))
        if (it.is_regular_file() && it.path().extension() == ".rig") ++files;
    const auto& order = rig::library::PresetLibrary::categoryOrder();
    std::size_t n = 0, inspired = 0;
    bool seen_showcase = false;
    for (const auto& e : lib.entries()) {
        if (e.showcase) seen_showcase = true;
        if (!e.library) continue;
        assert(!seen_showcase); // the library is listed before the showcase
        ++n;
        assert(e.author == "Headroom");
        assert(!e.lib_id.empty() && !e.comment.empty());
        assert(std::find(order.begin(), order.end(), e.category) != order.end() && e.category != "Featured");
        if (!e.inspired_artist.empty()) {
            ++inspired;
            assert(e.sources.size() >= 2);
        }
    }
    assert(n == files);
    const json j = json::parse(lib.json());
    for (const auto& pr : j["presets"])
        if (pr["source"] == "library") assert(pr.contains("sources"));
    const auto cats = j["categories"];
    assert(!cats.empty() && cats[0] == "Featured");
    fs::remove_all(tmp_dir, ec);
    std::cout << "[PASS] Shipped library: " << n << " rigs (" << inspired << " artist tones) parsed with metadata and categories.\n";
}

int main() {
    testCatalogCompleteness();
    testInternalSampleRateFormula();
    testNeuralMathAndPilots();
    testBundledShowcaseRigsParse();
    testPartitionedConvolverExact();
    testAllAmpsAndPedalsStable();
    testPitchToMidiMono();
    testSplitLaneRack();
    testCrossoverSplit();
    testNamCapture();
    testMidiClassification();
    testMidiStateAdoption();
    testComponentModelExport();
    rig::platform::unsetEnv("HEADROOM_LEGACY_MODELS");
    testProStudioSuite();
    testUserRigLineageAndOperations();
    testShippedLibrary();
    runPluginTests();
    std::cout << "ALL TESTS PASSED.\n";
    return 0;
}
