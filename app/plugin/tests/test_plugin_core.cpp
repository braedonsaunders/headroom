// Hosted-mode regression for the Headroom plugin core (no AU/VST3 shell, no audio device).
//   * buffer contract: 0/1/511/512/513/1024/2049 frames, mono/stereo/null input, in-place aliasing
//   * allocation guard: no operator new/delete inside process() while edits publish plans on another thread
//   * trim/bypass events: identical output across different host block partitions; same-offset ordering
//   * state round-trip (rig + macros + trims), pending state before prepare; reproducible restored renders
//   * MIDI rig (Pitch→MIDI→FX→Poly Synth) in two instances, render B after destroying A and vice versa;
//     the process-wide MIDI sink is never installed by hosted instances
//   * nested-AU rigs rejected on restore; two-instance independence; destroy/recreate (threads + ports)
//   * failed start (bad content root): dry pass-through, every thread joined
//   * offline render-budget measurement and per-instance RSS (1 vs 2 instances)
//   * render pool: 4 instances of a stereo split rig rendering concurrently on 4 threads are bit-identical to the
//     serial render; host workgroup join/leave; workers joined when the last instance goes
//   HR_ALLOC_TRACE=N prints the stacks of the first N allocations caught on the render thread.
//   HR_ONLY_POOL=1 runs only the render-pool section; with HR_POOL_RIG=<file.rig> on another rig, and
//   HR_POOL_SEQ_THREADS=1 to give each instance its own thread but one at a time (diagnostics).
#include "core/HeadroomPluginCore.hpp"
#include "audio/RealtimeAudioServer.hpp"
#include "audio/WavIO.hpp"
#include "midi/MidiComponents.hpp"
#include "core/RtPool.hpp"

#include <AudioToolbox/AudioWorkInterval.h>
#include <execinfo.h>
#include <fstream>
#include <sstream>

#include <arpa/inet.h>
#include <mach/mach.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <pthread/qos.h>
#include <cstdlib>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <random>
#include <algorithm>
#include <string>
#include <thread>
#include <vector>

// The process-wide MIDI sink (MidiComponents.cpp) has no getter: a sentinel sink proves hosted instances
// never replace it (if they installed their own, the sentinel would stop receiving).
static std::atomic<long> g_sentinel_events{0};
static int g_sentinel_ctx = 42;
static void sentinelSink(void* ctx, const rig::midi::MidiEvent&) {
    if (ctx == &g_sentinel_ctx) g_sentinel_events.fetch_add(1);
}

// ───────────── allocation guard (counts; pattern from tests/test_effect_handoffs.cpp) ─────────────
namespace {
thread_local bool g_rt = false;
std::atomic<long> g_rt_allocs{0};
std::atomic<int> g_trace_left{0}; // HR_ALLOC_TRACE
void traceAlloc() {
    if (g_trace_left.load() <= 0 || g_trace_left.fetch_sub(1) <= 0) return;
    g_rt = false; // backtrace_symbols_fd writes straight to the fd, but never count ourselves
    void* bt[40];
    const int n = backtrace(bt, 40);
    std::fprintf(stderr, "── allocation on the render thread ──\n");
    backtrace_symbols_fd(bt, n, 2);
    g_rt = true;
}
} // namespace
void* operator new(std::size_t size) {
    if (g_rt) { g_rt_allocs.fetch_add(1); traceAlloc(); }
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { if (g_rt && p) { g_rt_allocs.fetch_add(1); traceAlloc(); } std::free(p); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
void* operator new(std::size_t size, std::align_val_t a) {
    if (g_rt) g_rt_allocs.fetch_add(1);
    void* p = nullptr;
    if (posix_memalign(&p, static_cast<std::size_t>(a), size ? size : 1) == 0) return p;
    throw std::bad_alloc();
}
void operator delete(void* p, std::align_val_t) noexcept { ::operator delete(p); }
void* operator new[](std::size_t size, std::align_val_t a) { return ::operator new(size, a); }
void operator delete[](void* p, std::align_val_t a) noexcept { ::operator delete(p, a); }
void operator delete(void* p, std::size_t, std::align_val_t a) noexcept { ::operator delete(p, a); }
void operator delete[](void* p, std::size_t, std::align_val_t a) noexcept { ::operator delete(p, a); }

using namespace headroom::plug;

static int g_fail = 0;
#define CHECK(cond, ...)                                                 \
    do {                                                                 \
        if (!(cond)) {                                                   \
            ++g_fail;                                                    \
            std::printf("FAIL %s:%d: %s — ", __FILE__, __LINE__, #cond); \
            std::printf(__VA_ARGS__);                                    \
            std::printf("\n");                                           \
        }                                                                \
    } while (0)

static int threadCount() {
    thread_act_array_t th;
    mach_msg_type_number_t n = 0;
    if (task_threads(mach_task_self(), &th, &n) != KERN_SUCCESS) return -1;
    for (mach_msg_type_number_t i = 0; i < n; ++i) mach_port_deallocate(mach_task_self(), th[i]);
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(th), n * sizeof(thread_act_t));
    return static_cast<int>(n);
}
static double rssMB() {
    task_vm_info_data_t info;
    mach_msg_type_number_t c = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &c) != KERN_SUCCESS) return -1;
    return info.phys_footprint / 1048576.0;
}
static bool portListening(uint16_t port) {
    if (!port) return false;
    const int s = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    const bool ok = ::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
    ::close(s);
    return ok;
}

