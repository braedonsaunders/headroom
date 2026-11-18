#include "AudioIO.hpp"
#include "../core/RtPool.hpp"
#import <AVFoundation/AVFoundation.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace rig::audio {

namespace {

std::string cfToString(CFStringRef s) {
    if (!s) return {};
    char buf[512];
    if (CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8)) return buf;
    return {};
}

int channelCount(AudioObjectID dev, AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress a{kAudioDevicePropertyStreamConfiguration, scope, kAudioObjectPropertyElementMain};
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(dev, &a, 0, nullptr, &sz) != noErr || sz == 0) return 0;
    std::vector<uint8_t> mem(sz);
    auto* abl = reinterpret_cast<AudioBufferList*>(mem.data());
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &sz, abl) != noErr) return 0;
    int ch = 0;
    for (UInt32 i = 0; i < abl->mNumberBuffers; ++i) ch += static_cast<int>(abl->mBuffers[i].mNumberChannels);
    return ch;
}

std::vector<double> sampleRates(AudioObjectID dev) {
    AudioObjectPropertyAddress a{kAudioDevicePropertyAvailableNominalSampleRates, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 sz = 0;
    std::vector<double> out;
    if (AudioObjectGetPropertyDataSize(dev, &a, 0, nullptr, &sz) != noErr || sz == 0) return out;
    std::vector<AudioValueRange> r(sz / sizeof(AudioValueRange));
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &sz, r.data()) != noErr) return out;
    for (double std_rate : {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0}) {
        for (const auto& v : r) {
            if (std_rate >= v.mMinimum - 0.5 && std_rate <= v.mMaximum + 0.5) {
                out.push_back(std_rate);
                break;
            }
        }
    }
    return out;
}

bool setDeviceRate(AudioObjectID dev, double sr) {
    AudioObjectPropertyAddress a{kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    Float64 cur = 0;
    UInt32 sz = sizeof(cur);
    AudioObjectGetPropertyData(dev, &a, 0, nullptr, &sz, &cur);
    if (std::abs(cur - sr) < 0.5) return true;
    Float64 v = sr;
    if (AudioObjectSetPropertyData(dev, &a, 0, nullptr, sizeof(v), &v) != noErr) return false;
    for (int i = 0; i < 50; ++i) { // the change is asynchronous
        sz = sizeof(cur);
        AudioObjectGetPropertyData(dev, &a, 0, nullptr, &sz, &cur);
        if (std::abs(cur - sr) < 0.5) return true;
        usleep(10000);
    }
    return false;
}

double deviceRate(AudioObjectID dev) {
    AudioObjectPropertyAddress a{kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    Float64 cur = 48000;
    UInt32 sz = sizeof(cur);
    AudioObjectGetPropertyData(dev, &a, 0, nullptr, &sz, &cur);
    return cur;
}

void setBufferSize(AudioObjectID dev, UInt32 frames) {
    AudioObjectPropertyAddress a{kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioObjectSetPropertyData(dev, &a, 0, nullptr, sizeof(frames), &frames);
}

UInt32 deviceLatency(AudioObjectID dev, AudioObjectPropertyScope scope) {
    UInt32 total = 0, v = 0, sz = sizeof(v);
    AudioObjectPropertyAddress a{kAudioDevicePropertyLatency, scope, kAudioObjectPropertyElementMain};
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &sz, &v) == noErr) total += v;
    a.mSelector = kAudioDevicePropertySafetyOffset;
    sz = sizeof(v);
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &sz, &v) == noErr) total += v;
    a.mSelector = kAudioDevicePropertyBufferFrameSize;
    a.mScope = kAudioObjectPropertyScopeGlobal;
    sz = sizeof(v);
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &sz, &v) == noErr) total += v;
    return total;
}

uint32_t defaultDevice(AudioObjectPropertySelector sel) {
    AudioObjectPropertyAddress a{sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioObjectID dev = 0;
    UInt32 sz = sizeof(dev);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &sz, &dev);
    return dev;
}

// Single-producer / single-consumer stereo ring buffer.
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
        const std::size_t avail = fill();
        const std::size_t take = std::min(n, avail);
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

