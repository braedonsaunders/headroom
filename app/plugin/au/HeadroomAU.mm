// Headroom as an Audio Unit v2 effect (aufx Hdrm Hdrm): mono→mono, mono→stereo and stereo→stereo.
// Thin adapter over headroom::plug::PluginCore (see core/HeadroomPluginCore.hpp for threading rules).
#import <Cocoa/Cocoa.h>
#import <AudioUnit/AUCocoaUIView.h>
#include <AudioUnitSDK/AUEffectBase.h>
#include <AudioToolbox/AudioUnitUtilities.h>
#include "../core/HeadroomPluginCore.hpp"
#include "../core/LatencyObserver.hpp"
#include "../editor/HeadroomEditor.hpp"
#include <array>
#include <cstring>
#include <memory>
#include <vector>

using namespace headroom::plug;

namespace {
constexpr AudioUnitPropertyID kHeadroomCoreProperty = 64000; // private: PluginCore* for the Cocoa view
constexpr AudioUnitPropertyID kHeadroomPoolProperty = 64001; // private: PluginCore::PoolStats (hr_host diagnostics)
NSString* const kViewFactoryClass = @"HeadroomAUViewFactory_" HEADROOM_OBJC_SUFFIX_STR;

std::string bundleResources() {
    CFBundleRef b = CFBundleGetBundleWithIdentifier(CFSTR(HEADROOM_AU_BUNDLE_ID));
    if (!b) return {};
    CFURLRef u = CFBundleCopyResourcesDirectoryURL(b);
    char path[4096] = {};
    const bool ok = u && CFURLGetFileSystemRepresentation(u, true, reinterpret_cast<UInt8*>(path), sizeof(path));
    if (u) CFRelease(u);
    return ok ? std::string(path) : std::string();
}
} // namespace

class HeadroomAU final : public ausdk::AUEffectBase {
public:
    explicit HeadroomAU(AudioComponentInstance ci) : AUEffectBase(ci, /*inProcessesInPlace=*/false) {
        core_ = std::make_unique<PluginCore>(Format::AU, bundleResources());
        latency_observer_ = std::make_unique<LatencyObserver>([this] {
            const auto value = core_->latencySamples();
            if (value != notified_latency_) {
                notified_latency_ = value;
                PropertyChanged(kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0);
            }
        });
        // The OS fetches this once and calls it on the render thread before any render in a new context: the
        // engine's parallel workers then join the host's audio workgroup (or leave it, for a null workgroup).
        PluginCore* core = core_.get();
        observer_ = ^(const AudioUnitRenderContext* ctx) {
            core->setRenderWorkgroup(ctx ? (__bridge void*)ctx->workgroup : nullptr);
        };
        for (uint32_t id : kHostParams) {
            Globals()->SetParameter(id, paramSpec(id).def);
            last_[id] = paramSpec(id).def;
        }
    }
    ~HeadroomAU() override {
        latency_observer_.reset();
        detachEditorViews(core_.get());
        core_.reset(); // stops the engine: HTTP, library and worker threads joined, then the runtime reclaimed
    }

    OSStatus Initialize() override {
        if (const OSStatus r = AUEffectBase::Initialize(); r != noErr) return r;
        const UInt32 in_ch = GetInput(0)->GetStreamFormat().mChannelsPerFrame;
        const UInt32 out_ch = GetOutput(0)->GetStreamFormat().mChannelsPerFrame;
        if (in_ch != out_ch) SetProcessesInPlace(false);
        const double sr = GetOutput(0)->GetStreamFormat().mSampleRate;
        const UInt32 max_frames = GetMaxFramesPerSlice();
        scratch_r_.assign(max_frames, 0.0f);
        for (uint32_t id : kHostParams) last_[id] = Globals()->GetParameter(id);
        if (!core_->prepare(sr, max_frames)) return kAudioUnitErr_FailedInitialization;
        return noErr;
    }
    void Cleanup() override {
        core_->suspend();
        AUEffectBase::Cleanup();
    }
    // May arrive while the host renders: the rig is rebuilt off the callback and swapped in (fresh DSP state).
    OSStatus Reset(AudioUnitScope scope, AudioUnitElement element) override {
        if (scope == kAudioUnitScope_Global && IsInitialized()) core_->requestReset();
        return AUEffectBase::Reset(scope, element);
    }

