#pragma once
// Prepared spares of the units whose prepare is expensive (IComponent::preparesIndependently: circuit units settle their
// supplies, 0.2-2 s of simulated warm-up per channel, most of a rig's load time).
//
// A spare is a unit created and prepared at (sample rate, block size), never given parameters and never rendered. That
// is exactly the state RackGraph::buildNode puts a unit in before it applies the preset's parameters (prepare runs at
// the schema defaults), so a rig built with spares renders bit for bit like one built from scratch; the settle is just
// done ahead of time. Each spare has its own noise instance, like any new unit (CircuitUnit, docs §3.9).
//
// The standalone app (RealtimeAudioServer) asks for spares of the units in every rig it loads (want()), so switching
// back to a rig, or to another rig with the same amps and pedals, skips the settle. A utility-QoS thread makes them,
// paused while a load is building (hold()) so it never competes with the load the user is waiting for. At most
// kCapacity spares are kept (about 1-3 MB each, so up to ~50 MB); the units of the most recent rigs win.
#include "IComponent.hpp"
#include "EngineDiag.hpp" // threadCpuSeconds (/api/diag)
#include "../platform/Denormals.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif

namespace rig {

class PreparedUnitPool {
public:
    static constexpr std::size_t kCapacity = 24;
    static constexpr int kMaxPerUnit = 4;

    // Created on first use and never destroyed (stop() joins the thread), so no static-destruction order applies.
    static PreparedUnitPool& instance() {
        static auto* pool = new PreparedUnitPool;
        return *pool;
    }

    // A prepared, never-rendered unit of `table_id` at this rate and block size, or null (the caller prepares its own).
    std::unique_ptr<IComponent> take(int32_t table_id, double sample_rate, uint32_t block) {
        std::lock_guard<std::mutex> lock(m_);
        for (auto it = spares_.begin(); it != spares_.end(); ++it)
            if (it->tid == table_id && it->sr == sample_rate && it->block == block) {
                auto unit = std::move(it->unit);
                spares_.erase(it);
                taken_.fetch_add(1, std::memory_order_relaxed);
                cv_.notify_all(); // refill
                return unit;
            }
        return nullptr;
    }

