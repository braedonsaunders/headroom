// Headroom as a VST3 effect (single-component: processor and controller in one object, so the editor and
// the engine share one PluginCore). Thin adapter over headroom::plug::PluginCore.
#import <Cocoa/Cocoa.h>
#include "../core/LatencyObserver.hpp"
#include <CoreAudio/CoreAudio.h>
#include <os/object.h>
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"
#include "public.sdk/source/vst/vstparameters.h"
#include "public.sdk/source/common/pluginview.h"
#include "public.sdk/source/main/pluginfactory.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/ivstplugview.h"
#include "pluginterfaces/vst/vsttypes.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/base/ustring.h"
#include "../core/HeadroomPluginCore.hpp"
#include "../editor/HeadroomEditor.hpp"
#include <algorithm>
#include <cstdlib>
#include <array>
#include <memory>
#include <string>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;
using namespace headroom::plug;

namespace {
// Never change: hosts identify the plugin (and its saved projects) by this id.
const FUID kHeadroomUID(0x4ec729b3, 0xc4dc46bd, 0xae9d3ad7, 0xec768d30);
constexpr uint32_t kMaxEvents = 1024;

std::string bundleResources() {
    CFBundleRef b = CFBundleGetBundleWithIdentifier(CFSTR(HEADROOM_VST3_BUNDLE_ID));
    if (!b) return {};
    CFURLRef u = CFBundleCopyResourcesDirectoryURL(b);
    char path[4096] = {};
    const bool ok = u && CFURLGetFileSystemRepresentation(u, true, reinterpret_cast<UInt8*>(path), sizeof(path));
    if (u) CFRelease(u);
    return ok ? std::string(path) : std::string();
}
// VST3 has no render-context/workgroup API (SDK 3.7.x). Hosts on macOS 11+ run process() on threads joined to the
// audio device's IO workgroup, so a real-time instance offers the engine's workers the system default output
// device's workgroup: the usual setup. A host on another device just means the workers are accounted to the wrong
// deadline, which is no worse than no workgroup. HEADROOM_VST3_WORKGROUP=0 turns it off. Returns +1 retained.
void* defaultOutputWorkgroup() {
    if (const char* e = std::getenv("HEADROOM_VST3_WORKGROUP"); e && *e == '0') return nullptr;
    AudioObjectPropertyAddress a{kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioDeviceID dev = kAudioObjectUnknown;
    UInt32 sz = sizeof(dev);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &sz, &dev) != noErr || dev == kAudioObjectUnknown) return nullptr;
    a.mSelector = kAudioDevicePropertyIOThreadOSWorkgroup;
    void* wg = nullptr;
    sz = sizeof(wg);
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &sz, &wg) != noErr) return nullptr;
    return wg;
}

float toPlain(uint32_t id, double norm) {
    const auto& sp = paramSpec(id);
    if (sp.is_bool) return norm >= 0.5 ? 1.0f : 0.0f;
    return static_cast<float>(sp.min + std::clamp(norm, 0.0, 1.0) * (sp.max - sp.min));
}
double toNorm(uint32_t id, float plain) {
    const auto& sp = paramSpec(id);
    return std::clamp((plain - sp.min) / (sp.max - sp.min), 0.0f, 1.0f);
}

class HeadroomPlugView final : public CPluginView {
public:
    explicit HeadroomPlugView(PluginCore* core) : core_(core) {
        ViewRect r(0, 0, static_cast<int32>(kEditorWidth), static_cast<int32>(kEditorHeight));
        setRect(r);
    }
    ~HeadroomPlugView() override { releaseEditorView(view_); }
    tresult PLUGIN_API isPlatformTypeSupported(FIDString type) override {
        return std::string_view(type) == kPlatformTypeNSView ? kResultTrue : kResultFalse;
    }
    tresult PLUGIN_API attached(void* parent, FIDString type) override {
        if (isPlatformTypeSupported(type) != kResultTrue || !parent) return kResultFalse;
        if (!view_) view_ = createEditorView(core_, rect.getWidth(), rect.getHeight());
        [(__bridge NSView*)parent addSubview:(__bridge NSView*)view_];
        return CPluginView::attached(parent, type);
    }
    tresult PLUGIN_API removed() override {
        releaseEditorView(view_);
        view_ = nullptr;
        return CPluginView::removed();
    }
    tresult PLUGIN_API canResize() override { return kResultTrue; }
    tresult PLUGIN_API checkSizeConstraint(ViewRect* r) override {
        if (r->getWidth() < 800) r->right = r->left + 800;
        if (r->getHeight() < 500) r->bottom = r->top + 500;
        return kResultTrue;
    }
    tresult PLUGIN_API onSize(ViewRect* r) override {
        if (view_) [(__bridge NSView*)view_ setFrame:NSMakeRect(0, 0, r->getWidth(), r->getHeight())];
        return CPluginView::onSize(r);
    }
    void coreGone() { core_ = nullptr; }

private:
    PluginCore* core_;
    void* view_ = nullptr;
};

