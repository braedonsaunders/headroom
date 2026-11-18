// Portable device layer (Windows, Linux) behind the same AudioIO interface as the CoreAudio AudioIO.mm, using
// miniaudio (third_party/miniaudio, public domain / MIT-0).
//
//   * Backends: see AudioBackend.hpp (JACK → PulseAudio → ALSA on Linux, WASAPI on Windows; ASIO is a separate
//     implementation behind the same seam). miniaudio loads the Linux client libraries at run time, so the
//     binary has no hard dependency on libasound / libpulse / libjack.
//   * Device ids: a stable 32-bit hash of backend + device name (+ ordinal for duplicates), so a saved
//     input/output choice survives restarts and re-enumeration, like CoreAudio's AudioObjectIDs mostly do.
//   * Live input: full-duplex device first (one callback, lowest latency; JACK and ALSA run it natively).
//     If the pair can't open as one duplex device, input and output run as separate devices bridged by a
//     lock-free ring with drift control and underrun counting, exactly as AudioIO.mm does.
//   * Sample rate: the requested rate is tried; if the hardware runs at another rate the device is reopened at
//     the hardware rate so the engine never sits behind a resampler (AudioIO.mm falls back the same way).
//   * Real time: callbacks never allocate or lock; denormals are flushed for the duration of each callback.
#include "AudioIO.hpp"
#include "AudioBackend.hpp"
#if defined(_WIN32) && RIG_WITH_ASIO
#include "AsioDevice.hpp"
#endif
#include "../platform/Denormals.hpp"

#include "miniaudio.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace rig::audio {

namespace {

constexpr uint32_t kChunk = 4096;

// ───────────── backend selection ─────────────
std::mutex g_ctx_m;
#if defined(_WIN32) && RIG_WITH_ASIO
std::atomic<bool> g_asio_running{false};
#endif
AudioBackend g_pref = AudioBackend::Auto;
bool g_pref_from_env = false;
ma_context g_ctx;
bool g_ctx_ok = false;
AudioBackend g_ctx_for = AudioBackend::Auto;
int g_open_devices = 0; // the context can't be recreated while a device runs on it

struct DevEntry {
    ma_device_id id{};
    bool playback = false, capture = false;
    ma_device_id cap_id{};
};
std::map<uint32_t, DevEntry> g_ids; // DeviceInfo.id → miniaudio ids (both directions when the name matches)
uint32_t g_default_in = 0, g_default_out = 0;

std::vector<ma_backend> maOrder(AudioBackend b) {
    switch (b) {
        case AudioBackend::Jack: return {ma_backend_jack};
        case AudioBackend::Pulse: return {ma_backend_pulseaudio};
        case AudioBackend::Alsa: return {ma_backend_alsa};
        case AudioBackend::Wasapi:
        case AudioBackend::WasapiExclusive: return {ma_backend_wasapi};
        case AudioBackend::Asio: return {}; // not a miniaudio backend
        case AudioBackend::Auto: break;
    }
#ifdef _WIN32
    return {ma_backend_wasapi, ma_backend_dsound, ma_backend_winmm};
#else
    // JACK only when a JACK (or PipeWire-JACK) server is up; miniaudio skips it otherwise.
    return {ma_backend_jack, ma_backend_pulseaudio, ma_backend_alsa};
#endif
}

AudioBackend fromMa(ma_backend b) {
    switch (b) {
        case ma_backend_jack: return AudioBackend::Jack;
        case ma_backend_pulseaudio: return AudioBackend::Pulse;
        case ma_backend_alsa: return AudioBackend::Alsa;
        case ma_backend_wasapi: return AudioBackend::Wasapi;
        default: return AudioBackend::Auto;
    }
}

AudioBackend effectivePref() {
    if (!g_pref_from_env) {
        g_pref_from_env = true;
        if (const char* e = std::getenv("HEADROOM_AUDIO_BACKEND")) g_pref = backendFromName(e);
    }
    return g_pref;
}

// Caller holds g_ctx_m.
bool ensureContext() {
    const AudioBackend want = effectivePref();
    if (g_ctx_ok && (g_ctx_for == want || g_open_devices > 0)) return true;
    if (g_ctx_ok) {
        ma_context_uninit(&g_ctx);
        g_ctx_ok = false;
    }
    if (want == AudioBackend::Asio) return false;
    auto order = maOrder(want);
    ma_context_config cc = ma_context_config_init();
    cc.jack.pClientName = "Headroom";
    cc.pulse.pApplicationName = "Headroom";
    cc.alsa.useVerboseDeviceEnumeration = MA_FALSE;
    if (ma_context_init(order.data(), static_cast<ma_uint32>(order.size()), &cc, &g_ctx) != MA_SUCCESS) {
        if (want == AudioBackend::Auto) return false;
        auto fallback = maOrder(AudioBackend::Auto); // a forced backend that isn't there: use whatever is
        if (ma_context_init(fallback.data(), static_cast<ma_uint32>(fallback.size()), &cc, &g_ctx) != MA_SUCCESS) return false;
    }
    g_ctx_ok = true;
    g_ctx_for = want;
    return true;
}

uint32_t fnv1a(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) { h ^= c; h *= 16777619u; }
    return h ? h : 1u;
}

