#pragma once
// Live engine diagnostics for GET /api/diag (docs/ENGINE_API.md → "Diagnostics").
//
// Audio side (the render thread and the RtPool cores that render units for it): single-writer relaxed atomic
// counters, no locks, no allocation, no system calls. Every value is kept twice: cumulative (since start or the last
// ?reset=1) and in a ring of 250 ms buckets keyed by the render's own steady-clock time, so the reader can sum the
// last four complete buckets for a one-second window without any thread having to tick. A ring of per-block render
// times gives the window's exact p99; the cumulative p99 comes from a log histogram (1/8 octave bins).
//
// Reader side (HTTP thread, the HEADROOM_DIAG_LOG writer): plain snapshots of those atomics. A reset is a request the
// render thread carries out at the start of its next block, so it never races the writers.
#include "IComponent.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <thread>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <pthread.h>
#include <sys/resource.h>
#include <time.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sys/resource.h>
#include <time.h>
#endif

namespace rig {

inline uint64_t diagNowNs() noexcept {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// CPU time (user + system) of another thread, in seconds; -1 when unknown (not running, or no support). Control threads.
inline double threadCpuSeconds(std::thread::native_handle_type h) noexcept {
#if defined(__APPLE__)
    const mach_port_t port = pthread_mach_thread_np(h);
    if (port == MACH_PORT_NULL) return -1.0;
    thread_basic_info_data_t info{};
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    if (thread_info(port, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &count) != KERN_SUCCESS) return -1.0;
    return info.user_time.seconds + info.user_time.microseconds * 1e-6 + info.system_time.seconds + info.system_time.microseconds * 1e-6;
#elif defined(__linux__)
    clockid_t cid;
    if (pthread_getcpuclockid(h, &cid) != 0) return -1.0;
    timespec ts{};
    if (clock_gettime(cid, &ts) != 0) return -1.0;
    return ts.tv_sec + ts.tv_nsec * 1e-9;
#else
    (void)h;
    return -1.0;
#endif
}
inline double threadCpuSeconds(const std::thread& t) noexcept {
    return t.joinable() ? threadCpuSeconds(const_cast<std::thread&>(t).native_handle()) : -1.0;
}
// The calling thread's CPU time (control threads only).
inline double selfThreadCpuSeconds() noexcept {
#if defined(__APPLE__) || defined(__linux__)
    timespec ts{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return -1.0;
    return ts.tv_sec + ts.tv_nsec * 1e-9;
#else
    return -1.0;
#endif
}
inline double processCpuSeconds() noexcept {
#if defined(__APPLE__) || defined(__linux__)
    rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) != 0) return -1.0;
    return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec * 1e-6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec * 1e-6;
#else
    return -1.0;
#endif
}

class EngineDiagBank {
public:
    static constexpr int kSlots = 64;                       // rack slots tracked (active_idx); later ones are not
    static constexpr int kBuckets = 8;                      // ring of 250 ms buckets
    static constexpr uint64_t kBucketNs = 250'000'000ull;
    static constexpr int kWindowBuckets = 4;                // the window: the last 4 complete buckets (1 s)
    static constexpr uint32_t kRing = 4096;                 // per-block render times (power of two)
    static constexpr int kHistBins = 8 * 22;                // 1 µs .. ~4 s, 1/8 octave

