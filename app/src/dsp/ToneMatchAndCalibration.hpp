#pragma once
// 1. Custom User .wav IR library (`~/Library/Application Support/Headroom/IRs`) & Baked IR .wav Exporter
// 2. One-Click Auto Input Gain Calibrator (`InputCalibrator`)
// 3. AI Reference Tone Match EQ unit (table 900203) & spectral matcher (`/api/tonematch`)

#include "../core/IComponent.hpp"
#include "DSPPrimitives.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rig {

inline constexpr int32_t kToneMatchUnitTable = 900203;

// ─────────────────────────────────────────────────────────────────────────────
// 1. User .wav IR Folder & IR Baking
// ─────────────────────────────────────────────────────────────────────────────
struct UserIrEntry {
    std::string id;       // "user:<rel_path>"
    std::string rel_path; // e.g. "My Cab 4x12.wav"
    std::string full_path;
    std::string label;
    std::string group;    // "User IRs" or subfolder name
};

std::string userIrFolder();
std::string exportedIrFolder();
void rescanUserIrs();
std::vector<UserIrEntry> listUserIrs();
std::string userIrsJson();

// Renders the impulse response of `unit` (e.g. a CabinetUnit with dual mics + alignment + room + cuts)
// over `num_frames` samples and writes a WAV file into `exportedIrFolder()` (and optionally `userIrFolder()`).
bool bakeUnitIrToWav(IComponent& unit, double sample_rate, const std::string& name,
                     bool also_add_to_user_irs, std::string& out_path, std::string& out_err);

// ─────────────────────────────────────────────────────────────────────────────
// 2. Auto Input Gain Calibrator
// ─────────────────────────────────────────────────────────────────────────────
struct CalibrationResult {
    bool ok = false;
    float measured_peak_dbfs = -60.0f;
    float measured_rms_dbfs = -72.0f;
    float crest_db = 12.0f;
    float recommended_gain_db = 0.0f;
    float recommended_voltage_scale = 1.0f;
    std::string pickup_profile = "Standard Humbucker / Single-Coil";
    std::string message;
};

class InputCalibrator {
public:
    void observeBlock(const float* in_l, const float* in_r, uint32_t n, double sample_rate) noexcept;
    void resetWindow() noexcept;
    CalibrationResult computeRecommendation(float current_gain_db) const noexcept;
    std::string toJson(float current_gain_db) const;

private:
    std::atomic<float> rolling_peak_{0.0f};
    std::atomic<float> rolling_rms_sq_{0.0f};
    std::atomic<uint64_t> observed_frames_{0};
};

// ─────────────────────────────────────────────────────────────────────────────
// 3. Tone Match EQ Unit (table 900203) & Reference Matcher
// ─────────────────────────────────────────────────────────────────────────────
struct ToneMatchCurve {
    static constexpr std::size_t kNumBands = 8;
    static constexpr std::array<double, kNumBands> kCenterHz = {
        85.0, 180.0, 380.0, 800.0, 1650.0, 3400.0, 6500.0, 11000.0
    };
    std::array<float, kNumBands> delta_db{}; // per-band correction in dB (-15..+15 dB)
    std::string profile_name = "Matched Reference";
};

// Computes 8-band spectral envelope (in dB, normalized to 0 dB mean) of a mono buffer.
std::array<float, ToneMatchCurve::kNumBands> analyzeEightBandSpectrum(
    const float* samples, std::size_t num_frames, double sample_rate) noexcept;

// Computes a ToneMatchCurve that transforms `source_samples` to match the spectral envelope of `target_samples`.
ToneMatchCurve computeToneMatch(
    const float* source_samples, std::size_t source_frames, double source_sr,
    const float* target_samples, std::size_t target_frames, double target_sr,
    const std::string& name = "Custom Reference");

// Built-in studio reference targets for instant 1-click tone matching even without a reference WAV file.
ToneMatchCurve builtinReferenceTarget(int preset_index) noexcept;

std::unique_ptr<IComponent> makeToneMatchUnit(const catalog::ComponentSchema& schema);
const catalog::ComponentSchema& toneMatchUnitSchema() noexcept;
bool isToneMatchUnit(const IComponent& c) noexcept;
bool applyToneMatchCurve(IComponent& unit, const ToneMatchCurve& curve);

} // namespace rig