int maxChannels(const ma_device_info& info) {
    int ch = 0;
    for (ma_uint32 i = 0; i < info.nativeDataFormatCount; ++i) ch = std::max<int>(ch, static_cast<int>(info.nativeDataFormats[i].channels));
    return ch > 0 ? std::min(ch, 64) : 2; // 0 = "any": stereo
}

std::vector<double> rates(const ma_device_info& a, const ma_device_info* b) {
    static const double std_rates[] = {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0};
    auto supports = [](const ma_device_info& info, double r) {
        if (info.nativeDataFormatCount == 0) return true;
        for (ma_uint32 i = 0; i < info.nativeDataFormatCount; ++i) {
            const auto sr = info.nativeDataFormats[i].sampleRate;
            if (sr == 0 || std::fabs(double(sr) - r) < 0.5) return true;
        }
        return false;
    };
    std::vector<double> out;
    for (double r : std_rates)
        if (supports(a, r) && (!b || supports(*b, r))) out.push_back(r);
    if (out.empty()) out = {44100.0, 48000.0};
    return out;
}

// Caller holds g_ctx_m. Rebuilds g_ids; returns the merged device list.
std::vector<DeviceInfo> enumerateLocked() {
    std::vector<DeviceInfo> out;
    g_ids.clear();
    g_default_in = g_default_out = 0;
    if (!ensureContext()) return out;
    ma_device_info *play = nullptr, *cap = nullptr;
    ma_uint32 np = 0, nc = 0;
    if (ma_context_get_devices(&g_ctx, &play, &np, &cap, &nc) != MA_SUCCESS) return out;
    // Copy: the arrays are owned by the context and change on the next enumeration.
    std::vector<ma_device_info> P(play, play + np), C(cap, cap + nc);
    const std::string be = ma_get_backend_name(g_ctx.backend);
    std::map<std::string, int> seen;
    auto makeId = [&](const std::string& name) {
        const int k = seen[name]++;
        return fnv1a(be + "|" + name + (k ? "#" + std::to_string(k) : std::string()));
    };
    std::vector<bool> cap_used(C.size(), false);
    for (auto& p : P) {
        ma_device_info full = p;
        ma_context_get_device_info(&g_ctx, ma_device_type_playback, &p.id, &full);
        DeviceInfo d;
        d.name = p.name;
        d.output_channels = maxChannels(full);
        DevEntry e;
        e.id = p.id;
        e.playback = true;
        ma_device_info cfull{};
        bool paired = false;
        for (std::size_t j = 0; j < C.size(); ++j) {
            if (cap_used[j] || std::strcmp(C[j].name, p.name) != 0) continue;
            cap_used[j] = true;
            cfull = C[j];
            ma_context_get_device_info(&g_ctx, ma_device_type_capture, &C[j].id, &cfull);
            d.input_channels = maxChannels(cfull);
            e.capture = true;
            e.cap_id = C[j].id;
            paired = true;
            if (C[j].isDefault) g_default_in = 1; // patched below once the id is known
            break;
        }
        d.sample_rates = rates(full, paired ? &cfull : nullptr);
        d.id = makeId(d.name);
        if (g_default_in == 1) g_default_in = d.id;
        if (p.isDefault) g_default_out = d.id;
        g_ids[d.id] = e;
        out.push_back(std::move(d));
    }
    for (std::size_t j = 0; j < C.size(); ++j) {
        if (cap_used[j]) continue;
        ma_device_info full = C[j];
        ma_context_get_device_info(&g_ctx, ma_device_type_capture, &C[j].id, &full);
        DeviceInfo d;
        d.name = C[j].name;
        d.input_channels = maxChannels(full);
        d.sample_rates = rates(full, nullptr);
        d.id = makeId(d.name);
        DevEntry e;
        e.capture = true;
        e.cap_id = C[j].id;
        g_ids[d.id] = e;
        if (C[j].isDefault) g_default_in = d.id;
        out.push_back(std::move(d));
    }
    return out;
}