struct AudioIO::Impl {
    AudioIO* owner = nullptr;
    AudioComponentInstance out_unit = nullptr;
    AudioComponentInstance in_unit = nullptr;
    RenderFn render;
    StereoRing ring;
    int in_channels = 0;
    int sel_channel = 0;
    bool stereo = false;
    bool live = false;
    std::vector<float> in_scratch;            // non-interleaved device channels
    std::vector<AudioBuffer> in_bufs_mem;
    std::vector<uint8_t> in_abl_mem;
    std::array<float, 4096> tmpL{}, tmpR{}, liveL{}, liveR{};
    uint32_t buffer_size = 256;
    int wg_slot = -1; // RtPool workgroup slot: the pool's workers join the output device's IO-thread workgroup
    AudioObjectID overload_dev = 0; // device whose overload notifications are being counted (0 = none)

    static OSStatus outputCb(void* ref, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 n, AudioBufferList* io) {
        auto* self = static_cast<Impl*>(ref);
        float* L = static_cast<float*>(io->mBuffers[0].mData);
        float* R = io->mNumberBuffers > 1 ? static_cast<float*>(io->mBuffers[1].mData) : L;
        UInt32 done = 0;
        while (done < n) {
            const UInt32 chunk = std::min<UInt32>(n - done, static_cast<UInt32>(self->tmpL.size()));
            if (self->live) {
                // Keep latency bounded: if the input side ran ahead (clock drift), drop the excess.
                const std::size_t target = static_cast<std::size_t>(self->buffer_size) * 2 + chunk;
                if (self->ring.fill() > target + self->buffer_size * 2) self->ring.skip(self->ring.fill() - target);
                const std::size_t got = self->ring.pop(self->liveL.data(), self->liveR.data(), chunk);
                if (got < chunk) {
                    std::fill(self->liveL.begin() + static_cast<std::ptrdiff_t>(got), self->liveL.begin() + chunk, 0.0f);
                    std::fill(self->liveR.begin() + static_cast<std::ptrdiff_t>(got), self->liveR.begin() + chunk, 0.0f);
                    self->owner->underruns_.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                std::fill(self->liveL.begin(), self->liveL.begin() + chunk, 0.0f);
                std::fill(self->liveR.begin(), self->liveR.begin() + chunk, 0.0f);
            }
            if (self->render) self->render(self->liveL.data(), self->liveR.data(), L + done, R + done, chunk);
            else {
                std::fill(L + done, L + done + chunk, 0.0f);
                if (R != L) std::fill(R + done, R + done + chunk, 0.0f);
            }
            done += chunk;
        }
        return noErr;
    }

    static OSStatus inputCb(void* ref, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* ts, UInt32 bus, UInt32 n, AudioBufferList*) {
        auto* self = static_cast<Impl*>(ref);
        if (n > 4096 || self->in_channels <= 0) return noErr;
        auto* abl = reinterpret_cast<AudioBufferList*>(self->in_abl_mem.data());
        abl->mNumberBuffers = static_cast<UInt32>(self->in_channels);
        for (int c = 0; c < self->in_channels; ++c) {
            abl->mBuffers[c].mNumberChannels = 1;
            abl->mBuffers[c].mDataByteSize = n * sizeof(float);
            abl->mBuffers[c].mData = self->in_scratch.data() + static_cast<std::size_t>(c) * 4096;
        }
        if (AudioUnitRender(self->in_unit, flags, ts, bus, n, abl) != noErr) return noErr;
        const int c0 = std::clamp(self->sel_channel, 0, self->in_channels - 1);
        const int c1 = self->stereo ? std::min(c0 + 1, self->in_channels - 1) : c0;
        const float* a = self->in_scratch.data() + static_cast<std::size_t>(c0) * 4096;
        const float* b = self->in_scratch.data() + static_cast<std::size_t>(c1) * 4096;
        self->ring.push(a, b, n);
        return noErr;
    }
};

AudioIO::AudioIO() : impl_(std::make_unique<Impl>()) { impl_->owner = this; }
AudioIO::~AudioIO() { stop(); }

std::vector<DeviceInfo> AudioIO::devices() {
    std::vector<DeviceInfo> out;
    AudioObjectPropertyAddress a{kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &sz) != noErr) return out;
    std::vector<AudioObjectID> ids(sz / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &sz, ids.data()) != noErr) return out;
    for (auto id : ids) {
        DeviceInfo d;
        d.id = id;
        CFStringRef name = nullptr;
        AudioObjectPropertyAddress na{kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        UInt32 nsz = sizeof(name);
        if (AudioObjectGetPropertyData(id, &na, 0, nullptr, &nsz, &name) == noErr && name) {
            d.name = cfToString(name);
            CFRelease(name);
        }
        d.input_channels = channelCount(id, kAudioDevicePropertyScopeInput);
        d.output_channels = channelCount(id, kAudioDevicePropertyScopeOutput);
        d.sample_rates = sampleRates(id);
        if (d.input_channels > 0 || d.output_channels > 0) out.push_back(std::move(d));
    }
    return out;
}