    // ---------- plain snapshots (reader) ----------
    struct RenderVals {
        uint64_t blocks = 0, over = 0, ns_sum = 0, ns_max = 0, budget_sum = 0, frames = 0, bridges = 0;
        uint64_t gaps = 0, gap_ns_sum = 0, gap_max_ns = 0; // callbacks that came late: device cycles the render never saw
        void merge(const RenderVals& o) {
            blocks += o.blocks, over += o.over, ns_sum += o.ns_sum, budget_sum += o.budget_sum, frames += o.frames, bridges += o.bridges;
            gaps += o.gaps, gap_ns_sum += o.gap_ns_sum;
            ns_max = std::max(ns_max, o.ns_max), gap_max_ns = std::max(gap_max_ns, o.gap_max_ns);
        }
    };
    struct OutputVals {
        uint64_t samples = 0, clip_l = 0, clip_r = 0, guard_l = 0, guard_r = 0, nonfinite = 0, in_clip = 0;
        float peak_l = 0, peak_r = 0, pre_guard_peak = 0, in_peak = 0;
        double sumsq = 0;
        void merge(const OutputVals& o) {
            samples += o.samples, clip_l += o.clip_l, clip_r += o.clip_r, guard_l += o.guard_l, guard_r += o.guard_r;
            nonfinite += o.nonfinite, in_clip += o.in_clip, sumsq += o.sumsq;
            peak_l = std::max(peak_l, o.peak_l), peak_r = std::max(peak_r, o.peak_r);
            pre_guard_peak = std::max(pre_guard_peak, o.pre_guard_peak), in_peak = std::max(in_peak, o.in_peak);
        }
    };
    struct MasterVals {
        uint64_t samples = 0, limiter = 0, soft = 0, nonfinite = 0;
        float limiter_max_env = 1.0f, rig_peak = 0, post_gain_peak = 0;
        double rig_sumsq = 0;
        void merge(const MasterVals& o) {
            samples += o.samples, limiter += o.limiter, soft += o.soft, nonfinite += o.nonfinite, rig_sumsq += o.rig_sumsq;
            limiter_max_env = std::max(limiter_max_env, o.limiter_max_env);
            rig_peak = std::max(rig_peak, o.rig_peak), post_gain_peak = std::max(post_gain_peak, o.post_gain_peak);
        }
    };
    struct SlotVals {
        uint64_t key = 0;
        uint64_t blocks = 0, frames = 0, ns_sum = 0, ns_max = 0, over_share = 0, over_block = 0, nonfinite = 0;
        float out_peak = 0, max_load = 0;
        double out_sumsq = 0;
        uint64_t c_samples = 0, c_iters = 0, c_homotopies = 0, c_failures = 0, c_ptc = 0, c_half = 0, c_lte = 0;
        void merge(const SlotVals& o) {
            blocks += o.blocks, frames += o.frames, ns_sum += o.ns_sum, over_share += o.over_share, over_block += o.over_block;
            nonfinite += o.nonfinite, out_sumsq += o.out_sumsq;
            ns_max = std::max(ns_max, o.ns_max), out_peak = std::max(out_peak, o.out_peak), max_load = std::max(max_load, o.max_load);
            c_samples += o.c_samples, c_iters += o.c_iters, c_homotopies += o.c_homotopies, c_failures += o.c_failures;
            c_ptc += o.c_ptc, c_half += o.c_half, c_lte += o.c_lte;
        }
    };
    struct SlotInfo { // cumulative side only
        int32_t table_id = 0;
        uint64_t last_ns = 0, since_ns = 0;
        bool circuit = false, multirate = false;
        int oversampling = 0, max_iterations = 0;
        double internal_rate = 0;
    };
    struct PipeVals { uint64_t runs = 0, stages = 0; int last_stages = 0; };

