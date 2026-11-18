#pragma once
// Rig-level tempo.
//
// One tempo per rig drives every tempo-syncable unit (delays, LFO modulation, the trance gate, synced
// predelays, hosted plugins). The value reaches components through AudioBlock::bpm, which RackGraph
// fills once per block:
//
//   * standalone: the rig's own tempo (RackGraph::setTempo, saved with the rig, tap tempo in the UI);
//   * plugin:     the host's tempo. A plugin wrapper either fills AudioBlock::bpm itself and sets
//                 AudioBlock::host_tempo, or calls hostTempo().setTempo(bpm) from its process callback.
//                 Both paths are lock-free and allocation-free. hostTempo().clear() hands control back
//                 to the rig tempo.
//
// Components never read a clock themselves; they only read block.bpm, so offline renders, calibration
// graphs and tests stay deterministic.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>

namespace rig {

inline constexpr double kTempoMin = 20.0;
inline constexpr double kTempoMax = 300.0;
inline constexpr double kTempoDefault = 120.0;

inline double clampTempo(double bpm) noexcept {
    if (!std::isfinite(bpm) || bpm <= 0.0) return kTempoDefault;
    return std::clamp(bpm, kTempoMin, kTempoMax);
}

// A lock-free tempo source for hosts. 0 = not driving (the rig tempo applies).
class TempoClock {
public:
    void setTempo(double bpm) noexcept { bpm_.store(bpm > 0.0 ? clampTempo(bpm) : 0.0, std::memory_order_relaxed); }
    void clear() noexcept { bpm_.store(0.0, std::memory_order_relaxed); }
    // Audio thread: one relaxed load per block.
    double bpm() const noexcept { return bpm_.load(std::memory_order_relaxed); }
    bool driving() const noexcept { return bpm() > 0.0; }

private:
    std::atomic<double> bpm_{0.0};
};

// Process-wide host tempo (a plugin wrapper's single entry point). Standalone never sets it.
inline TempoClock& hostTempo() noexcept {
    static TempoClock clock;
    return clock;
}

// ───────────── note divisions shared by every synced unit ─────────────
struct NoteDivision {
    const char* label;
    double beats; // quarter notes
};

// Delay times, shortest → longest (a time knob sweeps this list when Sync is on). Dotted / triplet
// variants come from a unit's own sync mode (× 1.5 / × 2/3).
inline constexpr std::array<NoteDivision, 8> kStraightDivisions = {{
    {"1/64", 1.0 / 16.0}, {"1/32", 0.125}, {"1/16", 0.25}, {"1/8", 0.5}, {"1/4", 1.0}, {"1/2", 2.0}, {"1/1", 4.0}, {"2/1", 8.0}}};

// LFO periods, slowest → fastest (a rate knob sweeps this list when Sync is on).
inline constexpr std::array<NoteDivision, 16> kLfoDivisions = {{
    {"4 bars", 16.0}, {"2 bars", 8.0}, {"1 bar", 4.0}, {"1/2D", 3.0}, {"1/2", 2.0}, {"1/4D", 1.5}, {"1/2T", 4.0 / 3.0},
    {"1/4", 1.0}, {"1/8D", 0.75}, {"1/4T", 2.0 / 3.0}, {"1/8", 0.5}, {"1/16D", 0.375}, {"1/8T", 1.0 / 3.0},
    {"1/16", 0.25}, {"1/16T", 1.0 / 6.0}, {"1/32", 0.125}}};

// Gate / stutter steps, slowest → fastest.
inline constexpr std::array<NoteDivision, 8> kGateDivisions = {{
    {"1/2", 2.0}, {"1/4", 1.0}, {"1/4T", 2.0 / 3.0}, {"1/8", 0.5}, {"1/8T", 1.0 / 3.0}, {"1/16", 0.25}, {"1/16T", 1.0 / 6.0}, {"1/32", 0.125}}};

template <std::size_t N>
inline std::size_t divisionIndex(const std::array<NoteDivision, N>& /*table*/, float norm) noexcept {
    const float n = std::isfinite(norm) ? std::clamp(norm, 0.0f, 1.0f) : 0.5f;
    return static_cast<std::size_t>(std::lround(n * static_cast<float>(N - 1)));
}

inline double beatsToSeconds(double beats, double bpm) noexcept { return beats * 60.0 / clampTempo(bpm); }

// Synced delay in seconds, halved (staying on the grid) until it fits `max_seconds`.
inline double syncedSeconds(double beats, double bpm, double max_seconds) noexcept {
    double s = beatsToSeconds(beats, bpm);
    while (s > max_seconds && s > 1e-4) s *= 0.5;
    return s;
}

inline std::string tempoLabel(double bpm) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", clampTempo(bpm));
    std::string s(buf);
    if (s.size() > 2 && s.compare(s.size() - 2, 2, ".0") == 0) s.resize(s.size() - 2);
    return s;
}

} // namespace rig