uint32_t AudioIO::defaultInputDevice() { return defaultDevice(kAudioHardwarePropertyDefaultInputDevice); }
uint32_t AudioIO::defaultOutputDevice() { return defaultDevice(kAudioHardwarePropertyDefaultOutputDevice); }

std::string AudioIO::micPermission() {
    switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio]) {
        case AVAuthorizationStatusAuthorized: return "granted";
        case AVAuthorizationStatusDenied: return "denied";
        case AVAuthorizationStatusRestricted: return "restricted";
        default: return "undetermined";
    }
}

void AudioIO::requestMicPermission() {
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL) {}];
}

// kAudioDeviceProcessorOverload listener (a CoreAudio notification thread): counts IO cycles the render missed.
static OSStatus overloadListener(AudioObjectID, UInt32 n, const AudioObjectPropertyAddress* addrs, void* ref) {
    for (UInt32 i = 0; i < n; ++i)
        if (addrs[i].mSelector == kAudioDeviceProcessorOverload) static_cast<std::atomic<uint64_t>*>(ref)->fetch_add(1, std::memory_order_relaxed);
    return noErr;
}
static constexpr AudioObjectPropertyAddress kOverloadAddr{kAudioDeviceProcessorOverload, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};

void AudioIO::stop() {
    auto& I = *impl_;
    if (I.overload_dev) {
        AudioObjectRemovePropertyListener(I.overload_dev, &kOverloadAddr, overloadListener, &overloads_);
        I.overload_dev = 0;
    }
    if (I.wg_slot >= 0) {
        RtPool::instance().releaseWorkgroupSlot(I.wg_slot);
        I.wg_slot = -1;
    }
    if (I.out_unit) {
        AudioOutputUnitStop(I.out_unit);
        AudioUnitUninitialize(I.out_unit);
        AudioComponentInstanceDispose(I.out_unit);
        I.out_unit = nullptr;
    }
    if (I.in_unit) {
        AudioOutputUnitStop(I.in_unit);
        AudioUnitUninitialize(I.in_unit);
        AudioComponentInstanceDispose(I.in_unit);
        I.in_unit = nullptr;
    }
    running_.store(false);
    input_ok_.store(false);
}