    UInt32 SupportedNumChannels(const AUChannelInfo** outInfo) override {
        static const AUChannelInfo kInfos[] = {{1, 1}, {1, 2}, {2, 2}};
        if (outInfo) *outInfo = kInfos;
        return 3;
    }
    bool SupportsTail() override { return true; }
    Float64 GetTailTime() override { return core_->tailSeconds(); }
    Float64 GetLatency() override { const auto sr = GetOutput(0)->GetStreamFormat().mSampleRate; return sr > 0 ? core_->latencySamples() / sr : 0.0; }

    // Bypass is ours (crossfaded, works for mono→stereo); never let the base copy buffers.
    bool ShouldBypassEffect() override { return false; }
    void SetBypassEffect(bool on) override {
        AUEffectBase::SetBypassEffect(on);
        core_->setHostBypass(on);
    }

    OSStatus SetParameter(AudioUnitParameterID id, AudioUnitScope scope, AudioUnitElement element, AudioUnitParameterValue value,
                          UInt32 offset) override {
        const OSStatus r = AUEffectBase::SetParameter(id, scope, element, value, offset);
        if (r == noErr && scope == kAudioUnitScope_Global && isHostParam(id)) core_->setParamDirect(id, value);
        return r;
    }

    OSStatus GetParameterList(AudioUnitScope scope, AudioUnitParameterID* outList, UInt32& outNum) override {
        outNum = 0;
        if (scope != kAudioUnitScope_Global) return noErr;
        outNum = static_cast<UInt32>(kHostParams.size());
        if (outList) for (std::size_t i = 0; i < kHostParams.size(); ++i) outList[i] = kHostParams[i];
        return noErr;
    }

    OSStatus GetParameterInfo(AudioUnitScope scope, AudioUnitParameterID id, AudioUnitParameterInfo& info) override {
        if (scope != kAudioUnitScope_Global || !isHostParam(id)) return kAudioUnitErr_InvalidParameter;
        const ParamSpec& sp = paramSpec(id);
        info.flags = kAudioUnitParameterFlag_IsReadable | kAudioUnitParameterFlag_IsWritable | kAudioUnitParameterFlag_HasCFNameString;
        if (!sp.is_bool) info.flags |= kAudioUnitParameterFlag_CanRamp; // all host params are sample-accurate
        if (!sp.is_bool) info.flags |= kAudioUnitParameterFlag_IsHighResolution;
        AUBase::FillInParameterName(info, CFStringCreateWithCString(nullptr, sp.name, kCFStringEncodingUTF8), true);
        info.unit = sp.is_bool ? kAudioUnitParameterUnit_Boolean
                               : (std::string_view(sp.unit) == "dB" ? kAudioUnitParameterUnit_Decibels : kAudioUnitParameterUnit_Generic);
        info.minValue = sp.min;
        info.maxValue = sp.max;
        info.defaultValue = sp.def;
        return noErr;
    }