    // ---------- render thread: the server's block ----------
    // Start of a device callback. Applies a pending reset and starts a new bucket when the 250 ms epoch changed.
    void beginRender(uint64_t now_ns) noexcept {
        block_ns_.store(now_ns, std::memory_order_relaxed);
        const uint32_t req = reset_req_.load(std::memory_order_acquire);
        if (req != reset_done_.load(std::memory_order_relaxed)) {
            cum_.clear();
            for (auto& s : slot_cum_) s.clear(false);
            for (auto& b : hist_) b.store(0, std::memory_order_relaxed);
            pipe_runs_.store(0, std::memory_order_relaxed);
            pipe_stages_.store(0, std::memory_order_relaxed);
            reset_ns_.store(now_ns, std::memory_order_relaxed);
            reset_done_.store(req, std::memory_order_release);
        }
        if (start_ns_.load(std::memory_order_relaxed) == 0) {
            start_ns_.store(now_ns, std::memory_order_relaxed);
            reset_ns_.store(now_ns, std::memory_order_relaxed);
        }
        const uint64_t epoch = now_ns / kBucketNs;
        if (epoch != cur_epoch_) {
            cur_epoch_ = epoch;
            Bucket& b = buckets_[epoch % kBuckets];
            b.epoch.store(kNoEpoch, std::memory_order_release);
            b.clear();
            b.epoch.store(epoch, std::memory_order_release);
            cur_bucket_.store(static_cast<int>(epoch % kBuckets), std::memory_order_relaxed);
        }
        // A callback later than its predecessor's period (+50 %, +1 ms): the device skipped cycles (an overload) or
        // stopped calling. The missing time is what the listener heard as a dropout.
        if (prev_begin_ns_ && prev_period_ns_ && now_ns > prev_begin_ns_) {
            const uint64_t gap = now_ns - prev_begin_ns_;
            if (gap > prev_period_ns_ + prev_period_ns_ / 2 + 1'000'000ull) {
                const uint64_t missing = gap - prev_period_ns_;
                for (RenderA* r : {&cum_.render, &bucket().render}) {
                    inc(r->gaps, 1);
                    inc(r->gap_ns_sum, missing);
                    maxu(r->gap_max_ns, gap);
                }
            }
        }
        prev_begin_ns_ = now_ns;
    }
    // End of a device callback: its wall time against the period it had.
    void endRender(uint64_t end_ns, uint64_t elapsed_ns, uint64_t budget_ns, uint32_t frames) noexcept {
        auto apply = [&](RenderA& r) {
            inc(r.blocks, 1);
            if (elapsed_ns > budget_ns) inc(r.over, 1);
            inc(r.ns_sum, elapsed_ns);
            maxu(r.ns_max, elapsed_ns);
            inc(r.budget_sum, budget_ns);
            inc(r.frames, frames);
        };
        apply(cum_.render);
        apply(bucket().render);
        const double us = std::max(1.0, static_cast<double>(elapsed_ns) * 1e-3);
        const int bin = std::clamp(static_cast<int>(8.0 * std::log2(us)), 0, kHistBins - 1);
        inc(hist_[static_cast<std::size_t>(bin)], 1);
        const uint32_t p = ring_pos_.load(std::memory_order_relaxed);
        ring_t_[p & (kRing - 1)].store(end_ns, std::memory_order_relaxed);
        ring_e_[p & (kRing - 1)].store(elapsed_ns, std::memory_order_relaxed);
        ring_b_[p & (kRing - 1)].store(budget_ns, std::memory_order_relaxed);
        ring_pos_.store(p + 1, std::memory_order_release);
        last_frames_.store(frames, std::memory_order_relaxed);
        prev_period_ns_ = budget_ns;
        sample_budget_ns_.store(frames ? budget_ns / frames : 0, std::memory_order_relaxed);
    }
    void noteBridge() noexcept { // an output bridge (a non-continuous plan handoff) started
        inc(cum_.render.bridges, 1);
        inc(bucket().render.bridges, 1);
    }
    // The output stage of one chunk (after the guard and the backing-track mix).
    void output(const OutputVals& v) noexcept {
        auto apply = [&](OutputA& o) {
            inc(o.samples, v.samples), inc(o.clip_l, v.clip_l), inc(o.clip_r, v.clip_r), inc(o.guard_l, v.guard_l), inc(o.guard_r, v.guard_r);
            inc(o.nonfinite, v.nonfinite), inc(o.in_clip, v.in_clip);
            maxf(o.peak_l, v.peak_l), maxf(o.peak_r, v.peak_r), maxf(o.pre_guard_peak, v.pre_guard_peak), maxf(o.in_peak, v.in_peak);
            addd(o.sumsq, v.sumsq);
        };
        apply(cum_.out);
        apply(bucket().out);
    }

