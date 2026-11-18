#pragma once
// Real-time fork/join pool for the audio render: parallel rack lanes, the two channels of a stereo circuit, nested
// splits. Process-wide, so every engine in the process (the app's, or 3-4 plugin instances in one DAW) shares one
// set of workers. Lock-free and allocation-free on the audio path (bounded Vyukov MPMC ring, C++20 atomic
// wait/notify). Design notes and measurements: app/docs/PLUGIN_PERFORMANCE.md.
//
// fork/join contract
//   fork(j, fn, arg) queues fn(arg); join(j) returns once every task forked into j has run. A forked task runs
//   exactly once, either on a worker or, if no worker has started it by the time the caller reaches join, inline
//   on the caller (join *reclaims* it). So the pool can never make a render slower than serial by more than the
//   queueing cost, and when the host is saturating the cores (workers not getting scheduled) rendering falls back
//   to serial by itself. The caller never runs another site's task, so one instance never pays for another.
//   Tasks must be independent of the thread they run on; the output is then bit-identical to serial rendering.
//
// DAW safety
//   * Spinning is bounded in time and adaptive: a worker only spins right after it ran a task (audio work comes
//     in bursts), for at most kSpinMaxUs, shrinking when spins find nothing; at most kMaxSpinners spin at once.
//     With no parallel work the workers sleep.
//   * Forking backs off while most forks end up reclaimed (the workers aren't being scheduled in time), so idle
//     workers aren't woken just to compete with the host's own threads; probes keep re-testing.
//   * Host audio workgroup (macOS 11+): plugin instances report their render context's os_workgroup_t
//     (setWorkgroup, from the AU render context observer); every worker joins it so the OS schedules them as part
//     of the host's real-time work, and the number of workers taking tasks is bounded by the workgroup's
//     os_workgroup_max_parallel_threads().
//   * Lifetime: plugin instances retainUsers()/releaseUsers(); the last release stops and joins the workers (a
//     DAW may unload the plugin binary). The app never releases, so its workers live for the process.
//   * HEADROOM_RTPOOL_WORKERS=N overrides the worker count (0 = always serial).
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <os/object.h>
#include <os/workgroup.h>
#else
#include <chrono>
#endif

namespace rig {

class RtPool {
public:
    static constexpr int kMaxForksPerJoin = 8;
    static constexpr int kWorkgroupSlots = 32; // plugin instances that may report a workgroup at once

    struct Join { // one per fork/join site, on the caller's stack
        std::atomic<uint32_t> pending{0}; // forked tasks not finished (+ kWaiter while the caller sleeps in join)
        int n = 0;
        struct Forked {
            std::size_t ticket;
            void (*fn)(void*);
            void* arg;
        } forked[kMaxForksPerJoin];
    };

    struct Stats {
        uint64_t forks = 0, on_worker = 0, reclaimed = 0, refused = 0;
        int workers = 0, active = 0, joined_workgroup = 0; // threads, allowed to take work, inside a host workgroup
        int workgroup_max_parallel = 0;                     // os_workgroup_max_parallel_threads of that workgroup
    };

    static RtPool& instance() { // first call from a control thread (RackGraph::prepare, PluginCore)
        static RtPool p;
        return p;
    }
    ~RtPool() {
        {
            std::lock_guard<std::mutex> lk(life_m_);
            stop();
        }
        for (auto& s : slots_) retire(s.exchange(nullptr));
        collectRetired();
    }

    int workers() const noexcept { return static_cast<int>(nthreads_.load(std::memory_order_relaxed)); }

