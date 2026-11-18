// DSP quality regressions for the drive-pedal dry path, the triode coupling-capacitor grid state,
// power-amp ↔ speaker-impedance coupling, and the partitioned convolver.
#undef NDEBUG
#include "platform/Environment.hpp"
#include "../src/core/IComponent.hpp"
#include "../src/core/RackGraph.hpp"
#include "../src/dsp/ComponentModel.hpp"
#include "../src/preset/PresetLoader.hpp"
#include "json.hpp"
#include <filesystem>
#include <fstream>
#include "../src/dsp/AmpModeling.hpp"
#include "../src/dsp/ConvolutionEngine.hpp"
#include "../src/dsp/circuit/TubeDevice.hpp"
#include "../src/platform/VDsp.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <numbers>
#include <random>
#include <string>
#include <vector>

namespace {
constexpr double kPi = std::numbers::pi;
int g_failures = 0;
void check(bool ok, const std::string& what) {
    std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what.c_str());
    if (!ok) ++g_failures;
}
double db(double x) { return 20.0 * std::log10(std::max(1e-15, x)); }

// Complex response at `hz` from the last `n` samples (Hann-windowed single-bin DFT, sine input).
std::complex<double> tone(const std::vector<float>& y, double sr, double hz, std::size_t n) {
    const std::size_t start = y.size() - n;
    std::complex<double> acc{};
    double wsum = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2 * kPi * static_cast<double>(i) / static_cast<double>(n - 1));
        const double ph = 2 * kPi * hz * static_cast<double>(start + i) / sr;
        acc += w * static_cast<double>(y[start + i]) * std::complex<double>(std::sin(ph), std::cos(ph));
        wsum += w;
    }
    return acc * (2.0 / wsum);
}

std::vector<float> render(rig::IComponent& c, const std::vector<float>& x, double sr, uint32_t block = 128) {
    std::vector<float> y(x.size()), l(block), r(block), in(block);
    const float* ins[2] = {in.data(), in.data()};
    float* outs[2] = {l.data(), r.data()};
    for (std::size_t p = 0; p < x.size(); p += block) {
        const uint32_t n = static_cast<uint32_t>(std::min<std::size_t>(block, x.size() - p));
        std::copy(x.begin() + static_cast<std::ptrdiff_t>(p), x.begin() + static_cast<std::ptrdiff_t>(p + n), in.begin());
        rig::AudioBlock b{};
        b.in = ins; b.out = outs; b.num_channels = 2; b.num_frames = n; b.sample_rate = sr;
        c.processAudio(b);
        std::copy(l.begin(), l.begin() + n, y.begin() + static_cast<std::ptrdiff_t>(p));
    }
    return y;
}

