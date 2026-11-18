// Offline AudioToolbox host for the installed Headroom AU (aufx Hdrm Hdrm). No DAW, no audio device.
//   * renders a DI clip through stereo→stereo and mono→stereo instances (block sizes 128 and 512)
//   * measures render wall time against the buffer budget
//   * host parameters (Input/Output/Bypass), the Cocoa UI property, Reset while rendering
//   * ClassInfo round trip into a fresh instance (before Initialize), reproducible restored renders
//   * two instances alive at once (RSS for 1 vs 2), then everything disposed
//   * --ui: opens the AU's Cocoa editor in an offscreen window, waits for the bundled UI to connect to the
//     instance's own engine port, prints what the page sees and writes a snapshot PNG
// Usage: hr_host [--bundle path/to/Headroom.component] [--ui out.png] [path/to/di.wav]
//        hr_host --bundle path/to/Headroom.component --perf [options]   (multi-instance deadline run; see perf::main)
#import <AudioToolbox/AudioToolbox.h>
#import <AudioUnit/AUCocoaUIView.h>
#import <Foundation/Foundation.h>
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <os/workgroup.h>
#include <AudioToolbox/AudioWorkInterval.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("FAIL line %d: %s — ", __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static double rssMB() {
    task_vm_info_data_t info;
    mach_msg_type_number_t c = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &c) != KERN_SUCCESS) return -1;
    return info.phys_footprint / 1048576.0;
}

static std::vector<float> g_di;
static bool loadWav(const char* path) {
    ExtAudioFileRef f = nullptr;
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
    if (ExtAudioFileOpenURL((__bridge CFURLRef)url, &f) != noErr) return false;
    AudioStreamBasicDescription fmt{};
    fmt.mSampleRate = 48000;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    fmt.mChannelsPerFrame = 1;
    fmt.mBitsPerChannel = 32;
    fmt.mBytesPerFrame = fmt.mBytesPerPacket = 4;
    fmt.mFramesPerPacket = 1;
    ExtAudioFileSetProperty(f, kExtAudioFileProperty_ClientDataFormat, sizeof(fmt), &fmt);
    std::vector<float> buf(4096);
    while (g_di.size() < 48000 * 8) {
        AudioBufferList bl{1, {{1, UInt32(buf.size() * 4), buf.data()}}};
        UInt32 n = UInt32(buf.size());
        if (ExtAudioFileRead(f, &n, &bl) != noErr || n == 0) break;
        g_di.insert(g_di.end(), buf.begin(), buf.begin() + n);
    }
    ExtAudioFileDispose(f);
    return !g_di.empty();
}

struct Feed { size_t pos = 0; };
static OSStatus inputCb(void* ref, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 n, AudioBufferList* io) {
    auto* fd = static_cast<Feed*>(ref);
    for (UInt32 b = 0; b < io->mNumberBuffers; ++b) {
        float* d = static_cast<float*>(io->mBuffers[b].mData);
        for (UInt32 i = 0; i < n; ++i) d[i] = g_di[(fd->pos + i) % g_di.size()] * (b ? 0.9f : 1.0f);
    }
    return noErr;
}

struct Unit {
    AudioUnit au = nullptr;
    Feed feed;
    UInt32 out_ch = 2;
    Float64 t = 0;
};

static AudioComponent findHeadroom() {
    AudioComponentDescription d{kAudioUnitType_Effect, 'Hdrm', 'Hdrm', 0, 0};
    return AudioComponentFindNext(nullptr, &d);
}

static bool create(Unit& u, AudioComponent comp, UInt32 in_ch, UInt32 out_ch, UInt32 max_frames, CFPropertyListRef state = nullptr,
                   bool offline = false) {
    if (AudioComponentInstanceNew(comp, &u.au) != noErr) return false;
    u.out_ch = out_ch;
    auto fmt = [](UInt32 ch) {
        AudioStreamBasicDescription f{};
        f.mSampleRate = 48000;
        f.mFormatID = kAudioFormatLinearPCM;
        f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
        f.mChannelsPerFrame = ch;
        f.mBitsPerChannel = 32;
        f.mBytesPerFrame = f.mBytesPerPacket = 4;
        f.mFramesPerPacket = 1;
        return f;
    };
    const auto fi = fmt(in_ch), fo = fmt(out_ch);
    OSStatus e = AudioUnitSetProperty(u.au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &fi, sizeof(fi));
    e |= AudioUnitSetProperty(u.au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &fo, sizeof(fo));
    e |= AudioUnitSetProperty(u.au, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &max_frames, sizeof(max_frames));
    AURenderCallbackStruct cb{inputCb, &u.feed};
    e |= AudioUnitSetProperty(u.au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof(cb));
    if (offline) {
        UInt32 one = 1;
        e |= AudioUnitSetProperty(u.au, kAudioUnitProperty_OfflineRender, kAudioUnitScope_Global, 0, &one, sizeof(one));
    }
    if (state) e |= AudioUnitSetProperty(u.au, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &state, sizeof(state));
    if (e != noErr) return false;
    return AudioUnitInitialize(u.au) == noErr;
}