    // ---------- render thread: RackGraph ----------
    // Once per RackGraph::process call: units rendered in the previous call (the per-unit budget share).
    void beginGraph() noexcept { units_prev_ = std::max<uint32_t>(1, units_cur_.exchange(0, std::memory_order_relaxed)); }
    void master(const MasterVals& v, float preset_gain_now) noexcept {
        auto apply = [&](MasterA& m) {
            inc(m.samples, v.samples), inc(m.limiter, v.limiter), inc(m.soft, v.soft), inc(m.nonfinite, v.nonfinite);
            maxf(m.limiter_max_env, v.limiter_max_env), maxf(m.rig_peak, v.rig_peak), maxf(m.post_gain_peak, v.post_gain_peak);
            addd(m.rig_sumsq, v.rig_sumsq);
        };
        apply(cum_.master);
        apply(bucket().master);
        preset_gain_now_.store(preset_gain_now, std::memory_order_relaxed);
    }
    // A pipelined series run (RackGraph::processSeriesPipelined); may run on two cores at once (split lanes).
    void notePipeline(int stages) noexcept {
        pipe_runs_.fetch_add(1, std::memory_order_relaxed);
        pipe_stages_.fetch_add(static_cast<uint64_t>(stages), std::memory_order_relaxed);
        pipe_last_.store(stages, std::memory_order_relaxed);
    }
    // One unit's whole block (the sums of its sub-blocks when pipelined). Called by whichever core rendered it; one
    // writer per slot per block. `key` identifies the unit instance (its address): a new instance restarts the slot.
    void slot(std::size_t idx, uint64_t key, int32_t table_id, uint64_t ns, uint32_t frames, double sample_rate, float peak, double sumsq,
              uint32_t nonfinite, const UnitSolverStats* ss) noexcept {
        units_cur_.fetch_add(1, std::memory_order_relaxed);
        if (idx >= static_cast<std::size_t>(kSlots) || frames == 0) return;
        SlotCum& sc = slot_cum_[idx];
        const uint64_t now = block_ns_.load(std::memory_order_relaxed);
        if (sc.a.key.load(std::memory_order_relaxed) != key) {
            sc.clear(true);
            sc.a.key.store(key, std::memory_order_relaxed);
            sc.table_id.store(table_id, std::memory_order_relaxed);
            sc.since_ns.store(now, std::memory_order_relaxed);
            sc.prev = ss ? *ss : UnitSolverStats{};
            sc.have_prev = ss != nullptr;
        }
        SlotA& sb = bucket().slots[idx];
        if (sb.key.load(std::memory_order_relaxed) != key) {
            sb.clear();
            sb.key.store(key, std::memory_order_relaxed);
        }
        const uint64_t budget = static_cast<uint64_t>(1e9 * frames / std::max(1.0, sample_rate));
        const uint64_t share = budget / units_prev_;
        const float load = budget ? static_cast<float>(static_cast<double>(ns) / static_cast<double>(budget)) : 0.0f;
        UnitSolverStats d{};
        if (ss) {
            if (!sc.have_prev || ss->samples < sc.prev.samples || ss->newton_iterations < sc.prev.newton_iterations) sc.prev = UnitSolverStats{};
            d.samples = ss->samples - sc.prev.samples;
            d.newton_iterations = ss->newton_iterations - sc.prev.newton_iterations;
            d.homotopies = ss->homotopies - std::min(ss->homotopies, sc.prev.homotopies);
            d.failures = ss->failures - std::min(ss->failures, sc.prev.failures);
            d.ptc_rescues = ss->ptc_rescues - std::min(ss->ptc_rescues, sc.prev.ptc_rescues);
            d.half_steps = ss->half_steps - std::min(ss->half_steps, sc.prev.half_steps);
            d.lte_substeps = ss->lte_substeps - std::min(ss->lte_substeps, sc.prev.lte_substeps);
            sc.prev = *ss;
            sc.have_prev = true;
            sc.circuit.store(true, std::memory_order_relaxed);
            sc.internal_rate.store(ss->internal_rate, std::memory_order_relaxed);
            sc.oversampling.store(ss->oversampling, std::memory_order_relaxed);
            sc.multirate.store(ss->multirate, std::memory_order_relaxed);
            sc.max_iterations.store(ss->max_iterations, std::memory_order_relaxed);
        }
        auto apply = [&](SlotA& s) {
            inc(s.blocks, 1), inc(s.frames, frames), inc(s.ns_sum, ns), maxu(s.ns_max, ns);
            if (ns > share) inc(s.over_share, 1);
            if (ns > budget) inc(s.over_block, 1);
            inc(s.nonfinite, nonfinite);
            maxf(s.out_peak, peak), maxf(s.max_load, load);
            addd(s.out_sumsq, sumsq);
            if (ss) {
                inc(s.c_samples, d.samples), inc(s.c_iters, d.newton_iterations), inc(s.c_homotopies, d.homotopies);
                inc(s.c_failures, d.failures), inc(s.c_ptc, d.ptc_rescues), inc(s.c_half, d.half_steps), inc(s.c_lte, d.lte_substeps);
            }
        };
        apply(sc.a);
        apply(sb);
        sc.last_ns.store(now, std::memory_order_relaxed);
    }

