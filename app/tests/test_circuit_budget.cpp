#undef NDEBUG
#include "../src/core/RackGraph.hpp"
#include "test_compat.hpp"
#include "../src/dsp/ComponentModel.hpp"
#include "../src/circuit/CircuitUnit.hpp"
#include "json.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <new>
#include <vector>

// Counts C++ heap operations, including aligned and array allocations. This
// does not interpose direct malloc calls or prove hard real-time scheduling.
static thread_local bool tracked = false;
static thread_local std::size_t allocations = 0, deletions = 0;
void* operator new(std::size_t n) {
    if (tracked) ++allocations;
    if (void* p = std::malloc(std::max(n, std::size_t(1)))) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { if (tracked && p) ++deletions; std::free(p); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
void* operator new(std::size_t n, std::align_val_t a) {
    if (tracked) ++allocations;
    if (void* p = rig_test::alignedAlloc(static_cast<std::size_t>(a), std::max(n, std::size_t(1)))) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n, std::align_val_t a) { return ::operator new(n, a); }
void operator delete(void* p, std::align_val_t) noexcept {
    if constexpr (rig_test::kAlignedNeedsOwnFree) rig_test::alignedFree(p); // _aligned_malloc block (Windows)
    else ::operator delete(p);
}
void operator delete[](void* p, std::align_val_t a) noexcept { ::operator delete(p, a); }
void operator delete(void* p, std::size_t, std::align_val_t a) noexcept { ::operator delete(p, a); }
void operator delete[](void* p, std::size_t, std::align_val_t a) noexcept { ::operator delete[](p, a); }

using Clock = std::chrono::steady_clock;
using nlohmann::json;

static auto make(int id, bool electrical, double rate, unsigned frames) {
    auto amp = rig::createComponentByTableId(id);
    amp->setStringParameter(rig::model::kAnalysisParam, "1");
    amp->prepare(rate, frames);
    amp->setParameter("amp_core", electrical ? 1 : 0);
    amp->setParameter("level_matching", 0);
    amp->processControlRate();
    amp->reset();
    if (electrical) {
        auto* model = dynamic_cast<rig::model::Introspectable*>(amp.get());
        assert(model && json::parse(model->modelJson())["backend"] == "hybrid-circuit");
    }
    return amp;
}

struct Render {
    std::array<float, 256> leftIn{}, rightIn{}, left{}, right{};
    std::size_t pos = 0;
    double energy = 0, lastCpuSeconds = 0;
    double process(rig::IComponent& amp, double rate, unsigned frames) {
        for (unsigned i = 0; i < frames; ++i) {
            const double t = (pos + i) / rate;
            // Independent stereo signals exercise both circuit instances.
            leftIn[i] = 0.12 * std::sin(2 * M_PI * 110 * t) + 0.03 * std::sin(2 * M_PI * 2393 * t);
            rightIn[i] = 0.11 * std::sin(2 * M_PI * 147 * t) + 0.04 * std::sin(2 * M_PI * 3203 * t);
        }
        const float* in[] = {leftIn.data(), rightIn.data()};
        float* out[] = {left.data(), right.data()};
        rig::AudioBlock b{};
        b.in = in; b.out = out; b.num_channels = 2; b.num_frames = frames; b.sample_rate = rate;
        tracked = true;
        double cpuStart = 0.0, cpuEnd = 0.0;
        const bool clockStartOk = rig_test::threadCpuSeconds(cpuStart);
        const auto start = Clock::now();
        amp.processAudio(b);
        const auto end = Clock::now();
        const bool clockEndOk = rig_test::threadCpuSeconds(cpuEnd);
        lastCpuSeconds = cpuEnd - cpuStart;
        tracked = false;
        assert(clockStartOk && clockEndOk);
        for (unsigned i = 0; i < frames; ++i) {
            assert(std::isfinite(left[i]) && std::isfinite(right[i]));
            energy += double(left[i]) * left[i] + double(right[i]) * right[i];
        }
        pos += frames;
        return std::chrono::duration<double>(end - start).count();
    }
};

static void handoff(int id) {
    auto source = make(id, true, 48000, 64);
    auto witness = make(id, true, 48000, 64);
    auto replacement = make(id, true, 48000, 64);
    Render a, b, c;
    for (unsigned i = 0; i < 64; ++i) { a.process(*source, 48000, 64); b.process(*witness, 48000, 64); }
    // A direct scalar edit must retain the same history as a prepared clone
    // receiving state at publication. Input voltage scale is an independent
    // persistent utility, so this also checks preservation of target settings.
    witness->setParameter("input_voltage_scale", 1);
    witness->setParameter("B", .73f);
    witness->processControlRate();
    replacement->setParameter("input_voltage_scale", 1);
    replacement->setParameter("B", .73f);
    replacement->processControlRate();
    tracked = true;
    replacement->adoptRuntimeStateFrom(*source);
    tracked = false;
    c.pos = b.pos;
    for (unsigned block = 0; block < 8; ++block) {
        b.process(*witness, 48000, 64); c.process(*replacement, 48000, 64);
        for (unsigned i = 0; i < 64; ++i) {
            assert(std::abs(b.left[i] - c.left[i]) < 1e-6f);
            assert(std::abs(b.right[i] - c.right[i]) < 1e-6f);
        }
    }
    assert(replacement->getParameter("input_voltage_scale") == 1);
    assert(c.energy > 1e-10);
}

int main() {
    json report = {{"scope", "Integrated stereo hybrid reference, C++ heap instrumentation; wall timing is affected by OS contention"},
                   {"hardware_verified", false}, {"cases", json::array()}};
    unsigned misses = 0;
    for (int id : {56000, 34000}) {
        // This budget covers the legacy hybrid-circuit amp backend. Units now bound to the circuit engine (the JCM800
        // since the 2026-11-09 clean cut) have their own budget in test_param_drag / circuit_ab and are skipped here.
        if (rig::ce::circuitEngineEnabledFor(id)) { std::printf("skip %d: on the circuit engine\n", id); continue; }
        handoff(id);
        for (double rate : {48000., 96000.}) for (unsigned frames : {64u, 128u, 256u}) {
            for (bool electrical : {false, true}) {
                auto amp = make(id, electrical, rate, frames);
                Render r;
                for (unsigned i = 0; i < unsigned(rate * .05 / frames); ++i) r.process(*amp, rate, frames);
                std::vector<double> durations, ratios, cpuRatios;
                const unsigned blocks = static_cast<unsigned>(std::ceil(rate * .25 / frames));
                durations.reserve(3 * blocks);
                for (int trial = 0; trial < 3; ++trial) {
                    double seconds = 0, cpuSeconds = 0;
                    for (unsigned i = 0; i < blocks; ++i) {
                        const double elapsed = r.process(*amp, rate, frames);
                        seconds += elapsed; cpuSeconds += r.lastCpuSeconds; durations.push_back(elapsed * rate / frames);
                    }
                    ratios.push_back(seconds / (blocks * frames / rate));
                    cpuRatios.push_back(cpuSeconds / (blocks * frames / rate));
                }
                assert(r.energy > 1e-10);
                std::sort(durations.begin(), durations.end());
                std::sort(ratios.begin(), ratios.end());
                std::sort(cpuRatios.begin(), cpuRatios.end());
                const auto late = std::count_if(durations.begin(), durations.end(), [](double d) { return d > 1; });
                misses += late;
                json row = {{"table_id", id}, {"electrical", electrical}, {"rate", rate}, {"frames", frames},
                            {"median_thread_cpu_ratio", cpuRatios[1]}, {"median_audio_ratio", ratios[1]}, {"median_block_ratio", durations[durations.size()/2]},
                            {"p95_block_ratio", durations[(durations.size()-1)*95/100]}, {"max_block_ratio", durations.back()},
                            {"deadline_misses", late}, {"blocks", durations.size()}};
                report["cases"].push_back(row);
                std::cerr << row.dump() << '\n';
            }
        }
    }
    report["cpp_allocations_in_callback_or_handoff"] = allocations;
    report["cpp_deletions_in_callback_or_handoff"] = deletions;
    report["deadline_misses"] = misses;
    report["pass"] = allocations == 0 && deletions == 0 && misses == 0;
    std::cout << report.dump(2) << '\n';
    return allocations || deletions ? 1 : (misses ? 2 : 0);
}
