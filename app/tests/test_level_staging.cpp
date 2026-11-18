// Level and staging regressions:
//  * Amp-Matched Cab (156000) Volume spans +/-12 dB, monotonic, unchanged at 0.5.
//  * Amp-Matched Cab (156000) controls: Cab A/B picks one capture, X-Fade blends in the room at
//    constant level, and no cabinet cancels (A and B used to be summed; several B captures are polarity-inverted
//    or 1.6 ms late against A, which cost up to 7 dB and a hollow, 125 Hz-peaked tone).
//  * Bass rigs are measured with a bass-register DI by the offline levelling (RackGraph::calibrateLoudness,
//    tools/rig_level); the engine itself never measures or re-levels a rig.
#undef NDEBUG
#include "platform/Environment.hpp"
#include "../src/core/AudioBlock.hpp"
#include "../src/core/IComponent.hpp"
#include "../src/core/RackGraph.hpp"
#include "../src/dsp/BassCalibration.hpp"
#include "../src/dsp/ConvolutionEngine.hpp"
#include "../src/preset/PresetLoader.hpp"
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace {
int g_failures = 0;
void check(bool ok, const std::string& what) {
    std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what.c_str());
    if (!ok) ++g_failures;
}
constexpr double kSr = 48000.0;

// Pink-ish noise, the same every call.
std::vector<float> noise(std::size_t n) {
    std::mt19937 rng(3);
    std::normal_distribution<float> nd(0.0f, 1.0f);
    std::vector<float> x(n);
    double b0 = 0, b1 = 0, b2 = 0;
    for (auto& v : x) {
        const float w = nd(rng);
        b0 = 0.99765 * b0 + w * 0.0990460; b1 = 0.96300 * b1 + w * 0.2965164; b2 = 0.57000 * b2 + w * 1.0526913;
        v = static_cast<float>(0.1 * (b0 + b1 + b2 + w * 0.1848));
    }
    return x;
}

double levelDb(rig::IComponent& c, const std::vector<float>& x) {
    c.prepare(kSr, 256);
    c.reset();
    std::vector<float> l(256), r(256);
    double e = 0.0;
    std::size_t cnt = 0;
    for (std::size_t p = 0; p + 256 <= x.size(); p += 256) {
        c.processControlRate();
        const float* ins[2] = {x.data() + p, x.data() + p};
        float* outs[2] = {l.data(), r.data()};
        rig::AudioBlock b{};
        b.in = ins; b.out = outs; b.num_channels = 2; b.num_frames = 256; b.sample_rate = kSr;
        c.processAudio(b);
        if (p > x.size() / 8)
            for (std::size_t i = 0; i < 256; ++i) { e += 0.5 * (double(l[i]) * l[i] + double(r[i]) * r[i]); ++cnt; }
    }
    return 10.0 * std::log10(std::max(1e-20, e / double(std::max<std::size_t>(1, cnt))));
}

double cabDb(const std::map<std::string, float>& p, const std::vector<float>& x) {
    auto c = rig::createComponentByTableId(156000);
    for (const auto& [k, v] : p) c->setParameter(k, v);
    return levelDb(*c, x);
}

std::string rigXml(const std::string& extra_head, const std::vector<std::pair<int, std::string>>& units) {
    std::string s = R"(<?xml version="1.0" encoding="UTF-8"?>
<headroom-preset version="1"><headroom-info><soundinfo><properties><name>t</name></properties></soundinfo></headroom-info>
)" + extra_head + R"(<headroom-rig><rack><non-fix-components>
)";
    for (const auto& [t, params] : units)
        s += "<component id=\"" + std::to_string(t) + "\"><component-audio version=\"1\"><parameters num-parameters=\"1\"><parameter id=\"Pwr\" value=\"1\"/>" +
             params + "</parameters></component-audio></component>\n";
    return s + "</non-fix-components></rack></headroom-rig></headroom-preset>";
}

bool load(const std::string& xml, rig::preset::RackPreset& out) {
    std::string err;
    return rig::preset::parsePresetData(xml, "test.rig", out, &err);
}
} // namespace