    // ---------- reader ----------
    void requestReset() noexcept { reset_req_.fetch_add(1, std::memory_order_acq_rel); }
    bool resetPending() const noexcept { return reset_req_.load(std::memory_order_acquire) != reset_done_.load(std::memory_order_acquire); }
    uint64_t startNs() const noexcept { return start_ns_.load(std::memory_order_relaxed); }
    uint64_t resetNs() const noexcept { return reset_ns_.load(std::memory_order_relaxed); }
    uint64_t lastBlockNs() const noexcept { return block_ns_.load(std::memory_order_relaxed); }
    uint32_t lastFrames() const noexcept { return last_frames_.load(std::memory_order_relaxed); }
    float presetGainNow() const noexcept { return preset_gain_now_.load(std::memory_order_relaxed); }
    PipeVals pipeline() const noexcept {
        return {pipe_runs_.load(std::memory_order_relaxed), pipe_stages_.load(std::memory_order_relaxed), pipe_last_.load(std::memory_order_relaxed)};
    }

    RenderVals renderTotal() const noexcept { return read(cum_.render); }
    OutputVals outputTotal() const noexcept { return read(cum_.out); }
    MasterVals masterTotal() const noexcept { return read(cum_.master); }
    SlotVals slotTotal(int i) const noexcept { return read(slot_cum_[static_cast<std::size_t>(i)].a); }
    SlotInfo slotInfo(int i) const noexcept {
        const SlotCum& s = slot_cum_[static_cast<std::size_t>(i)];
        SlotInfo o;
        o.table_id = s.table_id.load(std::memory_order_relaxed);
        o.last_ns = s.last_ns.load(std::memory_order_relaxed);
        o.since_ns = s.since_ns.load(std::memory_order_relaxed);
        o.circuit = s.circuit.load(std::memory_order_relaxed);
        o.multirate = s.multirate.load(std::memory_order_relaxed);
        o.oversampling = s.oversampling.load(std::memory_order_relaxed);
        o.max_iterations = s.max_iterations.load(std::memory_order_relaxed);
        o.internal_rate = s.internal_rate.load(std::memory_order_relaxed);
        return o;
    }