// Single-producer / single-consumer stereo ring buffer (same as AudioIO.mm).
class StereoRing {
public:
    void reset(std::size_t cap) {
        buf_.assign(cap * 2, 0.0f);
        cap_ = cap;
        w_.store(0);
        r_.store(0);
    }
    std::size_t fill() const { return w_.load(std::memory_order_acquire) - r_.load(std::memory_order_acquire); }
    void push(const float* l, const float* r, std::size_t n) {
        std::size_t w = w_.load(std::memory_order_relaxed);
        if (fill() + n > cap_) return; // overflow: drop
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t k = ((w + i) % cap_) * 2;
            buf_[k] = l[i];
            buf_[k + 1] = r[i];
        }
        w_.store(w + n, std::memory_order_release);
    }
    std::size_t pop(float* l, float* r, std::size_t n) {
        const std::size_t take = std::min(n, fill());
        std::size_t rd = r_.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < take; ++i) {
            const std::size_t k = ((rd + i) % cap_) * 2;
            l[i] = buf_[k];
            r[i] = buf_[k + 1];
        }
        r_.store(rd + take, std::memory_order_release);
        return take;
    }
    void skip(std::size_t n) { r_.fetch_add(std::min(n, fill()), std::memory_order_acq_rel); }

private:
    std::vector<float> buf_;
    std::size_t cap_ = 1;
    std::atomic<std::size_t> w_{0}, r_{0};
};

} // namespace

// ───────────── backend seam ─────────────
const char* backendName(AudioBackend b) {
    switch (b) {
        case AudioBackend::Auto: return "auto";
        case AudioBackend::Jack: return "jack";
        case AudioBackend::Pulse: return "pulse";
        case AudioBackend::Alsa: return "alsa";
        case AudioBackend::Wasapi: return "wasapi";
        case AudioBackend::WasapiExclusive: return "wasapi-exclusive";
        case AudioBackend::Asio: return "asio";
    }
    return "auto";
}
AudioBackend backendFromName(const std::string& n) {
    for (AudioBackend b : {AudioBackend::Jack, AudioBackend::Pulse, AudioBackend::Alsa, AudioBackend::Wasapi,
                           AudioBackend::WasapiExclusive, AudioBackend::Asio})
        if (n == backendName(b)) return b;
    return AudioBackend::Auto;
}
std::vector<AudioBackend> availableBackends() {
#ifdef _WIN32
#if RIG_WITH_ASIO
    return {AudioBackend::Wasapi, AudioBackend::WasapiExclusive, AudioBackend::Asio};
#else
    return {AudioBackend::Wasapi, AudioBackend::WasapiExclusive};
#endif
#else
    return {AudioBackend::Jack, AudioBackend::Pulse, AudioBackend::Alsa};
#endif
}
void setPreferredBackend(AudioBackend b) {
    std::lock_guard<std::mutex> lk(g_ctx_m);
    g_pref_from_env = true;
    if (g_pref != b) g_ids.clear();
    g_pref = b;
}
AudioBackend preferredBackend() {
    std::lock_guard<std::mutex> lk(g_ctx_m);
    return effectivePref();
}
AudioBackend activeBackend() {
    std::lock_guard<std::mutex> lk(g_ctx_m);
#if defined(_WIN32) && RIG_WITH_ASIO
    if (g_asio_running.load()) return AudioBackend::Asio;
#endif
    if (!g_ctx_ok || g_open_devices == 0) return AudioBackend::Auto;
    if (g_ctx.backend == ma_backend_wasapi && g_ctx_for == AudioBackend::WasapiExclusive)
        return AudioBackend::WasapiExclusive;
    return fromMa(g_ctx.backend);
}

// ───────────── AudioIO ─────────────
struct AudioIO::Impl {
    AudioIO* owner = nullptr;
#if defined(_WIN32) && RIG_WITH_ASIO
    AsioDevice asio;
#endif
    ma_device out_dev{}, in_dev{};
    bool out_open = false, in_open = false;
    bool duplex = false;
    RenderFn render;
    StereoRing ring;
    int in_channels = 0;
    int sel_channel = 0;
    bool stereo = false;
    bool live = false;
    uint32_t buffer_size = 256;
    std::array<float, kChunk> tmpL{}, tmpR{}, liveL{}, liveR{}, capL{}, capR{};

