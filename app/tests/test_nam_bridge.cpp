// NAM Capture wrapper regressions: sample-rate bridge (aliasing, imaging, latency, continuity,
// real-time safety), post-model hysteretic noise gate, and dBu calibration / SlimmableModel size.
// Uses tiny synthetic Linear .nam captures so the measurement isolates Headroom's wrapper.
#undef NDEBUG
#include "../src/core/IComponent.hpp"
#include "test_compat.hpp"
#include "../src/dsp/AmpUtilities.hpp"
#include "../src/dsp/ComponentModel.hpp"
#include "json.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <new>
#include <numbers>
#include <string>
#include <vector>

namespace {
thread_local bool g_in_audio = false;
thread_local long g_audio_allocs = 0;
} // namespace
void* operator new(std::size_t size) {
    if (g_in_audio) ++g_audio_allocs;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

namespace {
namespace fs = std::filesystem;
constexpr double kPi = std::numbers::pi;

int g_failures = 0;
void check(bool ok, const std::string& what) {
    std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what.c_str());
    if (!ok) ++g_failures;
}

fs::path tempDir() {
    const fs::path d = fs::temp_directory_path() / "headroom_nam_bridge_test";
    fs::create_directories(d);
    return d;
}

// Linear capture: y = w0 * x + bias, trained at `sr`.
std::string writeLinearCapture(const std::string& name, double sr, float w0, float bias, const nlohmann::json& metadata = nullptr) {
    nlohmann::json j;
    j["version"] = "0.5.4";
    j["architecture"] = "Linear";
    j["config"] = {{"receptive_field", 1}, {"bias", bias != 0.0f}};
    j["weights"] = bias != 0.0f ? std::vector<float>{w0, bias} : std::vector<float>{w0};
    j["sample_rate"] = sr;
    if (!metadata.is_null()) j["metadata"] = metadata;
    const fs::path p = tempDir() / name;
    std::ofstream(p) << j.dump();
    return p.string();
}

struct Rig {
    std::unique_ptr<rig::IComponent> c;
    double sr;
    uint32_t block;
    std::vector<float> in, l, r;
    Rig(double rate, uint32_t blk, const std::string& path) : sr(rate), block(blk), in(blk), l(blk), r(blk) {
        c = rig::createComponentByTableId(900005);
        assert(c);
        c->prepare(rate, blk);
        c->setParameter("normalize", 0.0f);
        c->setParameter("stack_react", 0.0f);
        c->setParameter("sag", 0.0f);
        c->setParameter("gate", 0.0f);
        c->setStringParameter("capture_path", path);
        c->processControlRate();
        c->reset();
    }
    nlohmann::json model() const {
        auto* m = dynamic_cast<rig::model::Introspectable*>(c.get());
        assert(m);
        return nlohmann::json::parse(m->modelJson());
    }
    // Streams `x` through the unit in blocks; returns the left output.
    std::vector<float> run(const std::vector<float>& x) {
        std::vector<float> y(x.size(), 0.0f);
        const float* ins[2] = {in.data(), in.data()};
        float* outs[2] = {l.data(), r.data()};
        for (std::size_t pos = 0; pos < x.size(); pos += block) {
            const uint32_t n = static_cast<uint32_t>(std::min<std::size_t>(block, x.size() - pos));
            std::copy(x.begin() + static_cast<std::ptrdiff_t>(pos), x.begin() + static_cast<std::ptrdiff_t>(pos + n), in.begin());
            rig::AudioBlock b{};
            b.in = ins; b.out = outs; b.num_channels = 2; b.num_frames = n; b.sample_rate = sr;
            g_in_audio = true;
            c->processAudio(b);
            g_in_audio = false;
            std::copy(l.begin(), l.begin() + n, y.begin() + static_cast<std::ptrdiff_t>(pos));
        }
        return y;
    }
};

std::vector<float> sine(double sr, double hz, double amp, std::size_t n) {
    std::vector<float> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<float>(amp * std::sin(2.0 * kPi * hz * static_cast<double>(i) / sr));
    return v;
}

// Blackman-Harris windowed single-bin DFT over the last `n` samples: amplitude and phase at `hz`.
struct Tone { double amp, phase; };
Tone measure(const std::vector<float>& y, double sr, double hz, std::size_t n) {
    const std::size_t start = y.size() - n;
    double re = 0, im = 0, wsum = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(n - 1);
        const double w = 0.35875 - 0.48829 * std::cos(2 * kPi * t) + 0.14128 * std::cos(4 * kPi * t) - 0.01168 * std::cos(6 * kPi * t);
        const double ph = 2.0 * kPi * hz * static_cast<double>(start + i) / sr;
        re += w * y[start + i] * std::cos(ph);
        im += w * y[start + i] * std::sin(ph);
        wsum += w;
    }
    // y = A sin(ph + p) → re ≈ A/2 sin p, im ≈ A/2 cos p
    return {2.0 * std::hypot(re, im) / wsum, std::atan2(re, im)};
}
double db(double x) { return 20.0 * std::log10(std::max(1e-12, x)); }