    // The units a rig just loaded uses (one entry per instance): keep that many spares of each ready. Wants for another
    // rate or block size, and the oldest wants past kCapacity spares, are dropped with their spares.
    void want(const std::vector<int32_t>& table_ids, double sample_rate, uint32_t block) {
        std::vector<std::unique_ptr<IComponent>> dropped; // released outside the lock
        {
            std::lock_guard<std::mutex> lock(m_);
            if (stopped_) return;
            ++stamp_;
            for (int32_t tid : table_ids) {
                const int n = static_cast<int>(std::count(table_ids.begin(), table_ids.end(), tid));
                auto it = std::find_if(wants_.begin(), wants_.end(), [&](const Want& w) { return w.tid == tid; });
                if (it == wants_.end()) it = wants_.insert(wants_.end(), Want{tid, 0, 0});
                it->count = std::min(kMaxPerUnit, std::max(it->count, n)); // enough for this rig and the earlier ones
                it->stamp = stamp_;
            }
            sr_ = sample_rate;
            block_ = block;
            // newest first; trim to capacity
            std::stable_sort(wants_.begin(), wants_.end(), [](const Want& a, const Want& b) { return a.stamp > b.stamp; });
            std::size_t total = 0;
            for (auto& w : wants_) {
                w.count = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(w.count), kCapacity - std::min(total, kCapacity)));
                total += static_cast<std::size_t>(w.count);
            }
            wants_.erase(std::remove_if(wants_.begin(), wants_.end(), [](const Want& w) { return w.count <= 0; }), wants_.end());
            // spares nobody wants any more (or at another rate), and the excess over a reduced count
            for (auto it = spares_.begin(); it != spares_.end();) {
                const Want* w = findWant(it->tid);
                const bool keep = w && it->sr == sr_ && it->block == block_ && have(it->tid, it) < w->count;
                if (!keep) {
                    dropped.push_back(std::move(it->unit));
                    it = spares_.erase(it);
                } else ++it;
            }
            if (!worker_.joinable()) worker_ = std::thread([this] { run(); });
        }
        cv_.notify_all();
    }

    // While held (a preset load is building), no new spare is started.
    void hold(bool on) {
        {
            std::lock_guard<std::mutex> lock(m_);
            holds_ += on ? 1 : -1;
        }
        cv_.notify_all();
    }

    // Shutdown: drops the spares and joins the thread (after the spare it may be preparing).
    void stop() {
        std::vector<Spare> dropped;
        {
            std::lock_guard<std::mutex> lock(m_);
            stopped_ = true;
            wants_.clear();
            dropped.swap(spares_);
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    std::size_t spareCount() const {
        std::lock_guard<std::mutex> lock(m_);
        return spares_.size();
    }

    // /api/diag: the pool thread's CPU time (s, -1 = not started), spares handed out / made, the unit being prepared.
    struct Diag { double cpu_s = -1.0; uint64_t taken = 0, prepared = 0; int32_t preparing = 0; std::size_t spares = 0, wants = 0; bool held = false; };
    Diag diag() const {
        std::lock_guard<std::mutex> lock(m_);
        Diag d;
        d.cpu_s = threadCpuSeconds(worker_);
        d.taken = taken_.load(std::memory_order_relaxed);
        d.prepared = prepared_.load(std::memory_order_relaxed);
        d.preparing = preparing_.load(std::memory_order_relaxed);
        d.spares = spares_.size();
        d.wants = wants_.size();
        d.held = holds_ > 0;
        return d;
    }

private:
    struct Spare {
        int32_t tid;
        double sr;
        uint32_t block;
        std::unique_ptr<IComponent> unit;
    };
    struct Want {
        int32_t tid;
        int count;
        uint64_t stamp;
    };

    const Want* findWant(int32_t tid) const {
        for (const auto& w : wants_)
            if (w.tid == tid) return &w;
        return nullptr;
    }
    // spares of `tid` at the current rate before `end`
    int have(int32_t tid, std::vector<Spare>::const_iterator end) const {
        int n = 0;
        for (auto it = spares_.cbegin(); it != end; ++it)
            if (it->tid == tid && it->sr == sr_ && it->block == block_) ++n;
        return n;
    }
    // The next want short of spares, or false. The rigs loaded before the current one come first, newest first (going
    // back is the likely next load), then the current rig's own units (to switch back to it later).
    bool nextJob(int32_t& tid, double& sr, uint32_t& block) const {
        for (int pass = 0; pass < 2; ++pass)
            for (const auto& w : wants_)
                if ((pass == 0) == (w.stamp != stamp_) && have(w.tid, spares_.cend()) < w.count) {
                    tid = w.tid;
                    sr = sr_;
                    block = block_;
                    return true;
                }
        return false;
    }

    void run() {
#ifdef __APPLE__
        pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#endif
        rig::platform::ScopedNoDenormalsX86 ftz; // the preset loader's floating-point mode, so spares match its builds
        std::unique_lock<std::mutex> lock(m_);
        for (;;) {
            int32_t tid = 0;
            double sr = 0;
            uint32_t block = 0;
            cv_.wait(lock, [&] { return stopped_ || (holds_ <= 0 && nextJob(tid, sr, block)); });
            if (stopped_) return;
            lock.unlock();
            std::unique_ptr<IComponent> unit;
            preparing_.store(tid, std::memory_order_relaxed);
            try {
                unit = createComponentByTableId(tid);
                if (unit && unit->preparesIndependently()) unit->prepare(sr, block), prepared_.fetch_add(1, std::memory_order_relaxed);
                else unit.reset();
            } catch (const std::exception& e) {
                std::fprintf(stderr, "Headroom: preparing a spare unit %d failed: %s\n", tid, e.what());
                unit.reset();
            } catch (...) {
                unit.reset();
            }
            preparing_.store(0, std::memory_order_relaxed);
            lock.lock();
            const Want* w = findWant(tid);
            if (unit && !stopped_ && w && sr == sr_ && block == block_ && have(tid, spares_.cend()) < w->count) {
                spares_.push_back(Spare{tid, sr, block, std::move(unit)});
            } else if (!unit && w) {
                // cannot be prepared here: stop asking for it
                wants_.erase(std::remove_if(wants_.begin(), wants_.end(), [&](const Want& x) { return x.tid == tid; }), wants_.end());
            }
            if (unit) { // not kept: release it unlocked
                lock.unlock();
                unit.reset();
                lock.lock();
            }
        }
    }

    mutable std::mutex m_;
    std::condition_variable cv_;
    std::vector<Spare> spares_;
    std::vector<Want> wants_; // newest first
    double sr_ = 0;
    uint32_t block_ = 0;
    uint64_t stamp_ = 0;
    int holds_ = 0;
    bool stopped_ = false;
    std::thread worker_;
    std::atomic<uint64_t> taken_{0}, prepared_{0};
    std::atomic<int32_t> preparing_{0};
};

} // namespace rig
