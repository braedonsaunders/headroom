#undef NDEBUG
#include "../src/core/RackGraph.hpp"
#include <cassert>
#include <iostream>

// An isolated rack ownership test. No Audio Unit instantiation, model files or
// hardware: the fake component exposes its render history and destruction thread.
namespace {
thread_local bool audioThread = false;
bool rejectPreparation = false;
struct Probe final : rig::SchemaComponentBase {
    explicit Probe(int32_t table) : SchemaComponentBase(*rig::catalog::findByTableId(26000)), table(table) {}
    ~Probe() override { assert(!audioThread); }
    int32_t tableId() const noexcept override { return table; }
    void adoptRuntimeStateFrom(rig::IComponent& old) noexcept override {
        if (auto* probe = dynamic_cast<Probe*>(&old)) ticks = probe->ticks;
    }
    void onProcessAudio(rig::AudioBlock& b) override { ++ticks; b.copyInToOut(); }
    void onPrepare(double, uint32_t) override {
        if (rejectPreparation) throw std::runtime_error("rejected preparation");
    }
    void onParameterChanged(std::string_view id, float value) override {
        if (id == "setter_reject" && value > 0.5f) throw std::invalid_argument("rejected setter");
    }
    void onProcessControlRate() override {
        if (getParameter("reject") > 0.5f || getStringParameter("model") == "invalid")
            throw std::invalid_argument("rejected control packet");
    }
    int32_t table;
    int ticks = 0;
};
}
namespace rig {
std::unique_ptr<IComponent> createComponentByTableId(int32_t table) { return std::make_unique<Probe>(table); }
bool switchReadsMidpointAsOn(int32_t, std::string_view) noexcept { return false; } // SchemaComponentBase::switchState
}
namespace rig::catalog {
const ComponentSchema* findExtraComponent(int32_t) noexcept { return nullptr; }
}
namespace rig::plugins {
void remapPreset(preset::RackPreset&) {}
const catalog::ComponentSchema* findSchema(int32_t) noexcept { return nullptr; }
}
int main() {
    rig::RackGraph model;
    rig::preset::RackPreset preset;
    rig::preset::PresetComponentNode a, b;
    a.table_id = 26000; b.table_id = 28000;
    preset.root_rack.children = {a, b};
    assert(model.buildFromPreset(preset, 48000, 64));
    auto first = model.renderSnapshot();
    auto firstViews = first->activeNodeViews();
    auto* untouched = firstViews[1].component;
    auto* changed = firstViews[0].component;
    assert(model.activeNodeViews()[0].component == changed);
    // An edit detaches only its target; pending and audible snapshots are immutable.
    model.setComponentParameter(0, "test", 0.75f);
    assert(model.activeNodeViews()[0].component != changed);
    assert(model.activeNodeViews()[1].component == untouched);
    assert(changed->getParameter("test") == 0.0f);
    assert(model.activeNodeViews()[0].component->getParameter("test") == 0.75f);
    dynamic_cast<Probe*>(changed)->ticks = 123;
    auto next = model.renderSnapshot();
    audioThread = true;
    next->inheritRenderState(*first);
    audioThread = false;
    assert(dynamic_cast<Probe*>(next->activeNodeViews()[0].component)->ticks == 123);
    assert(next->activeNodeViews()[1].component == untouched);
    // A structural edit preserves instance identity and serializes the new order.
    assert(model.moveSlot(1, 0));
    auto moved = model.renderSnapshot();
    assert(moved->activeNodeViews()[0].component == untouched);
    const auto saved = model.toPreset("edited", "test");
    assert(saved.root_rack.children[0].table_id == 28000);
    assert(saved.root_rack.children[1].table_id == 26000);
    const int stable = moved->activeNodeViews()[0].stable_id;
    assert(stable == model.activeNodeViews()[0].stable_id);
    // Coalescing publication never loses edits on another slot.
    model.setComponentParameter(0, "test", 0.25f);
    model.setComponentParameter(1, "test", 0.9f);
    auto coalesced = model.renderSnapshot();
    assert(coalesced->activeNodeViews()[0].component->getParameter("test") == 0.25f);
    assert(coalesced->activeNodeViews()[1].component->getParameter("test") == 0.9f);
    assert(coalesced->activeNodeViews()[0].stable_id == stable);
    // Rejected preparation/setters/control packets leave model, save data and
    // already audible component untouched, including labels and dirty status.
    model.setDirty(false);
    auto beforeFailure = model.toPreset("valid", "test");
    auto* validComponent = model.activeNodeViews()[0].component;
    const auto validLabel = model.slotPresetLabel(0);
    auto checkUnchanged = [&] {
        assert(model.activeNodeViews()[0].component == validComponent);
        assert(!model.isDirty());
        assert(model.slotPresetLabel(0) == validLabel);
        assert(validComponent->getParameter("reject") == 0.0f);
        assert(validComponent->getStringParameter("model").empty());
        auto after = model.toPreset("valid", "test");
        const auto& a = beforeFailure.root_rack.children[0].params;
        const auto& b = after.root_rack.children[0].params;
        assert(a.size() == b.size());
        for (std::size_t i = 0; i < a.size(); ++i) {
            assert(a[i].id == b[i].id && a[i].float_val == b[i].float_val && a[i].str_val == b[i].str_val);
        }
        assert(coalesced->activeNodeViews()[0].component == validComponent);
    };
    auto reject = [&](auto edit) {
        bool rejected = false;
        try { edit(); } catch (const std::exception&) { rejected = true; }
        assert(rejected);
        checkUnchanged();
    };
    assert(!model.editComponent(0, [](rig::IComponent* c) { c->setParameter("test", 0.99f); return false; }));
    checkUnchanged();
    reject([&] { model.setComponentParameter(0, "reject", 1.0f); });
    reject([&] { model.setComponentNormalizedParameter(0, "reject", 1.0f); });
    reject([&] { model.setComponentParameter(0, "setter_reject", 1.0f); });
    reject([&] { model.applyParams(0, {{"test", 0.8f}, {"reject", 1.0f}}, "Rejected preset"); });
    reject([&] { model.editComponent(0, [](rig::IComponent* c) {
        c->setStringParameter("model", "invalid");
        c->processControlRate();
        return true;
    }); });
    rejectPreparation = true;
    reject([&] { model.setComponentParameter(0, "test", 0.8f); });
    rejectPreparation = false;
    // A later successful edit cannot accidentally publish any rejected field.
    model.setComponentParameter(0, "test", 0.7f);
    auto afterSuccess = model.renderSnapshot();
    assert(afterSuccess->activeNodeViews()[0].component->getParameter("test") == 0.7f);
    assert(afterSuccess->activeNodeViews()[0].component->getParameter("reject") == 0.0f);
    assert(afterSuccess->activeNodeViews()[0].component->getStringParameter("model").empty());
    // A macro is one transaction even when an earlier target succeeds and a
    // later target rejects the packet.
    model.assignMacro(0, 0, "test", 0.0f, 1.0f, "Transactional", false);
    model.assignMacro(0, 1, "reject", 0.0f, 1.0f, "Transactional", true);
    model.setDirty(false);
    const auto oldMacro = model.getMacroNormalized(0);
    const auto macroNodes = model.activeNodeViews();
    bool macroRejected = false;
    try { model.setMacroNormalized(0, 1.0f); } catch (const std::invalid_argument&) { macroRejected = true; }
    assert(macroRejected && !model.isDirty());
    assert(model.getMacroNormalized(0) == oldMacro);
    assert(model.activeNodeViews()[0].component == macroNodes[0].component);
    assert(model.activeNodeViews()[1].component == macroNodes[1].component);
    assert(model.activeNodeViews()[0].component->getParameter("test") == 0.7f);
    assert(model.activeNodeViews()[1].component->getParameter("reject") == 0.0f);
    auto checkpoint = model.controlSnapshot();
    assert(checkpoint->macroViews()[0].targets.size() == model.macroViews()[0].targets.size());
    model.removeActiveSlot(0);
    assert(checkpoint->activeNodeViews().size() == 2);
    assert(checkpoint->macroViews()[0].targets.size() == 2);
    // Hosted units retain the exact object used by their native editor while
    // scalar controls go through the hosting API instead of cloning the unit.
    rig::RackGraph hosted;
    a.table_id = 910000;
    preset.root_rack.children = {a};
    hosted.buildFromPreset(preset, 48000, 64);
    auto hostedPlan = hosted.renderSnapshot();
    auto* instance = hostedPlan->activeNodeViews()[0].component;
    hosted.setComponentParameter(0, "test", 0.6f);
    assert(hosted.activeNodeViews()[0].component == instance);
    assert(hosted.renderSnapshot()->activeNodeViews()[0].component == instance);
    // A rejected native target must not leak an earlier macro value to a
    // shared hosted instance (its editor and render plan use the same object).
    rig::RackGraph mixed;
    b.table_id = 28000;
    preset.root_rack.children = {a, b};
    assert(mixed.buildFromPreset(preset, 48000, 64));
    auto mixedPlan = mixed.renderSnapshot();
    auto* hostedInstance = mixedPlan->activeNodeViews()[0].component;
    mixed.assignMacro(0, 0, "test", 0, 1, "Mixed", false);
    mixed.assignMacro(0, 1, "reject", 0, 1, "Mixed", true);
    bool mixedRejected = false;
    try { mixed.setMacroNormalized(0, 1); } catch (const std::invalid_argument&) { mixedRejected = true; }
    assert(mixedRejected);
    assert(hostedInstance->getParameter("test") == 0);
    assert(mixed.activeNodeViews()[0].component == hostedInstance);
    std::cout << "rack snapshots: target-only detach, immutable params, identity, history and serialization passed\n";
}