static void dispose(Unit& u) {
    if (!u.au) return;
    AudioUnitUninitialize(u.au);
    AudioComponentInstanceDispose(u.au);
    u.au = nullptr;
}

// Renders `frames` in blocks of `block`; appends to L/R; returns {mean, peak} render time / budget.
static std::pair<double, double> render(Unit& u, size_t frames, UInt32 block, std::vector<float>& L, std::vector<float>& R,
                                        void (*before)(Unit&, size_t) = nullptr) {
    std::vector<float> l(block), r(block);
    double total = 0, worst = 0;
    size_t blocks = 0;
    for (size_t pos = 0; pos < frames; pos += block, ++blocks) {
        if (before) before(u, pos);
        const UInt32 n = UInt32(std::min<size_t>(block, frames - pos));
        AudioBufferList* bl = static_cast<AudioBufferList*>(alloca(sizeof(AudioBufferList) + sizeof(AudioBuffer)));
        bl->mNumberBuffers = u.out_ch;
        bl->mBuffers[0] = {1, n * 4, l.data()};
        if (u.out_ch > 1) bl->mBuffers[1] = {1, n * 4, r.data()};
        AudioTimeStamp ts{};
        ts.mFlags = kAudioTimeStampSampleTimeValid;
        ts.mSampleTime = u.t;
        AudioUnitRenderActionFlags flags = 0;
        const auto t0 = std::chrono::steady_clock::now();
        const OSStatus e = AudioUnitRender(u.au, &flags, &ts, 0, n, bl);
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        total += el;
        worst = std::max(worst, el);
        if (e != noErr) { ++g_fail; std::printf("FAIL AudioUnitRender %d\n", int(e)); return {0, 0}; }
        u.feed.pos += n;
        u.t += n;
        L.insert(L.end(), l.begin(), l.begin() + n);
        if (u.out_ch > 1) R.insert(R.end(), r.begin(), r.begin() + n);
    }
    const double budget = block / 48000.0;
    return {total / std::max<size_t>(1, blocks) / budget, worst / budget};
}

static double rms(const std::vector<float>& v, size_t from = 0) {
    double a = 0;
    for (size_t i = from; i < v.size(); ++i) a += double(v[i]) * v[i];
    return v.size() > from ? std::sqrt(a / double(v.size() - from)) : 0;
}
static bool finite(const std::vector<float>& v) { return std::all_of(v.begin(), v.end(), [](float x) { return std::isfinite(x); }); }

static CFPropertyListRef classInfo(Unit& u) {
    CFPropertyListRef p = nullptr;
    UInt32 sz = sizeof(p);
    if (AudioUnitGetProperty(u.au, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &p, &sz) != noErr) return nullptr;
    return p;
}
static std::string rigOf(CFPropertyListRef p) {
    if (!p || CFGetTypeID(p) != CFDictionaryGetTypeID()) return {};
    auto d = static_cast<CFDataRef>(CFDictionaryGetValue(static_cast<CFDictionaryRef>(p), CFSTR("headroom-state")));
    if (!d) return {};
    std::string s(reinterpret_cast<const char*>(CFDataGetBytePtr(d)), size_t(CFDataGetLength(d)));
    const auto at = s.find("rig_bytes=");
    return at == std::string::npos ? std::string() : s.substr(at);
}

static void automation(Unit& u, size_t pos) { // identical host automation for both reproducibility runs
    if (pos == 9600) AudioUnitSetParameter(u.au, 8, kAudioUnitScope_Global, 0, 4.0f, 0);   // Input +4 dB
    if (pos == 24000) AudioUnitSetParameter(u.au, 9, kAudioUnitScope_Global, 0, -5.0f, 0); // Output −5 dB
    if (pos == 38400) AudioUnitSetParameter(u.au, 10, kAudioUnitScope_Global, 0, 1.0f, 0); // Bypass on
    if (pos == 48000) AudioUnitSetParameter(u.au, 10, kAudioUnitScope_Global, 0, 0.0f, 0); // Bypass off
}

static WKWebView* findWebView(NSView* v) {
    if ([v isKindOfClass:[WKWebView class]]) return (WKWebView*)v;
    for (NSView* s in v.subviews)
        if (WKWebView* w = findWebView(s)) return w;
    return nil;
}
static void pump(double seconds, Unit* u) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    std::vector<float> L, R;
    while (std::chrono::steady_clock::now() < end) {
        [[NSRunLoop mainRunLoop] runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
        if (u) { L.clear(); R.clear(); render(*u, 960, 480, L, R); }
    }
}

