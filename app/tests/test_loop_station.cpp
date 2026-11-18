// LoopStation (global multitrack looper): record / close, overdub, multi-level undo / redo, feedback decay,
// tempo-quantized length and count-in, reverse / ½× / 2×, loop-sync multitrack lengths and phase, stop
// modes, WAV export, the HTTP surface, and no allocation on the audio path.
#include "../src/dsp/LoopStation.hpp"
#include "test_compat.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <new>
#include <string>
#include <sys/stat.h>
#include <vector>

static std::atomic<bool> g_count{false};
static std::atomic<long> g_allocs{0};
void* operator new(std::size_t size) {
    if (g_count.load(std::memory_order_relaxed)) g_allocs.fetch_add(1);
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using rig::LoopStation;
using rig::LoopTrackState;
using rig::LoopSync;
using rig::LoopStopMode;

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d  %s  ", __FILE__, __LINE__, #cond); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

constexpr double kSr = 48000.0;
constexpr uint32_t kBlock = 256;
constexpr double kBpm = 120.0;   // bar = 96000 frames at 4/4

struct Run {
    explicit Run(LoopStation& l) : ls(l) {}
    LoopStation& ls;
    int64_t t = 0;   // input time (frames)
    std::vector<float> out;
    // process `frames` of input from gen(time); keeps the output (left)
    void go(int64_t frames, const std::function<float(int64_t)>& gen, double bpm = kBpm) {
        float l[kBlock], r[kBlock];
        while (frames > 0) {
            const uint32_t n = static_cast<uint32_t>(std::min<int64_t>(frames, kBlock));
            for (uint32_t i = 0; i < n; ++i) l[i] = r[i] = gen ? gen(t + i) : 0.0f;
            g_count.store(true);
            ls.processStereo(l, r, n, bpm);
            g_count.store(false);
            out.insert(out.end(), l, l + n);
            t += n;
            frames -= n;
        }
    }
    void cmd(int c, int track = 0, float v = 0.0f) { ls.post(c, track, v); }
};

static float sineA(int64_t i) { return 0.5f * static_cast<float>(std::sin(2.0 * M_PI * 220.0 * i / kSr)); }
static float sineB(int64_t i) { return 0.3f * static_cast<float>(std::sin(2.0 * M_PI * 331.0 * i / kSr + 0.7)); }
static float sineC(int64_t i) { return 0.2f * static_cast<float>(std::sin(2.0 * M_PI * 517.0 * i / kSr + 1.3)); }

static void testRecordOverdubUndoRedo() {
    LoopStation ls;
    ls.prepare(kSr);
    ls.setMonitor(false);
    Run R(ls);
    R.go(1024, nullptr);
    const int64_t N = kBlock * 188;   // 48128
    // record A: the take starts at input time t0 → index k holds A(t0 + k)
    R.cmd(LoopStation::kPress);
    const int64_t t0 = R.t;
    R.go(N, sineA);
    CHECK(ls.trackState(0) == LoopTrackState::Recording, "state %d", static_cast<int>(ls.trackState(0)));
    R.cmd(LoopStation::kPress);
    R.go(kBlock, nullptr);
    CHECK(ls.trackLength(0) == N, "len %lld", static_cast<long long>(ls.trackLength(0)));
    CHECK(ls.trackState(0) == LoopTrackState::Playing, "state %d", static_cast<int>(ls.trackState(0)));
    CHECK(ls.trackLayers(0) == 1, "layers %d", ls.trackLayers(0));
    R.go(N, nullptr);   // past the progressive seam
    auto A = [&](int64_t k) { return k < 500 ? 0.0f : sineA(t0 + k); };
    // compare away from the seam
    double e = 0.0; int64_t c = 0;
    {
        const int64_t len = ls.trackLength(0);
        for (int64_t done = 0; done < N; done += kBlock) {
            const double ph = ls.trackPhase(0);
            const std::size_t b = R.out.size();
            R.go(kBlock, nullptr);
            for (uint32_t i = 0; i < kBlock; ++i) {
                const int64_t idx = (static_cast<int64_t>(ph) + i) % len;
                if (idx < 500) continue;
                const double d = R.out[b + i] - A(idx);
                e += d * d; ++c;
            }
        }
    }
    CHECK(std::sqrt(e / c) < 1e-4, "loop playback error %g", std::sqrt(e / c));

    // overdub B for one full cycle: remember what was written at each index
    std::vector<float> wB(static_cast<std::size_t>(N), 0.0f);
    std::vector<char> okB(static_cast<std::size_t>(N), 0);
    R.cmd(LoopStation::kPress);
    R.go(kBlock, sineB);   // punch-in ramp
    CHECK(ls.trackState(0) == LoopTrackState::Overdubbing, "state %d", static_cast<int>(ls.trackState(0)));
    for (int64_t done = 0; done < N - 4 * kBlock; done += kBlock) {
        const double ph = ls.trackPhase(0);
        const int64_t t1 = R.t;
        R.go(kBlock, sineB);
        for (uint32_t i = 0; i < kBlock; ++i) {
            const int64_t idx = (static_cast<int64_t>(ph) + i) % N;
            wB[static_cast<std::size_t>(idx)] = sineB(t1 + i);
            okB[static_cast<std::size_t>(idx)] = 1;
        }
    }
    R.cmd(LoopStation::kPress);
    R.go(kBlock, sineB);   // punch-out ramp
    CHECK(ls.trackLayers(0) == 2, "layers %d", ls.trackLayers(0));
    R.go(4096, nullptr);
    auto errMix = [&](bool withB, bool withC, const std::vector<float>* wC, const std::vector<char>* okC) {
        double ee = 0.0; int64_t cc = 0;
        for (int64_t done = 0; done < N; done += kBlock) {
            const double ph = ls.trackPhase(0);
            const std::size_t b = R.out.size();
            R.go(kBlock, nullptr);
            for (uint32_t i = 0; i < kBlock; ++i) {
                const int64_t idx = (static_cast<int64_t>(ph) + i) % N;
                if (idx < 500 || !okB[static_cast<std::size_t>(idx)]) continue;
                if (withC && !(*okC)[static_cast<std::size_t>(idx)]) continue;
                double ex = A(idx) + (withB ? wB[static_cast<std::size_t>(idx)] : 0.0f) + (withC ? (*wC)[static_cast<std::size_t>(idx)] : 0.0f);
                const double d = R.out[b + i] - ex;
                ee += d * d; ++cc;
            }
        }
        return cc ? std::sqrt(ee / cc) : 1.0;
    };
    double em = errMix(true, false, nullptr, nullptr);
    CHECK(em < 1e-3, "A+B error %g", em);

    // second pass C → 3 layers
    std::vector<float> wC(static_cast<std::size_t>(N), 0.0f);
    std::vector<char> okC(static_cast<std::size_t>(N), 0);
    R.cmd(LoopStation::kOverdub);
    R.go(kBlock, sineC);
    for (int64_t done = 0; done < N - 4 * kBlock; done += kBlock) {
        const double ph = ls.trackPhase(0);
        const int64_t t1 = R.t;
        R.go(kBlock, sineC);
        for (uint32_t i = 0; i < kBlock; ++i) {
            const int64_t idx = (static_cast<int64_t>(ph) + i) % N;
            wC[static_cast<std::size_t>(idx)] = sineC(t1 + i);
            okC[static_cast<std::size_t>(idx)] = 1;
        }
    }
    R.cmd(LoopStation::kPlay);
    R.go(4096, nullptr);
    CHECK(ls.trackLayers(0) == 3, "layers %d", ls.trackLayers(0));
    em = errMix(true, true, &wC, &okC);
    CHECK(em < 1e-3, "A+B+C error %g", em);

    // undo twice → A only; redo once → A+B; redo → A+B+C
    R.cmd(LoopStation::kUndo); R.go(kBlock, nullptr);
    R.cmd(LoopStation::kUndo); R.go(8192, nullptr);
    CHECK(ls.trackLayers(0) == 1 && ls.trackRedo(0) == 2, "layers %d redo %d", ls.trackLayers(0), ls.trackRedo(0));
    em = errMix(false, false, nullptr, nullptr);
    CHECK(em < 1e-3, "after 2× undo error %g", em);
    R.cmd(LoopStation::kRedo); R.go(8192, nullptr);
    CHECK(ls.trackLayers(0) == 2 && ls.trackRedo(0) == 1, "layers %d redo %d", ls.trackLayers(0), ls.trackRedo(0));
    em = errMix(true, false, nullptr, nullptr);
    CHECK(em < 1e-3, "after redo error %g", em);
    R.cmd(LoopStation::kUndo); R.go(kBlock, nullptr);   // undo again then a new pass drops the redo stack
    R.cmd(LoopStation::kPress); R.go(4 * kBlock, sineC);
    R.cmd(LoopStation::kPress); R.go(4 * kBlock, nullptr);
    CHECK(ls.trackLayers(0) == 2 && ls.trackRedo(0) == 0, "new pass clears redo: layers %d redo %d", ls.trackLayers(0), ls.trackRedo(0));

    // feedback: a pass ages the layers under it
    ls.setFeedback(0.5f);
    R.cmd(LoopStation::kPress); R.go(8 * kBlock, sineB);
    R.cmd(LoopStation::kPress); R.go(8 * kBlock, nullptr);
    CHECK(std::fabs(ls.layerGain(0, 0) - 0.5f) < 1e-4f && std::fabs(ls.layerGain(0, 1) - 0.5f) < 1e-4f && std::fabs(ls.layerGain(0, 2) - 1.0f) < 1e-4f,
          "decay gains %g %g %g", ls.layerGain(0, 0), ls.layerGain(0, 1), ls.layerGain(0, 2));
    R.cmd(LoopStation::kUndo); R.go(kBlock, nullptr);
    CHECK(std::fabs(ls.layerGain(0, 0) - 1.0f) < 1e-4f, "undo restores gain %g", ls.layerGain(0, 0));
    ls.setFeedback(1.0f);

    // clear
    R.cmd(LoopStation::kClear); R.go(kBlock, nullptr);
    CHECK(ls.trackState(0) == LoopTrackState::Empty && ls.trackLength(0) == 0, "cleared");
    std::printf("record/overdub/undo/redo/feedback: done\n");
}

static void testQuantize() {
    LoopStation ls;
    ls.prepare(kSr);
    ls.setMonitor(false);
    ls.setSync(LoopSync::Tempo);
    ls.setCountIn(false);
    Run R(ls);
    const int64_t bar = 96000;
    // 1.3 bars → nearest bar is 1: truncated
    R.cmd(LoopStation::kPress); R.go(kBlock, sineA);
    CHECK(ls.trackState(0) == LoopTrackState::Recording, "rec starts at once with nothing running");
    R.go(static_cast<int64_t>(1.3 * bar) - kBlock, sineA);
    R.cmd(LoopStation::kPress); R.go(kBlock, sineA);
    CHECK(ls.trackLength(0) == bar, "1.3 bars → %lld", static_cast<long long>(ls.trackLength(0)));
    R.cmd(LoopStation::kClearAll); R.go(kBlock, nullptr);
    // 1.6 bars → keeps recording to bar 2
    R.cmd(LoopStation::kPress); R.go(static_cast<int64_t>(1.6 * bar), sineA);
    R.cmd(LoopStation::kPress); R.go(kBlock, sineA);
    CHECK(ls.trackState(0) == LoopTrackState::Recording, "waits for the bar line");
    R.go(static_cast<int64_t>(0.4 * bar), sineA);
    CHECK(ls.trackLength(0) == 2 * bar && ls.trackState(0) == LoopTrackState::Playing, "1.6 bars → %lld", static_cast<long long>(ls.trackLength(0)));
    R.cmd(LoopStation::kClearAll); R.go(kBlock, nullptr);
    // fixed 2 bars: closes by itself
    ls.setBars(2);
    R.cmd(LoopStation::kPress); R.go(static_cast<int64_t>(2.1 * bar), sineA);
    CHECK(ls.trackLength(0) == 2 * bar && ls.trackState(0) == LoopTrackState::Playing, "bars=2 → %lld", static_cast<long long>(ls.trackLength(0)));
    R.cmd(LoopStation::kClearAll); R.go(kBlock, nullptr);
    ls.setBars(0);
    // count-in: one bar of clicks, then recording on the downbeat
    ls.setCountIn(true);
    R.cmd(LoopStation::kPress); R.go(kBlock, nullptr);
    CHECK(ls.trackState(0) == LoopTrackState::Armed, "armed for count-in");
    const std::size_t o0 = R.out.size();
    R.go(bar - 2 * kBlock, nullptr);
    float pk = 0.0f;
    for (std::size_t i = o0; i < R.out.size(); ++i) pk = std::max(pk, std::fabs(R.out[i]));
    CHECK(pk > 0.05f, "count-in click audible (peak %g)", pk);
    CHECK(ls.trackState(0) == LoopTrackState::Armed, "still counting in");
    R.go(2 * kBlock, nullptr);
    CHECK(ls.trackState(0) == LoopTrackState::Recording, "recording after one bar (state %d)", static_cast<int>(ls.trackState(0)));
    const std::string j = ls.toJson();
    CHECK(j.find("\"sync\":\"tempo\"") != std::string::npos && j.find("\"tempo_locked\":true") != std::string::npos, "json tempo fields");
    std::printf("tempo quantize/count-in: done\n");
}

static void testSpeedReverse() {
    LoopStation ls;
    ls.prepare(kSr);
    ls.setMonitor(false);
    Run R(ls);
    const int64_t N = kBlock * 200;   // 51200
    // a ramp: index k holds ~k/N
    R.cmd(LoopStation::kPress); R.go(kBlock, nullptr);
    const int64_t t0 = R.t - kBlock;
    (void)t0;
    int64_t rec_t0 = -1;
    R.go(N, [&](int64_t t) { if (rec_t0 < 0) rec_t0 = t; return 0.8f * static_cast<float>(t - rec_t0) / static_cast<float>(N); });
    R.cmd(LoopStation::kPress); R.go(kBlock, nullptr);
    const int64_t len = ls.trackLength(0);
    CHECK(len > 0, "recorded");
    auto slopeOver = [&](int64_t frames) {
        // wait until the read position is in the middle third, then measure the slope
        for (int guard = 0; guard < 2000; ++guard) {
            const double p = ls.trackPhase(0) / static_cast<double>(len);
            const bool rev = R.ls.toJson().find("\"reverse\":true") != std::string::npos;
            const double rp = rev ? 1.0 - p : p;
            if (rp > 0.3 && rp < 0.6) break;
            R.go(kBlock, nullptr);
        }
        const std::size_t b = R.out.size();
        R.go(frames, nullptr);
        return (R.out[b + frames - 1] - R.out[b]) / static_cast<double>(frames - 1);
    };
    const double base = 0.8 / static_cast<double>(len);
    double s = slopeOver(1024);
    CHECK(std::fabs(s - base) < base * 0.05, "1× slope %g vs %g", s, base);
    ls.setTrackSpeed(0, 0.5f);
    s = slopeOver(1024);
    CHECK(std::fabs(s - base * 0.5) < base * 0.05, "½× slope %g vs %g", s, base * 0.5);
    ls.setTrackSpeed(0, 2.0f);
    s = slopeOver(1024);
    CHECK(std::fabs(s - base * 2.0) < base * 0.1, "2× slope %g vs %g", s, base * 2.0);
    ls.setTrackSpeed(0, 1.0f);
    ls.setTrackReverse(0, true);
    s = slopeOver(1024);
    CHECK(std::fabs(s + base) < base * 0.05, "reverse slope %g vs %g", s, -base);
    // reversing is continuous: no jump at the switch
    ls.setTrackReverse(0, false);
    R.go(kBlock, nullptr);
    const float a = R.out[R.out.size() - 1];
    ls.setTrackReverse(0, true);
    R.go(kBlock, nullptr);
    const float b = R.out[R.out.size() - kBlock];
    CHECK(std::fabs(a - b) < 0.01f, "reverse switch jump %g → %g", a, b);
    std::printf("reverse / half / double speed: done\n");
}

static bool running(LoopStation& ls) {
    for (int t = 0; t < LoopStation::kTracks; ++t) if (ls.trackState(t) == LoopTrackState::Playing) return true;
    return false;
}

static void testMultitrackSync() {
    LoopStation ls;
    ls.prepare(kSr);
    ls.setMonitor(false);
    ls.setSync(LoopSync::Loop);
    Run R(ls);
    const int64_t L = kBlock * 190;   // 48640
    R.cmd(LoopStation::kPress, 0); R.go(L, sineA);
    R.cmd(LoopStation::kPress, 0); R.go(kBlock, nullptr);
    CHECK(ls.trackLength(0) == L, "master %lld", static_cast<long long>(ls.trackLength(0)));
    R.go(3333, nullptr);
    // track 2 (index 1): ~2.1 masters → 2 masters (truncated)
    R.cmd(LoopStation::kPress, 1); R.go(static_cast<int64_t>(2.1 * L), sineB);
    R.cmd(LoopStation::kPress, 1); R.go(kBlock, nullptr);
    CHECK(ls.trackLength(1) == 2 * L, "track 2 → %lld (want %lld)", static_cast<long long>(ls.trackLength(1)), static_cast<long long>(2 * L));
    // track 3: 0.45 master → ½ master (keeps recording up to it)
    R.cmd(LoopStation::kPress, 2); R.go(static_cast<int64_t>(0.45 * L), sineC);
    R.cmd(LoopStation::kPress, 2); R.go(kBlock, sineC);
    CHECK(ls.trackState(2) == LoopTrackState::Recording, "track 3 waits for ½ master");
    R.go(static_cast<int64_t>(0.1 * L), sineC);
    CHECK(ls.trackLength(2) == L / 2, "track 3 → %lld (want %lld)", static_cast<long long>(ls.trackLength(2)), static_cast<long long>(L / 2));
    // phase relation survives Stop All / All Start
    R.go(kBlock, nullptr);
    auto rel = [&](int a, int b) {
        const int64_t la = ls.trackLength(a);
        return ((static_cast<int64_t>(ls.trackPhase(b)) - static_cast<int64_t>(ls.trackPhase(a))) % la + la) % la;
    };
    const int64_t r01 = rel(0, 1) % L, r02 = rel(2, 0) % (L / 2);
    R.cmd(LoopStation::kStopInstantAll); R.go(4 * kBlock, nullptr);
    CHECK(ls.trackState(0) == LoopTrackState::Stopped && ls.trackState(1) == LoopTrackState::Stopped && ls.trackState(2) == LoopTrackState::Stopped, "all stopped");
    R.go(12345, nullptr);
    R.cmd(LoopStation::kAllStartStop); R.go(kBlock, nullptr);
    CHECK(running(ls) , "restarted");
    const int64_t s01 = rel(0, 1) % L, s02 = rel(2, 0) % (L / 2);
    CHECK(std::llabs(s01 - r01) <= 2 || std::llabs(s01 - r01) >= L - 2, "track 1↔2 phase %lld vs %lld", static_cast<long long>(s01), static_cast<long long>(r01));
    CHECK(std::llabs(s02 - r02) <= 2 || std::llabs(s02 - r02) >= L / 2 - 2, "track 1↔3 phase %lld vs %lld", static_cast<long long>(s02), static_cast<long long>(r02));
    CHECK(ls.trackPhase(1) < 2.0 * kBlock, "longest loop restarts from its top (%g)", ls.trackPhase(1));
    // a stopped track restarts on its grid line while others play
    R.cmd(LoopStation::kStop, 2, 1.0f); R.go(4 * kBlock, nullptr);
    CHECK(ls.trackState(2) == LoopTrackState::Stopped, "track 3 stopped");
    R.go(777, nullptr);
    R.cmd(LoopStation::kPress, 2); R.go(kBlock, nullptr);
    CHECK(ls.trackState(2) == LoopTrackState::Armed, "track 3 waits for the master cycle (state %d)", static_cast<int>(ls.trackState(2)));
    R.go(L, nullptr);
    CHECK(ls.trackState(2) == LoopTrackState::Playing, "track 3 playing again");
    const int64_t a02 = rel(2, 0) % (L / 2);
    CHECK(std::llabs(a02 - r02) <= 2 || std::llabs(a02 - r02) >= L / 2 - 2, "track 3 rejoined in phase %lld vs %lld", static_cast<long long>(a02), static_cast<long long>(r02));

    // tempo sync: a second track arms for the next bar line
    LoopStation ts;
    ts.prepare(kSr);
    ts.setMonitor(false);
    ts.setSync(LoopSync::Tempo);
    ts.setCountIn(false);
    Run T(ts);
    const int64_t bar = 96000;
    T.cmd(LoopStation::kPress, 0); T.go(static_cast<int64_t>(2 * bar - 1000), sineA);
    T.cmd(LoopStation::kPress, 0); T.go(2000, sineA);
    CHECK(ts.trackLength(0) == 2 * bar, "tempo master %lld", static_cast<long long>(ts.trackLength(0)));
    T.go(static_cast<int64_t>(0.3 * bar), nullptr);   // now ~0.32 bars into bar 3
    T.cmd(LoopStation::kPress, 1); T.go(kBlock, nullptr);
    CHECK(ts.trackState(1) == LoopTrackState::Armed, "armed until the bar line");
    const int64_t clk = ts.clockFrames();
    const int64_t wait = (bar - clk % bar) % bar;
    T.go(wait - kBlock, nullptr);
    CHECK(ts.trackState(1) == LoopTrackState::Armed, "still armed just before the bar");
    T.go(2 * kBlock, sineB);
    CHECK(ts.trackState(1) == LoopTrackState::Recording, "recording from the bar line");
    std::printf("multitrack loop/tempo sync: done\n");
}

static void testStopModesAndExport() {
    LoopStation ls;
    ls.prepare(kSr);
    ls.setMonitor(false);
    Run R(ls);
    const int64_t N = kBlock * 100;
    R.cmd(LoopStation::kPress); R.go(N, sineA);
    R.cmd(LoopStation::kPress); R.go(kBlock, nullptr);
    // instant
    R.cmd(LoopStation::kStopAll); R.go(2 * kBlock, nullptr);
    CHECK(ls.trackState(0) == LoopTrackState::Stopped, "instant stop (state %d)", static_cast<int>(ls.trackState(0)));
    // end of loop
    ls.setStopMode(LoopStopMode::LoopEnd);
    R.cmd(LoopStation::kAllStartStop); R.go(N / 2, nullptr);
    R.cmd(LoopStation::kAllStartStop); R.go(kBlock, nullptr);
    CHECK(ls.trackState(0) == LoopTrackState::Playing, "keeps playing to the loop end");
    R.go(N / 2 - 2 * kBlock, nullptr);
    CHECK(ls.trackState(0) == LoopTrackState::Playing, "still before the end");
    R.go(3 * kBlock, nullptr);
    CHECK(ls.trackState(0) == LoopTrackState::Stopped, "stopped at the loop end (state %d, phase %g)", static_cast<int>(ls.trackState(0)), ls.trackPhase(0));
    // fade
    ls.setStopMode(LoopStopMode::Fade);
    ls.setFadeSeconds(0.5f);
    R.cmd(LoopStation::kAllStartStop); R.go(4 * kBlock, nullptr);
    R.cmd(LoopStation::kAllStartStop); R.go(kBlock, nullptr);
    const std::size_t f0 = R.out.size();
    R.go(static_cast<int64_t>(0.45 * kSr), nullptr);
    CHECK(ls.trackState(0) == LoopTrackState::Playing, "fading");
    float early = 0.0f, late = 0.0f;
    for (std::size_t i = f0; i < f0 + 4800; ++i) early = std::max(early, std::fabs(R.out[i]));
    for (std::size_t i = R.out.size() - 4800; i < R.out.size(); ++i) late = std::max(late, std::fabs(R.out[i]));
    CHECK(late < early * 0.25f, "fade level %g → %g", early, late);
    R.go(static_cast<int64_t>(0.1 * kSr), nullptr);
    CHECK(ls.trackState(0) == LoopTrackState::Stopped, "stopped after the fade");

    // export (mix and the HTTP surface)
    const std::string dir = rig_test::tmpPath("headroom-loopstation-test");
    rig_test::makeDir(dir);
    std::string err;
    const std::string p = dir + "/mix.wav";
    CHECK(ls.exportWav(p, -1, &err), "export: %s", err.c_str());
    struct stat stt{};
    ::stat(p.c_str(), &stt);
    CHECK(stt.st_size == 44 + N * 6, "wav size %lld (want %lld)", static_cast<long long>(stt.st_size), static_cast<long long>(44 + N * 6));
    std::remove(p.c_str());
    auto q = [](std::initializer_list<std::pair<const char*, const char*>> kv) {
        std::vector<std::pair<std::string, std::string>> v;
        for (auto& e : kv) v.emplace_back(e.first, e.second);
        return [v](const char* k) { for (auto& e : v) if (e.first == k) return e.second; return std::string(); };
    };
    std::string j = ls.handleApi("/api/looper", q({{"action", "export"}, {"what", "stems"}}), dir);
    CHECK(j.find("\"ok\":true") != std::string::npos && j.find("Track 1.wav") != std::string::npos, "stems export %s", j.c_str());
    std::system(("rm -rf '" + dir + "'").c_str());
    j = ls.handleApi("/api/looper", q({{"sync", "loop"}, {"track", "1"}, {"pan", "-0.5"}, {"speed", "2"}, {"stop_mode", "loop"}}), dir);
    CHECK(j.find("\"sync\":\"loop\"") != std::string::npos && j.find("\"stop_mode\":\"loop\"") != std::string::npos, "settings json");
    j = ls.handleApi("/api/looper/wave", q({{"track", "0"}, {"bins", "64"}}), dir);
    CHECK(j.find("\"layers\":[{\"gain\":1.000") != std::string::npos, "wave json %s", j.substr(0, 120).c_str());
    std::printf("stop modes / export / api: done\n");
}

int main() {
    testRecordOverdubUndoRedo();
    testQuantize();
    testSpeedReverse();
    testMultitrackSync();
    testStopModesAndExport();
    CHECK(g_allocs.load() == 0, "audio path allocated %ld times", g_allocs.load());
    if (g_fail) { std::printf("%d check(s) failed\n", g_fail); return 1; }
    std::printf("loop_station: all checks passed\n");
    return 0;
}
