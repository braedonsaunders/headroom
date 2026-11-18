#pragma once
#include "RealtimeGraphExchange.hpp"
#include <cstdint>
#include <memory>

namespace rig {

// Serialized producer / single audio consumer. The producer owns the editable
// model; this class owns only prepared render plans. Audio never acquires the
// model lock and never destroys a plan. A stalled collector delays handoffs,
// while the last complete plan continues rendering.
template<class Graph, std::size_t RetirementCapacity = 64>
class RealtimeGraphRuntime {
    struct Plan {
        std::unique_ptr<Graph> graph;
        std::uint64_t revision;
        std::uint64_t lineage;
    };
public:
    // Producer only. Revisions are monotonically increasing across all racks.
    // A lineage identifies edits of one rack; replacing a preset starts another.
    bool publish(std::unique_ptr<Graph> graph, std::uint64_t revision, std::uint64_t lineage) {
        if (!graph || (has_revision_ && revision <= published_revision_)) return false;
        auto plan = std::make_unique<Plan>(Plan{std::move(graph), revision, lineage});
        exchange_.publish(std::move(plan));
        published_revision_ = revision;
        has_revision_ = true;
        return true;
    }

    // Audio only, once per block. Shared components render exactly once. No
    // crossfade of complete edit snapshots: those snapshots share DSP state.
    Graph* beginBlock() noexcept {
        if (awaiting_retirement_) {
            if (!exchange_.retire(awaiting_retirement_)) return current_ ? current_->graph.get() : nullptr;
            awaiting_retirement_ = nullptr;
        }
        if (auto* next = exchange_.take()) {
            handoff_continuous_ = current_ && current_->lineage == next->lineage;
            if (current_) {
                if (handoff_continuous_)
                    next->graph->inheritRenderState(*current_->graph);
                if (!exchange_.retire(current_)) awaiting_retirement_ = current_;
            }
            current_ = next;
            audible_revision_.store(next->revision, std::memory_order_release);
        }
        return current_ ? current_->graph.get() : nullptr;
    }

    // Producer only. Call periodically even when no edits are arriving.
    void collect() { exchange_.collect(); }
    std::uint64_t audibleRevision() const noexcept { return audible_revision_.load(std::memory_order_acquire); }
    // Audio only: whether the latest handoff continued the same rack (an edit whose plan inherited the DSP
    // history, so its output is continuous) rather than replacing it (preset load, re-prepare).
    bool lastHandoffContinuous() const noexcept { return handoff_continuous_; }

    // Producer only, AFTER the audio device/callback has synchronously stopped.
    // Repeated calls are safe; pending publication is reclaimed by exchange_.
    void stop() {
        delete current_;
        current_ = nullptr;
        delete awaiting_retirement_;
        awaiting_retirement_ = nullptr;
        exchange_.collect();
    }
    ~RealtimeGraphRuntime() { stop(); }

private:
    RealtimeGraphExchange<Plan, RetirementCapacity> exchange_;
    Plan* current_ = nullptr;
    Plan* awaiting_retirement_ = nullptr;
    std::uint64_t published_revision_ = 0;
    bool has_revision_ = false;
    bool handoff_continuous_ = false; // audio thread only
    std::atomic<std::uint64_t> audible_revision_{0};
};
} // namespace rig