    OSStatus GetPropertyInfo(AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement element, UInt32& outSize,
                             bool& outWritable) override {
        if (scope == kAudioUnitScope_Global) {
            switch (id) {
                case kAudioUnitProperty_CocoaUI: outSize = sizeof(AudioUnitCocoaViewInfo); outWritable = false; return noErr;
                case kHeadroomCoreProperty: outSize = sizeof(PluginCore*); outWritable = false; return noErr;
                case kHeadroomPoolProperty: outSize = sizeof(PluginCore::PoolStats); outWritable = false; return noErr;
                case kAudioUnitProperty_RenderContextObserver: outSize = sizeof(AURenderContextObserver); outWritable = false; return noErr;
                case kAudioUnitProperty_OfflineRender: outSize = sizeof(UInt32); outWritable = true; return noErr;
                default: break;
            }
        }
        return AUEffectBase::GetPropertyInfo(id, scope, element, outSize, outWritable);
    }
    OSStatus GetProperty(AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement element, void* outData) override {
        if (scope == kAudioUnitScope_Global) {
            switch (id) {
                case kAudioUnitProperty_CocoaUI: {
                    CFBundleRef b = CFBundleGetBundleWithIdentifier(CFSTR(HEADROOM_AU_BUNDLE_ID));
                    if (!b) return kAudioUnitErr_InvalidProperty;
                    auto* info = static_cast<AudioUnitCocoaViewInfo*>(outData);
                    info->mCocoaAUViewBundleLocation = CFBundleCopyBundleURL(b);
                    info->mCocoaAUViewClass[0] = (__bridge_retained CFStringRef)[kViewFactoryClass copy];
                    return noErr;
                }
                case kHeadroomCoreProperty: *static_cast<PluginCore**>(outData) = core_.get(); return noErr;
                case kHeadroomPoolProperty: *static_cast<PluginCore::PoolStats*>(outData) = core_->poolStats(); return noErr;
                case kAudioUnitProperty_RenderContextObserver: { // the block stays owned (and alive) here
                    void* blk = (__bridge void*)observer_;
                    std::memcpy(outData, &blk, sizeof(blk));
                    return noErr;
                }
                case kAudioUnitProperty_OfflineRender: *static_cast<UInt32*>(outData) = offline_ ? 1 : 0; return noErr;
                default: break;
            }
        }
        return AUEffectBase::GetProperty(id, scope, element, outData);
    }
    OSStatus SetProperty(AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement element, const void* data, UInt32 size) override {
        if (scope == kAudioUnitScope_Global && id == kAudioUnitProperty_OfflineRender) {
            if (size < sizeof(UInt32)) return kAudioUnitErr_InvalidPropertyValue;
            offline_ = *static_cast<const UInt32*>(data) != 0;
            return noErr;
        }
        if (scope == kAudioUnitScope_Global && id == kAudioUnitProperty_InPlaceProcessing && IsInitialized() &&
            GetInput(0)->GetStreamFormat().mChannelsPerFrame != GetOutput(0)->GetStreamFormat().mChannelsPerFrame) {
            return noErr; // mono→stereo can't run in place; stay out-of-place
        }
        return AUEffectBase::SetProperty(id, scope, element, data, size);
    }

