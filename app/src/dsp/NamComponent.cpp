// "NAM Capture" rack unit (900005): plays Neural Amp Modeler (.nam) and knob-conditioned AIDA-X /
// RTNeural (.aidax / .json) captures of real amps / pedals / full rigs through NeuralAmpModelerCore (MIT),
// with loudness & dBu input/output calibration from capture metadata, SlimmableModel CPU scaling,
// band-limited 12-lobe Lanczos windowed-sinc sample-rate bridging, a 5-state hysteretic noise gate
// (clean DI sidechain detector + post-model gain stage) and a 3-band post EQ.
#include "../core/IComponent.hpp"
#include "AmpModeling.hpp"
#include "AmpUtilities.hpp"
#include "ComponentModel.hpp"
#include "NamComponent.hpp"
#include "../platform/Paths.hpp"
#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include "NAM/slimmable.h"
#include "json.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace rig {

namespace {

struct CaptureInfo {
    std::string path, label, group, gear_type;
    double loudness = 0.0;
    bool has_loudness = false;
    double input_level_dbu = 0.0;
    bool has_input_level_dbu = false;
    double output_level_dbu = 0.0;
    bool has_output_level_dbu = false;
    bool is_conditioned = false;
};

std::string captureDir() {
    const std::string d = platform::appDataDir() + "/NAM Captures";
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
}

// First run: seed the folder with the MIT-licensed example captures shipped with NeuralAmpModelerCore. They are
// found under the shipped content root (<root>/app/third_party/NeuralAmpModelerCore/example_models, staged by
// every package), falling back to the source tree this binary was built from (dev builds). An existing but
// empty "Demo Captures" folder is seeded too. Called only from captures(), under g_cap_mtx.
void seedDemos() {
    static bool done = false;
    if (done) return;
    std::error_code ec;
    const std::string dst = captureDir() + "/Demo Captures";
    if (fs::exists(dst, ec) && !fs::is_empty(dst, ec)) { done = true; return; }
    const std::string root = platform::appRoot();
    std::vector<std::string> srcs;
    if (!root.empty()) srcs.push_back(root + "/app/third_party/NeuralAmpModelerCore/example_models");
#ifdef RIG_NAM_EXAMPLES_DIR
    srcs.push_back(RIG_NAM_EXAMPLES_DIR);
#endif
    for (const auto& r : srcs) {
        if (!fs::is_directory(r, ec)) continue;
        fs::create_directories(dst, ec);
        bool any = false;
        for (const auto& [src, name] : std::vector<std::pair<std::string, std::string>>{
                 {"A2.nam", "NAM A2 Demo Amp.nam"}, {"lstm.nam", "Darkglass Microtubes 900 v2 (LSTM demo).nam"},
                 {"my_model.nam", "NAM WaveNet Standard Demo.nam"}}) {
            std::error_code cec;
            fs::copy_file(r + "/" + src, dst + "/" + name, fs::copy_options::skip_existing, cec);
            any = any || fs::exists(dst + "/" + name, ec);
        }
        if (any) { done = true; return; }
    }
    // Nothing found: retry later only if the content root isn't known yet (the engine sets it at start).
    if (!root.empty()) done = true;
}

std::mutex g_cap_mtx;
std::vector<CaptureInfo> g_caps;
fs::file_time_type g_scan_stamp{};

const std::vector<CaptureInfo>& captures(bool rescan = false) {
    std::lock_guard<std::mutex> lk(g_cap_mtx);
    seedDemos();
    std::error_code ec;
    const std::string root = captureDir();
    auto stamp = fs::last_write_time(root, ec);
    if (!g_caps.empty() && !rescan && stamp == g_scan_stamp) return g_caps;
    g_scan_stamp = stamp;
    std::vector<CaptureInfo> v;
    for (const auto& it : fs::recursive_directory_iterator(root, ec)) {
        if (!it.is_regular_file()) continue;
        std::string ext = it.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != ".nam" && ext != ".aidax" && ext != ".json") continue;
        CaptureInfo c;
        c.path = it.path().string();
        c.label = it.path().stem().string();
        const auto rel = fs::relative(it.path().parent_path(), root, ec).string();
        c.group = (rel.empty() || rel == ".") ? "My Captures" : rel;
        try {
            std::ifstream f(c.path);
            const auto j = nlohmann::json::parse(f);
            if (j.contains("in_shape") && j.contains("layers")) {
                c.is_conditioned = true;
            }
            if (j.contains("metadata") && j["metadata"].is_object()) {
                const auto& m = j["metadata"];
                auto str = [&](const char* k) { return m.contains(k) && m[k].is_string() ? m[k].get<std::string>() : std::string(); };
                const std::string make = str("gear_make"), model = str("gear_model"), name = str("name"), tone = str("tone_type");
                c.gear_type = str("gear_type");
                if (!make.empty() || !model.empty()) c.label = (make + " " + model);
                else if (!name.empty()) c.label = name;
                if (!tone.empty()) c.label += " · " + tone;
                if (m.contains("loudness") && m["loudness"].is_number()) {
                    c.loudness = m["loudness"].get<double>();
                    c.has_loudness = true;
                }
                if (m.contains("input_level_dbu") && m["input_level_dbu"].is_number()) {
                    c.input_level_dbu = m["input_level_dbu"].get<double>();
                    c.has_input_level_dbu = true;
                }
                if (m.contains("output_level_dbu") && m["output_level_dbu"].is_number()) {
                    c.output_level_dbu = m["output_level_dbu"].get<double>();
                    c.has_output_level_dbu = true;
                }
            }
        } catch (...) {
        }
        v.push_back(std::move(c));
    }
    std::sort(v.begin(), v.end(), [](const CaptureInfo& a, const CaptureInfo& b) { return a.group == b.group ? a.label < b.label : a.group < b.group; });
    g_caps = std::move(v);
    return g_caps;
}

