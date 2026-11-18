#pragma once
// Format-agnostic Headroom plugin instance. The AU (au/), VST3 (vst3/) and any future AAX wrapper are thin
// adapters around this class: they translate host buses, parameter events, transport and state blobs.
//
// Threads
//   control : host main thread (or whatever thread the host uses for lifecycle/state calls). prepare,
//             suspend, reset, state and editor calls; serialised by control_mutex_. May allocate and lock.
//   render  : the host's audio callback. process() only: no allocation, no locks, no blocking, in every
//             process mode (realtime and offline alike).
//
// Host parameters (v1). Stable ids, shared by every format; never renumber (hosts store automation by id).
//   8   Input   dB   −24..+24   plugin input trim before the rig      sample-accurate, automatable
//   9   Output  dB   −36..+12   plugin output trim after the rig      sample-accurate, automatable
//   10  Bypass  0/1             10 ms crossfade to the dry input       sample-accurate, automatable
//   Ids 0..7 are reserved for macro automation in a later version. In v1 the eight rack macros are NOT host
//   parameters: they are editor/state controls (their values travel in the plugin state), because changing
//   a macro publishes a new render plan through the engine's edit path, which can't run on the callback.
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace rig::audio { class RealtimeAudioServer; }
namespace rig { class RtPool; }

namespace headroom::plug {

enum ParamId : uint32_t {
    kInputTrim = 8,
    kOutputTrim = 9,
    kBypass = 10,
    kParamIdEnd = 11, // array bound; ids below kFirstHostParam are reserved
};
constexpr uint32_t kFirstHostParam = kInputTrim;
constexpr std::array<uint32_t, 3> kHostParams = {kInputTrim, kOutputTrim, kBypass};
inline bool isHostParam(uint32_t id) { return id >= kFirstHostParam && id < kParamIdEnd; }

struct ParamSpec {
    uint32_t id;
    const char* name;
    const char* short_name;
    const char* unit; // "", "dB"
    float min, max, def;
    bool is_bool;
};
const ParamSpec& paramSpec(uint32_t id); // id must satisfy isHostParam()

struct ParamEvent {  // a plain (not normalised) value at a sample offset within process()
    uint32_t offset;
    uint32_t id;
    float value;
};

struct Transport {
    double bpm = 0.0, beat = 0.0;
    bool playing = false;
    bool valid = false;
};

enum class Format { AU, VST3, AAX, Test };

class PluginCore {
public:
    PluginCore(Format f, std::string resource_dir);
    ~PluginCore();
    PluginCore(const PluginCore&) = delete;
    PluginCore& operator=(const PluginCore&) = delete;

    // ── control thread ──
    // Host rate + largest block; the render callback must be synchronously quiescent (AU Initialize,
    // VST3 setActive(true)). Starts the engine on first use and fully re-prepares it (fresh DSP history).
    // Returns false (stay suspended) on an invalid configuration or a failed engine preparation. A missing
    // content root, or a sample rate outside 8 kHz…384 kHz, is not a failure: the engine stays suspended
    // and the instance passes audio through dry (the editor explains a missing content root).
    bool prepare(double sample_rate, uint32_t max_frames);
    void suspend();               // AU Uninitialize / VST3 setActive(false)
    // Host reset while possibly rendering (AU Reset): rebuilds the rig from its own state off the callback
    // (fresh components, fresh lineage) and swaps it in like a preset load. Coalesced, asynchronous.
    void requestReset();
    std::string saveState();      // opaque text blob, see the .cpp
    bool loadState(const std::string& blob);

    // Editor attach: makes sure the engine runs, starts the library index and (process-wide, once) the
    // warm-up. Returns the instance's loopback API port (0 if the engine could not start).
    uint16_t attachEditor();
    std::string apiToken() const;
    std::string uiDirectory() const { return resource_dir_ + "/ui"; }
    std::string engineError() const;
    std::string settingsFilePath() const;