class HeadroomVST3 final : public SingleComponentEffect {
public:
    static FUnknown* createInstance(void*) { return static_cast<IAudioProcessor*>(new HeadroomVST3); }

    tresult PLUGIN_API initialize(FUnknown* context) override {
        if (const tresult r = SingleComponentEffect::initialize(context); r != kResultOk) return r;
        addAudioInput(STR16("Input"), SpeakerArr::kStereo);
        addAudioOutput(STR16("Output"), SpeakerArr::kStereo);
        processContextRequirements.needTempo().needTransportState().needProjectTimeMusic(); // host tempo → synced units
        for (uint32_t id : kHostParams) { // v1: only the sample-accurate wrapper params are host parameters
            const auto& sp = paramSpec(id);
            String128 title, unit;
            Steinberg::UString(title, 128).fromAscii(sp.name);
            Steinberg::UString(unit, 128).fromAscii(sp.unit);
            int32 flags = ParameterInfo::kCanAutomate;
            if (id == kBypass) flags |= ParameterInfo::kIsBypass;
            if (sp.is_bool) {
                parameters.addParameter(title, unit, 1, sp.def, flags, id);
            } else if (std::string_view(sp.unit) == "dB") {
                auto* p = new RangeParameter(title, id, unit, sp.min, sp.max, sp.def, 0, flags);
                p->setPrecision(1);
                parameters.addParameter(p);
            } else {
                parameters.addParameter(title, unit, 0, sp.def, flags, id);
            }
        }
        core_ = std::make_unique<PluginCore>(Format::VST3, bundleResources());
        latency_observer_ = std::make_unique<LatencyObserver>([this] {
            const auto value = core_->latencySamples();
            if (value != notified_latency_) {
                notified_latency_ = value;
                if (componentHandler) componentHandler->restartComponent(kLatencyChanged);
            }
        });
        return kResultOk;
    }
    tresult PLUGIN_API terminate() override {
        latency_observer_.reset();
        if (core_) detachEditorViews(core_.get());
        core_.reset();
        return SingleComponentEffect::terminate();
    }

    tresult PLUGIN_API setBusArrangements(SpeakerArrangement* ins, int32 nIn, SpeakerArrangement* outs, int32 nOut) override {
        if (nIn != 1 || nOut != 1) return kResultFalse;
        const bool in_ok = ins[0] == SpeakerArr::kMono || ins[0] == SpeakerArr::kStereo;
        const bool out_ok = outs[0] == SpeakerArr::kStereo || (outs[0] == SpeakerArr::kMono && ins[0] == SpeakerArr::kMono);
        if (!in_ok || !out_ok) return kResultFalse;
        return SingleComponentEffect::setBusArrangements(ins, nIn, outs, nOut);
    }
    tresult PLUGIN_API canProcessSampleSize(int32 s) override { return s == kSample32 ? kResultTrue : kResultFalse; }
    uint32 PLUGIN_API getLatencySamples() override { return core_ ? core_->latencySamples() : 0; }
    uint32 PLUGIN_API getTailSamples() override {
        return static_cast<uint32>(processSetup.sampleRate * (core_ ? core_->tailSeconds() : 10.0));
    }
    tresult PLUGIN_API setupProcessing(ProcessSetup& setup) override {
        if (setup.symbolicSampleSize != kSample32) return kResultFalse;
        return SingleComponentEffect::setupProcessing(setup);
    }
    tresult PLUGIN_API setActive(TBool state) override {
        if (!core_) return kResultFalse;
        if (state) {
            const uint32_t max_frames = static_cast<uint32_t>(std::max<int32>(1, processSetup.maxSamplesPerBlock));
            scratch_.assign(max_frames, 0.0f);
            if (!core_->prepare(processSetup.sampleRate, max_frames)) return kResultFalse;
            void* wg = processSetup.processMode == kOffline ? nullptr : defaultOutputWorkgroup();
            core_->setRenderWorkgroup(wg); // the pool retains its own reference
            if (wg) (os_release)(wg);
        } else {
            core_->suspend();
        }
        return SingleComponentEffect::setActive(state);
    }