    // The window: the last kWindowBuckets complete buckets before `now_ns`. `covered` = how many had data.
    struct Window {
        RenderVals render;
        OutputVals out;
        MasterVals master;
        std::array<SlotVals, kSlots> slots{};
        int covered = 0;
    };
    void window(uint64_t now_ns, Window& w) const noexcept {
        w = Window{};
        const uint64_t cur = now_ns / kBucketNs;
        for (int k = 1; k <= kWindowBuckets; ++k) {
            if (cur < static_cast<uint64_t>(k)) break;
            const uint64_t e = cur - static_cast<uint64_t>(k);
            const Bucket& b = buckets_[e % kBuckets];
            if (b.epoch.load(std::memory_order_acquire) != e) continue;
            Window part;
            part.render = read(b.render);
            part.out = read(b.out);
            part.master = read(b.master);
            for (int s = 0; s < kSlots; ++s) part.slots[static_cast<std::size_t>(s)] = read(b.slots[static_cast<std::size_t>(s)]);
            if (b.epoch.load(std::memory_order_acquire) != e) continue; // recycled while read
            w.render.merge(part.render);
            w.out.merge(part.out);
            w.master.merge(part.master);
            for (int s = 0; s < kSlots; ++s) {
                auto& dst = w.slots[static_cast<std::size_t>(s)];
                const auto& src = part.slots[static_cast<std::size_t>(s)];
                if (src.blocks == 0) continue;
                const uint64_t key = src.key;
                if (dst.blocks && dst.key != key) dst = SlotVals{}; // another unit lived here before: keep the newer one
                dst.merge(src);
                dst.key = key;
            }
            ++w.covered;
        }
    }
    // Exact percentile of the block render times that ended in [now - span, now]; also their count.
    double windowPercentileNs(uint64_t now_ns, uint64_t span_ns, double pct, std::size_t* n_out = nullptr) const {
        static thread_local std::array<uint64_t, kRing> v; // reader threads only
        const uint32_t pos = ring_pos_.load(std::memory_order_acquire);
        std::size_t n = 0;
        for (uint32_t k = 1; k <= kRing && k <= pos; ++k) {
            const uint32_t i = (pos - k) & (kRing - 1);
            const uint64_t t = ring_t_[i].load(std::memory_order_relaxed);
            if (t + span_ns < now_ns || t > now_ns + kBucketNs) break;
            v[n++] = ring_e_[i].load(std::memory_order_relaxed);
        }
        if (n_out) *n_out = n;
        if (n == 0) return 0.0;
        const std::size_t r = std::min(n - 1, static_cast<std::size_t>(std::ceil(pct * static_cast<double>(n))) - (pct > 0 ? 1 : 0));
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(r), v.begin() + static_cast<std::ptrdiff_t>(n));
        return static_cast<double>(v[r]);
    }
    // Cumulative percentile from the histogram: the upper edge of the bin it falls in (≈9 % resolution).
    double totalPercentileNs(double pct) const noexcept {
        uint64_t total = 0;
        for (const auto& b : hist_) total += b.load(std::memory_order_relaxed);
        if (!total) return 0.0;
        const auto want = static_cast<uint64_t>(std::ceil(pct * static_cast<double>(total)));
        uint64_t acc = 0;
        for (int i = 0; i < kHistBins; ++i) {
            acc += hist_[static_cast<std::size_t>(i)].load(std::memory_order_relaxed);
            if (acc >= want) return 1e3 * std::exp2((i + 1) / 8.0);
        }
        return 1e3 * std::exp2(kHistBins / 8.0);
    }

    static bool nonFinite(float x) noexcept { // independent of -ffast-math
        uint32_t b;
        std::memcpy(&b, &x, sizeof b);
        return (b & 0x7f800000u) == 0x7f800000u;
    }

private:
    static constexpr uint64_t kNoEpoch = ~0ull;
    using U = std::atomic<uint64_t>;
    using F = std::atomic<float>;
    using D = std::atomic<double>;
    static void inc(U& a, uint64_t v) noexcept { if (v) a.store(a.load(std::memory_order_relaxed) + v, std::memory_order_relaxed); }
    static void maxu(U& a, uint64_t v) noexcept { if (v > a.load(std::memory_order_relaxed)) a.store(v, std::memory_order_relaxed); }
    static void maxf(F& a, float v) noexcept { if (v > a.load(std::memory_order_relaxed)) a.store(v, std::memory_order_relaxed); }
    static void addd(D& a, double v) noexcept { a.store(a.load(std::memory_order_relaxed) + v, std::memory_order_relaxed); }

