#undef NDEBUG
#include "../src/core/RealtimeGraphRuntime.hpp"
#include <atomic>
#include <cassert>
#include <memory>
#include <mutex>
#include <thread>
#include <iostream>

namespace {
thread_local bool audioThread = false;
std::atomic<int> destroyed{0};
struct Graph {
    explicit Graph(int value) : value(value) {}
    ~Graph() { assert(!audioThread); ++destroyed; }
    void inheritRenderState(Graph& previous) noexcept { history = previous.history; }
    int render() { ++history; return value; }
    int value, history = 0;
};
}
int main() {
    {
        rig::RealtimeGraphRuntime<Graph, 4> runtime;
        assert(runtime.publish(std::make_unique<Graph>(1), 1, 1));
        audioThread = true;
        assert(runtime.beginBlock()->render() == 1);
        audioThread = false;
        assert(!runtime.publish(std::make_unique<Graph>(99), 1, 1));
        for (int revision = 2; revision <= 12; ++revision) {
            assert(runtime.publish(std::make_unique<Graph>(revision), revision, 1));
            audioThread = true;
            auto* graph = runtime.beginBlock();
            assert(graph->value == revision);
            assert(graph->history == revision - 1);
            graph->render();
            audioThread = false;
        }
        // New preset lineage must not inherit previous preset's DSP state.
        assert(runtime.publish(std::make_unique<Graph>(13), 13, 2));
        audioThread = true;
        assert(runtime.beginBlock()->history == 0);
        audioThread = false;
        runtime.stop();
    }
    {
        rig::RealtimeGraphRuntime<Graph, 4> runtime;
        std::atomic<bool> done{false}, ready{false};
        std::atomic<int> blocks{0};
        std::mutex controlMutex;
        runtime.publish(std::make_unique<Graph>(1), 1, 1);
        std::thread audio([&] {
            audioThread = true;
            assert(runtime.beginBlock());
            ready.store(true);
            std::uint64_t last = 0;
            while (!done.load() || runtime.audibleRevision() < 5000) {
                auto* graph = runtime.beginBlock();
                assert(graph && graph->render() > 0);
                assert(runtime.audibleRevision() >= last);
                last = runtime.audibleRevision();
                ++blocks;
            }
            audioThread = false;
        });
        while (!ready.load()) std::this_thread::yield();
        // Hold the model lock while audio advances: the callback never sees it.
        {
            std::lock_guard<std::mutex> lock(controlMutex);
            const int start = blocks.load();
            while (blocks.load() < start + 1000) std::this_thread::yield();
        }
        for (int i = 2; i <= 5000; ++i) {
            std::lock_guard<std::mutex> lock(controlMutex);
            assert(runtime.publish(std::make_unique<Graph>(i), i, 1));
            if (i % 71 == 0) std::this_thread::yield();
        }
        done.store(true);
        // Collector can be delayed without a render gap; resume it to deliver
        // the last revision if audio filled the bounded retirement queue.
        while (runtime.audibleRevision() < 5000) {
            runtime.collect();
            std::this_thread::yield();
        }
        audio.join();
        assert(runtime.audibleRevision() == 5000);
        runtime.stop();
    }
    std::cout << "graph runtime: revision ordering, render continuity, history and off-audio destruction passed ("
              << destroyed.load() << " graphs)\n";
}