static int editorProbe(AudioComponent comp, const char* png) {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    Unit u;
    CHECK(create(u, comp, 2, 2, 512), "create instance for the editor");
    UInt32 sz = 0;
    Boolean w = false;
    if (AudioUnitGetPropertyInfo(u.au, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0, &sz, &w) != noErr) { CHECK(false, "no Cocoa UI"); return 1; }
    std::vector<uint8_t> buf(sz);
    auto* info = reinterpret_cast<AudioUnitCocoaViewInfo*>(buf.data());
    AudioUnitGetProperty(u.au, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0, info, &sz);
    NSBundle* bundle = [NSBundle bundleWithURL:(__bridge NSURL*)info->mCocoaAUViewBundleLocation];
    Class cls = [bundle classNamed:(__bridge NSString*)info->mCocoaAUViewClass[0]];
    std::printf("editor factory: %s from %s\n", [(__bridge NSString*)info->mCocoaAUViewClass[0] UTF8String], bundle.bundlePath.UTF8String);
    id<AUCocoaUIBase> factory = [[cls alloc] init];
    NSView* view = [factory uiViewForAudioUnit:u.au withSize:NSMakeSize(1280, 820)];
    CHECK(view != nil, "editor view");
    NSWindow* win = [[NSWindow alloc] initWithContentRect:NSMakeRect(-4000, -4000, 1280, 820) styleMask:NSWindowStyleMaskBorderless
                                                  backing:NSBackingStoreBuffered defer:NO];
    win.contentView = view;
    [win orderFront:nil];
    pump(12.0, &u);
    WKWebView* web = findWebView(view);
    __block NSString* seen = nil;
    [web evaluateJavaScript:@"JSON.stringify({url: location.href.replace(/^.*\\/Contents\\//, '…/Contents/'), port: window.RIG_PORT, hosted: window.RIG_HOSTED, api: (typeof API_BASE !== 'undefined' ? API_BASE : null), units: document.querySelectorAll('[data-slot], .unit, .slot').length, empty: !!document.querySelector('.stage-empty')})"
          completionHandler:^(id r, NSError* e) { seen = e ? e.localizedDescription : [r description]; }];
    pump(1.0, &u);
    std::printf("editor page: %s\n", seen.UTF8String ?: "(no result)");
    CHECK(seen && [seen containsString:@"\"hosted\":true"] && ![seen containsString:@"17770"] && [seen containsString:@"Contents/Resources/ui/index.html"],
          "bundled UI loaded and attached to its own port");
    CHECK(seen && [seen containsString:@"\"empty\":false"], "UI connected to the engine (no 'waiting for engine' card)");
    __block bool snapped = false;
    WKSnapshotConfiguration* cfg = [[WKSnapshotConfiguration alloc] init];
    [web takeSnapshotWithConfiguration:cfg completionHandler:^(NSImage* img, NSError*) {
        if (img) {
            NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithCGImage:[img CGImageForProposedRect:nullptr context:nil hints:nil]];
            [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:[NSString stringWithUTF8String:png] atomically:YES];
        }
        snapped = true;
    }];
    for (int i = 0; i < 200 && !snapped; ++i) pump(0.05, nullptr);
    std::printf("snapshot: %s\n", png);
    [win orderOut:nil];
    win.contentView = nil;
    view = nil;
    pump(0.3, nullptr);
    dispose(u);
    std::printf(g_fail ? "hr_host --ui: %d FAILURE(S)\n" : "hr_host --ui: all checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}

// ───────────── --bundle: load a built (not installed) Headroom.component into this process ─────────────
// Registers the bundle's factory under a private description (aufx Hdrm HdrT) so an installed copy can never be
// picked up instead. Nothing is written to ~/Library.
static AudioComponent registerBundle(const char* path) {
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
    CFBundleRef b = CFBundleCreate(nullptr, (__bridge CFURLRef)url); // kept for the process lifetime
    if (!b || !CFBundleLoadExecutable(b)) return nullptr;
    auto fac = reinterpret_cast<AudioComponentFactoryFunction>(CFBundleGetFunctionPointerForName(b, CFSTR("HeadroomAUFactory")));
    if (!fac) return nullptr;
    AudioComponentDescription d{kAudioUnitType_Effect, 'HdrT', 'Hdrm', 0, 0};
    return AudioComponentRegister(&d, CFSTR("Headroom: Headroom (build under test)"), 0x000900, fac);
}

// ───────────── --perf: several instances, real-time paced, against the buffer deadline ─────────────
// A model of a DAW: every cycle (one buffer period, paced on the wall clock) each instance renders one block.
//   host=par  one real-time host thread per instance (Logic's process threads, one track each), all joined to a
//             host audio workgroup (AudioWorkIntervalCreate) whose interval the cycle master starts/finishes
//   host=ser  one real-time thread renders every instance in turn (instances in series on one channel strip)
// Optional load: --load K (K default-priority threads burning cores), --rtload K:F (K more real-time threads in
// the host workgroup, each burning F of every cycle: other tracks' plugins).
namespace perf {

struct Rig { const char* tag; const char* file; UInt32 in_ch; };
static Rig kRigs[] = {
    {"A", "presets/Showcase/00 JCM800 British Crunch.rig", 1},           // TS808 + JCM800 circuits, cab, plate (mono guitar)
    {"B", "presets/Library/Clean/Funk Rhythm Snap.rig", 2},             // 1176 circuit, split: Twin Reverb circuit + cab | amp (stereo)
    {"C", nullptr, 2},                                                  // --rigc file[:in_ch] (absolute path)
};

static std::string slurp(const std::string& p) {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return {};
    std::string s;
    char buf[65536];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) s.append(buf, n);
    std::fclose(f);
    return s;
}

// ClassInfo of a fresh instance with its rig replaced by `rig_xml`.
static CFPropertyListRef stateWithRig(AudioComponent comp, UInt32 in_ch, const std::string& rig_xml, const char* name) {
    Unit t;
    if (!create(t, comp, in_ch, 2, 512)) return nullptr;
    CFPropertyListRef base = classInfo(t);
    dispose(t);
    if (!base) return nullptr;
    CFMutableDictionaryRef d = CFDictionaryCreateMutableCopy(nullptr, 0, static_cast<CFDictionaryRef>(base));
    CFRelease(base);
    std::string blob = "HEADROOM-STATE 1\nin_trim=0\nout_trim=0\nbypass=0\nname=" + std::string(name) +
                       "\nsource=factory\nrig_bytes=" + std::to_string(rig_xml.size()) + "\n" + rig_xml;
    CFDataRef data = CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(blob.data()), CFIndex(blob.size()));
    CFDictionarySetValue(d, CFSTR("headroom-state"), data);
    CFRelease(data);
    return d;
}