// Band-limited polyphase resampler (Kaiser-windowed sinc, β = 9 ≈ 90 dB stopband) for the
// host ↔ capture rate bridge.
//   * The kernel spans ±16 samples of the LOWER of the two rates, so when decimating (e.g. 96 → 48 kHz)
//     it widens to ±32 input samples and keeps the same ≈8.6 kHz transition band in Hz.
//   * The −6 dB cutoff sits at 0.94 × the lower Nyquist (22.6 kHz for 48 kHz): flat (< 0.01 dB) to
//     ≈18 kHz, ≥ 90 dB rejection from ≈26.8 kHz, so anything that could fold back below ≈21 kHz is gone.
//   * 512 precomputed phases, linearly interpolated, built by configure() on the control thread.
//     process() does no transcendental math and never allocates (output capacity is reserved by the
//     caller); reset() only clears history.
//   * Group delay is exactly halfWidthIn() input samples.
class BandlimitedResampler {
public:
    static constexpr int kLowRateHalfWidth = 16;
    static constexpr int kMaxTaps = 256;
    static constexpr int kRing = 256; // power of two >= kMaxTaps; stored twice for contiguous windows
    static constexpr int kPhases = 512;

    // Control thread: (re)designs the kernel when the rate pair changes.
    void configure(double in_sr, double out_sr) {
        in_sr = (std::isfinite(in_sr) && in_sr > 1000.0) ? in_sr : 48000.0;
        out_sr = (std::isfinite(out_sr) && out_sr > 1000.0) ? out_sr : 48000.0;
        step_ = in_sr / out_sr;
        const double ratio = out_sr / in_sr;
        const int half = std::clamp(static_cast<int>(std::ceil(kLowRateHalfWidth / std::min(1.0, ratio))), 4, kMaxTaps / 2);
        // Cutoff as a fraction of the INPUT Nyquist.
        const double fc = 0.94 * std::min(1.0, ratio);
        if (half != half_ || std::abs(fc - fc_) > 1e-9 || table_.empty()) {
            half_ = half;
            taps_ = 2 * half;
            fc_ = fc;
            buildTable();
        }
        reset();
    }

    void reset() noexcept {
        pos_ = 0.0;
        write_ = 0;
        ring_.fill(0.0f);
    }

    int halfWidthIn() const noexcept { return half_; }

    // Pushes `n` input samples and appends the resampled output to `out`.
    // The caller reserves `out` capacity outside the audio thread (push_back never reallocates then).
    void process(const float* in, std::size_t n, std::vector<float>& out) noexcept {
        const int T = taps_;
        const float* table = table_.data();
        for (std::size_t i = 0; i < n; ++i) {
            ring_[static_cast<std::size_t>(write_)] = in[i];
            ring_[static_cast<std::size_t>(write_ + kRing)] = in[i];
            write_ = (write_ + 1) & (kRing - 1);
            // Window: oldest at ring_[write_ + kRing - T], newest at ring_[write_ + kRing - 1].
            const float* s = &ring_[static_cast<std::size_t>(write_ + kRing - T)];
            while (pos_ < 1.0) {
                const double pf = pos_ * kPhases;
                const int p = std::min(static_cast<int>(pf), kPhases - 1);
                const float frac = static_cast<float>(pf - p);
                const float* w0 = table + static_cast<std::size_t>(p) * static_cast<std::size_t>(T);
                const float* w1 = w0 + T;
                float a0 = 0.0f, a1 = 0.0f;
                for (int k = 0; k < T; ++k) {
                    a0 += s[k] * w0[k];
                    a1 += s[k] * w1[k];
                }
                if (out.size() < out.capacity()) out.push_back(a0 + frac * (a1 - a0));
                pos_ += step_;
            }
            pos_ -= 1.0;
        }
    }

private:
    static double besselI0(double x) noexcept {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 64; ++k) {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
            if (term < 1e-14 * sum) break;
        }
        return sum;
    }

    void buildTable() {
        constexpr double kBeta = 9.0;
        const double inv_i0 = 1.0 / besselI0(kBeta);
        table_.assign(static_cast<std::size_t>(kPhases + 1) * static_cast<std::size_t>(taps_), 0.0f);
        std::vector<double> raw(static_cast<std::size_t>(taps_));
        for (int p = 0; p <= kPhases; ++p) {
            const double mu = static_cast<double>(p) / kPhases;
            double sum = 0.0;
            for (int k = 0; k < taps_; ++k) {
                // k = 0 oldest, k = taps-1 newest; the output instant lies mu after tap half-1.
                const double x = static_cast<double>(half_ - 1 - k) + mu;
                const double r = x / static_cast<double>(half_);
                double v = 0.0;
                if (std::abs(r) < 1.0) {
                    const double px = std::numbers::pi * fc_ * x;
                    const double sinc = std::abs(px) < 1e-12 ? 1.0 : std::sin(px) / px;
                    v = fc_ * sinc * besselI0(kBeta * std::sqrt(1.0 - r * r)) * inv_i0;
                }
                raw[static_cast<std::size_t>(k)] = v;
                sum += v;
            }
            const double norm = std::abs(sum) > 1e-12 ? 1.0 / sum : 1.0; // unity DC gain on every phase
            for (int k = 0; k < taps_; ++k)
                table_[static_cast<std::size_t>(p) * static_cast<std::size_t>(taps_) + static_cast<std::size_t>(k)] =
                    static_cast<float>(raw[static_cast<std::size_t>(k)] * norm);
        }
    }

    double step_ = 1.0, pos_ = 0.0, fc_ = -1.0;
    int half_ = kLowRateHalfWidth, taps_ = 2 * kLowRateHalfWidth, write_ = 0;
    std::array<float, 2 * kRing> ring_{};
    std::vector<float> table_;
};