    // Queue fn(arg) as part of `j`. Returns false (the caller runs it inline) when the pool is serial, stopped, the
    // join is full, the ring is full, or forking is backing off.
    bool fork(Join& j, void (*fn)(void*), void* arg) noexcept {
        if (j.n >= kMaxForksPerJoin || cap_.load(std::memory_order_relaxed) <= 0 || !enabled_.load(std::memory_order_relaxed))
            return false;
        if (credit_.load(std::memory_order_relaxed) < kBackoffBelow && (probe_.fetch_add(1, std::memory_order_relaxed) & 7) != 0) {
            refused_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        j.pending.fetch_add(1, std::memory_order_relaxed);
        std::size_t ticket = 0;
        if (!push(Task{fn, arg, &j}, ticket)) {
            j.pending.fetch_sub(1, std::memory_order_relaxed);
            return false;
        }
        j.forked[j.n++] = {ticket, fn, arg};
        forks_.fetch_add(1, std::memory_order_relaxed);
        // Wake a sleeper only when the spinning workers can't cover the queue (see worker() for the handshake).
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (sleeping_.load(std::memory_order_relaxed) > 0 && queued() > static_cast<std::size_t>(spinning_.load(std::memory_order_relaxed))) {
            work_.fetch_add(1, std::memory_order_release);
            work_.notify_one();
        }
        return true;
    }

    // Returns once every task forked into `j` has run. Tasks no worker has started yet run here, inline.
    void join(Join& j) noexcept {
        for (int i = j.n - 1; i >= 0; --i) {
            const auto& f = j.forked[i];
            std::size_t expect = f.ticket + 1;
            if (ring_[f.ticket & kMask].claim.compare_exchange_strong(expect, 0, std::memory_order_acq_rel)) {
                f.fn(f.arg);
                j.pending.fetch_sub(1, std::memory_order_relaxed);
                reclaimed_.fetch_add(1, std::memory_order_relaxed);
                addCredit(-kReclaimCost);
            }
        }
        j.n = 0;
        const uint64_t until = now() + join_spin_;
        for (;;) { // what's left is running on workers: brief spin, then sleep until the last one finishes
            const uint32_t v = j.pending.load(std::memory_order_acquire);
            if ((v & ~kWaiter) == 0) return;
            if (now() < until) {
                relax();
                continue;
            }
            if (!(v & kWaiter)) {
                j.pending.fetch_or(kWaiter, std::memory_order_acq_rel);
                continue;
            }
            j.pending.wait(v, std::memory_order_acquire);
        }
    }

    // ── lifetime (control thread) ──
    void retainUsers() {
        std::lock_guard<std::mutex> lk(life_m_);
        if (users_++ == 0) start();
    }
    void releaseUsers() {
        {
            std::lock_guard<std::mutex> lk(life_m_);
            if (users_ > 0 && --users_ == 0) stop();
        }
        collectRetired();
    }

    // ── host workgroups ──
    // A plugin instance takes a slot on a control thread and reports its render context's workgroup into it from
    // the render thread (null = none). Workers join the workgroup in the first occupied slot.
    int acquireWorkgroupSlot() noexcept {
        for (int i = 0; i < kWorkgroupSlots; ++i) {
            bool expect = false;
            if (slot_used_[i].compare_exchange_strong(expect, true)) return i;
        }
        return -1;
    }
    void releaseWorkgroupSlot(int slot) noexcept { // control thread; the instance no longer renders
        if (slot < 0 || slot >= kWorkgroupSlots) return;
#if defined(__APPLE__)
        setWorkgroup(slot, nullptr);
#else
        setWorkgroupPtr(slot, nullptr); // setWorkgroup(os_workgroup_t) exists only on Apple
#endif
        slot_used_[slot].store(false);
        collectRetired();
    }
    // Real-time safe (render context observer): one retain, an exchange, a counter bump and a wake-up.
#if defined(__APPLE__)
    void setWorkgroup(int slot, os_workgroup_t wg) noexcept { setWorkgroupPtr(slot, toPtr(wg)); }
#endif
    void setWorkgroupPtr(int slot, void* wg) noexcept {
        if (slot < 0 || slot >= kWorkgroupSlots) return;
        if (slots_[slot].load(std::memory_order_acquire) == wg) return;
#if defined(__APPLE__)
        if (wg) (os_retain)(wg);
#endif
        void* old = slots_[slot].exchange(wg, std::memory_order_seq_cst);
        wg_gen_.fetch_add(1, std::memory_order_seq_cst);
        retire(old);
        wg_gen_.notify_all();                          // parked workers re-sync too
        work_.fetch_add(1, std::memory_order_release); // sleeping workers re-join before their next task
        work_.notify_all();
    }
    // Releases workgroups no slot uses any more, once no worker is reading the slots. Control thread.
    void collectRetired() noexcept {
        for (auto& r : retired_)
            if (void* p = r.exchange(nullptr)) {
                while (hazard_.load(std::memory_order_seq_cst) != 0) relax();
                release(p);
            }
    }

    // ── tests and tools ──
    void setEnabled(bool on) noexcept { enabled_.store(on); } // false: every fork runs inline (serial reference)
    Stats stats() const noexcept {
        Stats s;
        s.forks = forks_.load();
        s.reclaimed = reclaimed_.load();
        s.refused = refused_.load();
        s.on_worker = s.forks > s.reclaimed ? s.forks - s.reclaimed : 0;
        s.workers = workers();
        s.active = std::max(0, cap_.load());
        s.joined_workgroup = joined_.load();
        s.workgroup_max_parallel = wg_mpt_.load();
        return s;
    }

private:
    struct Task {
        void (*fn)(void*) = nullptr;
        void* arg = nullptr;
        Join* join = nullptr;
    };
    static constexpr std::size_t kCap = 256; // power of two
    static constexpr std::size_t kMask = kCap - 1;
    static constexpr uint32_t kWaiter = 0x80000000u;
    static constexpr int kMaxSpinners = 2;
    static constexpr double kSpinMaxUs = 50.0, kSpinWakeUs = 15.0, kSpinMinUs = 2.0, kJoinSpinUs = 10.0;
    static constexpr int kCreditMax = 64, kCreditMin = -64, kReclaimCost = 2, kBackoffBelow = -16;
    struct Cell {
        std::atomic<std::size_t> seq{0};
        std::atomic<std::size_t> claim{0}; // ticket + 1 while queued and unclaimed; 0 once a worker or its join took it
        Task task;
    };
    struct Member { // a worker's own workgroup membership
        void* wg = nullptr;
#if defined(__APPLE__)
        os_workgroup_join_token_s token{};
#endif
    };

    RtPool() {
        for (std::size_t i = 0; i < kCap; ++i) ring_[i].seq.store(i, std::memory_order_relaxed);
        for (auto& s : slot_used_) s.store(false);
        for (auto& s : slots_) s.store(nullptr);
        for (auto& r : retired_) r.store(nullptr);
        const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
        want_ = std::min<unsigned>(hw > 1 ? hw - 1 : 0, 31);
        if (const char* e = std::getenv("HEADROOM_RTPOOL_WORKERS"); e && *e) want_ = static_cast<unsigned>(std::clamp(std::atoi(e), 0, 31));
#if defined(__APPLE__)
        mach_timebase_info_data_t tb{};
        mach_timebase_info(&tb);
        ticks_per_us_ = 1000.0 * tb.denom / tb.numer;
#else
        ticks_per_us_ = 1000.0;
#endif
        spin_max_ = us(kSpinMaxUs);
        spin_wake_ = us(kSpinWakeUs);
        spin_min_ = us(kSpinMinUs);
        join_spin_ = us(kJoinSpinUs);
        std::lock_guard<std::mutex> lk(life_m_);
        start();
    }
    RtPool(const RtPool&) = delete;
    RtPool& operator=(const RtPool&) = delete;

    void start() { // control thread, life_m_ held
        if (!threads_.empty() || want_ == 0) return;
        stop_.store(false);
        threads_.reserve(want_);
        for (unsigned i = 0; i < want_; ++i) threads_.emplace_back([this, i] { worker(static_cast<int>(i)); });
        nthreads_.store(want_);
        cap_.store(static_cast<int>(want_));
        wg_gen_.fetch_add(1);
    }
    void stop() { // control thread, life_m_ held; nothing may be rendering
        if (threads_.empty()) return;
        stop_.store(true);
        cap_.store(-1);
        wg_gen_.fetch_add(1); // after stop_: a worker parked on wg_gen_ or asleep on work_ sees it change
        wg_gen_.notify_all();
        work_.fetch_add(1);
        work_.notify_all();
        for (auto& t : threads_) t.join();
        threads_.clear();
        nthreads_.store(0);
    }

    uint64_t us(double v) const noexcept { return static_cast<uint64_t>(v * ticks_per_us_); }
    static uint64_t now() noexcept {
#if defined(__APPLE__)
        return mach_absolute_time();
#else
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
    }
    static void relax() noexcept {
#if defined(__aarch64__) || defined(__arm64__)
        __asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(_M_X64)
        __builtin_ia32_pause();
#endif
    }
    void addCredit(int d) noexcept {
        const int c = credit_.load(std::memory_order_relaxed) + d;
        credit_.store(std::clamp(c, kCreditMin, kCreditMax), std::memory_order_relaxed); // heuristic: races are harmless
    }
    std::size_t queued() const noexcept {
        const std::size_t h = head_.load(std::memory_order_relaxed), t = tail_.load(std::memory_order_relaxed);
        return h > t ? h - t : 0;
    }

    void runTask(const Task& t) noexcept {
        t.fn(t.arg);
        addCredit(1);
        // After this decrement the caller may return and its Join die: only the (address-keyed) wake follows.
        if (t.join->pending.fetch_sub(1, std::memory_order_acq_rel) == (kWaiter | 1u)) t.join->pending.notify_all();
    }
    bool push(const Task& t, std::size_t& ticket) noexcept { // Vyukov bounded MPMC enqueue
        std::size_t pos = head_.load(std::memory_order_relaxed);
        for (;;) {
            Cell& c = ring_[pos & kMask];
            const std::size_t seq = c.seq.load(std::memory_order_acquire);
            const auto dif = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);
            if (dif == 0) {
                if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    c.task = t;
                    c.claim.store(pos + 1, std::memory_order_relaxed);
                    c.seq.store(pos + 1, std::memory_order_release);
                    ticket = pos;
                    return true;
                }
            } else if (dif < 0) return false; // full
            else pos = head_.load(std::memory_order_relaxed);
        }
    }
    // Dequeue the next task this worker gets to run; entries their own join already reclaimed are dropped.
    bool popClaimed(Task& t) noexcept {
        std::size_t pos = tail_.load(std::memory_order_relaxed);
        for (;;) {
            Cell& c = ring_[pos & kMask];
            const std::size_t seq = c.seq.load(std::memory_order_acquire);
            const auto dif = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);
            if (dif == 0) {
                if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    t = c.task;
                    std::size_t expect = pos + 1;
                    const bool mine = c.claim.compare_exchange_strong(expect, 0, std::memory_order_acq_rel);
                    c.seq.store(pos + kCap, std::memory_order_release); // only now may the cell be reused
                    if (mine) return true;
                    pos = tail_.load(std::memory_order_relaxed);
                }
            } else if (dif < 0) return false; // empty
            else pos = tail_.load(std::memory_order_relaxed);
        }
    }

    void worker(int idx) {
        setRealtime();
        Member m;
        uint32_t seen_gen = ~0u;
        uint64_t budget = 0; // spin budget (ticks); grows when spins pay, shrinks when they don't
        while (!stop_.load(std::memory_order_acquire)) {
            if (const uint32_t g = wg_gen_.load(std::memory_order_acquire); g != seen_gen) {
                seen_gen = g;
                syncWorkgroup(m, idx);
            }
            if (idx >= cap_.load(std::memory_order_acquire)) { // parked by the workgroup bound: wait for a change
                if (!stop_.load()) wg_gen_.wait(seen_gen, std::memory_order_acquire);
                continue;
            }
            Task t;
            if (popClaimed(t)) {
                runTask(t);
                continue;
            }
            if (budget >= spin_min_ && spinning_.fetch_add(1, std::memory_order_acq_rel) < kMaxSpinners) {
                bool got = false;
                for (const uint64_t end = now() + budget; !got && now() < end;) {
                    if (popClaimed(t)) got = true;
                    else for (int k = 0; k < 16; ++k) relax();
                }
                spinning_.fetch_sub(1, std::memory_order_acq_rel);
                if (got) {
                    budget = std::min(spin_max_, budget * 2);
                    runTask(t);
                    continue;
                }
                budget /= 2;
            } else if (budget >= spin_min_) {
                spinning_.fetch_sub(1, std::memory_order_acq_rel); // enough workers are spinning already
            }
            // Sleep. Handshake with fork(): announce, fence, re-check the queue, then wait on the epoch read first.
            const uint32_t w = work_.load(std::memory_order_acquire);
            sleeping_.fetch_add(1, std::memory_order_seq_cst);
            std::atomic_thread_fence(std::memory_order_seq_cst);
            if (popClaimed(t)) {
                sleeping_.fetch_sub(1, std::memory_order_relaxed);
                runTask(t);
                budget = std::max(budget, spin_wake_);
                continue;
            }
            if (!stop_.load(std::memory_order_acquire) && wg_gen_.load(std::memory_order_acquire) == seen_gen) work_.wait(w, std::memory_order_acquire);
            sleeping_.fetch_sub(1, std::memory_order_relaxed);
            budget = std::max(budget, spin_wake_); // woken for work: a burst is likely to follow
        }
        leaveWorkgroup(m);
    }

    // Worker thread: follow the first occupied workgroup slot (leave the old workgroup, join the new one).
    void syncWorkgroup(Member& m, int idx) noexcept {
#if defined(__APPLE__)
        hazard_.fetch_add(1, std::memory_order_seq_cst);
        void* want = nullptr;
        for (auto& s : slots_)
            if ((want = s.load(std::memory_order_seq_cst)) != nullptr) break;
        if (want == m.wg) {
            hazard_.fetch_sub(1, std::memory_order_seq_cst);
            return;
        }
        if (want) (os_retain)(want);
        hazard_.fetch_sub(1, std::memory_order_seq_cst);
        leaveWorkgroup(m);
        if (want) {
            if (os_workgroup_join(toWg(want), &m.token) == 0) {
                m.wg = want;
                joined_.fetch_add(1, std::memory_order_relaxed);
            } else {
                release(want);
            }
        }
        if (idx == 0) { // one worker sets the bound for everyone
            int cap = static_cast<int>(nthreads_.load());
            int mpt = 0;
            if (m.wg) {
                mpt = os_workgroup_max_parallel_threads(toWg(m.wg), nullptr);
                if (mpt >= 1) cap = std::min(cap, mpt - 1); // leave one for the host's render thread
            }
            wg_mpt_.store(mpt);
            if (!stop_.load() && cap_.exchange(cap) != cap) { // wake parked workers (and re-sync everyone)
                wg_gen_.fetch_add(1, std::memory_order_seq_cst);
                wg_gen_.notify_all();
            }
        }
#else
        (void)m;
        (void)idx;
#endif
    }
    void leaveWorkgroup(Member& m) noexcept {
#if defined(__APPLE__)
        if (!m.wg) return;
        os_workgroup_leave(toWg(m.wg), &m.token);
        joined_.fetch_sub(1, std::memory_order_relaxed);
        release(m.wg);
        m.wg = nullptr;
#else
        (void)m;
#endif
    }
    void retire(void* p) noexcept { // a workgroup no slot holds any more; released later on a control thread
        if (!p) return;
        for (auto& r : retired_) {
            void* expect = nullptr;
            if (r.compare_exchange_strong(expect, p)) return;
        }
        while (hazard_.load(std::memory_order_seq_cst) != 0) relax(); // list full (never in practice): release now
        release(p);
    }
    static void release(void* p) noexcept {
#if defined(__APPLE__)
        if (p) (os_release)(p);
#else
        (void)p;
#endif
    }