static uint64_t nowAbs() { return mach_absolute_time(); }
static double absToSec(uint64_t a) {
    static mach_timebase_info_data_t tb = [] { mach_timebase_info_data_t t; mach_timebase_info(&t); return t; }();
    return double(a) * tb.numer / tb.denom * 1e-9;
}
static uint64_t secToAbs(double s) {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    return uint64_t(s * 1e9 * tb.denom / tb.numer);
}
static void makeRealtime(double period, double comp_frac) {
    thread_time_constraint_policy_data_t p{};
    p.period = uint32_t(secToAbs(period));
    p.computation = uint32_t(secToAbs(period * comp_frac));
    p.constraint = uint32_t(secToAbs(period));
    p.preemptible = 1;
    thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY, reinterpret_cast<thread_policy_t>(&p),
                      THREAD_TIME_CONSTRAINT_POLICY_COUNT);
}

struct Stat { double mean = 0, p99 = 0, max = 0; };
static Stat stat(std::vector<double> v) {
    Stat s;
    if (v.empty()) return s;
    for (double x : v) s.mean += x;
    s.mean /= double(v.size());
    std::sort(v.begin(), v.end());
    s.p99 = v[std::min(v.size() - 1, size_t(double(v.size()) * 0.99))];
    s.max = v.back();
    return s;
}

struct Config {
    int n = 1;
    UInt32 block = 128;
    bool parallel_host = true;
    double seconds = 5.0, warmup = 1.0;
    int rt_load = 0;
    double rt_frac = 0.0;
    bool use_wg = true;
};

struct Result {
    std::vector<double> trace0; // instance 0's block times in order (seconds), for --trace
    std::vector<Stat> inst; // per instance, seconds
    Stat cycle;             // cycle start → every block (and rt load) done, seconds
    long misses = 0, cycles = 0;
    double period = 0;
};