// ── Item 4: drive pedal Mix — the dry path must be phase/delay aligned with the oversampled wet path ──
void testPedalMix() {
    for (double sr : {44100.0, 48000.0, 96000.0}) {
        auto response = [&](float mix, double hz) {
            auto c = rig::createComponentByTableId(173000); // Character saturator with a Mix knob
            assert(c);
            c->prepare(sr, 128);
            c->setParameter("drive", 0.0f);
            c->setParameter("out", 0.5f);
            c->setParameter("mix", mix);
            c->processControlRate();
            c->reset();
            std::vector<float> x(static_cast<std::size_t>(sr * 0.25));
            for (std::size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(0.002 * std::sin(2 * kPi * hz * static_cast<double>(i) / sr));
            return tone(render(*c, x, sr), sr, hz, 4096) / 0.002;
        };
        // Reference: the pedal's own half-band oversampler round trip (what the wet signal rides through).
        auto osRoundTrip = [&](double hz) {
            rig::amp::Oversampler os;
            os.prepare(sr >= 88000.0 ? 2 : 4);
            os.reset();
            std::vector<float> y(static_cast<std::size_t>(sr * 0.25));
            float up[8];
            for (std::size_t i = 0; i < y.size(); ++i) {
                os.up(0, static_cast<float>(0.002 * std::sin(2 * kPi * hz * static_cast<double>(i) / sr)), up);
                y[i] = os.down(0, up);
            }
            return tone(y, sr, hz, 4096) / 0.002;
        };
        double worst_dip = 0, worst_hz = 0, worst_align_db = 0, worst_align_deg = 0;
        for (double hz = 100.0; hz < std::min(18000.0, 0.42 * sr); hz *= 1.12) {
            const auto dry = response(0.0f, hz), wet = response(1.0f, hz), half = response(0.5f, hz);
            const auto ref = osRoundTrip(hz);
            if (std::getenv("DSPQ_DEBUG"))
                std::printf("        %7.0f Hz dry %.2f dB %.1f deg | os %.1f deg | wet %.2f dB | half %.2f dB\n", hz, db(std::abs(dry)),
                            std::arg(dry) * 180 / kPi, std::arg(ref) * 180 / kPi, db(std::abs(wet)), db(std::abs(half)));
            worst_align_db = std::max(worst_align_db, std::abs(db(std::abs(dry / ref))));
            worst_align_deg = std::max(worst_align_deg, std::abs(std::arg(dry / ref)) * 180 / kPi);
            // A comb notch drops the blend far below either branch's own contribution.
            const double dip = db(std::max(0.5 * std::abs(dry), 0.5 * std::abs(wet)) / std::abs(half));
            if (dip > worst_dip) { worst_dip = dip; worst_hz = hz; }
        }
        std::printf("      %6.0f Hz: dry vs oversampler round trip %.3f dB / %.2f deg; mix 0.5 worst dip below a branch %.2f dB at %.0f Hz\n",
                    sr, worst_align_db, worst_align_deg, worst_dip, worst_hz);
        check(worst_align_db < 0.02 && worst_align_deg < 0.5, "pedal dry path is delay/phase matched to the oversampled wet path at " + std::to_string(static_cast<int>(sr)));
        check(worst_dip < 2.0, "pedal mix 0.5 has no comb notches at " + std::to_string(static_cast<int>(sr)));
    }
}


// ── Item 5: triode coupling-capacitor / grid RC state and the shared grid-current law ──
void testTriodeCoupling() {
    using rig::amp::TriodeStage;
    using rig::amp::StageSpec;
    // Grid law: TubeDevice (circuit reference) and CathodeTables (fast stages) are the same function.
    double worst_dev = 0, worst_tab = 0;
    const auto& T = rig::amp::CathodeTables::get();
    for (double v = -1.0; v <= 6.0; v += 0.01) {
        const double law = rig::circuit::GridLaw::current(v);
        worst_dev = std::max(worst_dev, std::abs(rig::circuit::TubeDevice::grid(v) - law) / std::max(1e-7, law));
    }
    for (double d = 0.2; d <= 40.0; d *= 1.1) {
        const double e = T.grid(static_cast<float>(d));
        const double ig = T.gridCurrent(static_cast<float>(d));
        worst_tab = std::max(worst_tab, std::abs(ig - rig::circuit::GridLaw::current(e)) / std::max(1e-6, ig));
    }
    std::printf("      grid law: TubeDevice vs GridLaw max rel err %.2e; fast-stage table vs GridLaw %.2e\n", worst_dev, worst_tab);
    check(worst_dev < 1e-3, "circuit::TubeDevice uses the shared 12AX7 grid-current law");
    check(worst_tab < 0.03, "fast TriodeStage tables use the same grid-current law");

    const double sr = 192000.0;
    auto spec = [](float gain, float hp, float bias, float lim_pos = 1.0f, float lim_neg = 1.6f, float blocking = 0.35f) {
        StageSpec s;
        s.gain = gain; s.hp_hz = hp; s.lp_hz = 20000.0f; s.bias = bias;
        s.lim_pos = lim_pos; s.lim_neg = lim_neg; s.blocking = blocking;
        return s;
    };
    struct Case { const char* name; StageSpec s; };
    const Case cases[] = {{"triode (dynamic)", spec(1.0f, 100.0f, -0.05f)},
                          {"cold clipper", spec(1.0f, 100.0f, -0.5f, 1.0f, 1.3f, 0.45f)},
                          {"cathode follower", spec(1.0f, 100.0f, 0.0f, 0.85f, 2.6f, 0.15f)}};
    // Small-signal: the RC node is the stage's coupling high-pass (−3 dB at hp_hz).
    for (const auto& c : cases) {
        auto gainAt = [&](double hz) {
            TriodeStage st;
            st.prepare(sr, c.s);
            std::vector<float> y(static_cast<std::size_t>(sr * 0.5));
            for (std::size_t i = 0; i < y.size(); ++i) y[i] = st.process(static_cast<float>(0.01 * std::sin(2 * kPi * hz * static_cast<double>(i) / sr)));
            return std::abs(tone(y, sr, hz, 65536)) / 0.01;
        };
        const double rel = db(gainAt(100.0) / gainAt(2000.0));
        std::printf("      %-17s linear corner: %.2f dB at hp_hz\n", c.name, rel);
        check(std::abs(rel + 3.01) < 0.5, std::string(c.name) + ": coupling RC gives the voicing's −3 dB corner");
    }
    // Blocking: a hard burst charges Cc through the conducting grid; the bias then recovers with τ = Rg·Cc.
    for (const auto& base : cases) {
        StageSpec s = base.s;
        s.gain = 9.0f;
        s.hp_hz = 30.0f;
        TriodeStage st;
        st.prepare(sr, s);
        const double tau = 1.0 / (2 * kPi * 30.0);
        float first_y = 0, last_y = 0;
        const int burst = static_cast<int>(sr * 0.1), cyc = static_cast<int>(sr / 200.0);
        double v0 = 0;
        for (int i = 0; i < burst; ++i) {
            const float y = st.process(static_cast<float>(1.0 * std::sin(2 * kPi * 200.0 * i / sr)));
            if (i < cyc) first_y = std::max(first_y, std::abs(y));
            if (i >= burst - cyc) { last_y = std::max(last_y, std::abs(y)); v0 += st.couplingVolts() / cyc; } // DC of Cc over the last cycle
        }
        // Recovery: exponential decay → τ from the ratio between 2τ and 4τ after the burst.
        double v2 = 0, v4 = 0, v_late = 0;
        for (int i = 1; i <= static_cast<int>(sr * 20 * tau); ++i) {
            st.process(0.0f);
            if (i == static_cast<int>(sr * 2 * tau)) v2 = st.couplingVolts();
            if (i == static_cast<int>(sr * 4 * tau)) v4 = st.couplingVolts();
            v_late = st.couplingVolts();
        }
        const double tau_ms = (v2 > 0 && v4 > 0 && v2 > v4) ? 1000.0 * 2 * tau / std::log(v2 / v4) : 0.0;
        std::printf("      %-17s blocking bias (cycle-average) %.2f V; recovery τ %.2f ms (Rg·Cc %.2f ms); after 20τ %.4f V; "
                    "peak output first→last cycle %.2f → %.2f\n", base.name, v0, tau_ms, 1000 * tau, v_late, first_y, last_y);
        check(v0 > 0.05, std::string(base.name) + ": hard transient charges the coupling cap (blocking bias)");
        check(std::abs(tau_ms / (1000 * tau) - 1.0) < 0.1, std::string(base.name) + ": bias recovers with τ ≈ Rg·Cc");
        check(std::abs(v_late) < 0.01 * v0 + 1e-4, std::string(base.name) + ": bias fully recovers");
    }
}


// ── Item 6: power amp ↔ speaker impedance coupling, and the cabinet → amp link ──
double powerAmpGainDb(rig::amp::PowerSpec s, double hz, float fs = 0.0f, float le = 1.0f) {
    rig::amp::PowerAmp pa;
    const double sr = 192000.0;
    pa.prepare(sr, s);
    if (fs > 0.0f || le != 1.0f) pa.setSpeakerLoadProfile(fs, 1.4f, le);
    std::vector<float> y(static_cast<std::size_t>(sr * 0.6));
    for (std::size_t i = 0; i < y.size(); ++i) y[i] = pa.process(static_cast<float>(0.002 * std::sin(2 * kPi * hz * static_cast<double>(i) / sr)));
    return db(std::abs(tone(y, sr, hz, 65536)) / 0.002);
}

void testSpeakerCoupling() {
    using rig::amp::PowerSpec;
    auto resonance = [](float damping, bool no_nfb) {
        PowerSpec s;
        s.damping = damping; s.spk_fs = 90.0f; s.class_a = no_nfb;
        return powerAmpGainDb(s, 90.0) - powerAmpGainDb(s, 400.0);
    };
    const double loose = resonance(0.15f, false), stiff = resonance(0.95f, false);
    const double open_loop = resonance(0.52f, true), closed_loop = resonance(0.52f, false);
    std::printf("      speaker resonance at fs (re 400 Hz): damping 0.15 %+.1f dB, 0.95 %+.1f dB; damping 0.52 without NFB %+.1f dB, with NFB %+.1f dB\n",
                loose, stiff, open_loop, closed_loop);
    check(loose - stiff > 3.0, "speaker resonance follows the output stage's source impedance (damping)");
    check(open_loop - closed_loop > 0.3, "global NFB taken across the speaker load damps the resonance");
    PowerSpec s;
    s.damping = 0.52f; s.spk_fs = 90.0f;
    double best = -1e9, best_hz = 0;
    for (double hz = 70.0; hz < 200.0; hz *= 1.03) {
        const double g = powerAmpGainDb(s, hz, 130.0f);
        if (g > best) { best = g; best_hz = hz; }
    }
    const double hf_lo = powerAmpGainDb(s, 4000.0, 0.0f, 0.5f) - powerAmpGainDb(s, 1000.0, 0.0f, 0.5f);
    const double hf_hi = powerAmpGainDb(s, 4000.0, 0.0f, 2.0f) - powerAmpGainDb(s, 1000.0, 0.0f, 2.0f);
    std::printf("      cabinet fs 130 Hz → peak at %.0f Hz; voice-coil inductance rise 1k→4k: Le×0.5 %+.1f dB, Le×2 %+.1f dB\n", best_hz, hf_lo, hf_hi);
    check(best_hz > 110.0 && best_hz < 150.0, "a cabinet's resonance moves the power amp's load resonance");
    check(hf_hi - hf_lo > 2.0, "voice-coil inductance raises the load's high-frequency impedance");

    // Graph: JCM800 → Amp-Matched Cab 156000 (a 4x12 whose profile differs from the amp's own 88 Hz speaker).
    // The rack must hand the cabinet's profile to the amp's power stage.
    namespace fs = std::filesystem;
    const fs::path rig_path = fs::temp_directory_path() / "headroom_dsp_quality_amp_cab.rig";
    std::ofstream(rig_path) << R"(<?xml version="1.0" encoding="UTF-8"?>
<headroom-preset version="1"><headroom-info><soundinfo><properties><name>t</name></properties></soundinfo></headroom-info>
<headroom-rig><rack><non-fix-components>
<component id="56000"><component-audio version="1"><parameters num-parameters="1"><parameter id="Pwr" value="1"/></parameters></component-audio></component>
<component id="156000"><component-audio version="1"><parameters num-parameters="2"><parameter id="Pwr" value="1"/><parameter id="Cab" value="20"/></parameters></component-audio></component>
</non-fix-components></rack></headroom-rig></headroom-preset>)";
    rig::preset::RackPreset preset;
    std::string err;
    const bool loaded = rig::preset::loadPreset(rig_path.string(), preset, &err);
    check(loaded, "amp → cabinet test rig loads " + err);
    if (!loaded) return;
    // 56000 runs on the circuit engine (its speaker load is in its netlist); this check covers the cab -> power-amp
    // impedance coupling of the behavioural amp path that unconverted amps still use.
    rig::platform::setEnv("HEADROOM_LEGACY_MODELS", "1");
    rig::RackGraph graph;
    graph.buildFromPreset(preset, 48000.0, 256);
    rig::platform::unsetEnv("HEADROOM_LEGACY_MODELS");
    rig::IComponent* amp = nullptr;
    rig::IComponent* cab = nullptr;
    for (const auto& v : graph.activeNodeViews()) {
        if (v.table_id == 56000) amp = v.component;
        if (v.table_id == 156000) cab = v.component;
    }
    check(amp && cab, "graph contains the amp and the cabinet");
    if (!amp || !cab) return;
    std::vector<float> in(256, 0.0f), l(256), r(256);
    const float* ins[2] = {in.data(), in.data()};
    float* outs[2] = {l.data(), r.data()};
    auto run = [&] {
        for (int b = 0; b < 4; ++b) {
            rig::AudioBlock blk{};
            blk.in = ins; blk.out = outs; blk.num_channels = 2; blk.num_frames = 256; blk.sample_rate = 48000.0;
            graph.process(blk);
        }
    };
    auto load = [&] {
        return nlohmann::json::parse(dynamic_cast<rig::model::Introspectable*>(amp)->modelJson())["speaker_load"];
    };
    run();
    const auto cab_profile = cab->speakerImpedanceProfile();
    auto l1 = load();
    std::printf("      cabinet profile fs %.0f Hz Q %.2f Le×%.2f → amp load %s fs %.0f Hz\n", cab_profile.fs_hz, cab_profile.q_res, cab_profile.le_scale,
                l1["from_cabinet"].get<bool>() ? "from cabinet" : "voicing default", l1["fs_hz"].get<double>());
    check(cab_profile.valid && cab_profile.fs_hz != 88.0f, "matched 4x12 cabinet reports its own impedance profile");
    check(l1["from_cabinet"].get<bool>() && std::abs(l1["fs_hz"].get<double>() - cab_profile.fs_hz) < 0.5,
          "the rack couples the downstream cabinet's impedance into the amp's power stage");
    cab->setBypassed(true);
    run();
    check(!load()["from_cabinet"].get<bool>(), "bypassing the cabinet returns the amp to its own voicing load");
}


