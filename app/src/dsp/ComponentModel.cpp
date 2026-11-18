// Component model export, offline analysis renders and user components. See ComponentModel.hpp.
#include "ComponentModel.hpp"
#include "AmpModeling.hpp"
#include "ConvolutionEngine.hpp"
#include "NamComponent.hpp"
#include "TubeAmpComponents.hpp"
#include "../library/Library.hpp"
#include "../midi/MidiComponents.hpp"
#include "../midi/MidiEvent.hpp"
#include "../core/SmoothingToolbox.hpp"
#include "json.hpp"
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <numbers>
#include <random>

namespace rig::model {

namespace {

using nlohmann::json;
namespace fs = std::filesystem;

json parseObj(const std::string& s) {
    if (s.empty()) return json::object();
    try {
        json j = json::parse(s);
        return j.is_object() ? j : json::object();
    } catch (...) {
        return json::object();
    }
}

std::string num(double v, int prec) {
    char b[48];
    std::snprintf(b, sizeof b, "%.*f", prec, v);
    return b;
}
std::string hz(double f) { return f >= 1000.0 ? num(f / 1000.0, f >= 10000.0 ? 1 : 2) + " kHz" : num(f, f < 10.0 ? 1 : 0) + " Hz"; }
std::string db(double d) { return (d > 0.05 ? "+" : "") + num(d, 1) + " dB"; }
std::string ms(double t) { return num(t * 1000.0, t < 0.01 ? 1 : 0) + " ms"; }
std::string pct(double v) { return num(v * 100.0, 0) + " %"; }
double toDb(double g) { return 20.0 * std::log10(std::max(1e-9, std::abs(g))); }
double r2(double v) { return std::round(v * 100.0) / 100.0; }

json val(const std::string& k, const std::string& v, const std::string& note = {}) {
    json o = {{"k", k}, {"v", v}};
    if (!note.empty()) o["note"] = note;
    return o;
}
json blk(const std::string& id, const std::string& type, const std::string& label, const std::string& sub, json values, const std::string& section) {
    return {{"id", id}, {"type", type}, {"label", label}, {"sub", sub}, {"values", std::move(values)}, {"section", section}};
}

// Same knob → raw mapping RackGraph::setComponentNormalizedParameter uses.
void setNormalized(IComponent& c, const std::string& id, float norm) {
    norm = std::clamp(norm, 0.0f, 1.0f);
    float raw = norm;
    for (const auto& ep : c.extraParams())
        if (ep.id == id && !ep.labels.empty()) raw = std::round(norm * static_cast<float>(ep.labels.size() - 1));
    for (const auto& p : c.schema().params) {
        if (p.id != id) continue;
        float mn = 0.0f, mx = 1.0f;
        getParamRangeFor(c.tableId(), p, mn, mx);
        raw = mn + norm * (mx - mn);
        if (mx - mn > 1.5f && mx - mn <= 64.0f && std::floor(mn) == mn && std::floor(mx) == mx) raw = std::round(raw);
        if (p.kind == catalog::ParamKind::Int) raw = std::round(raw);
        else if (p.kind == catalog::ParamKind::Bool) raw = norm >= 0.5f ? 1.0f : 0.0f;
        break;
    }
    c.setParameter(id, raw);
}

// A private, fully prepared instance (never the live graph).
std::unique_ptr<IComponent> makeInstance(int32_t tid, const json& params, const json& norms, const std::string& model, double sr) {
    auto c = createComponentByTableId(tid);
    if (!c) return nullptr;
    c->setStringParameter(kAnalysisParam, "1");
    c->prepare(sr, 512);
    for (const auto& [k, v] : params.items()) {
        if (k == kModelParam || k == kAnalysisParam) continue;
        if (v.is_number()) c->setParameter(k, v.get<float>());
        else if (v.is_string()) c->setStringParameter(k, v.get<std::string>());
    }
    for (const auto& [k, v] : norms.items())
        if (v.is_number()) setNormalized(*c, k, v.get<float>());
    if (!model.empty()) c->setStringParameter(kModelParam, model);
    c->processControlRate();
    c->reset();
    c->processControlRate();
    return c;
}

// Runs `in` (mono, both channels) through a unit; returns the left output.
std::vector<float> render(IComponent& c, const std::vector<float>& in, double sr, std::vector<float>* right = nullptr) {
    std::vector<float> outL(in.size()), outR(in.size());
    std::vector<midi::MidiEvent> midi_bus;
    for (std::size_t off = 0; off < in.size(); off += 512) {
        const uint32_t n = static_cast<uint32_t>(std::min<std::size_t>(512, in.size() - off));
        const float* ins[2] = {in.data() + off, in.data() + off};
        float* outs[2] = {outL.data() + off, outR.data() + off};
        AudioBlock b{};
        b.in = ins;
        b.out = outs;
        b.num_channels = 2;
        b.num_frames = n;
        b.sample_rate = sr;
        b.midi = &midi_bus;
        midi_bus.clear();
        c.processControlRate();
        c.processAudio(b);
    }
    if (right) *right = std::move(outR);
    return outL;
}

// Sine through the whole unit: harmonic levels, THD, one output period and the dynamic transfer (x, y).
json harmonics(IComponent& c, double sr, float level, float f0_req) {
    constexpr std::size_t N = 16384;
    const double bin = std::max(1.0, std::round(f0_req * N / sr));
    const double f0 = bin * sr / N;
    const std::size_t pre = static_cast<std::size_t>(sr * 0.45);
    std::vector<float> x(pre + N);
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double ramp = std::min(1.0, static_cast<double>(i) / (sr * 0.05));
        x[i] = static_cast<float>(level * ramp * std::sin(2.0 * std::numbers::pi * f0 * static_cast<double>(i) / sr));
    }
    c.reset();
    const auto y = render(c, x, sr);
    auto amp = [&](double k) {
        double re = 0.0, im = 0.0;
        const double w = 2.0 * std::numbers::pi * k * bin / N;
        for (std::size_t i = 0; i < N; ++i) {
            re += y[pre + i] * std::cos(w * static_cast<double>(i));
            im -= y[pre + i] * std::sin(w * static_cast<double>(i));
        }
        return 2.0 * std::sqrt(re * re + im * im) / N;
    };
    const double a1 = amp(1);
    json h = json::array();
    double sum = 0.0;
    const int kmax = std::min(40, static_cast<int>(0.45 * sr / f0));
    for (int k = 1; k <= kmax; ++k) {
        const double a = k == 1 ? a1 : amp(k);
        if (k > 1) sum += a * a;
        if (k <= 24) h.push_back(r2(toDb(a / std::max(1e-12, a1))));
    }
    double rms = 0.0, peak = 1e-9;
    for (std::size_t i = 0; i < N; ++i) {
        rms += static_cast<double>(y[pre + i]) * y[pre + i];
        peak = std::max(peak, static_cast<double>(std::abs(y[pre + i])));
    }
    rms = std::sqrt(rms / N);
    // One period of the output (and the matching input) for the waveform / transfer plots.
    const double per = sr / f0;
    json wx = json::array(), wy = json::array();
    const std::size_t start = pre + N / 2;
    for (int i = 0; i <= 128; ++i) {
        const double pos = static_cast<double>(start) + per * i / 128.0;
        const auto i0 = static_cast<std::size_t>(pos);
        const double fr = pos - static_cast<double>(i0);
        const double yi = y[i0] + fr * (y[std::min(i0 + 1, y.size() - 1)] - y[i0]);
        const double xi = x[i0] + fr * (x[std::min(i0 + 1, x.size() - 1)] - x[i0]);
        wx.push_back(std::round(xi / level * 1000.0) / 1000.0);
        wy.push_back(std::round(yi / peak * 1000.0) / 1000.0);
    }
    return {{"f0", f0}, {"level", level}, {"level_dbfs", toDb(level)}, {"h", h}, {"thd_pct", 100.0 * std::sqrt(sum) / std::max(1e-12, a1)},
            {"out_rms_db", toDb(rms)}, {"out_peak_db", toDb(peak)}, {"wave_in", wx}, {"wave_out", wy}};
}

std::vector<double> logFreqs(int n = 96) {
    std::vector<double> f(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) f[static_cast<std::size_t>(i)] = 20.0 * std::pow(1000.0, static_cast<double>(i) / (n - 1));
    return f;
}
json magnitude(const std::vector<float>& h, double sr) {
    const auto fr = logFreqs();
    json f = json::array(), d = json::array();
    std::vector<double> v;
    for (double x : fr) {
        const double w = 2.0 * std::numbers::pi * x / sr;
        double re = 0.0, im = 0.0;
        for (std::size_t i = 0; i < h.size(); ++i) {
            re += h[i] * std::cos(w * static_cast<double>(i));
            im -= h[i] * std::sin(w * static_cast<double>(i));
        }
        v.push_back(toDb(std::sqrt(re * re + im * im)));
        f.push_back(std::round(x * 10.0) / 10.0);
    }
    double mx = -300.0;
    for (double x : v) mx = std::max(mx, x);
    for (double x : v) d.push_back(r2(x - mx));
    return {{"f", f}, {"db", d}, {"peak_db", r2(mx)}};
}
// Impulse response of the unit (left channel), `seconds` long. `differential` subtracts the response to
// silence from an identical reset state, which removes bias/DC settling of nonlinear units and leaves
// their small-signal impulse response; the result is scaled back to a unit impulse.
std::vector<float> impulse(IComponent& c, double sr, double seconds, float amp_ = 1.0f, bool differential = false) {
    std::vector<float> x(static_cast<std::size_t>(sr * seconds), 0.0f);
    const std::vector<float> pre(2048, 0.0f);
    std::vector<float> base;
    if (differential) {
        c.reset();
        (void)render(c, pre, sr);
        base = render(c, x, sr);
    }
    x[0] = amp_;
    c.reset();
    // Let reset-time state settle with silence first (smoothers, delay buffers).
    (void)render(c, pre, sr);
    auto y = render(c, x, sr);
    if (differential)
        for (std::size_t i = 0; i < y.size(); ++i) y[i] = (y[i] - base[i]) / amp_;
    return y;
}
// Envelope (dB, 10 ms windows, normalised) and Schroeder energy-decay curve of an impulse response.
json decay(std::vector<float> h, double sr) {
    if (!h.empty()) h[0] = 0.0f; // drop the direct (dry) sample: the envelope / EDC describe the wet tail
    const std::size_t win = static_cast<std::size_t>(sr * 0.01);
    json t = json::array(), env = json::array(), edc = json::array();
    std::vector<double> e;
    double mx = 1e-12;
    for (std::size_t i = 0; i + win <= h.size(); i += win) {
        double a = 0.0;
        for (std::size_t k = 0; k < win; ++k) a += static_cast<double>(h[i + k]) * h[i + k];
        e.push_back(a);
        mx = std::max(mx, a);
    }
    std::vector<double> back(e.size());
    double acc = 0.0;
    for (std::size_t i = e.size(); i-- > 0;) { acc += e[i]; back[i] = acc; }
    double rt = 0.0;
    int i5 = -1, i25 = -1;
    for (std::size_t i = 0; i < e.size(); ++i) {
        t.push_back(r2(static_cast<double>(i) * 0.01));
        env.push_back(r2(std::max(-90.0, 10.0 * std::log10(e[i] / mx + 1e-12))));
        const double d = 10.0 * std::log10(back[i] / std::max(1e-12, back[0]) + 1e-12);
        edc.push_back(r2(std::max(-90.0, d)));
        if (i5 < 0 && d <= -5.0) i5 = static_cast<int>(i);
        if (i25 < 0 && d <= -25.0) i25 = static_cast<int>(i);
    }
    if (i5 >= 0 && i25 > i5) rt = 3.0 * (i25 - i5) * 0.01;
    return {{"t", t}, {"env_db", env}, {"edc_db", edc}, {"rt60", rt}};
}
// Measured static compression curve: steady 1 kHz sines at increasing level.
json dynamicsCurve(IComponent& c, double sr) {
    json in = json::array(), out = json::array();
    for (int L = -60; L <= 0; L += 3) {
        const double a = std::pow(10.0, L / 20.0);
        std::vector<float> x(static_cast<std::size_t>(sr * 0.3));
        for (std::size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(a * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(i) / sr));
        c.reset();
        const auto y = render(c, x, sr);
        double e = 0.0;
        const std::size_t from = y.size() * 2 / 3;
        for (std::size_t i = from; i < y.size(); ++i) e += static_cast<double>(y[i]) * y[i];
        const double rms = std::sqrt(e / static_cast<double>(y.size() - from));
        in.push_back(L);
        out.push_back(r2(toDb(rms * std::sqrt(2.0))));
    }
    return {{"in_db", in}, {"out_db", out}};
}

std::string paramLabel(const IComponent& c, const std::string& id) {
    for (const auto& ep : c.extraParams()) {
        if (ep.id != id) continue;
        const int i = static_cast<int>(std::lround(c.getParameter(id)));
        if (i >= 0 && i < static_cast<int>(ep.labels.size())) return ep.labels[static_cast<std::size_t>(i)];
    }
    return {};
}

// ───────────────────────────── generic units (by implementation kind) ─────────────────────────────
// Values below are computed with the same key lists and formulas as the ComponentFactory classes.
void describeGeneric(IComponent& c, const std::string& impl, json& m, double sr) {
    auto* sb = dynamic_cast<SchemaComponentBase*>(&c);
    const auto& s = c.schema();
    const int32_t tid = s.table_id;
    auto N = [&](std::initializer_list<std::string_view> keys, float def) { return sb ? sb->getNormalizedParam(keys, def) : def; };
    auto P = [&](const std::string& id, float def) { return sb ? sb->getParamOr(id, def) : def; };
    json blocks = json::array();
    auto in = [&](const std::string& sub = "") { blocks.push_back(blk("in", "jack", "Input", sub, json::array(), "Input")); };
    auto out = [&](const std::string& sub = "", json v = json::array()) { blocks.push_back(blk("out", "out", "Output", sub, std::move(v), "Output")); };
    m["generic"] = true;
    if (impl == "eq" || impl == "fallback") {
        const float bass = N({"bass", "low_gain", "lf_gain", "b1", "b2", "Band 1", "Band 2", "G1", "H0", "low"}, 0.5f);
        const float m1 = N({"mid", "gain_1", "lmf_gain", "b3", "b4", "Band 3", "Band 4", "G2", "H1", "low_mid"}, 0.5f);
        const float m2 = N({"gain_2", "hmf_gain", "b5", "b6", "Band 5", "Band 6", "G3", "H2", "high_mid"}, 0.5f);
        const float tr = N({"treble", "high_gain", "hf_gain", "b7", "b8", "Band 7", "Band 8", "G4", "H3", "high"}, 0.5f);
        const float f1 = 110.0f * std::pow(14.0f, std::clamp(N({"F1", "freq_1", "lmf_freq", "cutoff"}, 0.35f), 0.0f, 1.0f));
        const float f2 = 550.0f * std::pow(12.0f, std::clamp(N({"F2", "freq_2", "hmf_freq", "shift"}, 0.65f), 0.0f, 1.0f));
        const float vol = (N({"Output", "Volume", "Vol", "V", "output", "volume", "level"}, 0.5f) - 0.5f) * 8.0f;
        m["kind"] = "eq";
        m["summary"] = impl == "fallback" ? "Generic four-band EQ stand-in: Headroom has no dedicated model of this unit yet." : "Four biquad bands in series.";
        in();
        blocks.push_back(blk("low", "eq", "Low shelf", db((bass - 0.5f) * 12.0f) + " @ 120 Hz", json::array({val("Gain", db((bass - 0.5f) * 12.0f), "±6 dB"), val("Frequency", "120 Hz")}), "EQ"));
        blocks.push_back(blk("m1", "eq", "Mid band 1", db((m1 - 0.5f) * 12.0f) + " @ " + hz(f1), json::array({val("Gain", db((m1 - 0.5f) * 12.0f)), val("Frequency", hz(f1), "110 Hz × 14^knob"), val("Q", "0.9")}), "EQ"));
        blocks.push_back(blk("m2", "eq", "Mid band 2", db((m2 - 0.5f) * 12.0f) + " @ " + hz(f2), json::array({val("Gain", db((m2 - 0.5f) * 12.0f)), val("Frequency", hz(f2), "550 Hz × 12^knob"), val("Q", "0.9")}), "EQ"));
        blocks.push_back(blk("hi", "eq", "High shelf", db((tr - 0.5f) * 12.0f) + " @ 4.2 kHz", json::array({val("Gain", db((tr - 0.5f) * 12.0f)), val("Frequency", "4.2 kHz")}), "EQ"));
        out(db(vol), json::array({val("Output", db(vol), "±4 dB")}));
    } else if (impl == "drive") {
        const bool neural = s.family == catalog::EngineFamily::Neural;
        m["kind"] = "waveshaper";
        if (neural) {
            m["summary"] = "Generic oversampled tube/drive stand-in.";
            in();
            blocks.push_back(blk("pre", "nam", "Preamp stage", "State-space", json::array({val("Model", "nonlinear preamp stage")}), "Preamp"));
            blocks.push_back(blk("tone", "eq", "Tone", "3-band", json::array({val("Bands", "low shelf 125 Hz · bell · high shelf 3.3 kHz", "±12 / ±10 dB")}), "Tone"));
            blocks.push_back(blk("pow", "power", "Power stage", "Push-pull", json::array({val("Model", "push-pull saturation + tanh")}), "Power"));
            out();
        } else {
            const auto rc = s.fixed_4x_oversampling ? 4 : dsp::InternalRateConfig::forModernDSP(sr, s.target_sample_rate_k).oversampling_factor;
            const bool is_amp = s.fixed_4x_oversampling;
            const float g = N({"Preamp", "Gain", "Overdrive", "Drive", "Distortion", "Fuzz", "Sustain", "Volume I", "Br", "P1", "P0", "V1", "Dr", "G", "g", "gain", "drive", "overdrive", "boost"}, 0.55f);
            const float drive = (is_amp ? 0.78f : 0.65f) + std::pow(g, 1.30f) * (is_amp ? 1.95f : 1.45f);
            m["summary"] = "Generic waveshaper stand-in: two analytic asymmetric-triode lookup curves and a fixed tone network.";
            m["oversampling"] = rc;
            in("HP " + std::string(is_amp ? "75" : "85") + " Hz");
            blocks.push_back(blk("drive", "gain", "Drive", num(drive, 2) + "×", json::array({val("Drive", num(drive, 2) + "×", "smoothed 5 ms")}), "Clipper"));
            for (int i = 0; i < 2; ++i)
                blocks.push_back(blk("ws" + std::to_string(i), "clip", "Waveshaper " + std::to_string(i + 1), "512-pt LUT", json::array({val("Curve", "analytic asymmetric triode table", "synthesized curve")}), "Clipper"));
            blocks.push_back(blk("tone", "eq", "Tone network", "4 bands + LP", json::array({val("Low shelf", "135 Hz ±5.5 dB"), val("Mid", "680 Hz ±5 dB"), val("High shelf", "3.1 kHz ±5.5 dB"),
                                                                                             val("Presence", "4.6 kHz ±4 dB"), val("Low-pass", "6.8 kHz")}), "Tone"));
            out("tanh · master");
        }
    } else if (impl == "cab") {
        using Lib = dsp::CabinetIRLibrary;
        m["kind"] = "cab";
        m["summary"] = "Speaker cabinets as impulse responses, mixed per mic and convolved with a zero-latency partitioned FFT convolver.";
        json mics = json::array();
        auto room = [&](int rc) { return std::string(Lib::studioId(rc)); };
        if (tid == 156000 || tid == 88000) {
            const int cab = static_cast<int>(std::lround(P("Cab", tid == 156000 ? 9.0f : 24.0f)));
            const float x = std::clamp(P("c", 0.3f), 0.0f, 1.0f);
            std::string name = tid == 156000 ? std::string(Lib::matchedId(cab)) : (cab < 11 ? "Generic cab #" + std::to_string(cab) : "Matched cab #" + std::to_string(cab - 11));
            mics.push_back(val("Mic A", pct(std::cos(x * 1.5707963f)), "equal-power crossfade"));
            mics.push_back(val("Mic B", pct(std::sin(x * 1.5707963f))));
            m["cabinet"] = name;
            in();
            blocks.push_back(blk("cab", "speaker", "Cabinet", name, json::array({val("Cabinet", name)}), "Cabinet"));
            blocks.push_back(blk("mics", "mic", "Two matched mics", pct(x) + " B", mics, "Cabinet"));
        } else if (tid == 168000) {
            in();
            for (int i = 1; i <= 4; ++i) {
                const std::string n = std::to_string(i);
                const std::string lab = paramLabel(c, "IR" + n);
                if (lab.empty() || lab == "Empty") continue;
                const float fader = 2.0f * std::clamp(P("g" + n, 0.5f), 0.0f, 1.0f);
                blocks.push_back(blk("ir" + n, "mic", "IR " + n, lab, json::array({val("File", lab), val("Fader", db(toDb(fader))), val("Pan", pct(P("p" + n, 0.5f))),
                                                                                    val("Polarity", P("pi" + n, 0.0f) >= 0.5f ? "inverted" : "normal"),
                                                                                    val("Delay", ms(0.0005 * std::clamp(P("ph" + n, 0.0f), 0.0f, 1.0f)), "phase alignment")}), "Cabinet"));
            }
        } else {
            in();
            const bool legacy = tid == 100000;
            const int chans = legacy ? 8 : 8;
            for (int i = 1; i <= chans; ++i) {
                const std::string n = std::to_string(i);
                if (P("m" + n, 0.0f) >= 0.5f) continue;
                if (legacy) {
                    const float g = P("g" + n, 0.0f);
                    if (g < 0.01f) continue;
                    blocks.push_back(blk("mic" + n, "mic", "Mic " + n, room(static_cast<int>(std::lround(P("S", 3.0f)))), json::array({val("Fader", db(toDb(g * g * 2.0f)))}), "Cabinet"));
                    continue;
                }
                const int cab = static_cast<int>(std::lround(P("Cab" + n, i <= 2 ? 10.0f : 1.0f)));
                if (cab == 1) continue;
                const float fader = 2.0f * std::clamp(P("g" + n, 0.5f), 0.0f, 1.0f);
                if (cab == 0) {
                    blocks.push_back(blk("mic" + n, "jack", "DI " + n, "direct", json::array({val("Fader", db(toDb(0.35f * fader)))}), "Cabinet"));
                    continue;
                }
                const int mic = std::clamp(static_cast<int>(std::lround(P("Mic" + n, 0.0f))), 0, 4), pos = std::clamp(static_cast<int>(std::lround(P("MPos" + n, 0.0f))), 0, 2);
                static const char* kMic[] = {"Shure SM57", "Sennheiser MD 421-II", "Neumann U87 Ai", "Royer R-121", "Beyerdynamic M 160"};
                static const char* kPos[] = {"cap", "cap edge", "cone"};
                blocks.push_back(blk("mic" + n, "mic", "Mic " + n, room(cab - 2), json::array({val("Cabinet", room(cab - 2)), val("Mic", kMic[mic]), val("Position", kPos[pos]),
                                                                                               val("Fader", db(toDb(fader))), val("Pan", pct(P("p" + n, 0.5f))),
                                                                                               val("Room", pct(std::clamp(P("r" + n, 0.0f), 0.0f, 1.0f)))}), "Cabinet"));
            }
        }
        blocks.push_back(blk("conv", "convolver", "Convolver", "zero latency", json::array({val("Engine", "uniform-partitioned FFT (vDSP), direct head", "no added latency"),
                                                                                           val("Channels", "stereo IR, mono guitar in")}), "Cabinet"));
        if (tid == 119000 || tid == 100000)
            blocks.push_back(blk("eq", "eq", "Room EQ", "bass / treble", json::array({val("Bass", db((P("b", 0.5f) - 0.5f) * 16.0f) + " @ 120 Hz"), val("Treble", db((P("t", 0.5f) - 0.5f) * 16.0f) + " @ 3.5 kHz")}), "Output"));
        out();
    } else if (impl == "ir_reverb") {
        m["kind"] = "reverb";
        m["summary"] = "Convolution reverb with the bundled acoustic spaces; size trims the IR, decay applies an exponential envelope.";
        std::string file;
        float wet = 0.0f, dry = 1.0f;
        if (tid == 127000) {
            wet = std::clamp(P("M2", 0.2f), 0.0f, 1.0f) * 1.6f;
        } else if (tid == 126000) {
            const float mx = std::clamp(P("W1", 0.3f), 0.0f, 1.0f);
            wet = mx * 1.4f;
            dry = 1.0f - 0.5f * mx;
        } else {
            wet = std::clamp(P("W0", 0.3f), 0.0f, 1.0f) * 1.2f;
            dry = std::clamp(P("D7", 1.0f), 0.0f, 1.0f);
        }
        if (tid == 127000) {
            static const char* kModes[8] = {"Gold Plate Short", "Gold Plate Long", "Silver Plate Short", "Silver Plate Long", "Studio Spring", "Dual Spring", "Amp Spring", "Dub Spring"};
            file = kModes[std::clamp(static_cast<int>(std::lround(P("S1", 0.0f))), 0, 7)];
        } else if (tid == 126000) {
            static const char* kIrs[8] = {"Short A", "Short B", "Medium A", "Medium B", "Long A", "Long B", "XXL A", "XXL B"};
            file = kIrs[std::clamp(static_cast<int>(std::lround(P("I7", 0.3f) * 7.0f)), 0, 7)];
        } else {
            file = paramLabel(c, "IR");
        }
        m["space"] = file;
        in();
        blocks.push_back(blk("ir", "convolver", "Impulse response", file, json::array({val("Space", file), val("Engine", "partitioned FFT convolution, up to 3.2 s")}), "Reverb"));
        blocks.push_back(blk("mix", "mix", "Dry / wet", db(toDb(wet)) + " wet", json::array({val("Wet", num(wet, 2) + "× (" + db(toDb(wet)) + ")"), val("Dry", num(dry, 2) + "×")}), "Output"));
        out();
    } else if (impl == "fdn_reverb") {
        const bool conv_family = s.family == catalog::EngineFamily::Convolution;
        const float sz = conv_family ? N({"S1", "Size", "size"}, 0.55f) : N({"size", "room_size", "predelay", "preD"}, 0.55f);
        const float dec = conv_family ? 0.65f : N({"revTime", "decay", "feedback", "time", "reverb_time"}, 0.60f);
        const float dmp = conv_family ? 0.35f : N({"damp", "highCut", "high_cut", "treble", "color"}, 0.40f);
        const float md = conv_family ? 0.35f : N({"mod", "modulation", "diffusion", "detune"}, 0.35f);
        const float mix = conv_family ? std::clamp(N({"W0", "M2", "Mix", "Dry/Wet", "mix", "reverb"}, 0.25f), 0.0f, 0.75f)
                                      : std::clamp(N({"mix", "dry_wet", "W0", "M2"}, 0.22f), 0.0f, 0.65f);
        const float scale = 0.4f + 1.4f * sz, fb = 0.55f + 0.41f * dec, fc = 1200.0f + 10000.0f * (1.0f - dmp);
        m["kind"] = "reverb";
        m["summary"] = "Four-line feedback delay network with a Hadamard mixing matrix, per-line damping and slow modulation.";
        in();
        json lines = json::array();
        static constexpr float kT[4] = {0.0297f, 0.0371f, 0.0411f, 0.0437f};
        for (int i = 0; i < 4; ++i) lines.push_back(val("Line " + std::to_string(i + 1), ms(kT[i] * scale)));
        blocks.push_back(blk("fdn", "fdn", "4-line FDN", "×" + num(scale, 2), lines, "Reverb"));
        blocks.push_back(blk("mat", "matrix", "Hadamard 4×4", "× 0.5 · fb " + num(fb, 3), json::array({val("Feedback", num(fb, 3), "0.55 + 0.41 × decay"), val("Matrix", "orthogonal Hadamard ×0.5")}), "Reverb"));
        blocks.push_back(blk("damp", "filter_lp", "Damping", hz(fc), json::array({val("Low-pass", hz(fc), "biquad in every line")}), "Reverb"));
        blocks.push_back(blk("lfo", "lfo", "Modulation", "0.65 Hz", json::array({val("Depth", num(12.0f * md, 1) + " samples", "opposite polarity on odd lines")}), "Reverb"));
        blocks.push_back(blk("mix", "mix", "Dry / wet", pct(mix), json::array({val("Mix", pct(mix))}), "Output"));
        out();
    } else if (impl == "delay") {
        const float t = N({"time", "delay", "delay_time", "T", "tl", "rate", "speed"}, 0.38f);
        const float fbn = N({"feedback", "regen", "repeats", "F", "decay"}, 0.35f);
        const float mod = N({"depth", "mod", "flutter", "chorus", "shimmer"}, 0.20f);
        const float hc = N({"hicut", "high_cut", "treble", "tone", "damp"}, 0.65f);
        const float mix = std::clamp(N({"mix", "dry_wet", "D", "level"}, 0.24f), 0.0f, 0.50f);
        const double tl = 0.08 + 0.62 * t;
        m["kind"] = "delay";
        m["summary"] = "Stereo cross-feedback (ping-pong) delay with band-limited repeats and gentle modulation.";
        in();
        blocks.push_back(blk("dl", "delay", "Delay L", ms(tl), json::array({val("Time", ms(tl), "80 ms + 620 ms × time")}), "Delay"));
        blocks.push_back(blk("dr", "delay", "Delay R", ms(tl * 1.125), json::array({val("Time", ms(tl * 1.125), "1.125 × left")}), "Delay"));
        blocks.push_back(blk("fb", "matrix", "Cross feedback", pct(std::clamp(fbn * 0.78f, 0.0f, 0.82f)), json::array({val("Feedback", pct(std::clamp(fbn * 0.78f, 0.0f, 0.82f)), "L→R, R→L (ping-pong)")}), "Delay"));
        blocks.push_back(blk("lp", "filter_lp", "Repeat filter", hz(1400.0f + 7500.0f * hc) + " · HP 110 Hz",
                             json::array({val("Low-pass", hz(1400.0f + 7500.0f * hc)), val("High-pass", "110 Hz")}), "Delay"));
        blocks.push_back(blk("lfo", "lfo", "Modulation", "0.85 Hz", json::array({val("Depth", num(12.0f * mod, 1) + " samples", "opposite on L / R")}), "Delay"));
        blocks.push_back(blk("mix", "mix", "Mix", pct(mix), json::array({val("Wet", pct(mix)), val("Dry", pct(1.0f - 0.45f * mix))}), "Output"));
        out();
    } else if (impl == "mod") {
        const float r = N({"rate", "speed", "freq", "flutter", "Rt"}, 0.35f);
        const float d = N({"depth", "intensity", "width", "amount", "wow", "CI"}, 0.45f);
        const float f = N({"feedback", "resonance", "color"}, 0.20f);
        const float mix = std::clamp(N({"mix", "dry_wet", "blend", "DW"}, 0.35f), 0.0f, 0.65f);
        const float rate = 0.15f + 4.0f * r * r, depth = 1.0f + 5.0f * d, fb = (f - 0.5f) * 0.7f;
        m["kind"] = "mod";
        m["summary"] = "Generic stereo modulated delay line (chorus / flanger family) with quadrature LFOs.";
        m["lfo"] = {{"rate_hz", rate}, {"base_ms", 3.5}, {"depth_ms", depth}};
        in();
        blocks.push_back(blk("lfo", "lfo", "LFO", hz(rate), json::array({val("Rate", num(rate, 2) + " Hz", "0.15 + 4·rate²"), val("Phase", "sine L, cosine R (90°)")}), "Modulation"));
        blocks.push_back(blk("line", "delay", "Modulated delay", ms(0.0035) + " + " + num(depth, 1) + " ms", json::array({val("Base", "3.5 ms"), val("Sweep", num(depth, 2) + " ms", "1 + 5·depth")}), "Modulation"));
        blocks.push_back(blk("fb", "matrix", "Feedback", num(fb, 2), json::array({val("Feedback", num(fb, 2), "(color − 0.5) × 0.7")}), "Modulation"));
        blocks.push_back(blk("mix", "mix", "Mix", pct(mix), json::array({val("Mix", pct(mix))}), "Output"));
        out();
    } else if (impl == "dynamics") {
        const bool gate = s.reference_name.find("Gate") != std::string_view::npos || s.reference_name.find("Noise") != std::string_view::npos ||
                          s.display_name.find("Gate") != std::string_view::npos || s.display_name.find("Noise") != std::string_view::npos;
        const float th = N({"threshold", "thresh", "sustain", "sensitivity", "input", "I0", "Th", "Lm", "comp"}, gate ? 0.25f : 0.50f);
        const float mk = N({"output", "volume", "level", "makeup", "O4", "O1", "Out", "V", "gain"}, 0.55f);
        m["kind"] = "dynamics";
        m["summary"] = gate ? "Peak-envelope downward gate." : "Peak-envelope compressor (soft ratio ≈ 1.8:1 above threshold) with makeup gain.";
        in();
        blocks.push_back(blk("env", "detector", "Peak detector", "4 ms / 90 ms", json::array({val("Attack", "4 ms"), val("Release", "90 ms"), val("Link", "stereo max")}), "Dynamics"));
        if (gate) {
            const float thr = 0.0005f + 0.006f * th * th;
            blocks.push_back(blk("vca", "vca", "Gate", db(toDb(thr)), json::array({val("Threshold", db(toDb(thr)) + "FS"), val("Floor", "-16.5 dB (0.15)")}), "Dynamics"));
        } else {
            const float thr = 0.16f + 0.40f * (1.0f - th);
            blocks.push_back(blk("vca", "vca", "Gain computer", db(toDb(thr)), json::array({val("Threshold", db(toDb(thr)) + "FS"), val("Slope", "gain = (thr/env)^0.45"),
                                                                                            val("Makeup", db(toDb(0.85f + 0.55f * mk)))}), "Dynamics"));
        }
        out();
    } else if (impl == "midi" || impl == "instrument") {
        m["kind"] = "midi";
        if (tid == 900001) {
            const bool poly = P("mode", 0.0f) < 0.5f;
            m["summary"] = poly ? "Polyphonic guitar → MIDI: multi-resolution FFT harmonic salience with NNLS note templates, spectral-flux onsets, per-note hysteresis (6 voices)."
                                : "Monophonic guitar → MIDI: YIN pitch tracking on a 16 kHz analysis signal, 4 ms hop, continuous pitch bend.";
            in();
            blocks.push_back(blk("dec", "filter_lp", "Analysis decimator", "→ 16 kHz", json::array({val("Anti-alias", "4th-order Butterworth"), val("Rate", num(sr / std::max(1L, std::lround(sr / 16000.0)), 0) + " Hz")}), "Analysis"));
            blocks.push_back(blk("det", "detector", poly ? "Poly detector" : "YIN detector", poly ? "FFT salience" : "YIN",
                                 json::array({val("Mode", poly ? "polyphonic" : "monophonic"), val("Sensitivity", pct(P("sens", 0.5f))),
                                              val("Range", "MIDI " + std::to_string(static_cast<int>(P("low", 40))) + " – " + std::to_string(static_cast<int>(P("high", 88)))),
                                              val("Bend range", num(P("bend", 2.0f), 0) + " st"), val("Velocity curve", num(P("vel", 0.5f), 2))}), "Analysis"));
            blocks.push_back(blk("midi", "midi", "MIDI out", "notes + bend", json::array({val("Bus", "lane MIDI bus + virtual CoreMIDI port")}), "Output"));
            blocks.push_back(blk("thru", "out", "Guitar thru", P("thru", 1.0f) >= 0.5f ? "on" : "muted", json::array(), "Output"));
        } else if (tid == 900002) {
            static const char* kHarm[] = {"off", "octave", "fifth", "power", "third", "major", "minor", "sus4"};
            static const char* kScale[] = {"chromatic", "major", "minor", "dorian", "mixolydian", "pentatonic minor", "pentatonic major", "blues"};
            static const char* kArp[] = {"off", "up", "down", "up/down", "random"};
            static const char* kRate[] = {"1/4", "1/8", "1/16", "1/8T", "1/16T", "1/32"};
            auto idx = [&](const char* id, int mx, float def) { return std::clamp(static_cast<int>(std::lround(P(id, def))), 0, mx); };
            m["summary"] = "MIDI note processing: transpose, scale-aware harmony and a tempo-synced arpeggiator.";
            blocks.push_back(blk("in", "midi", "MIDI in", "lane bus", json::array(), "Input"));
            blocks.push_back(blk("tr", "gain", "Transpose", num(P("octave", 0), 0) + " oct " + num(P("transpose", 0), 0) + " st", json::array({val("Octave", num(P("octave", 0), 0)), val("Semitones", num(P("transpose", 0), 0))}), "MIDI"));
            blocks.push_back(blk("harm", "matrix", "Harmony", kHarm[idx("harmony", 7, 0)], json::array({val("Interval", kHarm[idx("harmony", 7, 0)]), val("Scale", kScale[idx("scale", 7, 0)]),
                                                                                                        val("Key", std::to_string(idx("root", 11, 4)))}), "MIDI"));
            blocks.push_back(blk("arp", "lfo", "Arpeggiator", kArp[idx("arp", 4, 0)], json::array({val("Pattern", kArp[idx("arp", 4, 0)]), val("Rate", kRate[idx("rate", 5, 2)]),
                                                                                                    val("Tempo", num(P("bpm", 120), 0) + " bpm"), val("Hold", P("hold", 0) >= 0.5f ? "on" : "off")}), "MIDI"));
            blocks.push_back(blk("out", "midi", "MIDI out", "", json::array(), "Output"));
        } else if (tid == 900003) {
            const std::string snd = paramLabel(c, "sound");
            m["summary"] = "Instrument sound library: Headroom FM keys, a drawbar organ, the poly-synth presets, General MIDI and installed AU instruments.";
            m["sound"] = snd;
            blocks.push_back(blk("in", "midi", "MIDI in", "lane bus", json::array(), "Input"));
            blocks.push_back(blk("gen", "synth", "Sound", snd, json::array({val("Sound", snd), val("Guitar follow", pct(P("follow", 0.3f)), "voice level tracks the picked string")}), "Instrument"));
            blocks.push_back(blk("tone", "filter_lp", "Tone", pct(P("tone", 0.5f)), json::array({val("Filter", "two 1-pole low-passes")}), "Instrument"));
            blocks.push_back(blk("mix", "mix", "Level / mix", pct(P("level", 0.7f)), json::array({val("Level", pct(P("level", 0.7f))), val("Mix", pct(P("mix", 1.0f)))}), "Output"));
            out();
        } else if (tid == 900006) {
            static const char* kTrig[] = {"Bloom", "Always", "Engage", "Freeze"};
            static const char* kPat[] = {"Up", "Down", "Up/Down", "Converge", "Walk", "Melody", "Pedal", "Random"};
            static const char* kRate[] = {"1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"};
            static const char* kExt[] = {"As played", "Triad", "7th", "9th", "11th"};
            static const char* kVoice[] = {"Synth (MIDI)", "Strings (your guitar)", "Both"};
            static const char* kKey[] = {"Auto", "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
            auto idx = [&](const char* id, int mx, float def) { return std::clamp(static_cast<int>(std::lround(P(id, def))), 0, mx); };
            m["summary"] = "Bloom: an arpeggiator played by the guitar. It completes the chord you hold from the detected key, voice-leads a pattern "
                           "across it (the same chord always plays the same figure, restarted on your attack), learns tempo, swing and accents from "
                           "your picking, follows bends, palm mutes and pick strength, and can "
                           "re-pluck your own strings (captured per-string spectra) instead of a synth.";
            blocks.push_back(blk("in", "midi", "Tracked notes", "lane bus + guitar audio", json::array(), "Input"));
            blocks.push_back(blk("hands", "detector", "Hands", "onsets · mutes · bends", json::array({val("Touch", pct(P("touch", 0.6f)), "pick strength → loudness only"),
                                                                                                      val("Mute choke", pct(P("choke", 0.7f))), val("Bend follow", pct(P("bend", 1.0f)), "per-string PLLs")}), "Analysis"));
            blocks.push_back(blk("harm", "matrix", "Harmony", kExt[idx("extend", 4, 1)], json::array({val("Key", kKey[idx("key", 12, 0)]), val("Extend", kExt[idx("extend", 4, 1)]),
                                                                                                    val("Range", num(P("range", 1) + 1, 0) + " oct")}), "Arpeggio"));
            blocks.push_back(blk("arp", "lfo", "Pattern", kPat[idx("pattern", 7, 4)], json::array({val("Trigger", kTrig[idx("trigger", 3, 0)]), val("Rate", kRate[idx("rate", 5, 3)]),
                                                                                                   val("Clock", P("clock", 1) >= 0.5f ? "Feel (learned)" : "Rig tempo"), val("Gate", pct(P("gate", 0.6f))),
                                                                                                   val("Swing", pct(P("swing", 0.0f))), val("Motion", pct(P("motion", 0.5f))),
                                                                                                   val("Retrigger", P("retrig", 0) >= 0.5f ? "every pick" : "new chord")}), "Arpeggio"));
            blocks.push_back(blk("voice", "synth", "Voice", kVoice[idx("voice", 2, 2)], json::array({val("String tone", pct(P("tone", 0.5f))), val("String level", pct(P("level", 0.5f))),
                                                                                                     val("Dry while arpeggiating", pct(P("dry", 0.5f)))}), "Output"));
            blocks.push_back(blk("out", "midi", "MIDI + audio out", "", json::array(), "Output"));
        } else {
            m["summary"] = "8-voice virtual-analog poly synth: PolyBLEP oscillators, zero-delay-feedback transistor ladder, exponential ADSRs, chorus / delay / plate.";
            blocks.push_back(blk("in", "midi", "MIDI in", "lane bus", json::array(), "Input"));
            blocks.push_back(blk("osc", "synth", "Oscillators", "2 osc + sub + noise", json::array({val("Waves", "PolyBLEP saw / square, triangle, sine"), val("Unison", "up to 3 voices"), val("Voices", "8")}), "Synth"));
            blocks.push_back(blk("vcf", "filter_lp", "Ladder filter", "4-pole ZDF", json::array({val("Model", "Zavalishin TPT transistor ladder, per-stage saturation"),
                                                                                              val("Cutoff", pct(P("cutoff", 0.55f))), val("Resonance", pct(P("resonance", 0.25f)))}), "Synth"));
            blocks.push_back(blk("vca", "vca", "Amp envelope", "ADSR", json::array({val("Attack", pct(P("amp_a", 0.05f))), val("Release", pct(P("amp_r", 0.4f)))}), "Synth"));
            blocks.push_back(blk("fx", "fdn", "FX", "chorus · delay · plate", json::array({val("Chorus", pct(P("fx_chorus", 0.3f))), val("Delay", pct(P("fx_delay", 0.15f))), val("Reverb", pct(P("fx_reverb", 0.3f)))}), "FX"));
            out();
        }
    } else {
        m["kind"] = "generic";
        m["summary"] = impl == "plugin" ? "Hosted Audio Unit / VST plug-in: Headroom passes audio through the plug-in's own processing."
                                        : "This unit has no dedicated engine model; it passes audio through.";
        in();
        blocks.push_back(blk("unit", impl == "plugin" ? "plugin" : "gain", s.display_name.empty() ? std::string(s.reference_name) : std::string(s.display_name), impl, json::array(), "Unit"));
        out();
    }
    m["blocks"] = std::move(blocks);
}

std::string paramsSummaryJson(const IComponent& c) {
    json p = json::object();
    for (const auto& e : c.exportParams()) {
        if (e.id == kAnalysisParam || e.id == kModelParam) continue;
        if (e.is_string) p[e.id] = e.str;
        else p[e.id] = e.num;
    }
    return p.dump();
}

// ───────────────────────────── user components store ─────────────────────────────
std::mutex g_uc_mtx;

// Saved components: {"schema":"headroom.user-component/1","base_model_id":tid,"overrides":{...}} (+ UI fields).
// Older / hand-written entries without a schema are migrated on load.
json migrateUserComponent(json it) {
    if (!it.is_object()) return it;
    if (!it.contains("schema")) it["schema"] = "headroom.user-component/1";
    if (!it.contains("base_model_id") && it.contains("base")) it["base_model_id"] = it["base"];
    if (!it.contains("base") && it.contains("base_model_id")) it["base"] = it["base_model_id"];
    if (!it.contains("overrides") && it.contains("model") && it["model"].is_object()) {
        json o = it["model"];
        for (const char* k : {"name", "id", "base", "schema"}) o.erase(k);
        it["overrides"] = o;
    }
    if (!it.contains("model") && it.contains("overrides")) it["model"] = it["overrides"];
    return it;
}
json loadUserComponents() {
    std::ifstream f(userComponentsPath());
    if (!f) return json::array();
    try {
        json j = json::parse(f);
        json items = (j.is_object() && j.contains("components") && j["components"].is_array()) ? j["components"] : (j.is_array() ? j : json::array());
        for (auto& it : items) it = migrateUserComponent(it);
        return items;
    } catch (...) {
    }
    return json::array();
}
bool storeUserComponents(const json& items) {
    std::error_code ec;
    const fs::path p(userComponentsPath());
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = p.string() + ".tmp";
    {
        std::ofstream f(tmp);
        if (!f) return false;
        f << json({{"schema", "headroom.user-components/1"}, {"version", 1}, {"components", items}}).dump(2) << "\n";
    }
    fs::rename(tmp, p, ec);
    return !ec;
}
std::string newId() {
    std::random_device rd;
    char b[32];
    std::snprintf(b, sizeof b, "uc-%08x%04x", rd(), rd() & 0xffff);
    return b;
}

void collectSlot(IComponent* c, int32_t& tid, json& params, std::string& model) {
    if (!c) return;
    tid = c->tableId();
    for (const auto& e : c->exportParams()) {
        if (e.id == kModelParam) { model = e.str; continue; }
        if (e.id == kAnalysisParam) continue;
        if (e.is_string) params[e.id] = e.str;
        else params[e.id] = e.num;
    }
}

} // namespace

std::string userComponentsPath() { return library::appSupportDir() + "/User Components.json"; }

std::string customName(const IComponent& c) {
    const std::string s = c.getStringParameter(kModelParam);
    if (s.empty()) return {};
    const json j = parseObj(s);
    return j.contains("name") && j["name"].is_string() ? j["name"].get<std::string>() : std::string("Custom");
}

void prewarm(int32_t table_id, const std::string& model_json, double sr) {
    if (isTubeAmpTable(table_id)) prewarmTubeAmpModel(table_id, model_json, sr);
}

std::string describe(int32_t tid, const std::string& params_json, const std::string& model_json, double sr) {
    const auto* s = catalog::findByTableId(tid);
    if (!s) return R"({"error":"unknown table id"})";
    const std::string impl = implementationKind(tid);
    auto c = makeInstance(tid, parseObj(params_json), json::object(), model_json, sr);
    json m = json::object();
    if (c) {
        if (const auto* in = dynamic_cast<const Introspectable*>(c.get())) m = parseObj(in->modelJson());
        else describeGeneric(*c, impl, m, sr);
    }
    m["table_id"] = tid;
    m["name"] = std::string(s->display_name);
    m["ref"] = std::string(s->reference_name);
    m["impl"] = impl;
    m["engine"] = m.value("backend", std::string()) == "hybrid-circuit" ? "Hybrid electrical reference (documented circuit and behavioral sections)" : engineDescription(tid);
    m["sample_rate"] = sr;
    m["editable"] = isTubeAmpTable(tid) || isDrivePedalTable(tid);
    if (!m.contains("oversampling")) m["oversampling"] = std::max(1, engineOversampling(tid, sr));
    if (c) m["params"] = parseObj(paramsSummaryJson(*c));
    // Versioned description shared with the future circuit engine: stable block ids and edit paths,
    // an honest fidelity label per block, and no hardware-verification claims.
    m["schema"] = "headroom.unit-model/1";
    m["model_id"] = tid;
    if (!m.contains("verification")) m["verification"] = "none";
    if (!m.contains("netlist")) m["netlist"] = nullptr; // opaque/DSP models have no electrical connectivity
    if (!m.contains("backend")) m["backend"] = "behavioral-dsp";
    if (!m.contains("capabilities")) m["capabilities"] = {{"scalarEdits", m["editable"]},
        {"arbitraryTopologyEdits", false}, {"physicalVoltageIO", false},
        {"hardwareCalibrated", false}, {"neuralDevice", false}};
    if (m.contains("blocks") && m["blocks"].is_array()) {
        for (auto& b : m["blocks"]) {
            if (!b.contains("fidelity")) {
                const std::string t = b.value("type", std::string());
                b["fidelity"] = impl == "nam" && t == "nam" ? "opaque" : (impl == "amp" && (t == "triode" || t == "follower") ? "behavioural" : "dsp-block");
            }
            if (b.contains("values") && b["values"].is_array())
                for (auto& v : b["values"])
                    if (v.contains("edit") && v["edit"].is_object()) {
                        const std::string p = v["edit"].value("path", std::string());
                        static const char* kSub[] = {".kind", ".type", ".off", ".preset", ".clip", ".class_a"};
                        bool sub = false;
                        for (const char* k : kSub) sub |= p.size() > std::strlen(k) && p.compare(p.size() - std::strlen(k), std::string::npos, k) == 0;
                        v["edit"]["op"] = sub ? "substitute" : "set"; // device / topology substitution vs scalar value edit
                    }
        }
    }
    m["fidelity_legend"] = {{"dsp-block", "signal-processing block with the listed parameters"},
                            {"behavioural", "static curves solved offline from device equations (Koren 12AX7), dynamic cathode state per sample"},
                            {"opaque", "trained network; no internal probes"},
                            {"circuit-solved", "electrical subcircuit solved from stated device laws and component values; see verification scope"}};
    const json mj = parseObj(model_json);
    m["overrides"] = mj;
    if (mj.contains("name")) m["custom_name"] = mj["name"];
    return m.dump();
}

std::string curves(int32_t tid, const std::string& params_json, const std::string& model_json, double sr, const std::string& what, float level, float freq) {
    const auto* s = catalog::findByTableId(tid);
    if (!s) return R"({"error":"unknown table id"})";
    const std::string impl = implementationKind(tid);
    auto c = makeInstance(tid, parseObj(params_json), json::object(), model_json, sr);
    if (!c) return R"({"error":"no engine"})";
    const bool all = what.empty() || what == "all";
    auto want = [&](const char* k) { return all || what.find(k) != std::string::npos; };
    json out = json::object();
    out["impl"] = impl;
    if (want("static"))
        if (const auto* in = dynamic_cast<const Introspectable*>(c.get())) out["static"] = parseObj(in->curvesJson());
    const bool nonlinear = impl == "amp" || impl == "drive" || impl == "nam";
    if (want("harmonics") && nonlinear) out["harmonics"] = harmonics(*c, sr, std::clamp(level, 0.001f, 1.0f), std::clamp(freq, 40.0f, 4000.0f));
    if (want("response")) {
        if (tid == 121000) {
            // Multiple boosted bands can clip the card on a full-scale impulse.
            // Subtract the matched silence render to isolate its small-signal gain.
            out["response"] = magnitude(impulse(*c, sr, 0.35, 0.01f, true), sr);
        } else if (impl == "cab" || impl == "eq" || impl == "fallback") {
            out["response"] = magnitude(impulse(*c, sr, 16384.0 / sr), sr);
        } else if (impl == "ir_reverb" || impl == "fdn_reverb") {
            const auto h = impulse(*c, sr, 3.0);
            out["decay"] = decay(h, sr);
            std::vector<float> early(h.begin(), h.begin() + std::min<std::size_t>(h.size(), 32768));
            if (!early.empty()) early[0] = 0.0f; // wet response only (the dry path is a unit impulse)
            out["response"] = magnitude(early, sr);
        } else if (impl == "delay") {
            out["decay"] = decay(impulse(*c, sr, 2.5), sr);
        } else if (impl == "dynamics") {
            out["dynamics"] = dynamicsCurve(*c, sr);
        } else if (impl == "amp" || impl == "drive") {
            // Small-signal response of the whole unit: a -60 dBFS impulse (far below any clipping) minus
            // the unit's response to silence from the same state.
            out["response"] = magnitude(impulse(*c, sr, 0.35, 1e-3f, true), sr);
        }
    }
    if (impl == "mod") {
        auto* sb = dynamic_cast<SchemaComponentBase*>(c.get());
        if (sb) {
            const float r = sb->getNormalizedParam({"rate", "speed", "freq", "flutter", "Rt"}, 0.35f);
            const float d = sb->getNormalizedParam({"depth", "intensity", "width", "amount", "wow", "CI"}, 0.45f);
            const float rate = 0.15f + 4.0f * r * r, depth = 1.0f + 5.0f * d;
            json t = json::array(), l = json::array(), rr = json::array();
            const double span = 2.0 / rate;
            for (int i = 0; i <= 200; ++i) {
                const double tt = span * i / 200.0, ph = 2.0 * std::numbers::pi * rate * tt;
                t.push_back(r2(tt));
                l.push_back(r2(3.5 + depth * (0.5 + 0.5 * std::sin(ph))));
                rr.push_back(r2(3.5 + depth * (0.5 + 0.5 * std::cos(ph))));
            }
            out["lfo"] = {{"t", t}, {"l_ms", l}, {"r_ms", rr}, {"rate_hz", rate}};
        }
    }
    return out.dump();
}

bool handlesPath(const std::string& path) {
    return path.rfind("/api/component/", 0) == 0 || path.rfind("/api/usercomponents", 0) == 0;
}

std::string handleApi(const std::string& path, const std::function<std::string(const std::string&)>& q, const WithSlot& with_slot, double sr,
                      bool* rack_modified) {
    if (rack_modified) *rack_modified = false;
    auto qlong = [&](const char* k, long def) {
        const std::string v = q(k);
        if (v.empty()) return def;
        try { return std::stol(v); } catch (...) { return def; }
    };
    auto qfloat = [&](const char* k, float def) {
        const std::string v = q(k);
        if (v.empty()) return def;
        try { return std::stof(v); } catch (...) { return def; }
    };
    try {
        if (path == "/api/component/model" || path == "/api/component/curves") {
            int32_t tid = static_cast<int32_t>(qlong("tid", 0));
            json params = parseObj(q("params"));
            std::string model = q("model");
            const long slot = qlong("slot", -1);
            if (slot >= 0) {
                with_slot(slot, [&](IComponent* c) {
                    if (c && (tid == 0 || c->tableId() == tid)) {
                        json p = json::object();
                        std::string mm;
                        collectSlot(c, tid, p, mm);
                        for (const auto& [k, v] : params.items()) p[k] = v;
                        params = std::move(p);
                        if (q("model").empty()) model = mm;
                    }
                    return false;
                });
            }
            // Knob positions (0..1) from the page, mapped like the rack maps them.
            const json norms = parseObj(q("norms"));
            if (!norms.empty()) {
                if (auto tmp = createComponentByTableId(tid)) {
                    for (const auto& [k, v] : norms.items()) {
                        if (!v.is_number()) continue;
                        setNormalized(*tmp, k, v.get<float>());
                        params[k] = tmp->getParameter(k);
                    }
                }
            }
            if (path == "/api/component/model") return describe(tid, params.dump(), model, sr);
            return curves(tid, params.dump(), model, sr, q("what"), qfloat("level", 0.25f), qfloat("freq", 330.0f));
        }
        if (path == "/api/component/apply") {
            const long slot = qlong("slot", -1);
            const std::string model = q("model");
            int32_t tid = 0;
            with_slot(slot, [&](IComponent* c) { if (c) tid = c->tableId(); return false; });
            if (!tid) return R"({"ok":false,"error":"no such slot"})";
            if (!isTubeAmpTable(tid) && !isDrivePedalTable(tid)) return R"({"ok":false,"error":"this unit has no editable model"})";
            prewarm(tid, model, sr); // expensive part (amp level trims) outside the graph lock
            std::string name;
            bool ok = false;
            with_slot(slot, [&](IComponent* c) {
                if (!c || c->tableId() != tid) return false;
                c->setStringParameter(kModelParam, model);
                c->processControlRate();
                name = customName(*c);
                ok = true;
                return true;
            });
            if (rack_modified) *rack_modified = ok;
            return json({{"ok", ok}, {"slot", slot}, {"custom_name", name}}).dump();
        }
        if (path == "/api/usercomponents") {
            std::lock_guard<std::mutex> lk(g_uc_mtx);
            return json({{"components", loadUserComponents()}, {"path", userComponentsPath()}}).dump();
        }
        if (path == "/api/usercomponents/save") {
            const int32_t tid = static_cast<int32_t>(qlong("tid", 0));
            const auto* s = catalog::findByTableId(tid);
            if (!s || (!isTubeAmpTable(tid) && !isDrivePedalTable(tid))) return R"({"ok":false,"error":"this unit can't be customised"})";
            std::string name = q("name");
            if (name.empty()) name = "My " + std::string(s->reference_name);
            json model = parseObj(q("model"));
            std::string id = q("id");
            std::lock_guard<std::mutex> lk(g_uc_mtx);
            json items = loadUserComponents();
            if (id.empty()) id = newId();
            model["name"] = name;
            model["id"] = id;
            model["base"] = tid;
            model["schema"] = "headroom.overrides/1";
            const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            json ov = model;
            for (const char* k : {"name", "id", "base", "schema"}) ov.erase(k);
            json item = {{"schema", "headroom.user-component/1"}, {"base_model_id", tid}, {"overrides", ov}, {"id", id}, {"name", name}, {"base", tid}, {"base_ref", std::string(s->reference_name)}, {"base_name", std::string(s->display_name)},
                         {"model", model}, {"norms", parseObj(q("norms"))}, {"notes", q("notes")}, {"modified", now}};
            bool found = false;
            for (auto& it : items) {
                if (it.is_object() && it.value("id", std::string()) == id) {
                    item["created"] = it.value("created", now);
                    it = item;
                    found = true;
                }
            }
            if (!found) {
                item["created"] = now;
                items.push_back(item);
            }
            const bool ok = storeUserComponents(items);
            prewarm(tid, model.dump(), sr);
            return json({{"ok", ok}, {"component", item}}).dump();
        }
        if (path == "/api/usercomponents/delete") {
            const std::string id = q("id");
            std::lock_guard<std::mutex> lk(g_uc_mtx);
            json items = loadUserComponents(), keep = json::array();
            bool removed = false;
            for (const auto& it : items) {
                if (it.is_object() && it.value("id", std::string()) == id) removed = true;
                else keep.push_back(it);
            }
            const bool ok = removed && storeUserComponents(keep);
            return json({{"ok", ok}}).dump();
        }
    } catch (const std::exception& e) {
        return json({{"error", e.what()}}).dump();
    }
    return R"({"error":"not_found"})";
}

} // namespace rig::model