// One measurement run over units[0..n). Real-time paced; returns timings of the measured (post-warm-up) cycles.
static Result run(std::vector<Unit>& units, const Config& c, std::vector<std::vector<float>>* capture = nullptr) {
    const int n = c.n;
    const double period = c.block / 48000.0;
    const long warm = long(c.warmup / period), total = warm + long(c.seconds / period);
    const uint64_t period_abs = secToAbs(period);
    std::vector<std::vector<double>> times(n);
    for (auto& t : times) t.reserve(size_t(total));
    std::vector<double> cyc;
    cyc.reserve(size_t(total));
    long misses = 0;

    os_workgroup_interval_t wg = c.use_wg ? AudioWorkIntervalCreate("hr_host", OS_CLOCK_MACH_ABSOLUTE_TIME, nullptr) : nullptr;
    std::atomic<long> cycle{-1};
    std::atomic<int> done{0};
    std::atomic<bool> quit{false};
    const int host_threads = c.parallel_host ? n : 1;
    const int participants = host_threads + c.rt_load;

    auto renderOne = [&](int i, long k) {
        Unit& u = units[size_t(i)];
        const UInt32 nb = c.block;
        float* l = static_cast<float*>(alloca(nb * 4));
        float* r = static_cast<float*>(alloca(nb * 4));
        AudioBufferList* bl = static_cast<AudioBufferList*>(alloca(sizeof(AudioBufferList) + sizeof(AudioBuffer)));
        bl->mNumberBuffers = 2;
        bl->mBuffers[0] = {1, nb * 4, l};
        bl->mBuffers[1] = {1, nb * 4, r};
        AudioTimeStamp ts{};
        ts.mFlags = kAudioTimeStampSampleTimeValid;
        ts.mSampleTime = u.t;
        AudioUnitRenderActionFlags flags = 0;
        const uint64_t t0 = nowAbs();
        AudioUnitRender(u.au, &flags, &ts, 0, nb, bl);
        const double el = absToSec(nowAbs() - t0);
        u.feed.pos += nb;
        u.t += nb;
        if (k >= warm) times[size_t(i)].push_back(el);
        if (capture && k >= warm) {
            auto& cap = (*capture)[size_t(i)];
            for (UInt32 s = 0; s < nb; ++s) { cap.push_back(l[s]); cap.push_back(r[s]); }
        }
    };
    auto waitCycle = [&](long seen) -> long {
        for (int s = 0; s < 2000; ++s) {
            const long v = cycle.load(std::memory_order_acquire);
            if (v != seen) return v;
        }
        long v;
        while ((v = cycle.load(std::memory_order_acquire)) == seen) cycle.wait(seen, std::memory_order_acquire);
        return v;
    };
    auto joinWg = [&](os_workgroup_join_token_s& tok) { return wg && os_workgroup_join(wg, &tok) == 0; };

    std::vector<std::thread> threads;
    // helper host threads (parallel host): instance i on thread i (thread 0 is the master)
    for (int i = 1; i < host_threads; ++i)
        threads.emplace_back([&, i] {
            makeRealtime(period, 0.5);
            os_workgroup_join_token_s tok{};
            const bool joined = joinWg(tok);
            for (long seen = -1;;) {
                seen = waitCycle(seen);
                if (quit.load()) break;
                renderOne(i, seen);
                done.fetch_add(1, std::memory_order_acq_rel);
            }
            if (joined) os_workgroup_leave(wg, &tok);
        });
    for (int i = 0; i < c.rt_load; ++i) // other tracks' processing: burns rt_frac of each cycle
        threads.emplace_back([&] {
            makeRealtime(period, std::max(0.05, c.rt_frac));
            os_workgroup_join_token_s tok{};
            const bool joined = joinWg(tok);
            const uint64_t burn = secToAbs(period * c.rt_frac);
            for (long seen = -1;;) {
                seen = waitCycle(seen);
                if (quit.load()) break;
                const uint64_t t0 = nowAbs();
                volatile double x = 1.0;
                while (nowAbs() - t0 < burn) x = x * 1.0000001 + 1e-9;
                done.fetch_add(1, std::memory_order_acq_rel);
            }
            if (joined) os_workgroup_leave(wg, &tok);
        });

    std::thread master([&] {
        makeRealtime(period, 0.5);
        os_workgroup_join_token_s tok{};
        const bool joined = joinWg(tok);
        uint64_t next = nowAbs() + secToAbs(0.02);
        for (long k = 0; k < total; ++k) {
            mach_wait_until(next);
            const uint64_t start = nowAbs();
            const uint64_t deadline = next + period_abs;
            if (joined) os_workgroup_interval_start(wg, start, deadline, nullptr);
            done.store(0, std::memory_order_relaxed);
            cycle.store(k, std::memory_order_release);
            if (participants > 1) cycle.notify_all();
            if (c.parallel_host) renderOne(0, k);
            else for (int i = 0; i < n; ++i) renderOne(i, k);
            while (done.load(std::memory_order_acquire) < participants - 1) {}
            const uint64_t end = nowAbs();
            if (joined) os_workgroup_interval_finish(wg, nullptr);
            if (k >= warm) {
                cyc.push_back(absToSec(end - next)); // from the cycle's scheduled start (wake-up latency counts)
                if (end > deadline) ++misses;
            }
            next += period_abs;
            if (end > next) next = end; // a host drops the late buffer and carries on from now
        }
        quit.store(true);
        cycle.store(total, std::memory_order_release);
        cycle.notify_all();
        if (joined) os_workgroup_leave(wg, &tok);
    });
    master.join();
    for (auto& t : threads) t.join();
    wg = nullptr; // ARC releases the workgroup

    Result r;
    r.period = period;
    if (!times.empty()) r.trace0 = times[0];
    for (auto& t : times) r.inst.push_back(stat(t));
    r.cycle = stat(cyc);
    r.misses = misses;
    r.cycles = long(cyc.size());
    return r;
}

static void burnCores(int k, std::atomic<bool>& stop, std::vector<std::thread>& out) {
    for (int i = 0; i < k; ++i)
        out.emplace_back([&stop] {
            volatile double x = 1.0;
            while (!stop.load(std::memory_order_relaxed))
                for (int j = 0; j < 4096; ++j) x = x * 1.0000001 + 1e-9;
        });
}

