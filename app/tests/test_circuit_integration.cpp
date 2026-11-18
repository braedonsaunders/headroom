#undef NDEBUG
#include "platform/Environment.hpp"
#include "../src/core/RackGraph.hpp"
#include "../src/dsp/ComponentModel.hpp"
#include "json.hpp"
#include <cassert>
#include <cstdlib>
#include <cmath>
#include <iostream>

using nlohmann::json;

static std::unique_ptr<rig::IComponent> make(bool electrical) {
    auto amp = rig::createComponentByTableId(56000);
    amp->setStringParameter(rig::model::kAnalysisParam, "1");
    amp->prepare(48000, 64);
    amp->setParameter("amp_core", electrical ? 1 : 0);
    amp->setParameter("level_matching", 0);
    amp->processControlRate();
    amp->reset();
    return amp;
}

static double render(rig::IComponent& amp, int samples, double phase = 0) {
    double energy = 0;
    float input[64], left[64], right[64];
    for (int offset = 0; offset < samples; offset += 64) {
        for (int i = 0; i < 64; ++i)
            input[i] = 0.15f * std::sin(2 * 3.141592653589793 * 440 * (offset + i + phase) / 48000);
        const float* in[] = {input, input};
        float* out[] = {left, right};
        rig::AudioBlock block{};
        block.in = in; block.out = out; block.num_channels = 2; block.num_frames = 64; block.sample_rate = 48000;
        amp.processAudio(block);
        for (float sample : left) { assert(std::isfinite(sample)); energy += sample * sample; }
    }
    return energy;
}

int main() {
    // 56000 now runs on the circuit engine (clean cut); this test covers the older hybrid tube-amp path, which
    // unconverted amps still use, so it asks for the legacy model explicitly.
    rig::platform::setEnv("HEADROOM_LEGACY_MODELS", "1");
    auto legacy = make(false);
    auto electrical = make(true);
    const auto legacyEnergy = render(*legacy, 4096);
    const auto circuitEnergy = render(*electrical, 4096);
    assert(legacyEnergy > 1e-9 && circuitEnergy > 1e-9);
    assert(std::abs(legacyEnergy - circuitEnergy) > 1e-5);
    auto* introspection = dynamic_cast<rig::model::Introspectable*>(electrical.get());
    assert(introspection);
    auto model = json::parse(introspection->modelJson());
    assert(model["backend"] == "hybrid-circuit");
    assert(!model["netlist"].is_null());
    assert(model["circuit_sample_rate"] == 96000);
    assert(model["utilities"]["input_gate"] == false);

    const auto apiModel = json::parse(rig::model::describe(56000, R"({"amp_core":1})", "{}", 48000));
    assert(apiModel["verification"] == "circuit-reference");
    assert(apiModel["backend"] == "hybrid-circuit" && !apiModel["netlist"].is_null());
    assert(apiModel["capabilities"].is_object());
    assert(apiModel["blocks"][1]["fidelity"] == "circuit-solved" ||
           apiModel["blocks"][1]["fidelity"] == "behavioural");

    // Exported native parameters round-trip both engine selection and explicit utilities.
    auto restored = make(false);
    for (const auto& p : electrical->exportParams()) {
        if (p.is_string) restored->setStringParameter(p.id, p.str);
        else restored->setParameter(p.id, p.num);
    }
    restored->processControlRate();
    auto* restoredModel = dynamic_cast<rig::model::Introspectable*>(restored.get());
    assert(json::parse(restoredModel->modelJson())["backend"] == "hybrid-circuit");

    // Physical editor edits reach the actual model; feedback is not overwritten
    // by a hard-coded native control during component preparation.
    restored->setStringParameter(rig::model::kModelParam,
        R"({"schema":"headroom.overrides/1","circuit":{"values":{"control":{"feedback":0},"preamp":{"slopeR":56000}}}})");
    restored->processControlRate();
    model = json::parse(restoredModel->modelJson());
    assert(model["circuit"]["components"]["control.feedback"] == 0);
    assert(model["circuit"]["components"]["preamp.slopeR"] == 56000);
    assert(render(*restored, 1024) > 1e-9);

    // Native Variac/Sag/Bias controls change physical circuit parameters.
    restored->setParameter("Sg", 0.75f);
    restored->setParameter("Var", 0.25f);
    restored->setParameter("Bi", 0.75f);
    restored->processControlRate();
    model = json::parse(restoredModel->modelJson());
    assert(std::abs(model["circuit"]["components"]["power.sourceR"].get<double>() - 250) < 1e-6);
    assert(std::abs(model["circuit"]["components"]["power.sourceV"].get<double>() - 427.5) < 1e-6);
    assert(std::abs(model["circuit"]["components"]["power.biasV"].get<double>() + 27.75) < 1e-6);

    // The editor presents the unscaled component value, not the effective value.
    // Reapplying the displayed resistance while Sag is away from noon must not
    // multiply the control twice.
    assert(model["circuit"]["editable_components"]["power.sourceR"] == 200);
    double displayedResistance = 0;
    for (const auto& block : model["blocks"])
        for (const auto& value : block["values"])
            if (value.contains("edit") && value["edit"]["path"] == "circuit.values.power.sourceR")
                displayedResistance = value["raw"].get<double>();
    assert(displayedResistance == 200);
    restored->setStringParameter(rig::model::kModelParam,
        json({{"schema", "headroom.overrides/1"}, {"circuit", {{"values", {{"power", {{"sourceR", displayedResistance}}}}}}}}).dump());
    restored->processControlRate();
    model = json::parse(restoredModel->modelJson());
    assert(model["circuit"]["components"]["power.sourceR"] == 250);

    // State handoff itself keeps an unchanged physical model's history exact.
    auto replacement = make(true);
    replacement->adoptRuntimeStateFrom(*electrical);
    const auto a = render(*electrical, 512, 4096);
    const auto b = render(*replacement, 512, 4096);
    assert(std::abs(a - b) < 1e-8);
    // Equal current inputs do not imply equal stored capacitor/filter states.
    // Stereo history must survive a later mono segment (including silence).
    for (bool physical : {false, true}) {
        auto stereoAmp = make(physical);
        auto rightWitness = make(physical);
        float il[64], ir[64], sl[64], sr[64], wl[64], wr[64];
        for (int blockIndex = 0; blockIndex < 20; ++blockIndex) {
            for (int i = 0; i < 64; ++i) {
                const double t = (blockIndex * 64 + i) / 48000.0;
                il[i] = 0.15f * std::sin(2 * 3.141592653589793 * 440 * t);
                ir[i] = blockIndex < 16 ? -0.08f * std::sin(2 * 3.141592653589793 * 220 * t) : il[i];
            }
            const float* input[] = {il, ir};
            const float* referenceInput[] = {ir, ir};
            float* output[] = {sl, sr};
            float* referenceOutput[] = {wl, wr};
            rig::AudioBlock block{};
            block.in = input; block.out = output; block.num_channels = 2; block.num_frames = 64; block.sample_rate = 48000;
            stereoAmp->processAudio(block);
            block.in = referenceInput; block.out = referenceOutput;
            rightWitness->processAudio(block);
            for (int i = 0; i < 64; ++i) assert(std::abs(sr[i] - wl[i]) < 1e-6f);
        }
    }
    std::cout << "Circuit integration: legacy energy=" << legacyEnergy
              << ", electrical energy=" << circuitEnergy << "; persistence, physical controls and state handoff pass\n";
}
