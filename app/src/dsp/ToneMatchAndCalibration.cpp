#include "ToneMatchAndCalibration.hpp"
#include "../audio/WavIO.hpp"
#include "../platform/Paths.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <numbers>
#include <sstream>

namespace rig {

namespace {

using catalog::ParamKind;
using catalog::ParamSchema;
namespace fs = std::filesystem;

std::mutex g_user_ir_mutex;
std::vector<UserIrEntry> g_user_irs;
bool g_user_irs_scanned = false;

std::string jsonEscape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (static_cast<unsigned char>(c) < 0x20) out += ' ';
        else out += c;
    }
    return out;
}

std::string sanitizeFilename(std::string_view name) {
    std::string out;
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_') {
            out += c;
        } else {
            out += '_';
        }
    }
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out.empty() ? "Custom_Cab_IR" : out;
}

// Synthesizes a physicallyvoiced minimum-phase cabinet impulse response so the User IRs folder
// has ready-to-play open-source starter IRs on first launch.
void seedStarterUserIrs(const fs::path& dir) {
    struct StarterSpec {
        const char* filename;
        double hp_hz;
        double thump_hz;
        double thump_db;
        double scoop_hz;
        double scoop_db;
        double pres_hz;
        double pres_db;
        double lp_hz;
    };
    static constexpr StarterSpec kStarters[] = {
        {"Headroom Cali Oversized 4x12 V30 (SM57 + R121).wav", 78.0, 118.0, 3.8, 480.0, -3.2, 2850.0, 4.2, 5600.0},
        {"Headroom British Greenback 4x12 (Blend).wav",        86.0, 130.0, 2.9, 620.0, -1.8, 2450.0, 3.5, 5200.0},
        {"Headroom Boutique Open-Back 2x12 Alnico.wav",        92.0, 145.0, 2.2, 520.0, -2.1, 3200.0, 3.8, 6100.0},
    };

    const double sr = 48000.0;
    const uint32_t len = 2048;

    for (const auto& sp : kStarters) {
        fs::path target = dir / sp.filename;
        if (fs::exists(target)) continue;

        dsp::BiquadFilter hp1, hp2, thump, scoop, pres, cone_notch, lp1, lp2;
        hp1.setHighPass(sr, sp.hp_hz, 0.707);
        hp2.setHighPass(sr, sp.hp_hz * 0.85, 0.85);
        thump.setPeakingEQ(sr, sp.thump_hz, 1.35, sp.thump_db);
        scoop.setPeakingEQ(sr, sp.scoop_hz, 1.1, sp.scoop_db);
        pres.setPeakingEQ(sr, sp.pres_hz, 1.5, sp.pres_db);
        cone_notch.setPeakingEQ(sr, 4150.0, 4.2, -5.5);
        lp1.setLowPass(sr, sp.lp_hz, 0.85);
        lp2.setLowPass(sr, sp.lp_hz * 1.18, 0.707);

        std::vector<float> ir_mono(len, 0.0f);
        float peak = 0.0f;
        for (uint32_t i = 0; i < len; ++i) {
            float x = (i == 0) ? 1.0f : 0.0f;
            // Subtle speaker cone breakup & cabinet wood reflection modes
            if (i == 19) x += 0.14f;
            if (i == 43) x -= 0.08f;
            if (i == 97) x += 0.04f;
            float y = hp2.process(hp1.process(x));
            y = thump.process(y);
            y = scoop.process(y);
            y = pres.process(y);
            y = cone_notch.process(y);
            y = lp2.process(lp1.process(y));
            // Smooth exponential cabinet decay window
            const float env = std::exp(-static_cast<float>(i) / 420.0f);
            y *= env;
            ir_mono[i] = y;
            peak = std::max(peak, std::fabs(y));
        }
        if (peak > 1e-6f) {
            const float norm = 0.85f / peak;
            for (float& s : ir_mono) s *= norm;
        }
        audio::writeWavFile16(target.string(), 48000, ir_mono, ir_mono);
    }
}