static void printResult(const char* rig, const Config& c, const Result& r) {
    const double us = 1e6, pct = 100.0 / r.period;
    double sum_mean = 0, worst_inst = 0, inst_mean = 0;
    for (const auto& s : r.inst) { sum_mean += s.mean; worst_inst = std::max(worst_inst, s.max); inst_mean += s.mean / r.inst.size(); }
    std::printf("PERF rig=%s n=%d buf=%u host=%s rtload=%d:%.2f | inst mean %.0fus (%.1f%%) max %.0fus (%.1f%%) | "
                "total mean %.0fus (%.1f%%) | cycle mean %.0fus p99 %.0fus max %.0fus (%.1f%% of %.0fus) | misses %ld/%ld\n",
                rig, c.n, c.block, c.parallel_host ? "par" : "ser", c.rt_load, c.rt_frac, inst_mean * us, inst_mean * pct,
                worst_inst * us, worst_inst * pct, sum_mean * us, sum_mean * pct, r.cycle.mean * us, r.cycle.p99 * us,
                r.cycle.max * us, r.cycle.max * pct, r.period * us, r.misses, r.cycles);
}

struct PoolStats { // mirror of PluginCore::PoolStats (private AU property 64001)
    uint64_t forks, on_worker, reclaimed, refused;
    int32_t workers, active, joined_workgroup, workgroup_max_parallel;
};
static PoolStats poolStats(Unit& u) {
    PoolStats s{};
    UInt32 sz = sizeof(s);
    if (AudioUnitGetProperty(u.au, 64001, kAudioUnitScope_Global, 0, &s, &sz) != noErr) s = PoolStats{};
    return s;
}

static int ints(const char* s, std::vector<int>& out) {
    out.clear();
    for (const char* p = s; *p;) {
        out.push_back(std::atoi(p));
        while (*p && *p != ',') ++p;
        if (*p == ',') ++p;
    }
    return int(out.size());
}

// hr_host --bundle B --perf [--rigs A,B] [--n 1,2,3,4] [--buf 64,128,256,512] [--host par|ser|both] [--secs S]
//         [--load K] [--rtload K:F] [--nowg] [--dump file]   (--dump: rig, n, buf, host must each be single)
static int main(AudioComponent comp, int argc, char** argv) {
    std::string rigs = "A,B";
    std::vector<int> ns{1, 2, 3, 4}, bufs{64, 128, 256, 512};
    std::string host = "par";
    double secs = 5.0;
    int load = 0, rt_load = 0;
    double rt_frac = 0;
    bool use_wg = true;
    const char* dump = nullptr;
    const char* trace = nullptr;
    for (int i = 0; i < argc; ++i) {
        const std::string a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : "";
        if (a == "--rigs") rigs = v, ++i;
        else if (a == "--n") ints(v, ns), ++i;
        else if (a == "--buf") ints(v, bufs), ++i;
        else if (a == "--host") host = v, ++i;
        else if (a == "--secs") secs = std::atof(v), ++i;
        else if (a == "--load") load = std::atoi(v), ++i;
        else if (a == "--rtload") { rt_load = std::atoi(v); if (const char* c = std::strchr(v, ':')) rt_frac = std::atof(c + 1); ++i; }
        else if (a == "--nowg") use_wg = false;
        else if (a == "--dump") dump = v, ++i;
        else if (a == "--trace") trace = v, ++i; // instance 0's block times (us), one per line, last config
        else if (a == "--rigc") {
            static std::string path;
            path = v;
            if (const auto c = path.rfind(':'); c != std::string::npos) { kRigs[2].in_ch = UInt32(std::atoi(path.c_str() + c + 1)); path.resize(c); }
            kRigs[2].file = path.c_str();
            if (rigs == "A,B") rigs = "C";
            ++i;
        }
    }
    const int max_n = *std::max_element(ns.begin(), ns.end());
    std::atomic<bool> stop{false};
    std::vector<std::thread> burners;
    burnCores(load, stop, burners);
    std::printf("perf: %d background burner thread(s), %d rt load thread(s) at %.0f%% of each cycle, workgroup %s\n", load, rt_load,
                rt_frac * 100, use_wg ? "on" : "off");
    for (const Rig& rig : kRigs) {
        if (rigs.find(rig.tag) == std::string::npos || !rig.file) continue;
        const std::string xml = slurp(rig.file[0] == '/' ? std::string(rig.file) : std::string(HR_APP_ROOT) + "/" + rig.file);
        if (xml.empty()) { CHECK(false, "cannot read %s", rig.file); continue; }
        CFPropertyListRef st = stateWithRig(comp, rig.in_ch, xml, rig.tag);
        CHECK(st != nullptr, "state for rig %s", rig.tag);
        if (!st) continue;
        std::vector<Unit> units(static_cast<size_t>(max_n));
        for (auto& u : units) CHECK(create(u, comp, rig.in_ch, 2, 512, st), "create rig %s instance", rig.tag);
        CFRelease(st);
        std::printf("rig %s (%s, %u in → 2 out): %d instance(s) ready; rss %.0f MB\n", rig.tag, rig.file, rig.in_ch, max_n, rssMB());
        for (int b : bufs)
            for (int n : ns)
                for (int hp = 0; hp < 2; ++hp) {
                    const bool par = hp == 0;
                    if ((par && host == "ser") || (!par && host == "par")) continue;
                    Config c;
                    c.n = n;
                    c.block = UInt32(b);
                    c.parallel_host = par;
                    c.seconds = secs;
                    c.rt_load = rt_load;
                    c.rt_frac = rt_frac;
                    c.use_wg = use_wg;
                    std::vector<std::vector<float>> cap(static_cast<size_t>(n));
                    const PoolStats p0 = poolStats(units[0]);
                    const Result r = run(units, c, dump ? &cap : nullptr);
                    const PoolStats p1 = poolStats(units[0]);
                    printResult(rig.tag, c, r);
                    if (trace) {
                        FILE* f = std::fopen(trace, "w");
                        for (double t : r.trace0) std::fprintf(f, "%.1f\n", t * 1e6);
                        std::fclose(f);
                    }
                    std::printf("     pool: %d workers, %d active, %d in the host workgroup (max parallel %d); forks %llu: "
                                "%llu on workers, %llu reclaimed inline, %llu refused (backoff)\n",
                                p1.workers, p1.active, p1.joined_workgroup, p1.workgroup_max_parallel,
                                (unsigned long long)(p1.forks - p0.forks), (unsigned long long)(p1.on_worker - p0.on_worker),
                                (unsigned long long)(p1.reclaimed - p0.reclaimed), (unsigned long long)(p1.refused - p0.refused));
                    if (dump) {
                        FILE* f = std::fopen(dump, "wb");
                        for (auto& v : cap) std::fwrite(v.data(), 4, v.size(), f);
                        std::fclose(f);
                        double rr = 0;
                        for (auto& v : cap) rr = std::max(rr, rms(v));
                        std::printf("dumped %d instance(s) to %s (rms %.4f)\n", n, dump, rr);
                    }
                }
        for (auto& u : units) dispose(u);
    }
    stop = true;
    for (auto& t : burners) t.join();
    std::printf(g_fail ? "hr_host --perf: %d FAILURE(S)\n" : "hr_host --perf: done\n", g_fail);
    return g_fail ? 1 : 0;
}
} // namespace perf