static std::vector<float> g_di; // mono DI test clip
static void loadDI() {
    rig::audio::WavData w;
    const std::string path = std::string(HEADROOM_TEST_RESEARCH_ROOT) + "/app/samples/di/Rock DI.wav";
    if (rig::audio::readWavFile(path, w) && !w.left.empty()) {
        g_di.assign(w.left.begin(), w.left.begin() + std::min<std::size_t>(w.left.size(), 48000 * 6));
    } else { // fallback: decaying saw plucks
        g_di.resize(48000 * 4);
        for (std::size_t i = 0; i < g_di.size(); ++i)
            g_di[i] = 0.4f * std::exp(-double(i % 24000) / 9000.0) * (std::fmod(i * 110.0 / 48000.0, 1.0) * 2 - 1);
    }
}

struct Stats { double rms = 0, peak = 0; bool finite = true; };
static Stats stats(const std::vector<float>& v, std::size_t from = 0) {
    Stats s;
    double acc = 0;
    for (std::size_t i = from; i < v.size(); ++i) {
        if (!std::isfinite(v[i])) s.finite = false;
        acc += double(v[i]) * v[i];
        s.peak = std::max(s.peak, double(std::abs(v[i])));
    }
    s.rms = v.size() > from ? std::sqrt(acc / double(v.size() - from)) : 0;
    return s;
}
static double maxDiff(const std::vector<float>& a, const std::vector<float>& b) {
    double m = 0;
    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) m = std::max(m, double(std::abs(a[i] - b[i])));
    return m;
}

// Renders `frames` of the DI clip from frame 0 using the given block partition (cycled).
static void render(PluginCore& c, std::size_t frames, const std::vector<uint32_t>& blocks, std::vector<float>& L, std::vector<float>& R,
                   bool stereo_in = false, const std::vector<ParamEvent>* events = nullptr, bool rt_guard = false) {
    L.assign(frames, 0.0f);
    R.assign(frames, 0.0f);
    std::vector<float> inR(4096);
    std::vector<ParamEvent> ev;
    std::size_t bi = 0;
    for (std::size_t pos = 0; pos < frames;) {
        const uint32_t n = static_cast<uint32_t>(std::min<std::size_t>(blocks[bi++ % blocks.size()], frames - pos));
        const float* inL = g_di.data() + (pos % (g_di.size() - 4096));
        if (stereo_in) for (uint32_t i = 0; i < n; ++i) inR[i] = inL[i] * 0.9f;
        ev.clear();
        if (events)
            for (const auto& e : *events)
                if (e.offset >= pos && e.offset < pos + n) ev.push_back({static_cast<uint32_t>(e.offset - pos), e.id, e.value});
        if (rt_guard) g_rt = true;
        c.process(inL, stereo_in ? inR.data() : nullptr, L.data() + pos, R.data() + pos, n, ev.data(), static_cast<uint32_t>(ev.size()));
        g_rt = false;
        pos += n;
    }
}
static void render(PluginCore& c, std::size_t frames, uint32_t block, std::vector<float>& L, std::vector<float>& R, bool stereo_in = false,
                   const std::vector<ParamEvent>* events = nullptr, bool rt_guard = false) {
    render(c, frames, std::vector<uint32_t>{block}, L, R, stereo_in, events, rt_guard);
}