// Multi-input knob-conditioned GRU/Dense model (RTNeural / AIDA-X .aidax JSON format)
// implementing nam::DSP so conditioned models (audio + up to 2 conditioning knobs) run
// natively inside NamComponent.
class ConditionedGruDsp final : public nam::DSP {
public:
    ConditionedGruDsp(double sr, int in_dim, int hidden_dim,
                      std::vector<float> w_ih, std::vector<float> w_hh,
                      std::vector<float> b_ih, std::vector<float> b_hh,
                      std::vector<float> dense_w, float dense_b, bool residual)
        : nam::DSP(1, 1, sr),
          in_dim_(std::clamp(in_dim, 1, 3)),
          hidden_(std::max(1, hidden_dim)),
          w_ih_(std::move(w_ih)),
          w_hh_(std::move(w_hh)),
          b_ih_(std::move(b_ih)),
          b_hh_(std::move(b_hh)),
          dense_w_(std::move(dense_w)),
          dense_b_(dense_b),
          residual_(residual),
          h_(static_cast<std::size_t>(hidden_), 0.0f),
          scratch_(static_cast<std::size_t>(3 * hidden_), 0.0f) {}

    void setConditioning(float c1, float c2) noexcept {
        cond1_ = c1;
        cond2_ = c2;
    }

    void process(NAM_SAMPLE** input, NAM_SAMPLE** output, const int num_frames) override {
        const std::size_t H = static_cast<std::size_t>(hidden_);
        const std::size_t G = 3 * H;
        for (int s = 0; s < num_frames; ++s) {
            const float x0 = static_cast<float>(input[0][s]);
            const float u[3] = {x0, cond1_, cond2_};
            for (std::size_t r = 0; r < G; ++r) {
                float a = b_ih_[r];
                for (int c = 0; c < in_dim_; ++c) {
                    a += w_ih_[static_cast<std::size_t>(c) * G + r] * u[c];
                }
                float b = b_hh_[r];
                for (std::size_t c = 0; c < H; ++c) {
                    b += w_hh_[c * G + r] * h_[c];
                }
                if (r < 2 * H) {
                    scratch_[r] = 1.0f / (1.0f + std::exp(-(a + b)));
                } else {
                    const std::size_t i = r - 2 * H;
                    const float reset_g = scratch_[i];
                    scratch_[r] = std::tanh(a + reset_g * b);
                }
            }
            float y = dense_b_;
            for (std::size_t i = 0; i < H; ++i) {
                const float z = scratch_[H + i];
                const float n = scratch_[2 * H + i];
                h_[i] = (1.0f - z) * n + z * h_[i];
                y += dense_w_[i] * h_[i];
            }
            output[0][s] = static_cast<NAM_SAMPLE>(residual_ ? (y + x0) : y);
        }
    }

    void Reset(const double sampleRate, const int maxBufferSize) override {
        nam::DSP::Reset(sampleRate, maxBufferSize);
        std::fill(h_.begin(), h_.end(), 0.0f);
    }

    static std::unique_ptr<ConditionedGruDsp> tryLoad(const fs::path& p) {
        std::ifstream f(p);
        if (!f) return nullptr;
        const auto j = nlohmann::json::parse(f, nullptr, false);
        if (j.is_discarded() || !j.contains("in_shape") || !j.contains("layers") || !j["layers"].is_array())
            return nullptr;
        const auto& in_shape = j["in_shape"];
        if (!in_shape.is_array() || in_shape.empty()) return nullptr;
        const int in_dim = in_shape.back().is_number_integer() ? in_shape.back().get<int>() : 1;
        if (j["layers"].size() < 2) return nullptr;
        const auto& l0 = j["layers"][0];
        const auto& l1 = j["layers"].back();
        if (l0.value("type", std::string()) != "gru" || l1.value("type", std::string()) != "dense")
            return nullptr;
        const auto& shape0 = l0["shape"];
        const int H = shape0.is_array() && !shape0.empty() ? shape0.back().get<int>() : 0;
        if (H <= 0 || H > 128) return nullptr;
        const auto& w0 = l0["weights"];
        const auto& w1 = l1["weights"];
        if (!w0.is_array() || w0.size() < 3 || !w1.is_array() || w1.size() < 2) return nullptr;

        const std::size_t G = static_cast<std::size_t>(3 * H);
        std::vector<float> w_ih(static_cast<std::size_t>(in_dim) * G, 0.0f);
        std::vector<float> w_hh( static_cast<std::size_t>(H) * G, 0.0f);
        std::vector<float> b_ih(G, 0.0f), b_hh(G, 0.0f);
        for (int c = 0; c < in_dim && c < static_cast<int>(w0[0].size()); ++c)
            for (std::size_t r = 0; r < G && r < w0[0][c].size(); ++r)
                w_ih[static_cast<std::size_t>(c) * G + r] = w0[0][c][r].get<float>();
        for (int c = 0; c < H && c < static_cast<int>(w0[1].size()); ++c)
            for (std::size_t r = 0; r < G && r < w0[1][c].size(); ++r)
                w_hh[static_cast<std::size_t>(c) * G + r] = w0[1][c][r].get<float>();
        if (w0[2].is_array() && w0[2].size() == 2 && w0[2][0].is_array()) {
            for (std::size_t r = 0; r < G && r < w0[2][0].size(); ++r) b_ih[r] = w0[2][0][r].get<float>();
            for (std::size_t r = 0; r < G && r < w0[2][1].size(); ++r) b_hh[r] = w0[2][1][r].get<float>();
        } else if (w0[2].is_array()) {
            for (std::size_t r = 0; r < G && r < w0[2].size(); ++r) b_ih[r] = w0[2][r].get<float>();
        }
        std::vector<float> dense_w(static_cast<std::size_t>(H), 0.0f);
        for (int i = 0; i < H && i < static_cast<int>(w1[0].size()); ++i) {
            dense_w[static_cast<std::size_t>(i)] = w1[0][i].is_array() ? w1[0][i][0].get<float>() : w1[0][i].get<float>();
        }
        const float dense_b = (w1[1].is_array() && !w1[1].empty()) ? w1[1][0].get<float>() : 0.0f;
        const double sr = j.value("samplerate", 48000.0);
        const bool skip = j.value("in_skip", 0) != 0;
        return std::make_unique<ConditionedGruDsp>(sr, in_dim, H, std::move(w_ih), std::move(w_hh),
                                                   std::move(b_ih), std::move(b_hh), std::move(dense_w), dense_b, skip);
    }

private:
    int in_dim_ = 1, hidden_ = 8;
    std::vector<float> w_ih_, w_hh_, b_ih_, b_hh_, dense_w_;
    float dense_b_ = 0.0f;
    bool residual_ = false;
    float cond1_ = 0.5f, cond2_ = 0.5f;
    std::vector<float> h_, scratch_;
};