    // Selected input channel(s) of an interleaved capture block → planar L/R.
    void splitInput(const float* in, uint32_t n, float* l, float* r) const noexcept {
        const int ch = in_channels;
        const int c0 = std::clamp(sel_channel, 0, ch - 1);
        const int c1 = stereo ? std::min(c0 + 1, ch - 1) : c0;
        for (uint32_t i = 0; i < n; ++i) {
            l[i] = in[static_cast<std::size_t>(i) * ch + c0];
            r[i] = in[static_cast<std::size_t>(i) * ch + c1];
        }
    }

    void renderInto(float* out, const float* in_interleaved, uint32_t n) noexcept {
        uint32_t done = 0;
        while (done < n) {
            const uint32_t chunk = std::min<uint32_t>(n - done, kChunk);
            if (live && duplex && in_interleaved) {
                splitInput(in_interleaved + static_cast<std::size_t>(done) * in_channels, chunk, liveL.data(), liveR.data());
            } else if (live) {
                // Keep latency bounded: if the input side ran ahead (clock drift), drop the excess.
                const std::size_t target = static_cast<std::size_t>(buffer_size) * 2 + chunk;
                if (ring.fill() > target + buffer_size * 2) ring.skip(ring.fill() - target);
                const std::size_t got = ring.pop(liveL.data(), liveR.data(), chunk);
                if (got < chunk) {
                    std::fill(liveL.begin() + static_cast<std::ptrdiff_t>(got), liveL.begin() + chunk, 0.0f);
                    std::fill(liveR.begin() + static_cast<std::ptrdiff_t>(got), liveR.begin() + chunk, 0.0f);
                    owner->underruns_.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                std::fill(liveL.begin(), liveL.begin() + chunk, 0.0f);
                std::fill(liveR.begin(), liveR.begin() + chunk, 0.0f);
            }
            if (render) render(liveL.data(), liveR.data(), tmpL.data(), tmpR.data(), chunk);
            else { std::fill(tmpL.begin(), tmpL.begin() + chunk, 0.0f); std::fill(tmpR.begin(), tmpR.begin() + chunk, 0.0f); }
            float* o = out + static_cast<std::size_t>(done) * 2;
            for (uint32_t i = 0; i < chunk; ++i) { o[2 * i] = tmpL[i]; o[2 * i + 1] = tmpR[i]; }
            done += chunk;
        }
    }

    static void outputCb(ma_device* d, void* out, const void* in, ma_uint32 n) {
        rig::platform::ScopedNoDenormals ftz;
        auto* self = static_cast<Impl*>(d->pUserData);
        self->renderInto(static_cast<float*>(out), static_cast<const float*>(in), n);
    }
    static void inputCb(ma_device* d, void*, const void* in, ma_uint32 n) {
        rig::platform::ScopedNoDenormals ftz;
        auto* self = static_cast<Impl*>(d->pUserData);
        if (!in || self->in_channels <= 0) return;
        const float* src = static_cast<const float*>(in);
        uint32_t done = 0;
        while (done < n) {
            const uint32_t chunk = std::min<uint32_t>(n - done, kChunk);
            self->splitInput(src + static_cast<std::size_t>(done) * self->in_channels, chunk, self->capL.data(), self->capR.data());
            self->ring.push(self->capL.data(), self->capR.data(), chunk);
            done += chunk;
        }
    }
};

AudioIO::AudioIO() : impl_(std::make_unique<Impl>()) { impl_->owner = this; }
AudioIO::~AudioIO() { stop(); }
#if defined(_WIN32) && RIG_WITH_ASIO
std::vector<std::string> AudioIO::asioDrivers() { return AsioDevice::drivers(); }
std::string AudioIO::activeAsioDriver() const { return impl_->asio.activeName(); }
int AudioIO::asioInputChannels() const { return impl_->asio.inputChannels(); }
std::string AudioIO::asioError() const { return impl_->asio.error(); }
bool AudioIO::openAsioControlPanel(std::string* err) { return impl_->asio.openPanel(err); }
#endif

std::vector<DeviceInfo> AudioIO::devices() {
    std::lock_guard<std::mutex> lk(g_ctx_m);
    return enumerateLocked();
}
uint32_t AudioIO::defaultInputDevice() {
    std::lock_guard<std::mutex> lk(g_ctx_m);
    if (g_ids.empty()) enumerateLocked();
    return g_default_in;
}
uint32_t AudioIO::defaultOutputDevice() {
    std::lock_guard<std::mutex> lk(g_ctx_m);
    if (g_ids.empty()) enumerateLocked();
    return g_default_out;
}

// No per-app microphone prompt on Linux; on Windows the privacy switch makes WASAPI capture fail (reported
// through start()'s error) rather than prompting.
std::string AudioIO::micPermission() { return "granted"; }
void AudioIO::requestMicPermission() {}

void AudioIO::stop() {
    auto& I = *impl_;
#if defined(_WIN32) && RIG_WITH_ASIO
    I.asio.stop();
#endif
    if (I.out_open) ma_device_uninit(&I.out_dev);
    if (I.in_open) ma_device_uninit(&I.in_dev);
    {
        std::lock_guard<std::mutex> lk(g_ctx_m);
        g_open_devices -= (I.out_open ? 1 : 0) + (I.in_open ? 1 : 0);
        if (g_open_devices < 0) g_open_devices = 0;
    }
    I.out_open = I.in_open = I.duplex = false;
    running_.store(false);
    input_ok_.store(false);
}

bool AudioIO::start(const AudioSettings& s, bool live_input, RenderFn fn, std::string* err) {
    stop();
    auto& I = *impl_;
#if defined(_WIN32) && RIG_WITH_ASIO
    if (s.backend == "asio" || preferredBackend() == AudioBackend::Asio) {
        out_dev_ = in_dev_ = 0; // ASIO drivers have their own namespace.
        auto publish = [this, live_input, first = true, active = false](const AsioDevice::Status& state) mutable {
            if (state.running) {
                if (first) sample_rate_ = state.rate;
                first = false;
                buffer_size_ = state.frames;
                latency_ms_ = state.latencyMs;
            }
            if (state.running != active) g_asio_running.store(state.running);
            active = state.running;
            input_ok_.store(live_input && state.running);
            running_.store(state.running);
        };
        return I.asio.start(s, live_input, std::move(fn), std::move(publish), overloads_, err);
    }
#else
    if (preferredBackend() == AudioBackend::Asio) {
        if (err) *err = "Native ASIO support is unavailable in this build";
        return false;
    }
#endif
    I.render = std::move(fn);
    std::unique_lock<std::mutex> lk(g_ctx_m);
    if (g_ids.empty() || (s.output_id && !g_ids.count(s.output_id)) || (s.input_id && !g_ids.count(s.input_id))) enumerateLocked();
    if (!g_ctx_ok) {
        if (err) *err = "No audio backend is available";
        return false;
    }
    out_dev_ = s.output_id && g_ids.count(s.output_id) ? s.output_id : g_default_out;
    in_dev_ = s.input_id && g_ids.count(s.input_id) ? s.input_id : g_default_in;
    const DevEntry* oe = g_ids.count(out_dev_) && g_ids[out_dev_].playback ? &g_ids[out_dev_] : nullptr;
    const DevEntry* ie = g_ids.count(in_dev_) && g_ids[in_dev_].capture ? &g_ids[in_dev_] : nullptr;
    buffer_size_ = std::clamp(s.buffer_size, 16, 4096);
    I.buffer_size = static_cast<uint32_t>(buffer_size_);
    const bool exclusive = effectivePref() == AudioBackend::WasapiExclusive;

    I.in_channels = 0;
    if (live_input && ie) {
        ma_device_info info{};
        if (ma_context_get_device_info(&g_ctx, ma_device_type_capture, &ie->cap_id, &info) == MA_SUCCESS) I.in_channels = maxChannels(info);
        else I.in_channels = 2;
    }
    I.sel_channel = s.input_channel;
    I.stereo = s.input_mode == "stereo";

    auto config = [&](ma_device_type type, double sr, const ma_device_id* out_id, const ma_device_id* in_id, ma_device_data_proc cb) {
        ma_device_config c = ma_device_config_init(type);
        c.sampleRate = static_cast<ma_uint32>(std::lround(sr));
        c.periodSizeInFrames = static_cast<ma_uint32>(buffer_size_);
        c.periods = 2;
        c.performanceProfile = ma_performance_profile_low_latency;
        c.noPreSilencedOutputBuffer = MA_TRUE; // we write every frame
        c.noClip = MA_TRUE;
        c.dataCallback = cb;
        c.pUserData = &I;
        c.playback.format = ma_format_f32;
        c.playback.channels = 2;
        c.playback.pDeviceID = out_id;
        c.capture.format = ma_format_f32;
        c.capture.channels = static_cast<ma_uint32>(std::max(1, I.in_channels));
        c.capture.pDeviceID = in_id;
        if (exclusive) c.playback.shareMode = c.capture.shareMode = ma_share_mode_exclusive;
        c.wasapi.noAutoConvertSRC = MA_TRUE; // reopen at the hardware rate instead of converting
        c.alsa.noMMap = MA_FALSE;
        return c;
    };
    // Opens `dev`; if the hardware runs at another rate, reopens it there so nothing resamples.
    auto open = [&](ma_device& dev, ma_device_type type, const ma_device_id* out_id, const ma_device_id* in_id, ma_device_data_proc cb, double& sr) {
        ma_device_config c = config(type, sr, out_id, in_id, cb);
        if (ma_device_init(&g_ctx, &c, &dev) != MA_SUCCESS) return false;
        const ma_uint32 hw = type == ma_device_type_capture ? dev.capture.internalSampleRate : dev.playback.internalSampleRate;
        if (hw && std::fabs(double(hw) - sr) > 0.5) {
            ma_device_uninit(&dev);
            sr = hw;
            c = config(type, sr, out_id, in_id, cb);
            if (ma_device_init(&g_ctx, &c, &dev) != MA_SUCCESS) return false;
        }
        return true;
    };

    double sr = s.sample_rate > 0 ? s.sample_rate : 48000.0;
    I.live = false;
    std::string in_err;
    if (live_input && ie && I.in_channels > 0) {
        // 1) one full-duplex device
        if (open(I.out_dev, ma_device_type_duplex, oe ? &oe->id : nullptr, &ie->cap_id, &Impl::outputCb, sr)) {
            I.out_open = true;
            I.duplex = true;
            I.live = true;
        }
    }
    if (!I.out_open) {
        if (!open(I.out_dev, ma_device_type_playback, oe ? &oe->id : nullptr, nullptr, &Impl::outputCb, sr)) {
            if (err) *err = "Could not start the output device";
            return false;
        }
        I.out_open = true;
        // 2) separate input device bridged through the ring
        if (live_input && ie && I.in_channels > 0) {
            double isr = sr;
            ma_device_config c = config(ma_device_type_capture, isr, nullptr, &ie->cap_id, &Impl::inputCb);
            I.ring.reset(32768);
            if (ma_device_init(&g_ctx, &c, &I.in_dev) == MA_SUCCESS) {
                I.in_open = true;
                I.live = true;
            } else {
                in_err = "Could not start the input device at " + std::to_string(static_cast<int>(sr)) + " Hz";
            }
        } else if (live_input) {
            in_err = ie ? "The selected input device has no input channels" : "No input device";
        }
    }
    g_open_devices += (I.out_open ? 1 : 0) + (I.in_open ? 1 : 0);
    sample_rate_ = sr;
    lk.unlock();

    if (I.in_open && ma_device_start(&I.in_dev) != MA_SUCCESS) {
        ma_device_uninit(&I.in_dev);
        I.in_open = false;
        I.live = false;
        std::lock_guard<std::mutex> g(g_ctx_m);
        --g_open_devices;
        in_err = "Could not start the input device";
    }
    if (ma_device_start(&I.out_dev) != MA_SUCCESS) {
        if (err) *err = "Could not start the output device";
        stop();
        return false;
    }
    if (live_input && !I.live && err) *err = in_err;
    input_ok_.store(I.live);

    const double out_frames = double(I.out_dev.playback.internalPeriodSizeInFrames) * std::max<ma_uint32>(1, I.out_dev.playback.internalPeriods);
    double in_frames = 0.0;
    if (I.duplex) in_frames = double(I.out_dev.capture.internalPeriodSizeInFrames) * std::max<ma_uint32>(1, I.out_dev.capture.internalPeriods);
    else if (I.in_open) in_frames = double(I.in_dev.capture.internalPeriodSizeInFrames) * std::max<ma_uint32>(1, I.in_dev.capture.internalPeriods) + buffer_size_;
    const double hw_sr = I.out_dev.playback.internalSampleRate ? double(I.out_dev.playback.internalSampleRate) : sr;
    latency_ms_ = 1000.0 * (out_frames + in_frames) / hw_sr;
    running_.store(true);
    return true;
}

} // namespace rig::audio