    // ── any thread (RT-safe) ──
    float paramValue(uint32_t id) const noexcept;               // host-visible value
    void setParamDirect(uint32_t id, float value) noexcept;     // applied at the next block start
    void setHostBypass(bool b) noexcept { host_bypass_prop_.store(b, std::memory_order_relaxed); }
    void setTransport(const Transport& t) noexcept;
    // The host's audio workgroup for this instance's render thread (an os_workgroup_t, or null): the engine's
    // parallel workers join it (AU: from the render context observer, on the render thread; RT-safe).
    void setRenderWorkgroup(void* os_workgroup) noexcept;
    struct PoolStats {             // the process-wide render pool (diagnostics; any thread)
        uint64_t forks, on_worker, reclaimed, refused;
        int32_t workers, active, joined_workgroup, workgroup_max_parallel;
    };
    PoolStats poolStats() const noexcept;

    // ── render thread ──
    // Planar buffers; inR may be null (mono input → duplicated), inL may be null (no input → silence).
    // outL/outR must be valid for n frames and may alias the inputs. n == 0 or null outputs = parameter
    // flush. `events` must be sorted by offset; events at the same offset apply in array order.
    void process(const float* inL, const float* inR, float* outL, float* outR, uint32_t n,
                 const ParamEvent* events, uint32_t num_events) noexcept;

    bool engineRunning() const noexcept { return server_live_.load(std::memory_order_acquire) != nullptr; }
    uint32_t latencySamples() const noexcept { return latency_samples_.load(std::memory_order_acquire); }
    double tailSeconds() const noexcept { return 10.0; }
    rig::audio::RealtimeAudioServer* serverForTests() const noexcept { return server_.get(); }
    uint64_t faults() const noexcept { return faults_.load(); }

private:
    struct Ramp {
        float cur = 1.0f, target = 1.0f, step = 0.0f;
        int left = 0;
        void reset(float v) noexcept { cur = target = v; left = 0; step = 0.0f; }
        void set(float t, int len) noexcept {
            if (t == target) return;
            target = t;
            left = len;
            step = (t - cur) / static_cast<float>(len);
        }
        float next() noexcept {
            if (left > 0) { cur += step; if (--left == 0) cur = target; }
            return cur;
        }
    };

    bool startEngineLocked(double sr);
    void stopEngineLocked();
    void applyEventRT(const ParamEvent& e) noexcept;
    void renderSegment(uint32_t from, uint32_t to, float* outL, float* outR) noexcept;

    const Format format_;
    const std::string resource_dir_;
    rig::RtPool* const pool_;     // process-wide render pool; retained for this instance's lifetime
    const int wg_slot_;           // this instance's workgroup slot in the pool (-1: none free)
    mutable std::mutex control_mutex_;
    std::unique_ptr<rig::audio::RealtimeAudioServer> server_;
    std::atomic<rig::audio::RealtimeAudioServer*> server_live_{nullptr}; // non-null while prepared OK
    std::string engine_error_, pending_state_;
    bool engine_failed_ = false;
    double sample_rate_ = 48000.0;
    uint32_t max_frames_ = 0;

    // render-thread scratch (sized in prepare, while quiescent)
    std::vector<float> dry_l_, dry_r_, trim_l_, trim_r_;
    // Fixed capacity keeps bypass delay allocation off the render thread.
    static constexpr uint32_t kDryRingSize = 1u << 20;
    std::vector<float> bypass_l_, bypass_r_;
    uint32_t bypass_pos_ = 0;
    std::atomic<uint32_t> latency_samples_{0};

    std::array<std::atomic<float>, kParamIdEnd> host_value_{};
    std::array<std::atomic<uint32_t>, kParamIdEnd> direct_seq_{};
    std::array<uint32_t, kParamIdEnd> direct_seen_{};   // render thread only
    std::atomic<bool> host_bypass_prop_{false};
    std::atomic<uint64_t> faults_{0};
    Ramp in_gain_, out_gain_, wet_;                      // render thread only
    bool param_bypass_ = false, last_bypass_ = false;    // render thread only
    bool faulted_ = false;                               // render thread only; cleared by prepare
};

// Checkout that holds the Headroom presets and resources. Resolution order:
// $HEADROOM_RESEARCH_ROOT, `research_root=` in ~/Library/Application Support/Headroom/plugin-settings.txt,
// then the source checkout this plugin was built from (written to that file, commented, if it's missing).
std::string resolveResearchRoot(std::string* settings_path = nullptr);

} // namespace headroom::plug