int main(int argc, char** argv) {
    @autoreleasepool {
        setvbuf(stdout, nullptr, _IOLBF, 0);
        const char* ui_png = nullptr;
        const char* bundle = nullptr;
        if (argc > 2 && std::string(argv[1]) == "--bundle") { bundle = argv[2]; argv += 2; argc -= 2; }
        if (argc > 2 && std::string(argv[1]) == "--ui") { ui_png = argv[2]; argv += 2; argc -= 2; }
        const bool perf_mode = argc > 1 && std::string(argv[1]) == "--perf";
        const char* wav = (argc > 1 && !perf_mode) ? argv[1] : HR_DEFAULT_DI;
        if (!loadWav(wav)) { std::printf("cannot read %s\n", wav); return 2; }
        AudioComponent comp = bundle ? registerBundle(bundle) : findHeadroom();
        if (!comp) { std::printf(bundle ? "cannot load the AU factory from %s\n" : "Headroom AU (aufx Hdrm Hdrm) not registered\n", bundle); return 2; }
        if (perf_mode) return perf::main(comp, argc - 2, argv + 2);
        if (!comp) { std::printf("Headroom AU (aufx Hdrm Hdrm) not registered\n"); return 2; }
        CFStringRef name = nullptr;
        AudioComponentCopyName(comp, &name);
        UInt32 ver = 0;
        AudioComponentGetVersion(comp, &ver);
        std::printf("found %s v%u.%u.%u; rss %.0f MB\n", [(__bridge NSString*)name UTF8String], ver >> 16, (ver >> 8) & 255, ver & 255, rssMB());
        if (name) CFRelease(name);
        if (ui_png) return editorProbe(comp, ui_png);

        // ── A: stereo → stereo ──
        Unit a;
        auto t0 = std::chrono::steady_clock::now();
        CHECK(create(a, comp, 2, 2, 1024), "create/initialize stereo instance");
        const double rss1 = rssMB();
        std::printf("A initialized in %.2f s; rss %.0f MB\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), rss1);
        std::vector<float> L, R;
        render(a, 48000, 512, L, R); // settle
        L.clear(); R.clear();
        const auto b128 = render(a, 48000 * 3, 128, L, R);
        std::printf("A stereo render @128: rms L=%.4f R=%.4f; render time mean %.1f%% / peak %.1f%% of buffer\n", rms(L), rms(R), b128.first * 100, b128.second * 100);
        CHECK(finite(L) && finite(R) && rms(L) > 1e-3, "A renders sound");
        L.clear(); R.clear();
        const auto b512 = render(a, 48000 * 2, 512, L, R);
        std::printf("A stereo render @512: render time mean %.1f%% / peak %.1f%% of buffer\n", b512.first * 100, b512.second * 100);

        // parameters
        UInt32 sz = 0;
        Boolean w = false;
        CHECK(AudioUnitGetPropertyInfo(a.au, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, &sz, &w) == noErr && sz == 3 * sizeof(AudioUnitParameterID),
              "3 host parameters (got %u bytes)", sz);
        AudioUnitSetParameter(a.au, 9, kAudioUnitScope_Global, 0, -12.0f, 0);
        L.clear(); R.clear();
        render(a, 9600, 512, L, R);
        const double before = rms(L, 4800);
        AudioUnitParameterValue pv = 0;
        AudioUnitGetParameter(a.au, 9, kAudioUnitScope_Global, 0, &pv);
        CHECK(pv == -12.0f, "Output param readback");
        AudioUnitSetParameter(a.au, 9, kAudioUnitScope_Global, 0, 0.0f, 0);
        L.clear(); R.clear();
        render(a, 9600, 512, L, R);
        const double after = rms(L, 4800);
        std::printf("Output trim −12 dB → 0 dB: rms %.4f → %.4f (%.1f dB)\n", before, after, 20 * std::log10(after / std::max(1e-9, before)));
        CHECK(after > before * 2.5, "Output trim audibly changes level");
        CHECK(AudioUnitGetPropertyInfo(a.au, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0, &sz, &w) == noErr && sz > 0, "Cocoa UI advertised");
        // Reset while rendering (AU Reset): rig rebuilt off the callback; output stays finite
        AudioUnitReset(a.au, kAudioUnitScope_Global, 0);
        L.clear(); R.clear();
        render(a, 24000, 256, L, R);
        CHECK(finite(L) && rms(L, 12000) > 1e-3, "renders after Reset");

        // ── B: mono → stereo, alive together with A ──
        Unit b;
        CHECK(create(b, comp, 1, 2, 512), "create/initialize mono→stereo instance");
        const double rss2 = rssMB();
        std::vector<float> BL, BR;
        L.clear(); R.clear();
        for (int i = 0; i < 20; ++i) { // interleaved, as a host would
            render(a, 2400, 480, L, R);
            render(b, 2400, 480, BL, BR);
        }
        std::printf("two instances alive: rss %.0f MB → %.0f MB (+%.0f MB for the 2nd); B mono→stereo rms L=%.4f R=%.4f\n", rss1, rss2, rss2 - rss1, rms(BL), rms(BR));
        CHECK(finite(BL) && finite(BR) && rms(BL) > 1e-3 && rms(BR) > 1e-3, "B renders both channels");
        CHECK(finite(L) && rms(L) > 1e-3, "A keeps rendering beside B");
        dispose(b);

        // ── ClassInfo round trip into a fresh instance (state set before Initialize) ──
        AudioUnitSetParameter(a.au, 8, kAudioUnitScope_Global, 0, 3.0f, 0);
        AudioUnitSetParameter(a.au, 9, kAudioUnitScope_Global, 0, -2.0f, 0);
        L.clear(); R.clear();
        render(a, 1024, 512, L, R);
        CFPropertyListRef st = classInfo(a);
        CHECK(st && !rigOf(st).empty(), "ClassInfo carries the rig");
        dispose(a);
        Unit c;
        CHECK(create(c, comp, 2, 2, 1024, st), "fresh instance from ClassInfo");
        AudioUnitGetParameter(c.au, 8, kAudioUnitScope_Global, 0, &pv);
        CHECK(pv == 3.0f, "Input restored (%f)", pv);
        CFPropertyListRef st2 = classInfo(c);
        CHECK(rigOf(st) == rigOf(st2), "rig XML identical after round trip");
        if (st2) CFRelease(st2);
        dispose(c);

        // ── reproducible restored renders: two fresh instances, same state, same automation ──
        std::vector<float> L1, R1, L2, R2;
        for (int run = 0; run < 2; ++run) {
            Unit d;
            CHECK(create(d, comp, 2, 2, 512, st, /*offline=*/true), "offline instance %d", run);
            render(d, 60000, 300, run ? L2 : L1, run ? R2 : R1, automation);
            dispose(d);
        }
        double md = 0;
        for (size_t i = 0; i < L1.size(); ++i) md = std::max(md, double(std::abs(L1[i] - L2[i]) + std::abs(R1[i] - R2[i])));
        std::printf("restored renders with identical automation: max |diff| = %g over %zu frames\n", md, L1.size());
        CHECK(md == 0.0, "renders must be reproducible");
        if (st) CFRelease(st);
        std::printf("all instances disposed: rss %.0f MB\n", rssMB());
    }
    std::printf(g_fail ? "hr_host: %d FAILURE(S)\n" : "hr_host: all checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