class NamComponent final : public SchemaComponentBase, public model::Introspectable {
public:
    int subBlockSafety() const noexcept override { return 2; } // not yet verified for sub-blocks
    bool monoPreserving() const noexcept override { return true; } // renders the mean of its inputs to both channels
    using SchemaComponentBase::SchemaComponentBase;

    // Rate-bridge group delay in host samples: the host→capture filter delays by its half width (host
    // samples), the capture→host filter by its half width in capture samples, plus the FIFO cushion
    // used for non-integer ratios. Verified against measured group delay in tests/test_nam_bridge.cpp.
    uint32_t latencySamples() const noexcept override {
        if (!dsp_ || !resampled_) return 0;
        return static_cast<uint32_t>(std::lround(static_cast<double>(up_.halfWidthIn()) +
                                                 static_cast<double>(down_.halfWidthIn()) * host_sr_ / std::max(8000.0, model_sr_) +
                                                 static_cast<double>(cushion_)));
    }

    void adoptRuntimeStateFrom(IComponent& component) noexcept override {
        auto* previous = dynamic_cast<NamComponent*>(&component);
        if (!previous || previous->loaded_path_ != loaded_path_ || previous->model_sr_ != model_sr_ ||
            previous->host_sr_ != host_sr_ || previous->max_block_ != max_block_) return;
        // Both instances have been prepared; swapping keeps the trained network's
        // recurrent history and moves the unused new instance to deferred disposal.
        // A different SlimmableModel size was applied to this instance's freshly loaded network.
        if (previous->applied_slim_ != applied_slim_) return;
        dsp_.swap(previous->dsp_);
        std::swap(up_, previous->up_);
        std::swap(down_, previous->down_);
        gate_ring_ = previous->gate_ring_;
        gate_ring_pos_ = previous->gate_ring_pos_;
        pending_.swap(previous->pending_);
        mid_in_.swap(previous->mid_in_); mid_out_.swap(previous->mid_out_);
        gate_.adoptRuntimeStateFrom(previous->gate_);
        sag_env_ = previous->sag_env_;
        pre_bass_.adoptRuntimeStateFrom(previous->pre_bass_);
        pre_mid_.adoptRuntimeStateFrom(previous->pre_mid_);
        pre_treb_.adoptRuntimeStateFrom(previous->pre_treb_);
        for (int c = 0; c < 2; ++c) {
            bass_[c].adoptRuntimeStateFrom(previous->bass_[c]);
            mid_[c].adoptRuntimeStateFrom(previous->mid_[c]);
            treb_[c].adoptRuntimeStateFrom(previous->treb_[c]);
        }
    }

