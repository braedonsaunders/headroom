#undef NDEBUG
#include "platform/Environment.hpp"
#include "../src/core/IComponent.hpp"
#include "test_compat.hpp"
#include "../src/dsp/ComponentModel.hpp"
#include "json.hpp"
#include <array>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>
#include <cstring>

namespace { thread_local bool inHandoff = false; }
void* operator new(std::size_t size) {
    assert(!inHandoff);
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { assert(!inHandoff); std::free(p); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

void* operator new(std::size_t size, std::align_val_t alignment) {
    assert(!inHandoff);
    if (void* p = rig_test::alignedAlloc(static_cast<std::size_t>(alignment), size)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p, std::align_val_t) noexcept {
    if constexpr (rig_test::kAlignedNeedsOwnFree) rig_test::alignedFree(p); // _aligned_malloc block (Windows)
    else ::operator delete(p);
}
void* operator new[](std::size_t size, std::align_val_t alignment) { return ::operator new(size, alignment); }
void operator delete[](void* p, std::align_val_t alignment) noexcept { ::operator delete(p, alignment); }
void operator delete(void* p, std::size_t, std::align_val_t alignment) noexcept { ::operator delete(p, alignment); }
void operator delete[](void* p, std::size_t, std::align_val_t alignment) noexcept { ::operator delete(p, alignment); }

// A scalar edit replaces the wrapper, but the capture's recurrent/convolution
// history, resampler phase/FIFO and EQ/gate histories must remain continuous.
static void checkNam(double rate) {
    auto source = rig::createComponentByTableId(900005);
    auto reference = rig::createComponentByTableId(900005);
    auto next = rig::createComponentByTableId(900005);
    assert(source && reference && next);
    int demo = -1;
    for (const auto& extra : source->extraParams()) {
        if (extra.id != "capture") continue;
        for (std::size_t i = 0; i < extra.labels.size(); ++i)
            if (i < extra.groups.size() && extra.groups[i] == "Demo Captures" &&
                extra.labels[i].find("Microtubes 900 v2") != std::string::npos)
                demo = static_cast<int>(i);
    }
    assert(demo >= 0); // Select the shipped 48 kHz LSTM, independent of user capture order.
    for (auto* component : {source.get(), reference.get(), next.get()}) {
        component->prepare(rate, 64);
        component->setParameter("capture", static_cast<float>(demo));
        component->setParameter("gate", 0.35f);
        component->processControlRate();
    }
    auto* inspect = dynamic_cast<rig::model::Introspectable*>(source.get());
    assert(inspect);
    const auto model = nlohmann::json::parse(inspect->modelJson());
    if (!model["loaded"].get<bool>()) std::cerr << model.dump() << std::endl;
    assert(model["loaded"].get<bool>()); // A missing capture's passthrough cannot pass this test.
    assert(model["model_rate"].get<double>() == 48000);
    assert(model["resampled"].get<bool>() == (rate != 48000));

    std::array<float, 64> input{}, left{}, right{}, refLeft{}, refRight{};
    const float* inputs[2] = {input.data(), input.data()};
    float* outputs[2] = {left.data(), right.data()};
    float* refOutputs[2] = {refLeft.data(), refRight.data()};
    rig::AudioBlock block{};
    block.in = inputs; block.out = outputs; block.num_channels = 2;
    block.num_frames = 64; block.sample_rate = rate;
    auto refBlock = block; refBlock.out = refOutputs;
    auto signal = [&](int b) {
        for (int i = 0; i < 64; ++i) {
            const double t = (b * 64 + i) / rate;
            input[i] = static_cast<float>(0.12 * std::sin(2764.6 * t) + 0.03 * std::sin(6565.4 * t));
        }
    };
    for (int b = 0; b < 96; ++b) {
        signal(b);
        inHandoff = true;
        source->processAudio(block);
        reference->processAudio(refBlock);
        inHandoff = false;
    }
    // Apply the same actual change to a continuously running witness and the
    // new prepared wrapper. No reset occurs between warm-up and comparison.
    for (auto* component : {reference.get(), next.get()}) {
        component->setParameter("input", 3.0f);
        component->setParameter("output", -2.0f);
        component->setParameter("bass", 0.72f);
        component->setParameter("mid", 0.31f);
        component->setParameter("treble", 0.61f);
        component->processControlRate();
    }
    inHandoff = true;
    next->adoptRuntimeStateFrom(*source);
    inHandoff = false;
    double energy = 0;
    for (int b = 96; b < 144; ++b) {
        signal(b);
        if (b > 128) input.fill(0); // Also compare the post-edit network and filter tail.
        inHandoff = true;
        next->processAudio(block);
        reference->processAudio(refBlock);
        inHandoff = false;
        for (int i = 0; i < 64; ++i) {
            assert(std::isfinite(left[i]) && std::isfinite(right[i]));
            assert(std::abs(left[i] - refLeft[i]) < 1e-6f);
            assert(std::abs(right[i] - refRight[i]) < 1e-6f);
            energy += left[i] * left[i] + right[i] * right[i];
        }
    }
    assert(energy > 1e-8);
    std::cout << "NAM edited-state handoff passed: " << rate << " Hz, resampled=" << (rate != 48000) << '\n';
}

static void check(int32_t table) {
    auto source = rig::createComponentByTableId(table);
    auto reference = rig::createComponentByTableId(table);
    auto next = rig::createComponentByTableId(table);
    assert(source && reference && next);
    for (auto* component : {source.get(), reference.get(), next.get()}) {
        component->prepare(48000, 64);
        component->processControlRate();
    }
    // Identical scalar packet reconstructs an edited component. This regression
    // isolates history transfer from changes in filter coefficients or controls.
    for (const auto& p : source->exportParams()) {
        if (p.is_string) next->setStringParameter(p.id, p.str);
        else next->setParameter(p.id, p.num);
    }
    next->processControlRate();
    std::array<float, 64> input{}, left{}, right{}, refLeft{}, refRight{};
    const float* inputs[2] = {input.data(), input.data()};
    float* outputs[2] = {left.data(), right.data()};
    float* refOutputs[2] = {refLeft.data(), refRight.data()};
    rig::AudioBlock block{};
    block.in = inputs; block.out = outputs; block.num_frames = 64; block.sample_rate = 48000;
    auto refBlock = block; refBlock.out = refOutputs;
    for (int b = 0; b < 600; ++b) {
        for (int i = 0; i < 64; ++i) input[i] = 0.1f * std::sin((b * 64 + i) * 0.017f);
        source->processAudio(block);
        reference->processAudio(refBlock);
    }
    inHandoff = true;
    next->adoptRuntimeStateFrom(*source);
    inHandoff = false;
    input.fill(0);
    double energy = 0;
    for (int b = 0; b < 400; ++b) {
        next->processAudio(block); reference->processAudio(refBlock);
        for (int i = 0; i < 64; ++i) {
            assert(std::isfinite(left[i]) && std::isfinite(right[i]));
            assert(std::abs(left[i] - refLeft[i]) < 1e-6f);
            assert(std::abs(right[i] - refRight[i]) < 1e-6f);
            energy += left[i] * left[i] + right[i] * right[i];
        }
    }
    if (std::strcmp(rig::implementationKind(table), "delay") == 0 ||
        std::strcmp(rig::implementationKind(table), "fdn_reverb") == 0) assert(energy > 1e-8);
    std::cout << "effect history handoff passed: " << table << '\n';
}
// A switch that moves contacts the circuit leaves out while open (CompileOptions::openSwitches, docs/CIRCUIT_ENGINE.md
// §10.29): the edit copy is rebuilt for the new positions on the control thread and takes the running state across the
// topology at the swap (Circuit::adoptStateFrom), allocation-free. The witness runs the same unit with every contact
// as its roff resistor (CE_ROFFSW=1, one partition for every position) and flips the switch in place at the same sample.
static void checkSwitchTopology(int32_t table, const char* param, float to) {
    auto make = [&](bool roff) {
        if (roff) rig::platform::setEnv("CE_ROFFSW", "1");
        auto c = rig::createComponentByTableId(table);
        assert(c);
        c->setStringParameter(rig::model::kModelParam, R"({"noise":false})"); // noise and hum off: their draws follow the partition
        c->prepare(48000, 64);
        c->processControlRate();
        if (roff) rig::platform::unsetEnv("CE_ROFFSW");
        return c;
    };
    auto reference = make(true), source = make(false), next = make(false);
    std::array<float, 64> input{}, left{}, right{}, refLeft{}, refRight{};
    const float* inputs[2] = {input.data(), input.data()};
    float* outputs[2] = {left.data(), right.data()};
    float* refOutputs[2] = {refLeft.data(), refRight.data()};
    rig::AudioBlock block{};
    block.in = inputs; block.out = outputs; block.num_frames = 64; block.sample_rate = 48000;
    auto refBlock = block; refBlock.out = refOutputs;
    auto signal = [&](int b) {
        for (int i = 0; i < 64; ++i) input[i] = 0.3f * std::sin((b * 64 + i) * 0.031f) + 0.1f * std::sin((b * 64 + i) * 0.27f);
    };
    double eBefore = 0, dBefore = 0;
    for (int b = 0; b < 600; ++b) {
        signal(b);
        source->processAudio(block);
        reference->processAudio(refBlock);
        if (b >= 300)
            for (int i = 0; i < 64; ++i) eBefore += refLeft[i] * refLeft[i], dBefore += (left[i] - refLeft[i]) * (left[i] - refLeft[i]);
    }
    // the edit: a detached copy with the source's parameters and the switch moved (RackGraph::detachForEdit)
    for (const auto& p : source->exportParams()) {
        if (p.is_string) next->setStringParameter(p.id, p.str);
        else next->setParameter(p.id, p.num);
    }
    next->setParameter(param, to);
    next->processControlRate();
    reference->setParameter(param, to);
    reference->processControlRate();
    inHandoff = true;
    next->adoptRuntimeStateFrom(*source);
    inHandoff = false;
    // The witness's contacts move at its next control refresh (Circuit::process: every 0.33 ms while controls glide), the
    // rebuilt circuit's at the swap: the switching transient itself is shifted by up to that much (about -40 dB in the
    // first block, measured -18 dB for a 20-sample shift on the bare circuits). The hand-off is gated after it: a lost or
    // mis-mapped state (a capacitor's current, a device's warm start) persists for tens of milliseconds.
    double e = 0, d = 0, dmax = 0, pk = 0, d0 = 0, e0 = 0;
    for (int b = 600; b < 1000; ++b) {
        signal(b);
        next->processAudio(block);
        reference->processAudio(refBlock);
        if (std::getenv("HANDOFF_TRACE") && (b < 612 || b % 50 == 0)) {
            double eb = 0, db = 0;
            for (int i = 0; i < 64; ++i) eb += refLeft[i] * refLeft[i], db += (left[i] - refLeft[i]) * (left[i] - refLeft[i]);
            std::cout << "  block " << b << ": " << 10 * std::log10(std::max(db, 1e-300) / std::max(eb, 1e-300)) << " dB\n";
        }
        for (int i = 0; i < 64; ++i) {
            assert(std::isfinite(left[i]) && std::isfinite(right[i]));
            if (b < 610) {
                e0 += refLeft[i] * refLeft[i];
                d0 += (left[i] - refLeft[i]) * (left[i] - refLeft[i]);
                continue;
            }
            e += refLeft[i] * refLeft[i];
            d += (left[i] - refLeft[i]) * (left[i] - refLeft[i]);
            dmax = std::max(dmax, static_cast<double>(std::abs(left[i] - refLeft[i])));
            pk = std::max(pk, static_cast<double>(std::abs(refLeft[i])));
        }
    }
    const double before = 10 * std::log10(std::max(dBefore, 1e-300) / std::max(eBefore, 1e-300)), after = 10 * std::log10(std::max(d, 1e-300) / std::max(e, 1e-300));
    const double first = 10 * std::log10(std::max(d0, 1e-300) / std::max(e0, 1e-300));
    std::cout << "switch topology handoff: " << table << " " << param << " -> " << to << ": against the roff witness " << before << " dB before, "
              << first << " dB over the switching transient (10 blocks), " << after << " dB after it (max |d| " << dmax << " of " << pk << ")\n";
    assert(e > 1e-6);
    assert(before < -100.0 && after < -100.0);
}

int main() {
    rig::platform::setEnv("HEADROOM_CIRCUIT_NOISE", "1"); // exercise circuit noise despite the user default
    rig::platform::setEnv("HEADROOM_NOISE_FIXED_SEED", "1"); // reproducible noise streams (CircuitUnit, docs §3.9)
    checkSwitchTopology(24000, "mode", 1.0f); // Fairchild 670: S301 into lateral / vertical (its two halves join)
    checkNam(48000);
    checkNam(44100);
    for (int table : {26000, 24000, 29000, 33000, 148000, 31000}) check(table); // 31000: circuit amp + rotor block
    for (const auto& schema : rig::catalog::kAllComponents)
        if (std::strcmp(rig::implementationKind(schema.table_id), "delay") == 0) { check(schema.table_id); break; }
}