    tresult PLUGIN_API process(ProcessData& data) override {
        // Parameter points → plain-value events at their sample offsets (sorted, stable).
        uint32_t nev = 0;
        if (auto* changes = data.inputParameterChanges) {
            const int32 nq = changes->getParameterCount();
            for (int32 q = 0; q < nq; ++q) {
                IParamValueQueue* queue = changes->getParameterData(q);
                if (!queue) continue;
                const ParamID id = queue->getParameterId();
                if (!isHostParam(id)) continue;
                const int32 np = queue->getPointCount();
                for (int32 i = 0; i < np; ++i) {
                    int32 offset = 0;
                    ParamValue v = 0;
                    if (queue->getPoint(i, offset, v) != kResultTrue) continue;
                    if (nev < kMaxEvents) events_[nev++] = {static_cast<uint32_t>(std::max<int32>(0, offset)), id, toPlain(id, v)};
                    else events_[kMaxEvents - 1] = {static_cast<uint32_t>(std::max<int32>(0, offset)), id, toPlain(id, v)};
                }
            }
            std::stable_sort(events_.begin(), events_.begin() + nev, [](const ParamEvent& a, const ParamEvent& b) { return a.offset < b.offset; });
        }
        if (auto* ctx = data.processContext) {
            Transport t;
            if (ctx->state & ProcessContext::kTempoValid) {
                t.bpm = ctx->tempo;
                t.valid = true;
            }
            if (ctx->state & ProcessContext::kProjectTimeMusicValid) t.beat = ctx->projectTimeMusic;
            t.playing = (ctx->state & ProcessContext::kPlaying) != 0;
            core_->setTransport(t);
        }
        const uint32_t n = static_cast<uint32_t>(std::max<int32>(0, data.numSamples));
        if (data.numOutputs < 1 || !data.outputs || data.outputs[0].numChannels < 1 || n == 0) {
            core_->process(nullptr, nullptr, nullptr, nullptr, 0, events_.data(), nev); // parameter flush
            return kResultOk;
        }
        const float* inL = nullptr;
        const float* inR = nullptr;
        if (data.numInputs > 0 && data.inputs && data.inputs[0].numChannels > 0 && data.inputs[0].channelBuffers32) {
            inL = data.inputs[0].channelBuffers32[0];
            inR = data.inputs[0].numChannels > 1 ? data.inputs[0].channelBuffers32[1] : nullptr;
        }
        float* outL = data.outputs[0].channelBuffers32[0];
        const bool mono_out = data.outputs[0].numChannels < 2;
        if (mono_out && n > scratch_.size()) return kResultFalse;
        float* outR = mono_out ? scratch_.data() : data.outputs[0].channelBuffers32[1];
        core_->process(inL, inR, outL, outR, n, events_.data(), nev);
        if (mono_out) for (uint32_t i = 0; i < n; ++i) outL[i] = 0.5f * (outL[i] + outR[i]);
        data.outputs[0].silenceFlags = 0;
        return kResultOk;
    }

    // Component state = the whole rig + host-level params (see PluginCore::saveState).
    tresult PLUGIN_API getState(IBStream* state) override {
        if (!state || !core_) return kResultFalse;
        const std::string blob = core_->saveState();
        int32 written = 0;
        return state->write(const_cast<char*>(blob.data()), static_cast<int32>(blob.size()), &written) == kResultOk &&
                       written == static_cast<int32>(blob.size())
                   ? kResultOk
                   : kResultFalse;
    }
    tresult PLUGIN_API setState(IBStream* state) override {
        if (!state || !core_) return kResultFalse;
        std::string blob;
        char buf[16384];
        for (;;) {
            int32 got = 0;
            if (state->read(buf, sizeof(buf), &got) != kResultOk || got <= 0) break;
            blob.append(buf, static_cast<std::size_t>(got));
        }
        if (!core_->loadState(blob)) return kResultFalse;
        for (uint32_t id : kHostParams) setParamNormalized(id, toNorm(id, core_->paramValue(id)));
        return kResultOk;
    }

    IPlugView* PLUGIN_API createView(FIDString name) override {
        if (!core_ || !name || std::string_view(name) != ViewType::kEditor) return nullptr;
        return new HeadroomPlugView(core_.get());
    }

private:
    std::unique_ptr<PluginCore> core_;
    std::unique_ptr<LatencyObserver> latency_observer_;
    uint32_t notified_latency_ = 0;
    std::array<ParamEvent, kMaxEvents> events_{};
    std::vector<float> scratch_;
};
} // namespace

BEGIN_FACTORY_DEF("Headroom", "https://headroom.audio", "mailto:support@headroom.audio")
DEF_CLASS2(INLINE_UID_FROM_FUID(kHeadroomUID), PClassInfo::kManyInstances, kVstAudioEffectClass, "Headroom",
           0 /* single component: not distributable */, "Fx|Distortion|Amp", HEADROOM_VERSION_STR, kVstVersionString, HeadroomVST3::createInstance)
END_FACTORY
