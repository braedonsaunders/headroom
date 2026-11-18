#pragma once
// One block of Circuit::process() calls on the circuit's intra-unit lanes (docs/CIRCUIT_ENGINE.md §10.33): arms them and
// forks the helper lane onto the RtPool; the destructor releases the helper and joins it. Sequential (and nothing
// forked) when the circuit has no lane plan, `allow` is false, or the pool refuses the fork. Audio-thread safe: no
// allocation, no locks; the join only waits for a helper that is running (one the pool never started runs inline here
// and returns at once). Construct it on the thread that then calls process(), inside its ScopedFlushDenormals.
#include "../core/RtPool.hpp"
#include "Engine.hpp"

namespace rig::ce {

class LaneBlock {
public:
    LaneBlock(Circuit& c, bool allow) noexcept {
        if (!allow || !c.lanesPlanned() || !c.beginLanes()) return;
        if (RtPool::instance().fork(join_, &Circuit::laneHelper, &c)) c_ = &c;
        else c.endLanes();
    }
    ~LaneBlock() {
        if (!c_) return;
        c_->endLanes(); // first: a waiting helper returns
        RtPool::instance().join(join_);
    }
    LaneBlock(const LaneBlock&) = delete;
    LaneBlock& operator=(const LaneBlock&) = delete;

private:
    Circuit* c_ = nullptr;
    RtPool::Join join_;
};

} // namespace rig::ce