bool AudioIO::start(const AudioSettings& s, bool live_input, RenderFn fn, std::string* err) {
    stop();
    auto& I = *impl_;
    I.render = std::move(fn);
    out_dev_ = s.output_id ? s.output_id : defaultOutputDevice();
    in_dev_ = s.input_id ? s.input_id : defaultInputDevice();

    // Sample rate: try to run both devices at the requested rate; fall back to the output's rate.
    setDeviceRate(out_dev_, s.sample_rate);
    sample_rate_ = deviceRate(out_dev_);
    buffer_size_ = std::clamp(s.buffer_size, 16, 4096);
    I.buffer_size = static_cast<uint32_t>(buffer_size_);
    setBufferSize(out_dev_, static_cast<UInt32>(buffer_size_));

    AudioComponentDescription desc{kAudioUnitType_Output, kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (!comp || AudioComponentInstanceNew(comp, &I.out_unit) != noErr) {
        if (err) *err = "Could not create the output unit";
        return false;
    }
    AudioObjectID od = out_dev_;
    AudioUnitSetProperty(I.out_unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &od, sizeof(od));
    AudioStreamBasicDescription fmt{};
    fmt.mSampleRate = sample_rate_;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
    fmt.mBytesPerPacket = sizeof(float);
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerFrame = sizeof(float);
    fmt.mChannelsPerFrame = 2;
    fmt.mBitsPerChannel = 32;
    AudioUnitSetProperty(I.out_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &fmt, sizeof(fmt));
    AURenderCallbackStruct cb{&Impl::outputCb, &I};
    AudioUnitSetProperty(I.out_unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof(cb));

    I.live = false;
    if (live_input) {
        const int ch = channelCount(in_dev_, kAudioDevicePropertyScopeInput);
        bool ok = ch > 0 && micPermission() == "granted";
        if (ok && in_dev_ != out_dev_) ok = setDeviceRate(in_dev_, sample_rate_);
        if (ok) {
            setBufferSize(in_dev_, static_cast<UInt32>(buffer_size_));
            ok = AudioComponentInstanceNew(comp, &I.in_unit) == noErr;
        }
        if (ok) {
            UInt32 one = 1, zero = 0;
            AudioUnitSetProperty(I.in_unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &one, sizeof(one));
            AudioUnitSetProperty(I.in_unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &zero, sizeof(zero));
            AudioObjectID idv = in_dev_;
            ok = AudioUnitSetProperty(I.in_unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &idv, sizeof(idv)) == noErr;
        }
        if (ok) {
            I.in_channels = std::min(ch, 64);
            AudioStreamBasicDescription ifmt = fmt;
            ifmt.mChannelsPerFrame = static_cast<UInt32>(I.in_channels);
            ok = AudioUnitSetProperty(I.in_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &ifmt, sizeof(ifmt)) == noErr;
            I.in_scratch.assign(static_cast<std::size_t>(I.in_channels) * 4096, 0.0f);
            I.in_abl_mem.assign(sizeof(AudioBufferList) + sizeof(AudioBuffer) * static_cast<std::size_t>(I.in_channels), 0);
            I.sel_channel = s.input_channel;
            I.stereo = s.input_mode == "stereo";
            I.ring.reset(32768);
            AURenderCallbackStruct icb{&Impl::inputCb, &I};
            if (ok) ok = AudioUnitSetProperty(I.in_unit, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0, &icb, sizeof(icb)) == noErr;
            if (ok) ok = AudioUnitInitialize(I.in_unit) == noErr;
            if (ok) ok = AudioOutputUnitStart(I.in_unit) == noErr;
        }
        if (!ok) {
            if (I.in_unit) {
                AudioComponentInstanceDispose(I.in_unit);
                I.in_unit = nullptr;
            }
            if (err) {
                if (micPermission() != "granted") *err = "Microphone access is not granted (System Settings → Privacy & Security → Microphone)";
                else if (ch <= 0) *err = "The selected input device has no input channels";
                else *err = "Could not start the input device at " + std::to_string(static_cast<int>(sample_rate_)) + " Hz";
            }
        } else {
            I.live = true;
            input_ok_.store(true);
        }
    }

    if (AudioUnitInitialize(I.out_unit) != noErr || AudioOutputUnitStart(I.out_unit) != noErr) {
        if (err) *err = "Could not start the output device";
        stop();
        return false;
    }
    // The render forks parallel lanes and series-unit pipeline stages onto RtPool's workers. Joining them to the
    // device's IO-thread workgroup lets the OS schedule them as part of this audio work (performance cores, the
    // callback's deadline); without it a stage on an efficiency core can make the render slower than one core.
    if (@available(macOS 11.0, *)) {
        void* wg = nullptr; // os_workgroup_t, owned by us (+1); raw so the code is the same with and without ARC
        UInt32 sz = sizeof(wg);
        const AudioObjectPropertyAddress a{kAudioDevicePropertyIOThreadOSWorkgroup, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        if (AudioObjectGetPropertyData(out_dev_, &a, 0, nullptr, &sz, &wg) == noErr && wg) {
            if (I.wg_slot < 0) I.wg_slot = RtPool::instance().acquireWorkgroupSlot();
            RtPool::instance().setWorkgroupPtr(I.wg_slot, wg); // the slot takes its own reference
            (os_release)(wg);
        }
    }
    if (AudioObjectAddPropertyListener(out_dev_, &kOverloadAddr, overloadListener, &overloads_) == noErr) I.overload_dev = out_dev_;
    const UInt32 lat_out = deviceLatency(out_dev_, kAudioDevicePropertyScopeOutput);
    const UInt32 lat_in = I.live ? deviceLatency(in_dev_, kAudioDevicePropertyScopeInput) + static_cast<UInt32>(buffer_size_) : 0;
    latency_ms_ = 1000.0 * static_cast<double>(lat_out + lat_in) / sample_rate_;
    running_.store(true);
    return true;
}

} // namespace rig::audio