    // Model export: the loaded capture's architecture and metadata plus Headroom's wrapper around it.
    std::string modelJson() const override {
        nlohmann::json m = nlohmann::json::object();
        m["kind"] = "nam";
        m["backend"] = "opaque-neural";
        m["capabilities"] = {{"scalarEdits", false}, {"arbitraryTopologyEdits", false},
                              {"physicalVoltageIO", has_input_level_dbu_}, {"hardwareCalibrated", has_input_level_dbu_},
                              {"neuralDevice", true}, {"runtimeStateHandoff", true},
                              {"slimmable", is_slimmable_}, {"knobConditioned", is_conditioned_}};
        m["verification"] = "none";
        m["netlist"] = nullptr;
        m["capture"] = loaded_label_;
        m["loaded"] = static_cast<bool>(dsp_);
        m["model_rate"] = model_sr_;
        m["host_rate"] = host_sr_;
        m["resampled"] = std::abs(model_sr_ - host_sr_) >= 1.0;
        m["latency_samples"] = latencySamples();
        m["loudness_db"] = loudness_db_;
        m["has_input_level_dbu"] = has_input_level_dbu_;
        m["input_level_dbu"] = input_level_dbu_;
        m["has_output_level_dbu"] = has_output_level_dbu_;
        m["output_level_dbu"] = output_level_dbu_;
        m["slimmable"] = is_slimmable_;
        m["slim_ratio"] = slim_ratio_;
        m["slim_breakpoints"] = slim_breakpoints_;
        m["calibration"] = {{"enabled", cal_enabled_}, {"input_db", cal_in_db_}, {"output_db", cal_out_db_},
                            {"headroom_full_scale_dbu", ref_dbu_},
                            {"note", "capture dBu metadata vs Headroom's volts-per-full-scale (circuit::VoltageCalibration)"}};
        m["input_db"] = getParamOr("input", 0.0f);
        m["output_db"] = getParamOr("output", 0.0f);
        m["normalize"] = getParamOr("normalize", 1.0f) >= 0.5f;
        m["gate"] = gate_thr_;
        m["stack_react"] = stack_react_;
        m["power_sag"] = power_sag_;
        m["eq"] = {{"bass_db", (getParamOr("bass", 0.5f) - 0.5f) * 18.0f}, {"mid_db", (getParamOr("mid", 0.5f) - 0.5f) * 18.0f},
                   {"treble_db", (getParamOr("treble", 0.5f) - 0.5f) * 18.0f}};
        std::string arch, version, gear_type, gear, tone;
        long weights = 0;
        nlohmann::json cfg_summary = nlohmann::json::object();
        if (!loaded_path_.empty()) {
            try {
                std::ifstream f(loaded_path_);
                const auto j = nlohmann::json::parse(f);
                arch = j.value("architecture", j.contains("in_shape") ? std::string("ConditionedGRU") : std::string());
                version = j.value("version", std::string());
                if (j.contains("weights") && j["weights"].is_array()) weights = static_cast<long>(j["weights"].size());
                if (j.contains("metadata") && j["metadata"].is_object()) {
                    const auto& md = j["metadata"];
                    auto str = [&](const char* k) { return md.contains(k) && md[k].is_string() ? md[k].get<std::string>() : std::string(); };
                    gear_type = str("gear_type");
                    gear = str("gear_make") + (str("gear_make").empty() ? "" : " ") + str("gear_model");
                    tone = str("tone_type");
                }
                if (j.contains("config") && j["config"].is_object()) {
                    const auto& c = j["config"];
                    if (c.contains("layers") && c["layers"].is_array()) {
                        nlohmann::json layers = nlohmann::json::array();
                        for (const auto& l : c["layers"]) {
                            nlohmann::json o = nlohmann::json::object();
                            for (const char* k : {"input_size", "condition_size", "channels", "head_size", "kernel_size", "activation"})
                                if (l.contains(k)) o[k] = l[k];
                            if (l.contains("dilations") && l["dilations"].is_array()) o["dilations"] = l["dilations"].size();
                            layers.push_back(o);
                        }
                        cfg_summary["layers"] = layers;
                    }
                    for (const char* k : {"hidden_size", "num_layers", "input_size"})
                        if (c.contains(k)) cfg_summary[k] = c[k];
                }
            } catch (...) {
            }
        }
        m["architecture"] = arch;
        m["version"] = version;
        m["weights"] = weights;
        m["gear"] = gear;
        m["gear_type"] = gear_type;
        m["tone_type"] = tone;
        m["config"] = cfg_summary;
        auto mv = [](const std::string& k, const std::string& v, const std::string& note = {}) {
            nlohmann::json o = {{"k", k}, {"v", v}};
            if (!note.empty()) o["note"] = note;
            return o;
        };
        auto blk = [](const std::string& id, const std::string& type, const std::string& label, const std::string& sub, nlohmann::json v, const std::string& sec) {
            return nlohmann::json{{"id", id}, {"type", type}, {"label", label}, {"sub", sub}, {"values", std::move(v)}, {"section", sec}};
        };
        char b[64];
        nlohmann::json blocks = nlohmann::json::array();
        std::snprintf(b, sizeof b, "%+.1f dB", getParamOr("input", 0.0f));
        blocks.push_back(blk("in", "jack", "Input", b, nlohmann::json::array({mv("Input gain", b), mv("Gate", gate_thr_ > 0.001f ? "on" : "off", "hysteretic DI detector (6 dB open/close, 60 ms hold, 400 dB/s release) → post-model VCA")}), "Input"));
        if (m["resampled"].get<bool>()) {
            std::snprintf(b, sizeof b, "%.0f → %.0f Hz", host_sr_, model_sr_);
            blocks.push_back(blk("rs", "resampler", "Rate bridge", b, nlohmann::json::array({mv("Resampler", "Kaiser windowed-sinc polyphase", "≥ 90 dB anti-aliasing / anti-imaging stopband"), mv("Latency", std::to_string(latencySamples()) + " samples")}), "Input"));
        }
        nlohmann::json nv = nlohmann::json::array({mv("Architecture", arch.empty() ? "—" : arch), mv("Weights", std::to_string(weights)), mv("Format", version.empty() ? "—" : "v" + version)});
        if (!gear.empty()) nv.push_back(mv("Captured gear", gear));
        if (!tone.empty()) nv.push_back(mv("Tone", tone));
        if (has_input_level_dbu_) {
            char cb[96];
            std::snprintf(cb, sizeof cb, "%.1f dBu → %+.1f dB", input_level_dbu_, cal_in_db_);
            nv.push_back(mv("Input calibration", cb, cal_enabled_ ? "capture reamp level vs Headroom full scale" : "off"));
        }
        if (is_slimmable_) {
            char sb[32];
            std::snprintf(sb, sizeof sb, "%.0f %%", slim_ratio_ * 100.0f);
            nv.push_back(mv("Model size", sb, "SlimmableModel"));
        }
        blocks.push_back(blk("nam", "nam", "Neural capture", arch.empty() ? "NAM" : arch, std::move(nv), "Capture"));
        std::snprintf(b, sizeof b, "%+.1f dB", getParamOr("output", 0.0f));
        nlohmann::json ov = nlohmann::json::array({mv("Output", b)});
        char nb[64];
        std::snprintf(nb, sizeof nb, "capture loudness %.1f dB → -18", loudness_db_);
        ov.push_back(mv("Normalise", getParamOr("normalize", 1.0f) >= 0.5f ? "on" : "off", nb));
        blocks.push_back(blk("lvl", "gain", "Level", b, std::move(ov), "Output"));
        char eb[96];
        std::snprintf(eb, sizeof eb, "%+.1f / %+.1f / %+.1f dB", (getParamOr("bass", 0.5f) - 0.5f) * 18.0f, (getParamOr("mid", 0.5f) - 0.5f) * 18.0f, (getParamOr("treble", 0.5f) - 0.5f) * 18.0f);
        blocks.push_back(blk("eq", "eq", "Post EQ", eb, nlohmann::json::array({mv("Bass", "low shelf 120 Hz"), mv("Mid", "bell 750 Hz, Q 0.8"), mv("Treble", "high shelf 3.2 kHz"), mv("Range", "±9 dB")}), "Output"));
        blocks.push_back(blk("out", "out", "Output", "", nlohmann::json::array(), "Output"));
        m["blocks"] = std::move(blocks);
        return m.dump();
    }