#if defined(__APPLE__)
#if defined(__OBJC__) && __has_feature(objc_arc)
    static os_workgroup_t toWg(void* p) noexcept { return (__bridge os_workgroup_t)p; }
    static void* toPtr(os_workgroup_t w) noexcept { return (__bridge void*)w; }
#else
    static os_workgroup_t toWg(void* p) noexcept { return (os_workgroup_t)p; }
    static void* toPtr(os_workgroup_t w) noexcept { return (void*)w; }
#endif
#endif

    static void setRealtime() {
#if defined(__APPLE__)
        mach_timebase_info_data_t tb{};
        mach_timebase_info(&tb);
        const auto abs = [&](double ms) { return static_cast<uint32_t>(ms * 1e6 * tb.denom / tb.numer); };
        thread_time_constraint_policy_data_t p{};
        p.period = 0;
        p.computation = abs(1.5);
        p.constraint = abs(2.9);
        p.preemptible = 1;
        thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY, reinterpret_cast<thread_policy_t>(&p),
                          THREAD_TIME_CONSTRAINT_POLICY_COUNT);
#endif
    }

    std::array<Cell, kCap> ring_;
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
    alignas(64) std::atomic<uint32_t> work_{0};
    std::atomic<int> sleeping_{0};
    std::atomic<int> spinning_{0};
    alignas(64) std::atomic<int> cap_{0};        // workers allowed to take tasks (0 = serial / stopped)
    std::atomic<int> credit_{0};
    std::atomic<uint32_t> probe_{0};
    std::atomic<bool> enabled_{true};
    alignas(64) std::atomic<uint64_t> forks_{0}, reclaimed_{0}, refused_{0};
    alignas(64) std::atomic<uint32_t> wg_gen_{0};
    std::atomic<int> hazard_{0}, joined_{0}, wg_mpt_{0};
    std::array<std::atomic<bool>, kWorkgroupSlots> slot_used_;
    std::array<std::atomic<void*>, kWorkgroupSlots> slots_;
    std::array<std::atomic<void*>, kWorkgroupSlots> retired_;
    std::atomic<bool> stop_{false};
    std::mutex life_m_; // lifetime (start/stop, users_); control threads only
    int users_ = 0;
    std::atomic<unsigned> nthreads_{0};
    std::vector<std::thread> threads_; // control thread only
    unsigned want_ = 0;
    double ticks_per_us_ = 1000.0;
    uint64_t spin_max_ = 0, spin_wake_ = 0, spin_min_ = 0, join_spin_ = 0;
};

} // namespace rig