    struct RenderA {
        U blocks{0}, over{0}, ns_sum{0}, ns_max{0}, budget_sum{0}, frames{0}, bridges{0}, gaps{0}, gap_ns_sum{0}, gap_max_ns{0};
        void clear() noexcept {
            for (U* u : {&blocks, &over, &ns_sum, &ns_max, &budget_sum, &frames, &bridges, &gaps, &gap_ns_sum, &gap_max_ns})
                u->store(0, std::memory_order_relaxed);
        }
    };
    struct OutputA {
        U samples{0}, clip_l{0}, clip_r{0}, guard_l{0}, guard_r{0}, nonfinite{0}, in_clip{0};
        F peak_l{0}, peak_r{0}, pre_guard_peak{0}, in_peak{0};
        D sumsq{0};
        void clear() noexcept {
            for (U* u : {&samples, &clip_l, &clip_r, &guard_l, &guard_r, &nonfinite, &in_clip}) u->store(0, std::memory_order_relaxed);
            for (F* f : {&peak_l, &peak_r, &pre_guard_peak, &in_peak}) f->store(0, std::memory_order_relaxed);
            sumsq.store(0, std::memory_order_relaxed);
        }
    };
    struct MasterA {
        U samples{0}, limiter{0}, soft{0}, nonfinite{0};
        F limiter_max_env{1.0f}, rig_peak{0}, post_gain_peak{0};
        D rig_sumsq{0};
        void clear() noexcept {
            for (U* u : {&samples, &limiter, &soft, &nonfinite}) u->store(0, std::memory_order_relaxed);
            limiter_max_env.store(1.0f, std::memory_order_relaxed);
            rig_peak.store(0, std::memory_order_relaxed);
            post_gain_peak.store(0, std::memory_order_relaxed);
            rig_sumsq.store(0, std::memory_order_relaxed);
        }
    };
    struct SlotA {
        U key{0};
        U blocks{0}, frames{0}, ns_sum{0}, ns_max{0}, over_share{0}, over_block{0}, nonfinite{0};
        F out_peak{0}, max_load{0};
        D out_sumsq{0};
        U c_samples{0}, c_iters{0}, c_homotopies{0}, c_failures{0}, c_ptc{0}, c_half{0}, c_lte{0};
        void clear() noexcept {
            for (U* u : {&blocks, &frames, &ns_sum, &ns_max, &over_share, &over_block, &nonfinite, &c_samples, &c_iters, &c_homotopies,
                         &c_failures, &c_ptc, &c_half, &c_lte})
                u->store(0, std::memory_order_relaxed);
            out_peak.store(0, std::memory_order_relaxed);
            max_load.store(0, std::memory_order_relaxed);
            out_sumsq.store(0, std::memory_order_relaxed);
        }
    };
    struct SlotCum {
        SlotA a;
        std::atomic<int32_t> table_id{0};
        U last_ns{0}, since_ns{0};
        std::atomic<bool> circuit{false}, multirate{false};
        std::atomic<int> oversampling{0}, max_iterations{0};
        D internal_rate{0};
        UnitSolverStats prev{}; // writer only: the last totals, for deltas
        bool have_prev = false;
        void clear(bool identity) noexcept {
            a.clear();
            if (identity) {
                circuit.store(false, std::memory_order_relaxed);
                multirate.store(false, std::memory_order_relaxed);
                oversampling.store(0, std::memory_order_relaxed);
                max_iterations.store(0, std::memory_order_relaxed);
                internal_rate.store(0, std::memory_order_relaxed);
                have_prev = false;
            }
        }
    };
    struct Cum {
        RenderA render;
        OutputA out;
        MasterA master;
        void clear() noexcept { render.clear(), out.clear(), master.clear(); }
    };
    struct Bucket {
        std::atomic<uint64_t> epoch{kNoEpoch};
        RenderA render;
        OutputA out;
        MasterA master;
        std::array<SlotA, kSlots> slots;
        void clear() noexcept {
            render.clear(), out.clear(), master.clear();
            for (auto& s : slots) {
                s.clear();
                s.key.store(0, std::memory_order_relaxed);
            }
        }
    };

    Bucket& bucket() noexcept { return buckets_[static_cast<std::size_t>(cur_bucket_.load(std::memory_order_relaxed))]; }