// ── Item 7: two-stage (128 head/early + 1024 tail) partitioned convolver ──
// Reference: classic uniform 128-sample partitioned overlap-save (what a single-stage engine costs).
class UniformConvolver {
public:
    explicit UniformConvolver(const std::vector<float>& ir) {
        parts_ = static_cast<int>((ir.size() + P - 1) / P);
        hr_.assign(static_cast<std::size_t>(parts_), {}); hi_.assign(static_cast<std::size_t>(parts_), {});
        xr_.assign(static_cast<std::size_t>(parts_), {}); xi_.assign(static_cast<std::size_t>(parts_), {});
        for (int j = 0; j < parts_; ++j) {
            std::array<float, 2 * P> buf{};
            for (int k = 0; k < P && static_cast<std::size_t>(j * P + k) < ir.size(); ++k) buf[static_cast<std::size_t>(k)] = ir[static_cast<std::size_t>(j * P + k)];
            DSPSplitComplex sc{hr_[static_cast<std::size_t>(j)].data(), hi_[static_cast<std::size_t>(j)].data()};
            vDSP_ctoz(reinterpret_cast<const DSPComplex*>(buf.data()), 2, &sc, 1, P);
            vDSP_fft_zrip(rig::dsp::sharedConvFFT(), &sc, 1, rig::dsp::kConvLog2N, kFFTDirection_Forward);
            for (int k = 0; k < P; ++k) { hr_[static_cast<std::size_t>(j)][static_cast<std::size_t>(k)] /= 4.0f * 2 * P; hi_[static_cast<std::size_t>(j)][static_cast<std::size_t>(k)] /= 4.0f * 2 * P; }
        }
    }
    // Processes one P-sample block (a real-time host would add one block of latency to gather it).
    void block(const float* in, float* out) {
        std::array<float, 2 * P> buf{};
        std::copy(prev_.begin(), prev_.end(), buf.begin());
        std::copy(in, in + P, buf.begin() + P);
        std::copy(in, in + P, prev_.begin());
        pos_ = (pos_ + parts_ - 1) % parts_;
        DSPSplitComplex sc{xr_[static_cast<std::size_t>(pos_)].data(), xi_[static_cast<std::size_t>(pos_)].data()};
        vDSP_ctoz(reinterpret_cast<const DSPComplex*>(buf.data()), 2, &sc, 1, P);
        vDSP_fft_zrip(rig::dsp::sharedConvFFT(), &sc, 1, rig::dsp::kConvLog2N, kFFTDirection_Forward);
        std::array<float, P> ar{}, ai{};
        DSPSplitComplex acc{ar.data(), ai.data()};
        float dc = 0, ny = 0;
        for (int j = 0; j < parts_; ++j) {
            auto& xr = xr_[static_cast<std::size_t>((pos_ + j) % parts_)];
            auto& xi = xi_[static_cast<std::size_t>((pos_ + j) % parts_)];
            dc += xr[0] * hr_[static_cast<std::size_t>(j)][0];
            ny += xi[0] * hi_[static_cast<std::size_t>(j)][0];
            DSPSplitComplex X{xr.data(), xi.data()}, H{hr_[static_cast<std::size_t>(j)].data(), hi_[static_cast<std::size_t>(j)].data()};
            vDSP_zvma(&X, 1, &H, 1, &acc, 1, &acc, 1, P);
        }
        ar[0] = dc; ai[0] = ny;
        vDSP_fft_zrip(rig::dsp::sharedConvFFT(), &acc, 1, rig::dsp::kConvLog2N, kFFTDirection_Inverse);
        std::array<float, 2 * P> o{};
        vDSP_ztoc(&acc, 1, reinterpret_cast<DSPComplex*>(o.data()), 2, P);
        std::copy(o.begin() + P, o.end(), out);
    }
private:
    static constexpr int P = rig::dsp::kConvPart;
    int parts_ = 0, pos_ = 0;
    std::array<float, P> prev_{};
    std::vector<std::array<float, P>> hr_, hi_, xr_, xi_;
};

