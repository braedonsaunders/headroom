#pragma once
// CoreAudio device layer for the standalone app: device enumeration, independent input/output devices
// (aggregate-free, bridged by a lock-free ring buffer with drift control), sample-rate / buffer-size
// control, latency reporting and microphone-permission handling.
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rig::audio {

struct DeviceInfo {
    uint32_t id = 0;
    std::string name;
    int input_channels = 0;
    int output_channels = 0;
    std::vector<double> sample_rates;
};

struct AudioSettings {
    std::string source = "player";   // "player" | "live"
    uint32_t input_id = 0;           // 0 = system default
    uint32_t output_id = 0;          // 0 = system default
    int input_channel = 0;           // first channel of the pair (0-based)
    std::string input_mode = "mono"; // "mono" | "stereo"
    double sample_rate = 48000.0;
    int buffer_size = 256;
    float input_gain_db = 0.0f;
    float rig_input_db = 0.0f;       // top-bar IN: level into the rig (player and live), −24…+12 dB
    float output_gain_db = 0.0f;     // top-bar OUT: master output level, −60…+12 dB (−60 = off)
    bool monitor = true;
#if defined(_WIN32) && RIG_WITH_ASIO
    std::string backend = "default";
    std::string asio_driver;         // registry name, empty selects the first installed driver
#endif
};

// Render callback: `in` holds the live input (already channel-selected, stereo, may be silent when
// source=player); fill `outL/outR` with `n` frames.
using RenderFn = std::function<void(const float* inL, const float* inR, float* outL, float* outR, uint32_t n)>;

class AudioIO {
public:
    AudioIO();
    ~AudioIO();

    static std::vector<DeviceInfo> devices();
#if defined(_WIN32) && RIG_WITH_ASIO
    static std::vector<std::string> asioDrivers();
    std::string activeAsioDriver() const;
    int asioInputChannels() const;
    std::string asioError() const;
    bool openAsioControlPanel(std::string* err);
#endif
    static uint32_t defaultInputDevice();
    static uint32_t defaultOutputDevice();

    // (Re)starts audio with these settings. `live_input` opens the input device too.
    bool start(const AudioSettings& s, bool live_input, RenderFn fn, std::string* err);
    void stop();

    bool running() const { return running_.load(); }
    bool inputRunning() const { return input_ok_.load(); }
    double sampleRate() const { return sample_rate_; }
    int bufferSize() const { return buffer_size_; }
    uint32_t outputDevice() const { return out_dev_; }
    uint32_t inputDevice() const { return in_dev_; }
    double latencyMs() const { return latency_ms_; }
    uint64_t underruns() const { return underruns_.load(); }
    // Device-reported processor overloads (CoreAudio kAudioDeviceProcessorOverload: an IO cycle the render missed);
    // 0 where the backend has no such notification. Counted on a CoreAudio notification thread, never in the render.
    uint64_t overloads() const { return overloads_.load(); }

    static std::string micPermission();       // granted | denied | undetermined | restricted
    static void requestMicPermission();

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> running_{false};
    std::atomic<bool> input_ok_{false};
    std::atomic<uint64_t> underruns_{0};
    std::atomic<uint64_t> overloads_{0};
    double sample_rate_ = 48000.0;
    uint32_t out_dev_ = 0, in_dev_ = 0;
#if defined(_WIN32) && RIG_WITH_ASIO
    std::atomic<int> buffer_size_{256};
    std::atomic<double> latency_ms_{0.0};
#else
    int buffer_size_ = 256;
    double latency_ms_ = 0.0;
#endif
    friend struct Impl;
};

} // namespace rig::audio