constexpr ParamSchema kToneMatchParams[] = {
    {"preset",   "Target Profile", ParamKind::Float, 0.0f, 0.0f, 6.0f, "", ""},
    {"amount",   "Match Amount",   ParamKind::Float, 0.75f, 0.0f, 1.0f, "", ""},
    {"smooth",   "Smoothing",      ParamKind::Float, 0.35f, 0.0f, 1.0f, "", ""},
    {"low_trim", "Low Body",       ParamKind::Float, 0.50f, 0.0f, 1.0f, "", ""}, // -6..+6 dB
    {"high_trim","High Air",       ParamKind::Float, 0.50f, 0.0f, 1.0f, "", ""}, // -6..+6 dB
    {"level",    "Output Level",   ParamKind::Float, 0.50f, 0.0f, 1.0f, "", ""}, // -12..+12 dB
    {"Pwr",      "On/Off",         ParamKind::Float, 1.0f, 0.0f, 1.0f, "", ""},
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 1. User IR Folder & IR Baking Implementation
// ─────────────────────────────────────────────────────────────────────────────
std::string userIrFolder() {
    fs::path dir = fs::path(platform::appDataDir()) / "IRs";
    std::error_code ec;
    fs::create_directories(dir, ec);
    seedStarterUserIrs(dir);
    return dir.string();
}

std::string exportedIrFolder() {
    fs::path dir = fs::path(platform::appDataDir()) / "Exported IRs";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir.string();
}

void rescanUserIrs() {
    std::lock_guard<std::mutex> lock(g_user_ir_mutex);
    g_user_irs.clear();
    const fs::path root(userIrFolder());
    std::error_code ec;
    if (!fs::exists(root, ec)) {
        g_user_irs_scanned = true;
        return;
    }

    std::vector<fs::path> wavs;
    for (auto it = fs::recursive_directory_iterator(root, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        auto ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".wav") {
            wavs.push_back(it->path());
        }
    }
    std::sort(wavs.begin(), wavs.end());

    for (const auto& p : wavs) {
        fs::path rel = fs::relative(p, root, ec);
        if (ec || rel.empty()) rel = p.filename();
        UserIrEntry e;
        e.rel_path = rel.generic_string();
        e.id = "user:" + e.rel_path;
        e.full_path = p.string();
        e.label = p.stem().string();
        e.group = rel.has_parent_path() && rel.parent_path() != "."
                    ? ("User IRs · " + rel.parent_path().filename().string())
                    : "User IRs";
        g_user_irs.push_back(std::move(e));
    }
    g_user_irs_scanned = true;
}

std::vector<UserIrEntry> listUserIrs() {
    {
        std::lock_guard<std::mutex> lock(g_user_ir_mutex);
        if (g_user_irs_scanned) return g_user_irs;
    }
    rescanUserIrs();
    std::lock_guard<std::mutex> lock(g_user_ir_mutex);
    return g_user_irs;
}

std::string userIrsJson() {
    const auto list = listUserIrs();
    std::ostringstream oss;
    oss << "{\"folder\":\"" << jsonEscape(userIrFolder()) << "\""
        << ",\"export_folder\":\"" << jsonEscape(exportedIrFolder()) << "\""
        << ",\"count\":" << list.size()
        << ",\"irs\":[";
    for (std::size_t i = 0; i < list.size(); ++i) {
        const auto& e = list[i];
        if (i > 0) oss << ",";
        oss << "{\"index\":" << i
            << ",\"id\":\"" << jsonEscape(e.id) << "\""
            << ",\"label\":\"" << jsonEscape(e.label) << "\""
            << ",\"group\":\"" << jsonEscape(e.group) << "\""
            << ",\"rel_path\":\"" << jsonEscape(e.rel_path) << "\"}";
    }
    oss << "]}";
    return oss.str();
}

bool bakeUnitIrToWav(IComponent& unit, double sample_rate, const std::string& name,
                     bool also_add_to_user_irs, std::string& out_path, std::string& out_err) {
    const double sr = sample_rate > 8000.0 ? sample_rate : 48000.0;
    constexpr uint32_t kIrFrames = 4096;
    constexpr uint32_t kBlockFrames = 256;

    unit.prepare(sr, kBlockFrames);
    unit.reset();
    unit.processControlRate();

    std::vector<float> out_l(kIrFrames, 0.0f);
    std::vector<float> out_r(kIrFrames, 0.0f);

    std::array<float, kBlockFrames> in_l{};
    std::array<float, kBlockFrames> in_r{};
    std::array<float, kBlockFrames> blk_l{};
    std::array<float, kBlockFrames> blk_r{};

    for (uint32_t offset = 0; offset < kIrFrames; offset += kBlockFrames) {
        in_l.fill(0.0f);
        in_r.fill(0.0f);
        if (offset == 0) {
            in_l[0] = 1.0f;
            in_r[0] = 1.0f;
        }
        const float* in_ptrs[2] = {in_l.data(), in_r.data()};
        float* out_ptrs[2] = {blk_l.data(), blk_r.data()};

        AudioBlock block{};
        block.in = in_ptrs;
        block.out = out_ptrs;
        block.num_channels = 2;
        block.num_frames = kBlockFrames;
        block.sample_rate = sr;
        block.bpm = 120.0;

        unit.processAudio(block);

        std::memcpy(out_l.data() + offset, blk_l.data(), kBlockFrames * sizeof(float));
        std::memcpy(out_r.data() + offset, blk_r.data(), kBlockFrames * sizeof(float));
    }

    float peak = 0.0f;
    for (uint32_t i = 0; i < kIrFrames; ++i) {
        peak = std::max(peak, std::max(std::fabs(out_l[i]), std::fabs(out_r[i])));
    }
    if (peak < 1e-7f) {
        out_err = "Rendered impulse response was silent";
        return false;
    }

    const float norm = 0.89f / peak;
    for (uint32_t i = 0; i < kIrFrames; ++i) {
        out_l[i] *= norm;
        out_r[i] *= norm;
    }

    const uint32_t sr_u32 = static_cast<uint32_t>(std::lround(sr));
    const std::string clean = sanitizeFilename(name) + ".wav";
    const fs::path export_path = fs::path(exportedIrFolder()) / clean;
    if (!audio::writeWavFile16(export_path.string(), sr_u32, out_l, out_r)) {
        out_err = "Could not write WAV file to " + export_path.string();
        return false;
    }
    out_path = export_path.string();

    if (also_add_to_user_irs) {
        const fs::path user_path = fs::path(userIrFolder()) / clean;
        audio::writeWavFile16(user_path.string(), sr_u32, out_l, out_r);
        rescanUserIrs();
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. Auto Input Gain Calibrator Implementation
// ─────────────────────────────────────────────────────────────────────────────
void InputCalibrator::observeBlock(const float* in_l, const float* in_r, uint32_t n, double sample_rate) noexcept {
    if (!in_l || n == 0) return;
    float blk_peak = 0.0f;
    double sum_sq = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
        const float l = in_l[i];
        const float r = in_r ? in_r[i] : l;
        const float v = std::max(std::fabs(l), std::fabs(r));
        blk_peak = std::max(blk_peak, v);
        sum_sq += static_cast<double>(v * v);
    }
    const float blk_rms_sq = static_cast<float>(sum_sq / static_cast<double>(n));

    // Exponential decay over ~3.5 seconds so recent strums dominate
    const float dt = static_cast<float>(n) / static_cast<float>(std::max(8000.0, sample_rate));
    const float decay = std::exp(-dt / 3.5f);

    const float prev_p = rolling_peak_.load(std::memory_order_relaxed);
    rolling_peak_.store(std::max(blk_peak, prev_p * decay), std::memory_order_relaxed);

    const float prev_r = rolling_rms_sq_.load(std::memory_order_relaxed);
    if (blk_peak > 0.002f) {
        rolling_rms_sq_.store(0.85f * prev_r + 0.15f * blk_rms_sq, std::memory_order_relaxed);
    } else {
        rolling_rms_sq_.store(prev_r * decay, std::memory_order_relaxed);
    }
    observed_frames_.fetch_add(n, std::memory_order_relaxed);
}

void InputCalibrator::resetWindow() noexcept {
    rolling_peak_.store(0.0f, std::memory_order_relaxed);
    rolling_rms_sq_.store(0.0f, std::memory_order_relaxed);
    observed_frames_.store(0, std::memory_order_relaxed);
}

CalibrationResult InputCalibrator::computeRecommendation(float current_gain_db) const noexcept {
    CalibrationResult r;
    const float peak = rolling_peak_.load(std::memory_order_relaxed);
    const float rms = std::sqrt(std::max(0.0f, rolling_rms_sq_.load(std::memory_order_relaxed)));

    if (peak < 0.001f) {
        r.ok = false;
        r.measured_peak_dbfs = -60.0f;
        r.measured_rms_dbfs = -72.0f;
        r.recommended_gain_db = current_gain_db;
        r.recommended_voltage_scale = 1.0f;
        r.pickup_profile = "No Signal Detected";
        r.message = "Strum a loud chord on your guitar (or play the DI track) and click Calibrate.";
        return r;
    }

    r.ok = true;
    r.measured_peak_dbfs = 20.0f * std::log10(std::max(peak, 1e-5f));
    r.measured_rms_dbfs = 20.0f * std::log10(std::max(rms, 1e-6f));
    r.crest_db = std::clamp(r.measured_peak_dbfs - r.measured_rms_dbfs, 3.0f, 24.0f);

    // Target peak input into the rig is -6.0 dBFS (leaves 6 dB clean transient headroom while hitting
    // 12AX7 cathode/grid stages and NAM captures at unity calibration).
    constexpr float kTargetPeakDbfs = -6.0f;
    const float delta_db = kTargetPeakDbfs - r.measured_peak_dbfs;
    r.recommended_gain_db = std::clamp(std::round((current_gain_db + delta_db) * 2.0f) * 0.5f, -18.0f, 18.0f);

    if (r.measured_peak_dbfs > -2.5f || r.crest_db < 8.5f) {
        r.pickup_profile = "Hot / Active Humbucker";
        r.recommended_voltage_scale = 2.0f;
        r.message = "Hot pickup output detected — trimmed input gain for clean headroom and tight palm mutes.";
    } else if (r.measured_peak_dbfs < -16.0f || r.crest_db > 14.5f) {
        r.pickup_profile = "Vintage Single-Coil";
        r.recommended_voltage_scale = 0.5f;
        r.message = "Dynamic single-coil transient crest detected — boosted input gain to hit tube sweet spot.";
    } else {
        r.pickup_profile = "Standard Humbucker / P90";
        r.recommended_voltage_scale = 1.0f;
        r.message = "Calibrated to -6.0 dBFS optimal tube & NAM reference level.";
    }
    return r;
}

std::string InputCalibrator::toJson(float current_gain_db) const {
    const auto c = computeRecommendation(current_gain_db);
    std::ostringstream oss;
    oss << "{\"ok\":" << (c.ok ? "true" : "false")
        << ",\"measured_peak_dbfs\":" << c.measured_peak_dbfs
        << ",\"measured_rms_dbfs\":" << c.measured_rms_dbfs
        << ",\"crest_db\":" << c.crest_db
        << ",\"recommended_gain_db\":" << c.recommended_gain_db
        << ",\"recommended_voltage_scale\":" << c.recommended_voltage_scale
        << ",\"pickup_profile\":\"" << jsonEscape(c.pickup_profile) << "\""
        << ",\"message\":\"" << jsonEscape(c.message) << "\"}";
    return oss.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. Tone Match EQ Analysis & Component (table 900203)
// ─────────────────────────────────────────────────────────────────────────────
std::array<float, ToneMatchCurve::kNumBands> analyzeEightBandSpectrum(
    const float* samples, std::size_t num_frames, double sample_rate) noexcept {
    std::array<float, ToneMatchCurve::kNumBands> db{};
    if (!samples || num_frames < 64) return db;

    const double sr = sample_rate > 8000.0 ? sample_rate : 48000.0;
    std::array<double, ToneMatchCurve::kNumBands> energy{};

    for (std::size_t b = 0; b < ToneMatchCurve::kNumBands; ++b) {
        dsp::BiquadFilter bp;
        bp.setPeakingEQ(sr, ToneMatchCurve::kCenterHz[b], 1.4, 12.0);
        dsp::BiquadFilter lp, hp;
        hp.setHighPass(sr, ToneMatchCurve::kCenterHz[b] * 0.707, 0.707);
        lp.setLowPass(sr, std::min(sr * 0.45, ToneMatchCurve::kCenterHz[b] * 1.414), 0.707);

        double sum = 0.0;
        std::size_t count = 0;
        for (std::size_t i = 0; i < num_frames; ++i) {
            const float x = samples[i];
            const float y = lp.process(hp.process(x));
            if (std::fabs(x) > 1e-4f) {
                sum += static_cast<double>(y * y);
                ++count;
            }
        }
        const double rms = count > 0 ? std::sqrt(sum / static_cast<double>(count)) : 1e-6;
        energy[b] = 20.0 * std::log10(std::max(rms, 1e-6));
    }

    double mean = 0.0;
    for (double e : energy) mean += e;
    mean /= static_cast<double>(ToneMatchCurve::kNumBands);

    for (std::size_t b = 0; b < ToneMatchCurve::kNumBands; ++b) {
        db[b] = static_cast<float>(std::clamp(energy[b] - mean, -18.0, 18.0));
    }
    return db;
}

ToneMatchCurve computeToneMatch(
    const float* source_samples, std::size_t source_frames, double source_sr,
    const float* target_samples, std::size_t target_frames, double target_sr,
    const std::string& name) {
    ToneMatchCurve out;
    out.profile_name = name;
    const auto src_env = analyzeEightBandSpectrum(source_samples, source_frames, source_sr);
    const auto tgt_env = analyzeEightBandSpectrum(target_samples, target_frames, target_sr);
    for (std::size_t b = 0; b < ToneMatchCurve::kNumBands; ++b) {
        out.delta_db[b] = std::clamp(tgt_env[b] - src_env[b], -12.0f, 12.0f);
    }
    return out;
}

ToneMatchCurve builtinReferenceTarget(int preset_index) noexcept {
    // 8 bands: 85, 180, 380, 800, 1650, 3400, 6500, 11000 Hz
    switch (std::clamp(preset_index, 0, 6)) {
        case 0: // Modern Metal Chug (Tight sub, punchy 120Hz, carved 380Hz mud, aggressive 1.6-3.4k bite, controlled fizz)
            return {{+2.8f, +2.2f, -3.8f, +1.2f, +3.6f, +2.9f, -1.8f, -4.5f}, "Modern Metal Chug"};
        case 1: // 80s Arena Lead (Warm mids, singing 800Hz-1.6k sustain, smooth top end)
            return {{-2.0f, +1.5f, +1.8f, +4.2f, +3.5f, +1.2f, -1.5f, -3.0f}, "80s Arena Lead"};
        case 2: // Mid-Forward Djent / Prog (Surgical low cut, upper-mid clank @ 1.65kHz)
            return {{-3.5f, +1.8f, -3.2f, +2.6f, +4.8f, +2.2f, -1.0f, -3.8f}, "Mid-Forward Prog / Djent"};
        case 3: // Edge-of-Breakup Texas Blues (Woody 380Hz body, glassy 3.4-6.5k bell chime)
            return {{+1.2f, +2.4f, +2.2f, +0.8f, -1.2f, +2.8f, +3.4f, +1.5f}, "Edge-of-Breakup Blues"};
        case 4: // Studio Mix-Ready Rhythm (De-mudded low-mids, notched 3.8k harshness, open air)
            return {{+1.5f, +1.0f, -2.8f, +1.5f, +2.2f, -1.6f, +1.8f, -2.0f}, "Mix-Ready Studio Rhythm"};
        case 5: // Vintage British Stack (Roaring 800Hz-1.65k bark, classic greenback roll-off)
            return {{+1.0f, +2.5f, +0.5f, +3.4f, +3.8f, +1.5f, -2.5f, -5.0f}, "Vintage British Stack"};
        default: // Custom / Flat
            return {{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}, "Custom Reference Match"};
    }
}

class ToneMatchComponent final : public SchemaComponentBase {
public:
    explicit ToneMatchComponent(const catalog::ComponentSchema& schema)
        : SchemaComponentBase(schema) {
        custom_curve_ = builtinReferenceTarget(0);
    }

    // Series pipelining (RackGraph): both channels run the same filters on the same state.
    bool monoPreserving() const noexcept override { return true; }

    std::vector<ExtraParam> extraParams() const override {
        return {
            {"preset", "Reference Profile", {
                "Modern Metal Chug",
                "80s Arena Lead",
                "Mid-Forward Prog / Djent",
                "Edge-of-Breakup Blues",
                "Mix-Ready Studio Rhythm",
                "Vintage British Stack",
                "Custom Reference Match"
            }, 0.0f, {}}
        };
    }

    void setCustomCurve(const ToneMatchCurve& c) {
        custom_curve_ = c;
        has_custom_ = true;
        setParameter("preset", 6.0f);
        rebuildFilters();
    }

protected:
    void onPrepare(double sample_rate, uint32_t /*max_block_size*/) override {
        host_sr_ = sample_rate > 8000.0 ? sample_rate : 48000.0;
        rebuildFilters();
    }

    void onReset() override {
        for (auto& f : bands_l_) f.reset();
        for (auto& f : bands_r_) f.reset();
        low_shelf_l_.reset();
        low_shelf_r_.reset();
        high_shelf_l_.reset();
        high_shelf_r_.reset();
    }

    void onParameterChanged(std::string_view /*param_id*/, float /*value*/) override {
        rebuildFilters();
    }

    void onProcessAudio(AudioBlock& block) override {
        block.copyInToOut();
        if (!block.out || !block.out[0] || block.num_frames == 0) return;

        float* l = block.out[0];
        float* r = (block.num_channels > 1 && block.out[1]) ? block.out[1] : nullptr;
        const float gain = out_gain_;

        for (uint32_t i = 0; i < block.num_frames; ++i) {
            float yl = l[i];
            for (auto& b : bands_l_) yl = b.process(yl);
            yl = high_shelf_l_.process(low_shelf_l_.process(yl)) * gain;
            l[i] = yl;

            if (r) {
                float yr = r[i];
                for (auto& b : bands_r_) yr = b.process(yr);
                yr = high_shelf_r_.process(low_shelf_r_.process(yr)) * gain;
                r[i] = yr;
            }
        }
    }

private:
    void rebuildFilters() noexcept {
        const int p_idx = std::clamp(static_cast<int>(std::lround(getParamOr("preset", 0.0f))), 0, 6);
        const ToneMatchCurve base = (p_idx == 6 && has_custom_) ? custom_curve_ : builtinReferenceTarget(p_idx);
        const float amt = std::clamp(getParamOr("amount", 0.75f), 0.0f, 1.0f);
        const float smooth = std::clamp(getParamOr("smooth", 0.35f), 0.0f, 1.0f);
        const float low_db = (getParamOr("low_trim", 0.5f) - 0.5f) * 12.0f;
        const float high_db = (getParamOr("high_trim", 0.5f) - 0.5f) * 12.0f;
        const float lvl_db = (getParamOr("level", 0.5f) - 0.5f) * 24.0f;
        out_gain_ = std::pow(10.0f, lvl_db / 20.0f);

        const double q = 1.65 - 0.85 * static_cast<double>(smooth);
        for (std::size_t b = 0; b < ToneMatchCurve::kNumBands; ++b) {
            float db = base.delta_db[b];
            if (smooth > 0.05f && b > 0 && b + 1 < ToneMatchCurve::kNumBands) {
                const float avg = 0.25f * base.delta_db[b - 1] + 0.5f * base.delta_db[b] + 0.25f * base.delta_db[b + 1];
                db = (1.0f - smooth) * db + smooth * avg;
            }
            db *= amt;
            bands_l_[b].setPeakingEQ(host_sr_, ToneMatchCurve::kCenterHz[b], q, db);
            bands_r_[b].setPeakingEQ(host_sr_, ToneMatchCurve::kCenterHz[b], q, db);
        }

        low_shelf_l_.setLowShelf(host_sr_, 140.0, low_db);
        low_shelf_r_.setLowShelf(host_sr_, 140.0, low_db);
        high_shelf_l_.setHighShelf(host_sr_, 4800.0, high_db);
        high_shelf_r_.setHighShelf(host_sr_, 4800.0, high_db);
    }

    ToneMatchCurve custom_curve_{};
    bool has_custom_ = false;
    float out_gain_ = 1.0f;
    std::array<dsp::BiquadFilter, ToneMatchCurve::kNumBands> bands_l_{}, bands_r_{};
    dsp::BiquadFilter low_shelf_l_{}, low_shelf_r_{};
    dsp::BiquadFilter high_shelf_l_{}, high_shelf_r_{};
};

const catalog::ComponentSchema& toneMatchUnitSchema() noexcept {
    static const catalog::ComponentSchema s{
        kToneMatchUnitTable,
        "Tone Match EQ",
        "8-Band Reference Spectral Matcher",
        "HeadroomToneMatch",
        catalog::EngineFamily::ModernDSP,
        -1,
        false,
        std::span<const ParamSchema>(kToneMatchParams)
    };
    return s;
}

std::unique_ptr<IComponent> makeToneMatchUnit(const catalog::ComponentSchema& schema) {
    return std::make_unique<ToneMatchComponent>(schema);
}

bool isToneMatchUnit(const IComponent& c) noexcept {
    return dynamic_cast<const ToneMatchComponent*>(&c) != nullptr;
}

bool applyToneMatchCurve(IComponent& unit, const ToneMatchCurve& curve) {
    auto* tm = dynamic_cast<ToneMatchComponent*>(&unit);
    if (!tm) return false;
    tm->setCustomCurve(curve);
    return true;
}

} // namespace rig
