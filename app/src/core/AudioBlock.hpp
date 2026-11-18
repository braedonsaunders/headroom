#pragma once
#include <vector>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <algorithm>

namespace rig {

inline constexpr std::size_t kMaxBlockSize = 2048;

namespace midi { struct MidiEvent; }

struct AudioBlock {
    std::vector<midi::MidiEvent>* midi = nullptr; // MIDI bus of the current lane (Pitch→MIDI writes, instruments read)
    const float* const* in = nullptr;
    float* const* out = nullptr;
    uint32_t num_channels = 2;
    uint32_t num_frames = 0;
    // A sub-block of a longer host block (RackGraph's series pipelining): its offset in that block and the block's
    // length (0 = this call is the whole block). Block-rate readouts (meters, LFO / gain-reduction lamps) update once,
    // at the end of the host block, over the whole block, so they read the same however the block was split.
    uint32_t host_offset = 0;
    uint32_t host_frames = 0;
    // RackGraph's channel decision for this unit's input, made once per host block before anything renders (the same
    // in sequential and pipelined rendering): 1 = proved one signal on both channels (the rack input was, and every
    // active unit ahead declares monoPreserving), 0 = treat as stereo, -1 = not given (a unit that picks a mono or a
    // stereo path then compares the channels of each call itself).
    int8_t mono_input = -1;
    bool hostBlockStart() const noexcept { return host_offset == 0; }
    bool hostBlockEnd() const noexcept { return host_frames == 0 || host_offset + num_frames >= host_frames; }
    uint32_t hostFrames() const noexcept { return host_frames ? host_frames : num_frames; }
    double sample_rate = 48000.0;
    // Tempo for this block. RackGraph::process() fills it from the rig tempo (or the host clock), so
    // components just read it. A plugin wrapper that sets host_tempo = true keeps its own bpm /
    // ppq_position / transport_playing (the host's) instead.
    double bpm = 120.0;
    double ppq_position = 0.0;
    bool transport_playing = false;
    bool host_tempo = false;
    // An offline render on a background thread (RackGraph's loudness calibration): never fork onto the real-time
    // pool (RtPool), whose workers belong to the audio device's workgroup and must stay free for the live render.
    bool offline = false;

    void copyInToOut() const noexcept {
        if (!in || !out || in == out) return;
        for (uint32_t c = 0; c < num_channels; ++c) {
            if (in[c] && out[c] && in[c] != out[c]) {
                std::memcpy(out[c], in[c], sizeof(float) * num_frames);
            }
        }
    }

    void clearOut() const noexcept {
        if (!out) return;
        for (uint32_t c = 0; c < num_channels; ++c) {
            if (out[c]) {
                std::memset(out[c], 0, sizeof(float) * num_frames);
            }
        }
    }

    void applyGainOut(float gain) const noexcept {
        if (!out) return;
        for (uint32_t c = 0; c < num_channels; ++c) {
            if (out[c]) {
                for (uint32_t i = 0; i < num_frames; ++i) {
                    out[c][i] *= gain;
                }
            }
        }
    }
};

struct StereoScratchBuffer {
    alignas(32) std::array<float, kMaxBlockSize> left{};
    alignas(32) std::array<float, kMaxBlockSize> right{};
    std::array<float*, 2> ptrs{left.data(), right.data()};
    std::array<const float*, 2> const_ptrs{left.data(), right.data()};

    void clear(uint32_t frames) noexcept {
        const uint32_t n = std::min<uint32_t>(frames, kMaxBlockSize);
        std::memset(left.data(), 0, sizeof(float) * n);
        std::memset(right.data(), 0, sizeof(float) * n);
    }

    void copyFrom(const float* const* src, uint32_t channels, uint32_t frames) noexcept {
        const uint32_t n = std::min<uint32_t>(frames, kMaxBlockSize);
        if (channels >= 1 && src[0]) {
            std::memcpy(left.data(), src[0], sizeof(float) * n);
        } else {
            std::memset(left.data(), 0, sizeof(float) * n);
        }
        if (channels >= 2 && src[1]) {
            std::memcpy(right.data(), src[1], sizeof(float) * n);
        } else {
            std::memcpy(right.data(), left.data(), sizeof(float) * n);
        }
    }
};

} // namespace rig
