#pragma once
// Real-time speed (tempo, pitch kept) and transpose (pitch, tempo kept) for the Practice backing-track player.
//
//   macOS   Apple's AUNewTimePitch (exactly what the player always used).
//   others  Signalsmith Stretch 1.4 (MIT, app/third_party/signalsmith-stretch), stereo, phase-vocoder based.
//
// Source audio is pulled through a callback, as an Audio Unit render callback would. prepare()/release() run on a
// control thread, never concurrently with the audio-thread calls; reset(), setParams() and render() never
// allocate, lock or block.
#include <cstdint>
#include <memory>

namespace rig::platform {

// Fills l/r with n source frames (the stretcher asks for whatever its rate needs).
using StretchPullFn = void (*)(void* ctx, float* l, float* r, uint32_t n) noexcept;

class TimeStretch {
public:
    TimeStretch();
    ~TimeStretch();
    TimeStretch(const TimeStretch&) = delete;
    TimeStretch& operator=(const TimeStretch&) = delete;

    // ---- control thread ----
    // (Re)creates the stretcher for this rate and largest render size. False when none is available.
    bool prepare(double sample_rate, uint32_t max_block, StretchPullFn pull, void* ctx);
    void release();
    bool ok() const noexcept;
    // Delay between the source read position and what render() returns, in seconds of output at speed 1.
    double latencySec() const noexcept;

    // ---- audio thread ----
    void reset() noexcept;                              // drops buffered audio (seek, re-engage)
    void setParams(float speed, float semitones) noexcept; // speed 0.5..1.5, semitones −12..12
    void render(float* l, float* r, uint32_t n) noexcept;  // n <= max_block; silence on failure

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace rig::platform
