#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <utility>

namespace rig {

// Ownership mailbox between one serialized control producer and one audio consumer.
// Preparation/destruction belong to the producer. Audio only exchanges pointers.
// The consumer owns every object returned by take(); it must return each through
// retire() after its final render (including any crossfade). collect() reclaims it.
// Stop the callback and return its held objects before destroying this mailbox.
template <class Graph, std::size_t RetirementCapacity = 64>
class RealtimeGraphExchange {
    static_assert(RetirementCapacity >= 4);
    static_assert(std::atomic<Graph*>::is_always_lock_free);
    static_assert(std::atomic<std::size_t>::is_always_lock_free);

public:
    RealtimeGraphExchange() = default;
    RealtimeGraphExchange(const RealtimeGraphExchange&) = delete;
    RealtimeGraphExchange& operator=(const RealtimeGraphExchange&) = delete;

    // Control thread only. Last unpublished edit wins; an abandoned graph was
    // never acquired by audio, so destroying it here is safe.
    void publish(std::unique_ptr<Graph> next) {
        collect();
        delete pending_.exchange(next.release(), std::memory_order_acq_rel);
    }

    // Audio thread only, once at a block boundary. The caller must have room to
    // retain its outgoing graph until retire() succeeds. No allocation or locks.
    Graph* take() noexcept {
        return pending_.exchange(nullptr, std::memory_order_acq_rel);
    }

    // Audio thread only. On false the caller still owns graph and must keep it
    // for a later block; it must never delete it on the callback.
    bool retire(Graph* graph) noexcept {
        if (!graph) return true;
        const auto write = write_.load(std::memory_order_relaxed);
        const auto next = (write + 1) % RetirementCapacity;
        if (next == read_.load(std::memory_order_acquire)) return false;
        retired_[write] = graph;
        write_.store(next, std::memory_order_release);
        return true;
    }

    // Producer only, regularly even if no edits are arriving.
    void collect() {
        auto read = read_.load(std::memory_order_relaxed);
        const auto write = write_.load(std::memory_order_acquire);
        while (read != write) {
            delete retired_[read];
            read = (read + 1) % RetirementCapacity;
        }
        read_.store(read, std::memory_order_release);
    }

    ~RealtimeGraphExchange() {
        // Precondition: producer and callback have both stopped.
        delete pending_.load(std::memory_order_relaxed);
        collect();
    }

private:
    std::atomic<Graph*> pending_{nullptr};
    std::array<Graph*, RetirementCapacity> retired_{};
    alignas(64) std::atomic<std::size_t> write_{0};
    alignas(64) std::atomic<std::size_t> read_{0};
};

} // namespace rig