void testRateBridge(const std::string& identity) {
    // ── 96 kHz host, 48 kHz capture: a 30 kHz tone must not fold to 18 kHz ──
    {
        Rig rig(96000.0, 256, identity);
        const auto m = rig.model();
        check(m["loaded"].get<bool>() && m["resampled"].get<bool>(), "96k host loads the 48k identity capture with the rate bridge");
        const auto y = rig.run(sine(96000.0, 30000.0, 0.5, 96000));
        const double alias = db(measure(y, 96000.0, 18000.0, 32768).amp / 0.5);
        const double through = db(measure(y, 96000.0, 30000.0, 32768).amp / 0.5);
        std::printf("      96k->48k: 30 kHz in -> 18 kHz alias %.1f dB, 30 kHz leakage %.1f dB\n", alias, through);
        check(alias < -80.0, "30 kHz @96k folds to 18 kHz below -80 dB");
        check(through < -80.0, "30 kHz above the capture's Nyquist is removed (< -80 dB)");
        for (double hz : {100.0, 1000.0, 5000.0, 10000.0, 16000.0}) {
            Rig pr(96000.0, 256, identity);
            const auto yp = pr.run(sine(96000.0, hz, 0.25, 48000));
            const double g = db(measure(yp, 96000.0, hz, 16384).amp / 0.25);
            std::printf("      96k passband %6.0f Hz: %+.3f dB\n", hz, g);
            check(std::abs(g) < (hz > 12000.0 ? 0.5 : 0.1), "96k passband flat at " + std::to_string(static_cast<int>(hz)) + " Hz");
        }
    }
    // ── 48 kHz host, 96 kHz capture: anti-imaging on the way back down ──
    {
        const std::string hi = writeLinearCapture("identity96.nam", 96000.0, 1.0f, 0.0f);
        Rig rig(48000.0, 128, hi);
        const auto y = rig.run(sine(48000.0, 1000.0, 0.25, 48000));
        const double g = db(measure(y, 48000.0, 1000.0, 16384).amp / 0.25);
        std::printf("      48k->96k->48k: 1 kHz %+.3f dB\n", g);
        check(std::abs(g) < 0.1, "48k host / 96k capture passband unity");
    }
    // ── Latency reporting and stream continuity at integer and fractional ratios ──
    for (double sr : {96000.0, 44100.0, 88200.0}) {
        for (uint32_t blk : {64u, 333u}) {
            Rig rig(sr, blk, identity);
            const double reported = static_cast<double>(rig.c->latencySamples());
            const double hz = 997.0;
            const auto x = sine(sr, hz, 0.25, static_cast<std::size_t>(sr));
            const auto y = rig.run(x);
            const Tone t = measure(y, sr, hz, 16384);
            double lag = -t.phase / (2.0 * kPi * hz) * sr; // samples
            while (lag < -0.5) lag += sr / hz;
            // Residual after removing the fitted tone: catches dropped/inserted samples and aliasing.
            double res = 0, ref = 0;
            const std::size_t n = 16384, start = y.size() - n;
            for (std::size_t i = start; i < y.size(); ++i) {
                const double ideal = t.amp * std::sin(2.0 * kPi * hz * static_cast<double>(i) / sr + t.phase);
                res += (y[i] - ideal) * (y[i] - ideal);
                ref += ideal * ideal;
            }
            const double snr = 10.0 * std::log10(ref / std::max(1e-30, res));
            std::printf("      %6.0f Hz host, block %3u: reported latency %.0f, measured %.2f samples, tone SNR %.1f dB\n", sr, blk, reported, lag, snr);
            check(std::abs(lag - reported) <= 1.0, "reported latency matches measured group delay at " + std::to_string(static_cast<int>(sr)));
            check(snr > 70.0, "bridged stream is continuous (no dropped/inserted samples) at " + std::to_string(static_cast<int>(sr)));
        }
    }
    // ── Real-time safety: the audio path never allocates ──
    {
        Rig rig(44100.0, 512, identity);
        g_audio_allocs = 0;
        (void)rig.run(sine(44100.0, 440.0, 0.3, 44100));
        check(g_audio_allocs == 0, "no heap allocations on the NAM audio path (" + std::to_string(g_audio_allocs) + ")");
    }
}