    std::vector<ExtraParam> extraParams() const override {
        const auto& caps = captures();
        ExtraParam p;
        p.id = "capture";
        p.name = "Capture";
        for (const auto& c : caps) {
            p.labels.push_back(c.label);
            p.groups.push_back(c.group);
        }
        if (p.labels.empty()) {
            p.labels.push_back("No captures — import .nam files");
            p.groups.push_back("My Captures");
        }
        p.def = 0.0f;
        std::vector<ExtraParam> out{p};
        if (is_slimmable_) {
            ExtraParam sz;
            sz.id = "model_size";
            sz.name = "Model Size";
            const std::size_t options = slimOptionCount();
            for (std::size_t i = 0; i < options; ++i) {
                char b[48];
                std::snprintf(b, sizeof b, "%s (%.0f %%)", i == 0 ? "Smallest" : (i + 1 == options ? "Full" : "Reduced"),
                              100.0 * slimValueFor(i));
                sz.labels.push_back(b);
            }
            sz.def = static_cast<float>(options - 1);
            out.push_back(std::move(sz));
        }
        return out;
    }

protected:
    void onPrepare(double sr, uint32_t max_block) override {
        if (params_.find("capture") == params_.end()) params_["capture"].num_val = 0.0f;
        max_block_ = std::max<uint32_t>(max_block, 4096);
        loaded_path_.clear();
        gate_.prepare(sr);
        gate_.setTiming(0.060, 400.0);
        configureBridge();
        const std::size_t max_cap = static_cast<std::size_t>(max_block_) * 8u + 128u;
        mid_in_.reserve(max_cap);
        mid_out_.reserve(max_cap);
        pending_.reserve(max_cap);
        sag_att_ = static_cast<float>(1.0 - std::exp(-1.0 / (0.012 * sr)));
        sag_rel_ = static_cast<float>(1.0 - std::exp(-1.0 / (0.145 * sr)));
        onProcessControlRate();
    }
    void onReset() override {
        if (dsp_) dsp_->ResetAndPrewarm(model_sr_, static_cast<int>(std::ceil(max_block_ * model_sr_ / host_sr_)) + 64);
        resetBridge();
        gate_.reset();
        gate_ring_.fill(1.0f);
        sag_env_ = 0.0f;
        pre_bass_.reset();
        pre_mid_.reset();
        pre_treb_.reset();
        for (int c = 0; c < 2; ++c) {
            bass_[c].reset();
            mid_[c].reset();
            treb_[c].reset();
        }
    }
    void onProcessControlRate() override {
        const auto& caps = captures();
        const int sel = static_cast<int>(std::lround(getParamOr("capture", 0.0f)));
        const std::string custom_path = getStringParameter("capture_path");
        std::string want_path;
        std::string want_label;
        CaptureInfo meta{};
        if (!custom_path.empty()) {
            want_path = custom_path;
            want_label = fs::path(custom_path).stem().string();
        } else if (sel >= 0 && sel < static_cast<int>(caps.size())) {
            meta = caps[static_cast<std::size_t>(sel)];
            want_path = meta.path;
            want_label = meta.label;
        }
        if (!want_path.empty() && want_path != loaded_path_) {
            try {
                std::unique_ptr<nam::DSP> d;
                bool cond = false;
                if (auto c_dsp = ConditionedGruDsp::tryLoad(fs::path(want_path))) {
                    d = std::move(c_dsp);
                    cond = true;
                } else {
                    d = nam::get_dsp(fs::path(want_path));
                }
                if (d) {
                    model_sr_ = d->GetExpectedSampleRate() > 0 ? d->GetExpectedSampleRate() : 48000.0;
                    const auto modelCapacity = static_cast<std::size_t>(std::ceil(max_block_ * model_sr_ / host_sr_)) + 64;
                    if (modelCapacity > max_block_ * 16u) throw std::runtime_error("Unsupported capture sample-rate ratio");
                    mid_in_.reserve(modelCapacity);
                    mid_out_.reserve(modelCapacity);
                    pending_.reserve(max_block_ * 4u + 128u);
                    d->ResetAndPrewarm(model_sr_, static_cast<int>(modelCapacity));
                    loudness_db_ = d->HasLoudness() ? d->GetLoudness() : (meta.has_loudness ? meta.loudness : -18.0);
                    has_input_level_dbu_ = d->HasInputLevel() || meta.has_input_level_dbu;
                    input_level_dbu_ = d->HasInputLevel() ? d->GetInputLevel() : meta.input_level_dbu;
                    has_output_level_dbu_ = d->HasOutputLevel() || meta.has_output_level_dbu;
                    output_level_dbu_ = d->HasOutputLevel() ? d->GetOutputLevel() : meta.output_level_dbu;
                    auto* slim = dynamic_cast<nam::SlimmableModel*>(d.get());
                    is_slimmable_ = slim != nullptr;
                    slim_breakpoints_ = slim ? slim->GetSlimmableSizeBreakpoints() : std::vector<double>{};
                    applied_slim_ = -1.0f;
                    is_conditioned_ = cond;
                    std::shared_ptr<nam::DSP> sp(std::move(d));
                    dsp_ = sp;
                    loaded_path_ = want_path;
                    loaded_label_ = want_label;
                    configureBridge();
                }
            } catch (...) {
                loaded_path_.clear();
            }
        }
        // SlimmableModel CPU scaling (e.g. A2 / SlimmableContainer captures). "model_size" is the
        // discrete selector offered through extraParams(); the continuous "slim" (0.1..1) is kept for
        // older rigs. SetSlimmableSize is not real-time safe: it only runs here, on the control thread,
        // and only when the size actually changes.
        if (params_.count("model_size")) {
            const std::size_t opts = slimOptionCount();
            const auto idx = static_cast<std::size_t>(std::clamp<long>(std::lround(getParamOr("model_size", 0.0f)), 0L, static_cast<long>(opts) - 1));
            slim_ratio_ = static_cast<float>(slimValueFor(idx));
        } else {
            slim_ratio_ = std::clamp(getParamOr("slim", 1.0f), 0.0f, 1.0f);
        }
        if (auto* slim = dynamic_cast<nam::SlimmableModel*>(dsp_.get())) {
            if (slim_ratio_ != applied_slim_) {
                try {
                    slim->SetSlimmableSize(static_cast<double>(slim_ratio_));
                    applied_slim_ = slim_ratio_;
                } catch (...) {
                }
            }
        }
        // Update conditioning knobs on multi-input RTNeural / AIDA-X models.
        if (auto* cond = dynamic_cast<ConditionedGruDsp*>(dsp_.get())) {
            const float c1 = std::clamp(0.5f + getParamOr("input", 0.0f) / 40.0f, 0.0f, 1.0f);
            const float c2 = std::clamp(getParamOr("treble", 0.5f), 0.0f, 1.0f);
            cond->setConditioning(getParamOr("cond_1", c1), getParamOr("cond_2", c2));
        }

        // Physical level calibration from the capture's dBu metadata.
        //   NAM: input_level_dbu  = RMS dBu of a sine that hit 0 dBFS at the capture's input (reamp send),
        //        output_level_dbu = RMS dBu of a sine at 0 dBFS of the recorded output.
        //   Headroom: 1.0 digital full scale = circuit::VoltageCalibration volts (assumed 1 V peak, the
        //        same scale the tube amp models use) → a full-scale sine is ref_dbu (-0.79 dBu).
        // So the capture sees the same guitar voltage a Headroom amp would: input gain ref - in_dbu, and
        // (when loudness normalisation is off) its output returns to Headroom's scale: out_dbu - ref.
        // "input_dbu" overrides the reference (e.g. a measured interface level); "calibrate" = 0 disables.
        ref_dbu_ = getParamOr("input_dbu", static_cast<float>(circuit::VoltageCalibration{}.fullScaleSineDbu()));
        cal_enabled_ = getParamOr("calibrate", 1.0f) >= 0.5f && has_input_level_dbu_;
        cal_in_db_ = 0.0f;
        cal_out_db_ = 0.0f;
        if (cal_enabled_) {
            cal_in_db_ = std::clamp(ref_dbu_ - static_cast<float>(input_level_dbu_), -30.0f, 30.0f);
            if (has_output_level_dbu_) cal_out_db_ = std::clamp(static_cast<float>(output_level_dbu_) - ref_dbu_, -30.0f, 30.0f);
        }

        in_gain_ = amp::dbToLin(getParamOr("input", 0.0f) + cal_in_db_);
        const bool normalize = getParamOr("normalize", 1.0f) >= 0.5f;
        const float norm_db = normalize ? std::clamp(static_cast<float>(-18.0 - loudness_db_), -24.0f, 30.0f) : cal_out_db_;
        out_gain_ = amp::dbToLin(getParamOr("output", 0.0f) + norm_db);
        gate_thr_ = getParamOr("gate", 0.0f);
        if (gate_thr_ > 0.001f) {
            gate_.setThresholdDb(-80.0f + 50.0f * gate_thr_, 6.0f); // open/close 6 dB apart
            gate_.setEnabled(true);
        } else {
            gate_.setEnabled(false);
        }

        // Interactive Pre-Capture Tone Stack + Post-EQ split:
        // When `stack_react` > 0, moving Bass/Mid/Treble shapes the input driving the neural capture's
        // nonlinear stages (with partial post-de-emphasis) so EQ tweaks feel like a real tube amp stack.
        stack_react_ = std::clamp(getParamOr("stack_react", 0.0f), 0.0f, 1.0f);
        power_sag_ = std::clamp(getParamOr("sag", 0.0f), 0.0f, 1.0f);
        const float bass_db = (getParamOr("bass", 0.5f) - 0.5f) * 18.0f;
        const float mid_db = (getParamOr("mid", 0.5f) - 0.5f) * 18.0f;
        const float treb_db = (getParamOr("treble", 0.5f) - 0.5f) * 18.0f;

        const float pre_scale = 0.55f * stack_react_;
        const float post_scale = 1.0f - 0.35f * stack_react_;
        pre_bass_.set(amp::Svf::Type::LowShelf, host_sr_, 135.0f, 0.7f, bass_db * pre_scale);
        pre_mid_.set(amp::Svf::Type::Bell, host_sr_, 680.0f, 0.85f, mid_db * pre_scale);
        pre_treb_.set(amp::Svf::Type::HighShelf, host_sr_, 2600.0f, 0.7f, treb_db * pre_scale);

        for (int c = 0; c < 2; ++c) {
            bass_[c].set(amp::Svf::Type::LowShelf, host_sr_, 120.0f, 0.7f, bass_db * post_scale);
            mid_[c].set(amp::Svf::Type::Bell, host_sr_, 750.0f, 0.8f, mid_db * post_scale);
            treb_[c].set(amp::Svf::Type::HighShelf, host_sr_, 3200.0f, 0.7f, treb_db * post_scale);
        }
    }
    void onProcessAudio(AudioBlock& b) override {
        const uint32_t n = std::min<uint32_t>(b.num_frames, kMaxBlockSize);
        const std::shared_ptr<nam::DSP> dsp = dsp_;
        if (!dsp) {
            b.copyInToOut();
            return;
        }
        std::array<float, kMaxBlockSize> x{};
        std::array<float, kMaxBlockSize> gate_gains{};
        std::array<float, kMaxBlockSize> sag_post{};
        const float* r = (b.num_channels > 1 && b.in[1]) ? b.in[1] : b.in[0];
        const bool use_pre_eq = (stack_react_ > 0.001f);
        const bool use_sag = (power_sag_ > 0.001f);
        const uint32_t gate_delay = std::min<uint32_t>(latencySamples(), kGateRing - 1);

        for (uint32_t i = 0; i < n; ++i) {
            const float di = 0.5f * (b.in[0][i] + r[i]);
            // The gate detects on the clean DI BEFORE the capture; its gain is applied AFTER the capture
            // (below), delayed by the rate-bridge latency so it lines up with the model output. Learned
            // idle hiss is muted while the DI-side hysteresis keeps sustaining notes open.
            gate_ring_[gate_ring_pos_] = gate_.detect(di);
            gate_gains[i] = gate_ring_[(gate_ring_pos_ + kGateRing - gate_delay) & (kGateRing - 1)];
            gate_ring_pos_ = (gate_ring_pos_ + 1) & (kGateRing - 1);
            float pre = di * in_gain_;
            if (use_pre_eq) {
                pre = pre_treb_.process(pre_mid_.process(pre_bass_.process(pre)));
            }
            if (use_sag) {
                const float e = std::fabs(pre);
                sag_env_ += (e > sag_env_ ? sag_att_ : sag_rel_) * (e - sag_env_);
                const float droop = 1.0f / (1.0f + power_sag_ * 0.65f * sag_env_);
                pre *= (1.0f + 0.18f * power_sag_ * sag_env_); // push tube grid slightly on attack
                sag_post[i] = droop;                           // B+ supply rail compression
            } else {
                sag_post[i] = 1.0f;
            }
            x[i] = pre;
        }
        std::array<float, kMaxBlockSize> y{};
        if (!resampled_) {
            float* ip = x.data();
            float* op = y.data();
            dsp->process(&ip, &op, static_cast<int>(n));
        } else {
            // host → capture rate (Kaiser band-limited) → NAM → host rate (anti-imaging)
            mid_in_.clear();
            up_.process(x.data(), n, mid_in_);
            mid_out_.resize(mid_in_.size(), 0.0f); // within reserved capacity
            if (!mid_in_.empty()) {
                float* ip = mid_in_.data();
                float* op = mid_out_.data();
                dsp->process(&ip, &op, static_cast<int>(mid_in_.size()));
            }
            down_.process(mid_out_.data(), mid_out_.size(), pending_);
            const std::size_t take = std::min<std::size_t>(n, pending_.size());
            for (std::size_t i = 0; i < take; ++i) y[n - take + i] = pending_[i];
            pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(take));
            if (pending_.size() > 64) pending_.erase(pending_.begin(), pending_.end() - 64);
        }
        for (uint32_t i = 0; i < n; ++i) {
            float v = y[i] * out_gain_ * gate_gains[i] * sag_post[i];
            v = treb_[0].process(mid_[0].process(bass_[0].process(v)));
            if (b.out[0]) b.out[0][i] = v;
            if (b.num_channels > 1 && b.out[1]) b.out[1][i] = v;
        }
    }