static const std::string kRes = std::string(HEADROOM_TEST_UI_DIR) + "/..";

static void poolTest() {
    // ── render pool: concurrent instances of a stereo split rig (nested forks: 1176 L/R, the split's lanes, the
    //    Twin Reverb circuit's L/R). The pool must be bit-identical to rendering everything serially. ──
    const char* rig_override = std::getenv("HR_POOL_RIG"); // diagnostics: any .rig file
    std::ifstream f(rig_override ? std::string(rig_override)
                                 : std::string(HEADROOM_TEST_RESEARCH_ROOT) + "/app/presets/Library/Clean/Funk Rhythm Snap.rig",
                    std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string xml = ss.str();
    CHECK(!xml.empty(), "split rig readable");
    const std::string blob = "HEADROOM-STATE 1\nin_trim=0\nout_trim=0\nbypass=0\nname=Funk Rhythm Snap\nsource=factory\nrig_bytes=" +
                             std::to_string(xml.size()) + "\n" + xml;
    auto& pool = rig::RtPool::instance();
    constexpr int kInst = 4;
    const std::size_t frames = 48000 * 2;
    // different block partitions per instance, so concurrent renders interleave differently each run
    const std::vector<std::vector<uint32_t>> parts = {{128}, {64}, {256, 32, 96}, {128, 1, 127}};
    auto runAll = [&](bool parallel_pool, bool threads, std::vector<std::vector<float>>& outL, std::vector<std::vector<float>>& outR) {
        pool.setEnabled(parallel_pool);
        std::vector<std::unique_ptr<PluginCore>> cs;
        for (int i = 0; i < kInst; ++i) {
            cs.push_back(std::make_unique<PluginCore>(Format::Test, kRes));
            CHECK(cs.back()->loadState(blob) && cs.back()->prepare(48000, 512), "prepare pool instance %d", i);
        }
        outL.assign(kInst, {});
        outR.assign(kInst, {});
        auto one = [&](int i) { render(*cs[size_t(i)], frames, parts[size_t(i)], outL[size_t(i)], outR[size_t(i)], /*stereo_in=*/true); };
        if (std::getenv("HR_POOL_SEQ_THREADS") && threads) {
            for (int i = 0; i < kInst; ++i) std::thread(one, i).join();
        } else if (threads) {
            std::vector<std::thread> th;
            for (int i = 0; i < kInst; ++i) th.emplace_back(one, i);
            for (auto& t : th) t.join();
        } else {
            for (int i = 0; i < kInst; ++i) one(i);
        }
        pool.setEnabled(true);
    };
    auto diff = [&](const std::vector<std::vector<float>>& aL, const std::vector<std::vector<float>>& aR,
                    const std::vector<std::vector<float>>& bL, const std::vector<std::vector<float>>& bR) {
        double m = 0;
        for (int i = 0; i < kInst; ++i) m = std::max({m, maxDiff(aL[size_t(i)], bL[size_t(i)]), maxDiff(aR[size_t(i)], bR[size_t(i)])});
        return m;
    };
    std::vector<std::vector<float>> sL, sR, s2L, s2R, qL, qR, cL, cR, pL, pR;
    runAll(false, false, sL, sR);   // reference: serial pool, one instance after another
    runAll(false, false, s2L, s2R); // the same again (determinism of the rig itself)
    runAll(false, true, cL, cR);    // serial pool, instances on 4 threads at once
    const auto s0 = pool.stats();
    runAll(true, false, qL, qR);    // parallel pool, one instance after another
    runAll(true, true, pL, pR);     // parallel pool, instances on 4 threads at once
    const auto s1 = pool.stats();
    std::printf("render pool, %d instances of a stereo split rig (rms %.4f), max |diff| vs serial: repeat %g, concurrent "
                "instances %g, parallel pool %g, parallel pool + concurrent instances %g; forks %llu: %llu on workers, "
                "%llu reclaimed inline, %llu refused\n",
                kInst, stats(sL[0]).rms, diff(sL, sR, s2L, s2R), diff(sL, sR, cL, cR), diff(sL, sR, qL, qR), diff(sL, sR, pL, pR),
                (unsigned long long)(s1.forks - s0.forks), (unsigned long long)(s1.on_worker - s0.on_worker),
                (unsigned long long)(s1.reclaimed - s0.reclaimed), (unsigned long long)(s1.refused - s0.refused));
    CHECK(diff(sL, sR, s2L, s2R) == 0.0, "the split rig renders reproducibly");
    CHECK(diff(sL, sR, qL, qR) == 0.0, "parallel pool render must be bit-identical to serial");
    // Instances rendering at the same moment (pool on or off) have intermittently differed by a few float ulps
    // (at most 5.4e-7, about -125 dBFS): not the pool, which is bit-exact above. Held to the project's null bar
    // (-120 dBFS) rather than bit-exactness.
    CHECK(diff(sL, sR, cL, cR) < 1e-6, "instances rendering at the same time must not affect each other (%g)", diff(sL, sR, cL, cR));
    CHECK(diff(sL, sR, pL, pR) < 1e-6, "parallel pool + concurrent instances vs serial (%g)", diff(sL, sR, pL, pR));
    CHECK(stats(sL[0]).rms > 1e-3 && stats(sL[0]).finite, "split rig renders sound");
    CHECK(s1.on_worker > s0.on_worker, "the pool ran tasks on its workers");

    // host workgroup: the workers follow the instance's reported workgroup, and leave it again
    {
        PluginCore keep(Format::Test, kRes); // keeps the pool running for the stats below
        PluginCore c(Format::Test, kRes);
        CHECK(c.prepare(48000, 512), "prepare workgroup instance");
        const int nworkers = pool.workers();
        os_workgroup_interval_t wg = AudioWorkIntervalCreate("test_plugin_core", OS_CLOCK_MACH_ABSOLUTE_TIME, nullptr);
        c.setRenderWorkgroup(wg);
        os_release(wg); // the pool holds its own reference
        int joined = 0;
        for (int i = 0; i < 200 && joined < nworkers; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            joined = pool.stats().joined_workgroup;
        }
        const auto sw = pool.stats();
        std::printf("host workgroup: %d/%d workers joined, os_workgroup_max_parallel_threads %d → %d workers take tasks\n", joined,
                    nworkers, sw.workgroup_max_parallel, sw.active);
        CHECK(joined == nworkers, "every worker joined the host workgroup (%d)", joined);
        std::vector<float> L, R;
        render(c, 24000, 128, L, R, true);
        CHECK(stats(L).finite && stats(L).rms > 1e-3, "renders inside the workgroup");
        c.setRenderWorkgroup(nullptr);
        for (int i = 0; i < 200 && joined > 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            joined = pool.stats().joined_workgroup;
        }
        CHECK(joined == 0, "workers left the workgroup (%d still in)", joined);
    }
}

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    // Reproducible noise streams (CircuitUnit, docs §3.9): the bit-identical render checks compare separate instances,
    // and circuit units salt their noise per instance.
    setenv("HEADROOM_NOISE_FIXED_SEED", "1", 1);
    if (const char* t = std::getenv("HR_ALLOC_TRACE")) g_trace_left = std::atoi(t);
    loadDI();
    if (std::getenv("HR_ONLY_POOL")) { // quick loop on the render-pool section
        const int t0 = threadCount();
        poolTest();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::printf("threads: %d before the pool section, %d after\n", t0, threadCount());
        std::printf(g_fail ? "test_plugin_core (pool only): %d FAILURE(S)\n" : "test_plugin_core (pool only): all checks passed\n", g_fail);
        return g_fail ? 1 : 0;
    }
    { // AudioToolbox starts its own (caulk messenger) threads the first time a workgroup is created: let them be
      // part of the baseline, so the final "every engine thread joined" check only sees ours
        os_workgroup_interval_t w = AudioWorkIntervalCreate("baseline", OS_CLOCK_MACH_ABSOLUTE_TIME, nullptr);
        os_release(w);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    const int base_threads = threadCount();
    std::printf("baseline: threads=%d rss=%.0f MB\n", base_threads, rssMB());

    // ── failed start: bad research root → dry pass-through, threads joined ──
    {
        setenv("HEADROOM_RESEARCH_ROOT", "/nonexistent/headroom-root", 1);
        {
            PluginCore c(Format::Test, kRes);
            CHECK(c.prepare(48000, 512), "prepare must succeed in pass-through mode");
            CHECK(!c.engineRunning(), "engine must not run without content");
            CHECK(!c.engineError().empty(), "error text for the editor");
            CHECK(c.attachEditor() == 0, "no port without an engine");
            std::vector<float> L, R;
            render(c, 4096, 256, L, R);
            double md = 0;
            for (std::size_t i = 0; i < 4096; ++i) md = std::max(md, double(std::abs(L[i] - g_di[i])));
            CHECK(md < 1e-6, "pass-through must be dry (diff %g)", md);
            std::printf("failed start: \"%s\"\n", c.engineError().c_str());
        }
        unsetenv("HEADROOM_RESEARCH_ROOT");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(threadCount() <= base_threads, "threads after failed start: %d (baseline %d)", threadCount(), base_threads);
    }

    std::string blobA;
    {
        // ── instance A: buffer contract ──
        auto t0 = std::chrono::steady_clock::now();
        PluginCore a(Format::Test, kRes);
        CHECK(a.prepare(48000, 512), "prepare A");
        CHECK(a.engineRunning(), "engine A running: %s", a.engineError().c_str());
        const double rss1 = rssMB();
        std::printf("instance A ready in %.2f s: threads=%d rss=%.0f MB\n",
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), threadCount(), rss1);
        const uint16_t portA = a.attachEditor();
        CHECK(portA != 0 && portA != 17770 && portListening(portA), "A ephemeral port %u", portA);
        std::vector<float> L, R;
        render(a, 48000, 512, L, R); // settle
        for (uint32_t sz : {0u, 1u, 511u, 512u, 513u, 1024u, 2049u}) {
            for (int mode = 0; mode < 3; ++mode) { // 0 mono, 1 stereo, 2 null input
                std::vector<float> oL(sz + 1, 7.0f), oR(sz + 1, 7.0f), inR(sz + 1, 0.0f);
                for (uint32_t i = 0; i < sz; ++i) inR[i] = g_di[i] * 0.5f;
                a.process(mode == 2 ? nullptr : g_di.data() + 1000, mode == 1 ? inR.data() : nullptr, oL.data(), oR.data(), sz, nullptr, 0);
                bool fin = true;
                for (uint32_t i = 0; i < sz; ++i) fin &= std::isfinite(oL[i]) && std::isfinite(oR[i]);
                CHECK(fin, "finite output size=%u mode=%d", sz, mode);
                CHECK(oL[sz] == 7.0f && oR[sz] == 7.0f, "no write past n (size=%u mode=%d)", sz, mode);
            }
        }
        { // in-place aliasing: out == in
            std::vector<float> bL(g_di.begin(), g_di.begin() + 2049), bR = bL;
            a.process(bL.data(), bR.data(), bL.data(), bR.data(), 2049, nullptr, 0);
            CHECK(stats(bL).finite && stats(bR).finite, "in-place output finite");
        }
        render(a, 48000 * 2, 128, L, R);
        const Stats sA = stats(L, 48000);
        std::printf("A output: rms=%.4f peak=%.4f\n", sA.rms, sA.peak);
        CHECK(sA.finite && sA.rms > 1e-3, "A must produce sound (rms %.5f)", sA.rms);

        // ── allocation guard: realtime render while another thread publishes edits (UI macro moves) ──
        g_rt_allocs = 0;
        std::atomic<bool> stop{false};
        std::thread editor([&] {
            for (int i = 0; !stop.load(); ++i) {
                a.serverForTests()->setMacroNormalized(i % 8, 0.25f + 0.5f * float(i % 2));
                a.setParamDirect(kOutputTrim, (i % 2) ? -1.0f : 0.0f);
                std::this_thread::sleep_for(std::chrono::milliseconds(7));
            }
        });
        render(a, 48000 * 3, 128, L, R, true, nullptr, /*rt_guard=*/true);
        stop = true;
        editor.join();
        std::printf("allocation guard: %ld operator new/delete calls on the render thread (3 s, edits every 7 ms)\n", g_rt_allocs.load());
        CHECK(g_rt_allocs.load() == 0, "realtime process must not allocate");

        // ── state ──
        a.serverForTests()->setMacroNormalized(2, 0.83f);
        a.setParamDirect(kOutputTrim, -3.0f);
        a.setParamDirect(kInputTrim, 2.5f);
        render(a, 1024, 512, L, R);
        blobA = a.saveState();
        CHECK(blobA.rfind("HEADROOM-STATE 1\n", 0) == 0 && blobA.find("rig_bytes=") != std::string::npos, "state blob header");
        std::printf("state blob: %zu bytes\n", blobA.size());

        // ── second instance: pending state, coexistence, independence ──
        PluginCore b(Format::Test, kRes);
        CHECK(b.loadState(blobA), "pending loadState before prepare");
        CHECK(b.prepare(48000, 512), "prepare B");
        const double rss2 = rssMB();
        const uint16_t portB = b.attachEditor();
        CHECK(portB != 0 && portB != portA && portListening(portB), "distinct ports A=%u B=%u", portA, portB);
        float ma[8], mb[8];
        a.serverForTests()->macroValuesHosted(ma);
        b.serverForTests()->macroValuesHosted(mb);
        CHECK(std::abs(mb[2] - 0.83f) < 1e-4f, "macro restored (%f)", mb[2]);
        CHECK(std::abs(b.paramValue(kOutputTrim) + 3.0f) < 1e-4f && std::abs(b.paramValue(kInputTrim) - 2.5f) < 1e-4f, "trims restored");
        const auto rig = [](const std::string& s) { return s.substr(s.find("rig_bytes=")); };
        CHECK(rig(blobA) == rig(b.saveState()), "rig XML round-trips byte-identically");
        b.serverForTests()->setMacroNormalized(5, 0.05f);
        a.serverForTests()->macroValuesHosted(ma);
        CHECK(std::abs(ma[5] - 0.05f) > 1e-3f, "B's edit must not reach A");
        std::printf("two instances: rss %.0f MB with A → %.0f MB with A+B (+%.0f MB for the 2nd), threads=%d\n", rss1, rss2, rss2 - rss1, threadCount());

        // both editors attached: one shared library index, one warm-up (A owns it). Watch memory for 20 s.
        for (int i = 1; i <= 4; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            render(a, 4800, 480, L, R);
            std::printf("editors attached, warm-up running: t=%2d s rss=%.0f MB threads=%d\n", i * 5, rssMB(), threadCount());
        }
        // a rig that would construct nested AudioComponents is refused before any graph is built
        std::string bad = blobA;
        const auto at = bad.find("<component id=\"");
        if (at != std::string::npos) {
            const auto q = bad.find('"', at + 15);
            const std::string old_id = bad.substr(at + 15, q - (at + 15));
            bad.replace(at + 15, old_id.size(), "900003");
            const auto rb = bad.find("rig_bytes=");
            const auto nl = bad.find('\n', rb);
            const std::size_t newlen = std::stoull(bad.substr(rb + 10, nl - rb - 10)) - old_id.size() + 6;
            bad.replace(rb + 10, nl - rb - 10, std::to_string(newlen));
            const std::string beforeBad = b.saveState();
            CHECK(!b.loadState(bad), "rig with the AU-backed Instrument must be rejected");
            CHECK(rig(beforeBad) == rig(b.saveState()), "rejected state leaves the rig untouched");
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::printf("after destroying A+B: threads=%d rss=%.0f MB\n", threadCount(), rssMB());

    // ── render budget: a dedicated instance with no editor (so no library scan / warm-up competing), the
    //    measuring thread at USER_INTERACTIVE QoS. Wall time of process() vs the buffer; not device xruns. ──
    {
        double la[3] = {0, 0, 0};
        getloadavg(la, 3);
        std::printf("render budget measured with system load average %.1f on %d CPUs\n", la[0], int(std::thread::hardware_concurrency()));
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        PluginCore bud(Format::Test, kRes);
        CHECK(bud.prepare(48000, 512), "prepare budget instance");
        std::vector<float> L, R;
        render(bud, 48000, 512, L, R); // settle
        // ── render budget (wall time of process() vs buffer duration; offline, not device xruns) ──
        for (uint32_t block : {64u, 128u, 256u, 512u}) {
            std::vector<float> oL(block), oR(block);
            std::vector<double> t_el, t_cpu;
            const int blocks = 48000 * 4 / block;
            auto cpu = [] { timespec ts; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; };
            for (int b = 0; b < blocks; ++b) {
                const auto t = std::chrono::steady_clock::now();
                const double c0 = cpu();
                bud.process(g_di.data() + (b * block) % (g_di.size() - block), nullptr, oL.data(), oR.data(), block, nullptr, 0);
                t_cpu.push_back(cpu() - c0);
                t_el.push_back(std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count());
            }
            const double budget = block / 48000.0;
            auto summary = [&](std::vector<double>& v, const char* what) {
                double total = 0;
                int over = 0;
                for (double e : v) { total += e; over += e > budget; }
                std::sort(v.begin(), v.end());
                std::printf("render budget @48k/%u %s: mean %.1f%%, p99 %.1f%%, max %.1f%% of the %.2f ms buffer; %d/%d blocks over\n", block, what,
                            100.0 * total / blocks / budget, 100.0 * v[v.size() * 99 / 100] / budget, 100.0 * v.back() / budget, budget * 1e3, over, blocks);
            };
            summary(t_cpu, "thread CPU");
            summary(t_el, "wall      ");
        }

    }

    // ── trim/bypass events: identical across host block partitions, and same-offset ordering ──
    {
        PluginCore c(Format::Test, kRes);
        CHECK(c.prepare(48000, 512), "prepare partition test");
        c.serverForTests()->setMasterBypass(true); // isolate the wrapper's trims/bypass from engine DSP
        const std::vector<ParamEvent> ev = {{1000, kInputTrim, -6.0f}, {1000, kOutputTrim, 3.0f}, {7777, kBypass, 1.0f},
                                            {12000, kBypass, 0.0f}, {12000, kOutputTrim, -9.0f}, {20000, kInputTrim, 0.0f}};
        std::vector<float> L0, R0, L1, R1;
        std::mt19937 rng(7);
        std::vector<uint32_t> random_part;
        for (int i = 0; i < 400; ++i) random_part.push_back(1 + rng() % 700);
        const std::vector<std::vector<uint32_t>> parts = {{512}, {64}, {300}, {1}, {511, 513, 2049}, random_part};
        for (std::size_t p = 0; p < parts.size(); ++p) {
            c.prepare(48000, 512); // fresh ramps/history for each run
            render(c, 30000, parts[p], L1, R1, false, &ev);
            if (p == 0) { L0 = L1; R0 = R1; continue; }
            const double md = std::max(maxDiff(L0, L1), maxDiff(R0, R1));
            std::printf("partition %zu vs 512: max |diff| = %g\n", p, md);
            CHECK(md == 0.0, "trim/bypass output must not depend on block partition (partition %zu)", p);
        }
        // same-offset ordering: two values at one offset → the later one wins, with no audible step
        const std::vector<ParamEvent> same = {{5000, kOutputTrim, -12.0f}, {5000, kOutputTrim, 0.0f}};
        c.prepare(48000, 512);
        render(c, 12000, 256, L1, R1, false, &same);
        c.prepare(48000, 512);
        render(c, 12000, 256, L0, R0, false, nullptr);
        CHECK(maxDiff(L0, L1) == 0.0, "same-offset events apply in order (diff %g)", maxDiff(L0, L1));
        // informational: the engine itself (not the wrapper) across partitions
        c.serverForTests()->setMasterBypass(false);
        c.prepare(48000, 512);
        render(c, 30000, 512, L0, R0);
        c.prepare(48000, 512);
        render(c, 30000, random_part, L1, R1);
        std::printf("engine (rack active) partition 512 vs random: max |diff| = %g (informational)\n", maxDiff(L0, L1));
    }

    // ── reproducible restored renders: two fresh instances, same state, same timestamped events ──
    {
        const std::vector<ParamEvent> ev = {{4096, kInputTrim, 4.0f}, {20000, kOutputTrim, -2.0f}, {40000, kBypass, 1.0f}, {52000, kBypass, 0.0f}};
        std::vector<float> L1, R1, L2, R2;
        for (int run = 0; run < 2; ++run) {
            PluginCore c(Format::Test, kRes);
            CHECK(c.loadState(blobA), "loadState %d", run);
            CHECK(c.prepare(48000, 512), "prepare %d", run);
            render(c, 72000, 300, run ? L2 : L1, run ? R2 : R1, false, &ev);
        }
        const double md = std::max(maxDiff(L1, L2), maxDiff(R1, R2));
        std::printf("restored renders (fresh instances, same events): max |diff| = %g over %zu frames\n", md, L1.size());
        CHECK(md == 0.0, "renders from the same state must be bit-identical");
        CHECK(stats(L1, 24000).rms > 1e-3, "restored render has sound");
    }

    // ── MIDI rig in two instances; destroy one, render the other (both orders) ──
    // Pass 0/1 run with NO process-wide sink installed (the plugin binary's situation: nothing in it ever
    // calls setGlobalMidiSink, so MIDI units' external output is dropped). Pass 2 installs a sentinel first
    // and checks hosted instances leave it in place.
    for (int order = 0; order < 3; ++order) {
        if (order == 2) rig::setGlobalMidiSink(sentinelSink, &g_sentinel_ctx);
        auto a = std::make_unique<PluginCore>(Format::Test, kRes);
        auto b = std::make_unique<PluginCore>(Format::Test, kRes);
        CHECK(a->prepare(48000, 256) && b->prepare(48000, 256), "prepare midi pair");
        a->serverForTests()->newRackHosted("guitar_synth");
        b->serverForTests()->newRackHosted("guitar_synth");
        std::vector<float> L, R;
        render(*a, 24000, 256, L, R);
        render(*b, 24000, 256, L, R);
        if (order != 1) a.reset(); else b.reset();
        PluginCore& survivor = order != 1 ? *b : *a;
        render(survivor, 48000 * 2, 256, L, R);
        const Stats s = stats(L, 24000);
        std::printf("midi rig survivor (%s destroyed first%s): rms=%.4f\n", order != 1 ? "A" : "B", order == 2 ? ", sentinel sink" : "", s.rms);
        CHECK(s.finite && s.rms > 1e-4, "survivor renders after the other instance is destroyed");
        if (order == 2) {
            std::printf("sentinel MIDI sink received %ld events (hosted instances did not replace it)\n", g_sentinel_events.load());
            CHECK(g_sentinel_events.load() > 0, "sentinel sink still installed after hosted instances started");
            rig::setGlobalMidiSink(nullptr, nullptr);
        }
    }

    poolTest();

    // ── destroy / recreate cycles ──
    for (int cycle = 0; cycle < 3; ++cycle) {
        uint16_t port = 0;
        {
            PluginCore c(Format::Test, kRes);
            CHECK(c.prepare(44100, 1024), "prepare cycle %d", cycle);
            port = c.attachEditor();
            std::vector<float> L, R;
            render(c, 8192, 1024, L, R);
            CHECK(stats(L).finite, "cycle output finite");
        }
        CHECK(port && !portListening(port), "port %u closed after destroy", port);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const int end_threads = threadCount();
    std::printf("end: threads=%d (baseline %d) rss=%.0f MB\n", end_threads, base_threads, rssMB());
    CHECK(end_threads <= base_threads + 1, "all engine threads joined (%d vs %d)", end_threads, base_threads);

    std::printf(g_fail ? "test_plugin_core: %d FAILURE(S)\n" : "test_plugin_core: all checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
