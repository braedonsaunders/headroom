#undef NDEBUG // test checks and setup remain active in Release builds
#include "../src/core/RealtimeGraphExchange.hpp"
#include <atomic>
#include <cassert>
#include <thread>

namespace {
struct Graph {
    static inline std::atomic<int> alive{0};
    static inline std::atomic<bool> audioDeleted{false};
    static inline thread_local bool onAudio = false;
    explicit Graph(int number) : revision(number) { ++alive; }
    ~Graph() { if (onAudio) audioDeleted = true; --alive; }
    int revision;
};
}

int main() {
    // Latest pending edit wins, without leaking an unpublished instance.
    {
        rig::RealtimeGraphExchange<Graph, 4> exchange;
        exchange.publish(std::make_unique<Graph>(1));
        exchange.publish(std::make_unique<Graph>(2));
        assert(Graph::alive == 1);
        Graph* two = exchange.take();
        assert(two && two->revision == 2);
        assert(exchange.retire(two));
        exchange.collect();
        assert(Graph::alive == 0);
        // Backpressure leaves ownership with audio instead of deleting there.
        Graph* held[4]{};
        for (int i = 0; i < 4; ++i) held[i] = new Graph(i);
        for (int i = 0; i < 3; ++i) assert(exchange.retire(held[i]));
        assert(!exchange.retire(held[3]));
        exchange.collect();
        assert(exchange.retire(held[3]));
        exchange.collect();
    }
    {
        rig::RealtimeGraphExchange<Graph> exchange;
        std::atomic<bool> done{false};
        std::thread callback([&] {
            Graph::onAudio = true;
            int previous = -1;
            Graph* held = nullptr;
            for (;;) {
                if (held) {
                    if (!exchange.retire(held)) { std::this_thread::yield(); continue; }
                    held = nullptr;
                }
                held = exchange.take();
                if (held) {
                    assert(held->revision > previous);
                    previous = held->revision;
                } else if (done.load(std::memory_order_acquire)) {
                    // Producer publishes before setting done; drain final pending.
                    held = exchange.take();
                    if (!held) break;
                }
            }
        });
        for (int i = 0; i < 20000; ++i) exchange.publish(std::make_unique<Graph>(i));
        done.store(true, std::memory_order_release);
        // Reclaim concurrently so callback can always return its last object.
        // Publication collects too; after done at most one object remains held.
        callback.join();
        exchange.collect();
    }
    assert(Graph::alive == 0);
    assert(!Graph::audioDeleted);
}
