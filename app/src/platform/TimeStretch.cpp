#include "TimeStretch.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>
#else
#include "signalsmith-stretch.h"
#endif

namespace rig::platform {

#ifdef __APPLE__

// AUNewTimePitch, set up and driven exactly as BackingTrackPlayer always did.
struct TimeStretch::Impl {
    AudioUnit au = nullptr;
    double latency_sec = 0.0;
    Float64 au_time = 0.0;
    StretchPullFn pull = nullptr;
    void* ctx = nullptr;

    static OSStatus input(void* ref, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 frames, AudioBufferList* io) {
        auto* self = static_cast<Impl*>(ref);
        if (!io || io->mNumberBuffers < 2) return kAudio_ParamError;
        self->pull(self->ctx, static_cast<float*>(io->mBuffers[0].mData), static_cast<float*>(io->mBuffers[1].mData), frames);
        return noErr;
    }
    void destroy() {
        if (!au) return;
        AudioUnitUninitialize(au);
        AudioComponentInstanceDispose(au);
        au = nullptr;
    }
};

TimeStretch::TimeStretch() : impl_(std::make_unique<Impl>()) {}
TimeStretch::~TimeStretch() { release(); }

void TimeStretch::release() { impl_->destroy(); }
bool TimeStretch::ok() const noexcept { return impl_->au != nullptr; }
double TimeStretch::latencySec() const noexcept { return impl_->latency_sec; }

bool TimeStretch::prepare(double sample_rate, uint32_t max_block, StretchPullFn pull, void* ctx) {
    Impl& m = *impl_;
    m.destroy();
    m.pull = pull;
    m.ctx = ctx;
    AudioComponentDescription d{kAudioUnitType_FormatConverter, kAudioUnitSubType_NewTimePitch, kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent comp = AudioComponentFindNext(nullptr, &d);
    if (!comp || AudioComponentInstanceNew(comp, &m.au) != noErr) { m.au = nullptr; return false; }
    AudioStreamBasicDescription f{};
    f.mSampleRate = sample_rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
    f.mBytesPerPacket = f.mBytesPerFrame = 4;
    f.mFramesPerPacket = 1;
    f.mChannelsPerFrame = 2;
    f.mBitsPerChannel = 32;
    UInt32 maxf = max_block;
    AURenderCallbackStruct cb{&Impl::input, &m};
    if (AudioUnitSetProperty(m.au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f) != noErr ||
        AudioUnitSetProperty(m.au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &f, sizeof f) != noErr ||
        AudioUnitSetProperty(m.au, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxf, sizeof maxf) != noErr ||
        AudioUnitSetProperty(m.au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb) != noErr ||
        AudioUnitInitialize(m.au) != noErr) {
        AudioComponentInstanceDispose(m.au);
        m.au = nullptr;
        return false;
    }
    Float64 lat = 0.0;
    UInt32 lsz = sizeof lat;
    if (AudioUnitGetProperty(m.au, kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0, &lat, &lsz) == noErr) m.latency_sec = lat;
    return true;
}

void TimeStretch::reset() noexcept {
    if (impl_->au) AudioUnitReset(impl_->au, kAudioUnitScope_Global, 0);
}

void TimeStretch::setParams(float speed, float semitones) noexcept {
    if (!impl_->au) return;
    AudioUnitSetParameter(impl_->au, kNewTimePitchParam_Rate, kAudioUnitScope_Global, 0, speed, 0);
    AudioUnitSetParameter(impl_->au, kNewTimePitchParam_Pitch, kAudioUnitScope_Global, 0, semitones * 100.0f, 0);
}

void TimeStretch::render(float* l, float* r, uint32_t n) noexcept {
    Impl& m = *impl_;
    if (!m.au) { std::fill(l, l + n, 0.0f); std::fill(r, r + n, 0.0f); return; }
    struct { AudioBufferList list; AudioBuffer second; } abl{};
    abl.list.mNumberBuffers = 2;
    abl.list.mBuffers[0] = {1, n * 4, l};
    abl.list.mBuffers[1] = {1, n * 4, r};
    AudioTimeStamp ts{};
    ts.mFlags = kAudioTimeStampSampleTimeValid;
    ts.mSampleTime = m.au_time;
    m.au_time += n;
    AudioUnitRenderActionFlags flags = 0;
    if (AudioUnitRender(m.au, &flags, &ts, 0, n, &abl.list) != noErr) { std::fill(l, l + n, 0.0f); std::fill(r, r + n, 0.0f); }
}

#else

// Signalsmith Stretch: every buffer is sized in prepare(); process() works in place on those.
struct TimeStretch::Impl {
    signalsmith::stretch::SignalsmithStretch<float> st{0x5eed}; // fixed seed: deterministic output
    bool ready = false;
    double sr = 48000.0;
    uint32_t max_block = 0;
    float speed = 1.0f, semis = 0.0f;
    double acc = 0.0;                // fractional input frames carried between renders
    std::vector<float> in_l, in_r;   // pulled source, up to max_block × 1.5 (+ rounding)
    StretchPullFn pull = nullptr;
    void* ctx = nullptr;
};

TimeStretch::TimeStretch() : impl_(std::make_unique<Impl>()) {}
TimeStretch::~TimeStretch() = default;

void TimeStretch::release() { impl_->ready = false; }
bool TimeStretch::ok() const noexcept { return impl_->ready; }
double TimeStretch::latencySec() const noexcept {
    const Impl& m = *impl_;
    return m.ready ? double(m.st.inputLatency() + m.st.outputLatency()) / m.sr : 0.0;
}

bool TimeStretch::prepare(double sample_rate, uint32_t max_block, StretchPullFn pull, void* ctx) {
    Impl& m = *impl_;
    m.ready = false;
    m.sr = sample_rate > 0 ? sample_rate : 48000.0;
    m.max_block = std::max<uint32_t>(1, max_block);
    m.pull = pull;
    m.ctx = ctx;
    // 90 ms blocks every 22.5 ms: close to the library's default quality (120/30 ms) with less delay, and the
    // spectral work is spread across render calls (splitComputation) so no single audio block takes the hit.
    const int block = static_cast<int>(m.sr * 0.09), interval = static_cast<int>(m.sr * 0.0225);
    m.st.configure(2, block, interval, true);
    const std::size_t cap = static_cast<std::size_t>(std::ceil(m.max_block * 1.5)) + 4;
    m.in_l.assign(cap, 0.0f);
    m.in_r.assign(cap, 0.0f);
    m.speed = 1.0f;
    m.semis = 0.0f;
    m.st.setTransposeSemitones(0.0f, 8000.0f / static_cast<float>(m.sr));
    m.st.reset();
    m.acc = 0.0;
    m.ready = true;
    return true;
}

void TimeStretch::reset() noexcept {
    Impl& m = *impl_;
    if (!m.ready) return;
    m.st.reset();
    m.acc = 0.0;
}

void TimeStretch::setParams(float speed, float semitones) noexcept {
    Impl& m = *impl_;
    if (!m.ready) return;
    m.speed = std::clamp(std::isfinite(speed) ? speed : 1.0f, 0.5f, 1.5f);
    if (semitones != m.semis) {
        m.semis = semitones;
        // tonality limit 8 kHz: the library's recommended setting for natural-sounding transposition
        m.st.setTransposeSemitones(semitones, 8000.0f / static_cast<float>(m.sr));
    }
}

void TimeStretch::render(float* l, float* r, uint32_t n) noexcept {
    Impl& m = *impl_;
    if (!m.ready || n == 0 || n > m.max_block) { std::fill(l, l + n, 0.0f); std::fill(r, r + n, 0.0f); return; }
    m.acc += double(n) * m.speed;
    uint32_t in = static_cast<uint32_t>(m.acc);
    in = std::min<uint32_t>(in, static_cast<uint32_t>(m.in_l.size()));
    m.acc -= in;
    if (in) m.pull(m.ctx, m.in_l.data(), m.in_r.data(), in);
    float* ins[2] = {m.in_l.data(), m.in_r.data()};
    float* outs[2] = {l, r};
    m.st.process(ins, static_cast<int>(in), outs, static_cast<int>(n));
}

#endif

} // namespace rig::platform