    static RenderVals read(const RenderA& a) noexcept {
        RenderVals v;
        v.blocks = a.blocks.load(std::memory_order_relaxed), v.over = a.over.load(std::memory_order_relaxed);
        v.ns_sum = a.ns_sum.load(std::memory_order_relaxed), v.ns_max = a.ns_max.load(std::memory_order_relaxed);
        v.budget_sum = a.budget_sum.load(std::memory_order_relaxed), v.frames = a.frames.load(std::memory_order_relaxed);
        v.bridges = a.bridges.load(std::memory_order_relaxed);
        v.gaps = a.gaps.load(std::memory_order_relaxed), v.gap_ns_sum = a.gap_ns_sum.load(std::memory_order_relaxed);
        v.gap_max_ns = a.gap_max_ns.load(std::memory_order_relaxed);
        return v;
    }
    static OutputVals read(const OutputA& a) noexcept {
        OutputVals v;
        v.samples = a.samples.load(std::memory_order_relaxed), v.clip_l = a.clip_l.load(std::memory_order_relaxed);
        v.clip_r = a.clip_r.load(std::memory_order_relaxed), v.guard_l = a.guard_l.load(std::memory_order_relaxed);
        v.guard_r = a.guard_r.load(std::memory_order_relaxed), v.nonfinite = a.nonfinite.load(std::memory_order_relaxed);
        v.in_clip = a.in_clip.load(std::memory_order_relaxed);
        v.peak_l = a.peak_l.load(std::memory_order_relaxed), v.peak_r = a.peak_r.load(std::memory_order_relaxed);
        v.pre_guard_peak = a.pre_guard_peak.load(std::memory_order_relaxed), v.in_peak = a.in_peak.load(std::memory_order_relaxed);
        v.sumsq = a.sumsq.load(std::memory_order_relaxed);
        return v;
    }
    static MasterVals read(const MasterA& a) noexcept {
        MasterVals v;
        v.samples = a.samples.load(std::memory_order_relaxed), v.limiter = a.limiter.load(std::memory_order_relaxed);
        v.soft = a.soft.load(std::memory_order_relaxed), v.nonfinite = a.nonfinite.load(std::memory_order_relaxed);
        v.limiter_max_env = a.limiter_max_env.load(std::memory_order_relaxed), v.rig_peak = a.rig_peak.load(std::memory_order_relaxed);
        v.post_gain_peak = a.post_gain_peak.load(std::memory_order_relaxed), v.rig_sumsq = a.rig_sumsq.load(std::memory_order_relaxed);
        return v;
    }
    static SlotVals read(const SlotA& a) noexcept {
        SlotVals v;
        v.key = a.key.load(std::memory_order_relaxed);
        v.blocks = a.blocks.load(std::memory_order_relaxed), v.frames = a.frames.load(std::memory_order_relaxed);
        v.ns_sum = a.ns_sum.load(std::memory_order_relaxed), v.ns_max = a.ns_max.load(std::memory_order_relaxed);
        v.over_share = a.over_share.load(std::memory_order_relaxed), v.over_block = a.over_block.load(std::memory_order_relaxed);
        v.nonfinite = a.nonfinite.load(std::memory_order_relaxed);
        v.out_peak = a.out_peak.load(std::memory_order_relaxed), v.max_load = a.max_load.load(std::memory_order_relaxed);
        v.out_sumsq = a.out_sumsq.load(std::memory_order_relaxed);
        v.c_samples = a.c_samples.load(std::memory_order_relaxed), v.c_iters = a.c_iters.load(std::memory_order_relaxed);
        v.c_homotopies = a.c_homotopies.load(std::memory_order_relaxed), v.c_failures = a.c_failures.load(std::memory_order_relaxed);
        v.c_ptc = a.c_ptc.load(std::memory_order_relaxed), v.c_half = a.c_half.load(std::memory_order_relaxed);
        v.c_lte = a.c_lte.load(std::memory_order_relaxed);
        return v;
    }

    Cum cum_;
    std::array<SlotCum, kSlots> slot_cum_{};
    std::array<Bucket, kBuckets> buckets_{};
    std::array<U, kHistBins> hist_{};
    std::array<U, kRing> ring_t_{}, ring_e_{}, ring_b_{};
    std::atomic<uint32_t> ring_pos_{0};
    std::atomic<int> cur_bucket_{0};
    uint64_t cur_epoch_ = kNoEpoch; // render thread only
    uint64_t prev_begin_ns_ = 0, prev_period_ns_ = 0; // render thread only
    U block_ns_{0}, start_ns_{0}, reset_ns_{0}, sample_budget_ns_{0};
    std::atomic<uint32_t> last_frames_{0};
    std::atomic<uint32_t> reset_req_{0}, reset_done_{0};
    std::atomic<uint32_t> units_cur_{0};
    uint32_t units_prev_ = 1; // render thread (read by the cores it forks to, after the fork)
    U pipe_runs_{0}, pipe_stages_{0};
    std::atomic<int> pipe_last_{0};
    F preset_gain_now_{0.0f};
};

} // namespace rig