void testConvolver() {
    std::mt19937 rng(7);
    std::normal_distribution<float> nd(0.0f, 1.0f);
    // Exactness and latency against direct convolution on a 6000-tap IR (exercises head, early and tail stages).
    {
        std::vector<float> ir(6000);
        for (std::size_t i = 0; i < ir.size(); ++i) ir[i] = nd(rng) * std::exp(-static_cast<float>(i) / 1500.0f);
        rig::dsp::PartitionedIR pir;
        pir.build(ir);
        rig::dsp::PartitionedConvolver conv(static_cast<int>(ir.size() / rig::dsp::kConvPart) + 1);
        std::vector<float> x(20000), y(x.size()), ref(x.size(), 0.0f);
        for (auto& v : x) v = nd(rng) * 0.1f;
        for (std::size_t n = 0; n < x.size(); ++n) y[n] = conv.process(x[n], pir);
        for (std::size_t n = 0; n < x.size(); ++n) {
            double acc = 0;
            for (std::size_t k = 0; k < ir.size() && k <= n; ++k) acc += static_cast<double>(ir[k]) * x[n - k];
            ref[n] = static_cast<float>(acc);
        }
        double err = 0, sig = 0;
        for (std::size_t n = 0; n < x.size(); ++n) { err += (y[n] - ref[n]) * (y[n] - ref[n]); sig += ref[n] * ref[n]; }
        rig::dsp::PartitionedConvolver imp(static_cast<int>(ir.size() / rig::dsp::kConvPart) + 1);
        double first = imp.process(1.0f, pir);
        const double rel = 10 * std::log10(err / sig);
        std::printf("      two-stage convolver vs direct: error %.1f dB; impulse → y[0] = %.4f (ir[0] = %.4f)\n", rel, first, ir[0]);
        check(rel < -100.0, "two-stage convolver matches direct convolution (< -100 dB)");
        check(std::abs(first - ir[0]) < 1e-6, "two-stage convolver adds zero latency");
        // Retarget mid-stream to a second IR: output must continue as the new IR's convolution (no glitch).
        std::vector<float> ir2(ir.size());
        for (std::size_t i = 0; i < ir2.size(); ++i) ir2[i] = nd(rng) * std::exp(-static_cast<float>(i) / 900.0f);
        rig::dsp::PartitionedIR pir2;
        pir2.build(ir2);
        rig::dsp::PartitionedConvolver c2(static_cast<int>(ir.size() / rig::dsp::kConvPart) + 1);
        const std::size_t sw = 9000 + 37;
        double e2 = 0, s2 = 0;
        for (std::size_t n = 0; n < x.size(); ++n) {
            if (n == sw) c2.retarget(pir2);
            const float out = c2.process(x[n], n < sw ? pir : pir2);
            if (n >= sw) {
                double acc = 0;
                for (std::size_t k = 0; k < ir2.size() && k <= n; ++k) acc += static_cast<double>(ir2[k]) * x[n - k];
                e2 += (out - acc) * (out - acc); s2 += acc * acc;
            }
        }
        std::printf("      retarget mid-block: post-switch error vs new-IR convolution %.1f dB\n", 10 * std::log10(e2 / s2));
        check(10 * std::log10(e2 / s2) < -100.0, "retarget switches to the new IR exactly, mid-block");
    }
    // CPU on a long reverb IR (3.2 s at 48 kHz = Studio IR Reverb's limit) vs uniform 128-sample partitions.
    {
        const std::size_t taps = static_cast<std::size_t>(48000 * 3.2);
        std::vector<float> ir(taps);
        for (std::size_t i = 0; i < taps; ++i) ir[i] = nd(rng) * std::exp(-static_cast<float>(i) / 30000.0f);
        rig::dsp::PartitionedIR pir;
        pir.build(ir, taps);
        rig::dsp::PartitionedConvolver conv(static_cast<int>(taps / rig::dsp::kConvPart) + 1);
        UniformConvolver uni(ir);
        const std::size_t n = 48000 * 4;
        std::vector<float> x(n), y(n), yu(n);
        for (auto& v : x) v = nd(rng) * 0.1f;
        auto t0 = std::chrono::steady_clock::now();
        double worst_block_us = 0;
        for (std::size_t b = 0; b < n; b += 128) {
            const auto tb = std::chrono::steady_clock::now();
            for (std::size_t i = b; i < b + 128; ++i) y[i] = conv.process(x[i], pir);
            worst_block_us = std::max(worst_block_us, std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - tb).count());
        }
        auto t1 = std::chrono::steady_clock::now();
        for (std::size_t b = 0; b < n; b += 128) uni.block(&x[b], &yu[b]);
        auto t2 = std::chrono::steady_clock::now();
        const double two = std::chrono::duration<double>(t1 - t0).count(), one = std::chrono::duration<double>(t2 - t1).count();
        // Same result (the uniform engine's output for a block is aligned with that block's input).
        double err = 0, sig = 0;
        for (std::size_t i = taps; i < n; ++i) { err += (y[i] - yu[i]) * (y[i] - yu[i]); sig += y[i] * y[i]; }
        std::printf("      3.2 s IR, 4 s of audio: two-stage %.1f ms (%.2f %% of real time, worst 128-block %.0f us), uniform-128 %.1f ms → %.1fx less CPU; agreement %.1f dB\n",
                    two * 1e3, two / 4.0 * 100, worst_block_us, one * 1e3, one / two, 10 * std::log10(err / sig));
        check(one / two > 3.0, "two-stage convolver uses far less CPU than uniform partitions on long IRs");
        check(10 * std::log10(err / sig) < -90.0, "two-stage and uniform engines agree on the long IR");
        check(worst_block_us < 2900.0, "worst 128-sample block stays within its 2.67 ms real-time budget");
    }
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    testPedalMix();
    testTriodeCoupling();
    testSpeakerCoupling();
    testConvolver();
    if (g_failures) std::printf("%d check(s) failed\n", g_failures);
    return g_failures ? 1 : 0;
}