int main() {
    // Reproducible noise streams (CircuitUnit, docs §3.9). Since circuit units salt their noise per instance, two
    // calibrations of the same rig differ by up to ~2e-4 dB (34000 + cab: 6.99428 / 6.99435 / 6.99442 dB), which the
    // exact "same DI, same gain" check below would read as a different DI.
    rig::platform::setEnv("HEADROOM_NOISE_FIXED_SEED", "1");
    const auto x = noise(static_cast<std::size_t>(kSr * 1.5));

    // ── Amp-Matched Cab (156000) Volume ──
    {
        check(std::fabs(rig::dsp::ampMatchedCabVolume(0.5f) - 1.15f) < 1e-6f, "Volume 0.5 keeps its old gain (1.15)");
        double prev = -1e9, lo = 0, hi = 0;
        bool mono = true;
        for (int i = 0; i <= 20; ++i) {
            const float mv = static_cast<float>(i) / 20.0f;
            const double d = cabDb({{"Cab", 9.0f}, {"MV", mv}}, x);
            mono &= d > prev + 0.2;
            prev = d;
            if (i == 0) lo = d;
            if (i == 20) hi = d;
        }
        const double mid = cabDb({{"Cab", 9.0f}, {"MV", 0.5f}}, x);
        check(mono, "Volume is monotonic over 0..1");
        check(std::fabs(hi - lo - 24.0) < 0.2 && std::fabs(mid - lo - 12.0) < 0.2,
              "Volume spans 24 dB, -12..+12 around 0.5 (" + std::to_string(lo - mid) + " .. +" + std::to_string(hi - mid) + " dB)");
        // Through the middle it tracks the old 0.7 + 0.9 * MV law, so existing settings there keep their level.
        double worst_mid = 0;
        for (float mv = 0.3f; mv <= 0.701f; mv += 0.05f) {
            const double old_db = 20.0 * std::log10((0.7 + 0.9 * mv) / 1.15);
            worst_mid = std::max(worst_mid, std::fabs(rig::dsp::ampMatchedCabVolumeDb(mv) - old_db));
        }
        check(worst_mid < 0.7, "Volume 0.3-0.7 stays within 0.7 dB of the old law (worst " + std::to_string(worst_mid) + " dB)");
    }

    // ── Cab A/B, room blend, and no cabinet cancels ──
    {
        // The synthesized cabinet IRs build the room from the same modal response 4.5 ms later, so only the end
        // points are compared.
        double worst_room = 0, worst_ab = 0;
        int differs = 0;
        for (int cab = 0; cab < 26; ++cab) {
            const float c = static_cast<float>(cab);
            const double a = cabDb({{"Cab", c}, {"c", 0.0f}, {"V", 0.0f}}, x);
            const double b = cabDb({{"Cab", c}, {"c", 0.0f}, {"V", 1.0f}}, x);
            const double room = cabDb({{"Cab", c}, {"c", 0.5f}, {"V", 0.0f}}, x);
            const double full_room = cabDb({{"Cab", c}, {"c", 1.0f}, {"V", 0.0f}}, x);
            worst_room = std::max(worst_room, std::fabs(full_room - a));
            (void)room;
            worst_ab = std::max(worst_ab, std::fabs(b - a));
            if (std::fabs(b - a) > 0.05) ++differs;
        }
        check(worst_room < 3.0, std::string("X-Fade (cabinet -> room, end points") +
                                    ") keeps the level within 3 dB on all 26 cabinets (worst " + std::to_string(worst_room) + " dB)");
        check(worst_ab < 4.5, "Cab A and B captures sit within 4.5 dB of each other, nothing cancels (worst " + std::to_string(worst_ab) + " dB)");
        check(differs >= 20, "Cab A/B switches the capture (" + std::to_string(differs) + "/26 differ)");
        // A fresh unit starts on the close capture (the schema's generic 0.5 would be half room).
        auto fresh = rig::createComponentByTableId(156000);
        check(fresh->getParameter("c") == 0.0f, "a new Amp-Matched Cab (156000) starts at X-Fade 0 (cabinet only)");
    }

    // ── bass rigs: detection and bass-register loudness match ──
    {
        const std::string bass_amp = R"(<parameter id="volume" value="0.55"/>)";
        rig::preset::RackPreset bass, guitar, tagged_guitar, mixed, lib_bass;
        const bool ok = load(rigXml("", {{177000, bass_amp}, {156000, R"(<parameter id="Cab" value="25"/>)"}}), bass) &&
                        load(rigXml("", {{34000, ""}, {156000, R"(<parameter id="Cab" value="9"/>)"}}), guitar) &&
                        load(rigXml(R"(<headroom-library version="1" id="x" category="Clean" instrument="guitar"></headroom-library>
)", {{177000, bass_amp}, {156000, ""}}), tagged_guitar) &&
                        load(rigXml("", {{177000, bass_amp}, {34000, ""}, {30000, ""}, {156000, ""}}), mixed) &&
                        load(rigXml(R"(<headroom-library version="1" id="y" category="Bass" instrument="bass"></headroom-library>
)", {{34000, ""}, {156000, ""}}), lib_bass);
        check(ok, "test rigs parse");
        check(lib_bass.info.instrument == "bass" && tagged_guitar.info.instrument == "guitar", "<headroom-library instrument> is read");
        auto graph = [](const rig::preset::RackPreset& p) { auto g = std::make_unique<rig::RackGraph>(); g->buildFromPreset(p, kSr, 512); return g; };
        check(graph(bass)->calibratesAsBass(), "a bass amp rig calibrates as bass");
        check(!graph(guitar)->calibratesAsBass(), "a guitar amp rig calibrates as guitar");
        check(!graph(tagged_guitar)->calibratesAsBass(), "library metadata 'guitar' wins over a bass amp");
        check(graph(lib_bass)->calibratesAsBass(), "library metadata 'bass' wins over a guitar amp");
        check(!graph(mixed)->calibratesAsBass(), "one bass amp among two guitar amps stays guitar");

        // The bass DI drives a bass rig harder than the guitar DI does, so the bass match asks for less level.
        const double c_bass = graph(bass)->calibrateLoudness();
        const double c_as_guitar = graph(tagged_guitar)->calibrateLoudness(); // same amp, guitar DI
        const double diff = c_as_guitar - c_bass;
        check(diff > 2.0, "bass rig: guitar-DI calibration would add " + std::to_string(diff) + " dB more level than the bass DI");
        // ...and a guitar preset is untouched by the change (still the guitar DI): same correction as an explicitly tagged one.
        rig::preset::RackPreset guitar_tagged = guitar;
        guitar_tagged.info.instrument = "guitar";
        check(std::fabs(graph(guitar)->calibrateLoudness() - graph(guitar_tagged)->calibrateLoudness()) < 1e-4f, "guitar presets keep the guitar calibration DI");
        // Measuring changes nothing: the rack plays at its own level before and after (no loudness gain anywhere).
        {
            auto g = graph(guitar);
            auto render = [&](rig::RackGraph& r) {
                const auto di = rig::amp::makeCalibrationDI(kSr, 1.0f);
                std::vector<float> l(256), rr(256);
                double e = 0;
                for (std::size_t p = 0; p + 256 <= di.size(); p += 256) {
                    const float* ins[2] = {di.data() + p, di.data() + p};
                    float* outs[2] = {l.data(), rr.data()};
                    rig::AudioBlock b{};
                    b.in = ins; b.out = outs; b.num_channels = 2; b.num_frames = 256; b.sample_rate = kSr;
                    r.process(b);
                    for (float v : l) e += double(v) * v;
                }
                return 10.0 * std::log10(e + 1e-30);
            };
            const double before = render(*graph(guitar));
            (void)g->calibrateLoudness();
            g->reset();
            check(std::fabs(render(*g) - before) < 0.01, "calibrateLoudness() measures only: the rack's level is unchanged after it");
        }

        const auto di = rig::amp::makeBassCalibrationDI(kSr, 4.0f);
        double e = 0, peak = 0;
        for (float v : di) { e += double(v) * v; peak = std::max(peak, double(std::fabs(v))); }
        const double rms_db = 10.0 * std::log10(e / di.size());
        check(std::fabs(rms_db + 19.0) < 0.3 && peak < 0.96, "bass calibration DI: about -19 dBFS RMS, no clipping (" + std::to_string(rms_db) + " dB)");
    }

    std::printf("%s\n", g_failures ? "FAILED" : "all level / staging checks passed");
    return g_failures ? 1 : 0;
}