private:
    static constexpr uint32_t kGateRing = 1024;

    // Control thread: design both bridge filters for the current host / capture rates.
    void configureBridge() {
        resampled_ = std::abs(model_sr_ - host_sr_) >= 1.0;
        up_.configure(host_sr_, model_sr_);
        down_.configure(model_sr_, host_sr_);
        // Integer ratios produce exactly one host sample per host sample in every block; fractional
        // ratios can come up a sample or two short, so a 2-sample FIFO cushion keeps the stream gapless.
        const double a = std::max(host_sr_, model_sr_) / std::min(host_sr_, model_sr_);
        cushion_ = (resampled_ && std::abs(a - std::round(a)) > 1e-9) ? 2u : 0u;
        resetBridge();
    }
    void resetBridge() noexcept {
        up_.reset();
        down_.reset();
        pending_.clear();
        for (uint32_t i = 0; i < cushion_; ++i) pending_.push_back(0.0f);
        gate_ring_.fill(1.0f);
        gate_ring_pos_ = 0;
    }
    std::size_t slimOptionCount() const {
        return slim_breakpoints_.empty() ? 5u : slim_breakpoints_.size() + 1;
    }
    // Option 0 = smallest network … last = full size. With breakpoints, pick the middle of each band.
    double slimValueFor(std::size_t option) const {
        const std::size_t opts = slimOptionCount();
        option = std::min(option, opts - 1);
        if (slim_breakpoints_.empty()) return static_cast<double>(option) / static_cast<double>(opts - 1);
        if (option + 1 == opts) return 1.0;
        const double lo = option == 0 ? 0.0 : slim_breakpoints_[option - 1];
        const double hi = slim_breakpoints_[option];
        return 0.5 * (lo + hi);
    }

    amp::HystereticNoiseGate gate_{};
    std::array<float, kGateRing> gate_ring_{};
    uint32_t gate_ring_pos_ = 0;
    std::shared_ptr<nam::DSP> dsp_;
    std::string loaded_path_, loaded_label_;
    double model_sr_ = 48000.0, loudness_db_ = -18.0;
    bool has_input_level_dbu_ = false, has_output_level_dbu_ = false;
    double input_level_dbu_ = 0.0, output_level_dbu_ = 0.0;
    bool cal_enabled_ = false;
    float ref_dbu_ = 0.0f, cal_in_db_ = 0.0f, cal_out_db_ = 0.0f;
    bool is_slimmable_ = false, is_conditioned_ = false;
    std::vector<double> slim_breakpoints_;
    float slim_ratio_ = 1.0f, applied_slim_ = -1.0f;
    float stack_react_ = 0.0f, power_sag_ = 0.0f, sag_env_ = 0.0f;
    float sag_att_ = 0.002f, sag_rel_ = 0.0002f;
    uint32_t max_block_ = 4096;
    float in_gain_ = 1.0f, out_gain_ = 1.0f, gate_thr_ = 0.0f;
    bool resampled_ = false;
    uint32_t cushion_ = 0;
    BandlimitedResampler up_, down_;
    std::vector<float> mid_in_, mid_out_, pending_;
    amp::Svf pre_bass_{}, pre_mid_{}, pre_treb_{};
    std::array<amp::Svf, 2> bass_{}, mid_{}, treb_{};
};

} // namespace

std::unique_ptr<IComponent> makeNamComponent(const catalog::ComponentSchema& s) { return std::make_unique<NamComponent>(s); }
std::string namCaptureFolder() { return captureDir(); }
void rescanNamCaptures() { captures(true); }
bool namCaptureWantsCab(int index) {
    const auto& c = captures();
    if (index < 0 || index >= static_cast<int>(c.size())) return true;
    const auto& g = c[static_cast<std::size_t>(index)].gear_type;
    return !(g == "amp_cab" || g == "amp_pedal_cab" || g == "pedal" || g == "preamp" || g == "studio");
}

} // namespace rig