    OSStatus SaveState(CFPropertyListRef* outData) override {
        if (const OSStatus r = AUBase::SaveState(outData); r != noErr) return r;
        const std::string blob = core_->saveState();
        CFDataRef d = CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(blob.data()), static_cast<CFIndex>(blob.size()));
        CFDictionarySetValue(static_cast<CFMutableDictionaryRef>(const_cast<void*>(*outData)), CFSTR("headroom-state"), d);
        CFRelease(d);
        return noErr;
    }
    OSStatus RestoreState(CFPropertyListRef plist) override {
        // Base first: it re-applies saved parameter values (queued as host automation) …
        if (const OSStatus r = AUBase::RestoreState(plist); r != noErr) return r;
        if (CFGetTypeID(plist) != CFDictionaryGetTypeID()) return noErr;
        auto d = static_cast<CFDataRef>(CFDictionaryGetValue(static_cast<CFDictionaryRef>(plist), CFSTR("headroom-state")));
        if (!d || CFGetTypeID(d) != CFDataGetTypeID()) return noErr;
        // … then the rig, which flushes that queue so stale values can't overwrite what was restored.
        const std::string blob(reinterpret_cast<const char*>(CFDataGetBytePtr(d)), static_cast<std::size_t>(CFDataGetLength(d)));
        if (!core_->loadState(blob)) return kAudioUnitErr_InvalidPropertyValue;
        for (uint32_t id : kHostParams) Globals()->SetParameter(id, core_->paramValue(id));
        return noErr;
    }

    OSStatus ProcessBufferLists(AudioUnitRenderActionFlags& flags, const AudioBufferList& in, AudioBufferList& out, UInt32 n) override {
        if (out.mNumberBuffers < 1 || !out.mBuffers[0].mData) return kAudioUnitErr_InvalidParameter;
        const float* inL = in.mNumberBuffers > 0 ? static_cast<const float*>(in.mBuffers[0].mData) : nullptr;
        const float* inR = in.mNumberBuffers > 1 ? static_cast<const float*>(in.mBuffers[1].mData) : nullptr;
        float* outL = static_cast<float*>(out.mBuffers[0].mData);
        const bool mono_out = out.mNumberBuffers < 2 || !out.mBuffers[1].mData;
        if (mono_out && n > scratch_r_.size()) return kAudioUnitErr_TooManyFramesToProcess;
        float* outR = mono_out ? scratch_r_.data() : static_cast<float*>(out.mBuffers[1].mData);

        // Parameter values for this slice (scheduled events are already applied to the element by AUBase).
        std::array<ParamEvent, kHostParams.size()> ev;
        uint32_t nev = 0;
        for (uint32_t id : kHostParams) {
            const float v = Globals()->GetParameter(id);
            if (v != last_[id]) {
                last_[id] = v;
                ev[nev++] = {0, id, v};
            }
        }
        Transport t;
        Float64 beat = 0, tempo = 0;
        if (CallHostBeatAndTempo(&beat, &tempo) == noErr && tempo > 0) {
            t.bpm = tempo;
            t.beat = beat;
            t.valid = true;
            Boolean playing = false, changed = false, looping = false;
            Float64 sample_in_ts = 0, cycle_start = 0, cycle_end = 0;
            if (CallHostTransportState(&playing, &changed, &sample_in_ts, &looping, &cycle_start, &cycle_end) == noErr) t.playing = playing;
            core_->setTransport(t);
        }
        core_->process(inL, inR, outL, outR, n, ev.data(), nev);
        if (mono_out) for (UInt32 i = 0; i < n; ++i) outL[i] = 0.5f * (outL[i] + outR[i]);
        flags &= ~kAudioUnitRenderAction_OutputIsSilence; // tails ring on after silent input
        return noErr;
    }

private:
    std::unique_ptr<PluginCore> core_;
    std::unique_ptr<LatencyObserver> latency_observer_;
    uint32_t notified_latency_ = 0;
    AURenderContextObserver observer_ = nil;
    std::array<float, kParamIdEnd> last_{}; // render thread
    std::vector<float> scratch_r_;
    bool offline_ = false; // kAudioUnitProperty_OfflineRender (reported back; rendering is identical)
};

AUSDK_COMPONENT_ENTRY(ausdk::AUBaseFactory, HeadroomAU)

// ───────────── Cocoa view factory ─────────────
#define HR_CAT2(a, b) a##b
#define HR_CAT(a, b) HR_CAT2(a, b)
#define HeadroomAUViewFactory HR_CAT(HeadroomAUViewFactory_, HEADROOM_OBJC_SUFFIX)

@interface HeadroomAUViewFactory : NSObject <AUCocoaUIBase>
@end
@implementation HeadroomAUViewFactory
- (unsigned)interfaceVersion { return 0; }
- (NSString *)description { return @"Headroom"; }
- (NSView *)uiViewForAudioUnit:(AudioUnit)au withSize:(NSSize)size {
    PluginCore *core = nullptr;
    UInt32 sz = sizeof(core);
    if (AudioUnitGetProperty(au, kHeadroomCoreProperty, kAudioUnitScope_Global, 0, &core, &sz) != noErr) core = nullptr;
    const double w = size.width > 100 ? size.width : kEditorWidth, h = size.height > 100 ? size.height : kEditorHeight;
    return (__bridge_transfer NSView *)createEditorView(core, w, h);
}
@end