void testGate(const std::string& hiss) {
    // A capture whose output idles at -55 dBFS (the learned "hiss" floor) with silent DI.
    {
        Rig open(48000.0, 128, hiss);
        const auto y = open.run(std::vector<float>(48000, 0.0f));
        const double idle = db(std::abs(y.back()));
        std::printf("      capture idle floor with gate off: %.1f dBFS\n", idle);
        check(idle > -60.0, "synthetic capture produces model-side idle noise");
    }
    {
        Rig gated(48000.0, 128, hiss);
        gated.c->setParameter("gate", 0.5f);
        gated.c->processControlRate();
        const auto y = gated.run(std::vector<float>(48000, 0.0f));
        double peak = 0;
        for (std::size_t i = y.size() / 2; i < y.size(); ++i) peak = std::max(peak, static_cast<double>(std::abs(y[i])));
        std::printf("      capture idle floor with gate on: %.1f dBFS\n", db(peak));
        check(db(peak) < -110.0, "gate gain is applied after the model: learned idle noise is muted");
    }
    // Gate state machine: a slowly decaying note that wobbles ±2 dB around the threshold must close once,
    // without chatter, and release linearly in dB.
    {
        rig::amp::HystereticNoiseGate g;
        g.prepare(48000.0);
        g.setEnabled(true);
        g.setThresholdDb(-50.0f, 6.0f);
        g.reset();
        const double sr = 48000.0;
        int closes = 0, reopen = 0;
        bool was_open = false, closed_once = false;
        std::vector<float> gains;
        for (int i = 0; i < static_cast<int>(sr * 4); ++i) {
            const double t = i / sr;
            // -20 dBFS note decaying 15 dB/s with a 4 Hz ±2 dB wobble; crosses -50 dB around 2 s.
            const double level_db = -20.0 - 15.0 * t + 2.0 * std::sin(2 * kPi * 4.0 * t);
            const float x = static_cast<float>(std::pow(10.0, level_db / 20.0) * std::sin(2 * kPi * 196.0 * t));
            const float gain = g.detect(x);
            gains.push_back(gain);
            const bool open = gain > 0.5f;
            if (was_open && !open) { ++closes; closed_once = true; }
            if (!was_open && open && closed_once) ++reopen;
            was_open = open;
        }
        std::printf("      decaying wobbling note: %d close(s), %d re-open(s)\n", closes, reopen);
        check(closes == 1 && reopen == 0, "hysteretic gate closes exactly once on a wobbling decay (no chatter)");
        // dB-linear release: find the release segment and check the slope is constant.
        int s0 = -1, s1 = -1, s2 = -1, last_open = 0;
        for (int i = 0; i < static_cast<int>(gains.size()); ++i)
            if (gains[static_cast<std::size_t>(i)] >= 1.0f) last_open = i;
        for (int i = last_open; i < static_cast<int>(gains.size()); ++i) {
            const double d = db(gains[static_cast<std::size_t>(i)]);
            if (s0 < 0 && d < -6.0) s0 = i;
            if (s1 < 0 && d < -30.0) s1 = i;
            if (s2 < 0 && d < -54.0) s2 = i;
        }
        const bool found = s0 > 0 && s1 > s0 && s2 > s1;
        const double slope_a = found ? 24.0 / (s1 - s0) : 0, slope_b = found ? 24.0 / (s2 - s1) : 1;
        std::printf("      release slope: %.1f dB/s then %.1f dB/s\n", slope_a * sr, slope_b * sr);
        check(found && std::abs(slope_a / slope_b - 1.0) < 0.1, "gate release is linear in dB");
    }
}

void testCalibration(const std::string& identity) {
    // A capture made with a +12 dBu reamp send and +6 dBu output reference.
    const std::string cal = writeLinearCapture("cal12.nam", 48000.0, 1.0f, 0.0f,
                                               {{"input_level_dbu", 12.0}, {"output_level_dbu", 6.0}, {"loudness", -20.0}});
    Rig rig(48000.0, 128, cal);
    const auto m = rig.model();
    const double cin = m["calibration"]["input_db"].get<double>();
    const double cout = m["calibration"]["output_db"].get<double>();
    const double ref = m["calibration"]["headroom_full_scale_dbu"].get<double>();
    // Headroom: 1.0 digital full scale = 1 V peak (circuit::VoltageCalibration) → 0 dBFS sine = -0.79 dBu RMS.
    std::printf("      Headroom full-scale %.2f dBu; capture in %.1f dBu → input %+.2f dB, output %+.2f dB\n", ref, 12.0, cin, cout);
    check(std::abs(ref - (-0.79)) < 0.02, "Headroom full-scale reference derives from VoltageCalibration (1 V/FS)");
    check(std::abs(cin - (ref - 12.0)) < 0.01, "input calibration maps Headroom volts onto the capture's dBu reference");
    check(std::abs(cout - (6.0 - ref)) < 0.01, "output calibration restores the capture's output dBu reference");
    const auto y = rig.run(sine(48000.0, 1000.0, 0.1, 24000));
    const double g = db(measure(y, 48000.0, 1000.0, 8192).amp / 0.1);
    std::printf("      calibrated identity capture gain %+.2f dB (expected %+.2f)\n", g, cin + cout);
    check(std::abs(g - (cin + cout)) < 0.05, "calibrated gain applied end to end (normalize off)");
    // Without metadata the wrapper is unity.
    Rig plain(48000.0, 128, identity);
    const auto yp = plain.run(sine(48000.0, 1000.0, 0.1, 24000));
    check(std::abs(db(measure(yp, 48000.0, 1000.0, 8192).amp / 0.1)) < 0.05, "uncalibrated capture stays at unity");

    // SlimmableModel: the shipped slimmable container exposes the size control.
    const fs::path slim_src = fs::path(RIG_NAM_EXAMPLES) / "slimmable_container.nam";
    if (fs::exists(slim_src)) {
        Rig s(48000.0, 128, slim_src.string());
        auto sm = s.model();
        check(sm["slimmable"].get<bool>(), "slimmable capture reports SlimmableModel support");
        const auto full = s.run(sine(48000.0, 220.0, 0.2, 9600));
        s.c->setParameter("model_size", 0.0f);
        s.c->processControlRate();
        sm = s.model();
        std::printf("      slim ratio after model_size=0: %.2f\n", sm["slim_ratio"].get<double>());
        check(sm["slim_ratio"].get<double>() < 0.2, "model_size selector reaches the smallest sub-model");
        const auto small = s.run(sine(48000.0, 220.0, 0.2, 9600));
        double diff = 0;
        for (std::size_t i = 4800; i < full.size(); ++i) diff += std::abs(full[i] - small[i]);
        check(diff > 1e-3, "smaller slimmable size changes the rendered output");
    }
}

} // namespace

int main() {
    // Keep the wrapper's capture library scan away from the user's real folder.
    const fs::path home = tempDir() / "home";
    fs::create_directories(home);
    rig_test::setEnv("HOME", home.string().c_str());
#ifndef __APPLE__
    // Off macOS the data folder isn't derived from HOME alone (XDG_DATA_HOME, %APPDATA%): pin it too.
    rig_test::setEnv("HEADROOM_DATA_DIR", (home / "Headroom").string().c_str());
#endif
    const std::string identity = writeLinearCapture("identity48.nam", 48000.0, 1.0f, 0.0f);
    const std::string hiss = writeLinearCapture("hiss48.nam", 48000.0, 1.0f, 0.00178f); // -55 dBFS idle floor
    testRateBridge(identity);
    testGate(hiss);
    testCalibration(identity);
    if (g_failures) std::printf("%d check(s) failed\n", g_failures);
    return g_failures ? 1 : 0;
}
