#pragma once
#include "AudioBlock.hpp"
#include "../circuit/CircuitUnit.hpp"
#include "IComponent.hpp"
#include "../preset/PresetLoader.hpp"
#include "../dsp/AmpModeling.hpp"
#include "../dsp/BassCalibration.hpp"
#include "../midi/MidiEvent.hpp"
#include "../dsp/NamComponent.hpp"
#include "../plugins/PluginHost.hpp"
#include "../dsp/CabinetUnit.hpp"
#include "Tempo.hpp"
#include "UnitMeters.hpp"
#include "EngineDiag.hpp"
#include "RtPool.hpp"
#include "PreparedUnitPool.hpp"
#if defined(__x86_64__)
#include <immintrin.h>
#endif
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <cfenv>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <stdexcept>
#include <exception>
#include <unordered_map>

namespace rig {

class RackGraph {
    struct GraphNode;

public:
    struct ActiveNodeView {
        std::size_t slot_idx = 0;
        int32_t table_id = 0;
        int32_t instance_index = 0;
        IComponent* component = nullptr;
        int stable_id = 0; // retained across reorders and prepared edit snapshots
    };

    RackGraph() = default;

    // Control thread only: split lanes are not yet delay-aligned, so report the slower lane.
    uint32_t latencySamples() const noexcept { return master_bypass_ ? 0 : latencyList(nodes_, 0, nodes_.size()); }

    // Series-unit pipelining: consecutive units of a chain (the top level, or a split lane) render as a wavefront over
    // sub-blocks on RtPool cores, bit-identical to rendering them one after another. A top-level run continues through a
    // split, into its lanes and back. On by default; HEADROOM_PIPE=0 or setPipelining(false) renders sequentially (the
    // reference for tests). sub_frames 0 = automatic.
    void setPipelining(bool on, uint32_t sub_frames = 0) noexcept { pipe_on_ = on; pipe_sub_ = sub_frames; }
    bool pipelining() const noexcept { return pipe_on_; }
    uint32_t pipeSubFrames() const noexcept { return pipe_sub_; } // 0 = automatic
    static constexpr int pipeMaxStages() noexcept { return kPipeJobs; } // one run: the series chain and a crossed split's lane B
    // CrossOver split (50000 / 51000 / 52000): the CrossOver Mix's "f" (0..1) as the band edge in Hz, 20 Hz to 20 kHz on
    // a log scale (0.5 = 632 Hz).
    static double crossoverHz(float f) noexcept { return 20.0 * std::pow(1000.0, static_cast<double>(std::clamp(f, 0.0f, 1.0f))); }


    // Control thread only. A render plan owns topology/scratch independently, while
    // unchanged components retain their delay lines and hosted editor identity.
    // Never render two plans that share components (including during a crossfade).
    std::unique_ptr<RackGraph> renderSnapshot() const {
        auto out = std::make_unique<RackGraph>();
        out->sample_rate_ = sample_rate_;
        out->max_block_size_ = max_block_size_;
        out->input_gain_ = input_gain_;
        out->master_bypass_ = master_bypass_;
        out->debug_taps_ = debug_taps_;
        out->unit_meters_ = unit_meters_; // per-unit face meters (shared by every plan of this rig)
        out->diag_ = diag_;               // /api/diag counters (EngineDiag.hpp), shared the same way
        out->tempo_bpm_ = tempo_bpm_;
        out->next_uid_ = next_uid_;
        out->render_node_index_.reserve(totalComponentCount());
        for (const auto& n : nodes_) out->nodes_.push_back(snapshotNode(*n));
        for (auto& n : out->nodes_) out->indexRenderNode(*n);
        out->collectActiveNodes();
        out->slot_gain_scale_ = slot_gain_scale_;
        out->slot_mix_scale_ = slot_mix_scale_;
        out->midi_main_.reserve(2048);
        return out;
    }

    // Control-side rollback checkpoint. It copies model metadata and retargets
    // macro pointers while shared components force subsequent edits through COW.
    std::unique_ptr<RackGraph> controlSnapshot() const {
        auto out = renderSnapshot();
        out->active_preset_ = active_preset_;
        out->macros_ = macros_;
        for (auto& macro : out->macros_)
            for (auto& target : macro.targets)
                if (target.node) target.node = out->render_node_index_.at(target.node->uid);
        out->dirty_ = dirty_;
        out->preset_source_ = preset_source_;
        return out;
    }

    template<class Fn>
    bool editComponent(std::size_t slot, Fn&& edit) {
        if (slot >= flat_active_nodes_.size()) return edit(nullptr);
        auto& node = *flat_active_nodes_[slot];
        ComponentEdit transaction(*this, node);
        const bool changed = edit(node.component.get());
        if (!changed) transaction.cancel();
        return changed;
    }

    // Audio thread at a handoff between edits of the same rack only. The model
    // never reads these mutable envelopes. Preset replacements start afresh.
    void inheritRenderState(RackGraph& previous) noexcept {
        for (auto& n : nodes_) inheritNodeState(*n, previous.render_node_index_);
        input_gain_cur_ = previous.input_gain_cur_;
        master_rms_env_ = previous.master_rms_env_;
        master_peak_env_ = previous.master_peak_env_;
        limiter_env_ = previous.limiter_env_;
    }

    static bool isSplitStartNode(int32_t tid) noexcept {
        return tid == 7000 || tid == 50000 || tid == 135000;
    }

    static bool isSplitMidNode(int32_t tid) noexcept {
        return tid == 8000 || tid == 51000 || tid == 136000;
    }

    static bool isSplitMixNode(int32_t tid) noexcept {
        return tid == 9000 || tid == 52000 || tid == 137000;
    }

    static bool isContainerOrSplitNode(int32_t tid) noexcept {
        return tid == 100 || tid == 7000 || tid == 8000 ||
               tid == 39000 || tid == 50000 || tid == 51000 ||
               tid == 90000 || tid == 135000 ||
               tid == 136000 || tid == 138000 || tid == 139000;
    }

    static bool isUtilityOrModulatorNode(int32_t tid) noexcept {
        return isContainerOrSplitNode(tid) || isSplitMixNode(tid) ||
               tid == 2000 || tid == 4000 || tid == 6000 || tid == 12000 ||
               tid == 55000 || tid == 57000 || tid == 58000 || tid == 59000 ||
               tid == 61000 || tid == 63000 || tid == 66000 || tid == 68000 ||
               tid == 69000 || tid == 70000 || tid == 83000 || tid == 89000 ||
               tid == 140000 || tid == 141000 || tid == 153000;
    }

    // A rig plays exactly at the level its units' settings give: there is no loudness gain, and the engine never
    // measures or re-levels a rig. (The shipped library is levelled offline, into its units' own level knobs:
    // tools/rig_level, core/LevelFold.hpp.)
    bool buildFromPreset(const preset::RackPreset& preset, double sample_rate = 48000.0, uint32_t max_block_size = 512) {
        sample_rate_ = sample_rate;
        max_block_size_ = max_block_size;
        active_preset_ = preset;
        tempo_bpm_ = preset.tempo_bpm > 0.0 ? clampTempo(preset.tempo_bpm) : kTempoDefault;
        plugins::remapPreset(active_preset_); // hosted plugins: match by identity to this machine's ids
        nodes_.clear();
        flat_active_nodes_.clear();
        master_rms_env_ = 0.14f;
        master_peak_env_ = 0.0f;

        std::vector<DeferredPrepare> deferred;
        prepared_with_params_.clear();
        spares_used_ = 0;
        for (const auto& child : active_preset_.root_rack.children) {
            if (auto node = buildNode(child, parallel_prepare_ ? &deferred : nullptr)) {
                nodes_.push_back(std::move(node));
            }
        }
        planDeferredCabFollow(nodes_, deferred);
        prepareDeferred(deferred);
        collectActiveNodes();
        initMacros();
        syncCabFollow();
        dirty_ = false;
        return !nodes_.empty();
    }

    // Bass rigs are levelled with a bass-register DI instead (they answer the guitar DI more quietly than real bass
    // playing, so they came out ~4-5 LU hot). The shipped library's metadata decides when present
    // (<headroom-library instrument>); otherwise a rig with at least as many active bass amps as guitar amps is bass.
    bool calibratesAsBass() const {
        const std::string& inst = active_preset_.info.instrument;
        if (inst == "bass") return true;
        if (!inst.empty()) return false;
        int bass = 0, other = 0;
        for (const auto* n : allNodesFlat())
            if (isAmpTable(n->table_id) && n->component && !n->component->isBypassed()) (amp::isBassAmpTable(n->table_id) ? bass : other)++;
        return bass > 0 && bass >= other;
    }

    // OFFLINE ONLY (tools/rig_level levels the shipped library with it once; tests). Renders a deterministic DI
    // performance through the rack and returns the level change (dB) that would put the rig at the library's
    // perceived loudness (K-weighted), like a mastered factory library. Changes nothing (the rack is reset after).
    // Returns NaN when the rack is empty, silent or the render was cancelled.
    float calibrateLoudness() {
        limiter_env_ = 0.0f;
        if (nodes_.empty()) return std::numeric_limits<float>::quiet_NaN();
        const bool bass = calibratesAsBass();
        const auto sig = bass ? amp::makeBassCalibrationDI(sample_rate_, 4.0f) : amp::makeCalibrationDI(sample_rate_, 4.0f);
        amp::LoudnessMeter meter;
        meter.prepare(sample_rate_);
        const uint32_t bs = std::min<uint32_t>(max_block_size_ > 0 ? max_block_size_ : 512, 512);
        std::vector<float> inL(bs), outL(bs), outR(bs);
        calibrating_ = true;
        for (std::size_t pos = 0; pos < sig.size(); pos += bs) {
            if (cancel_ && cancel_()) { // the caller discards this rack (setCancelCheck): stop rendering, level unchanged
                calibrating_ = false;
                reset();
                return std::numeric_limits<float>::quiet_NaN();
            }
            const uint32_t n = static_cast<uint32_t>(std::min<std::size_t>(bs, sig.size() - pos));
            std::copy(sig.begin() + static_cast<std::ptrdiff_t>(pos), sig.begin() + static_cast<std::ptrdiff_t>(pos + n), inL.begin());
            const float* ins[2] = {inL.data(), inL.data()};
            float* outs[2] = {outL.data(), outR.data()};
            AudioBlock b{};
            b.in = ins;
            b.out = outs;
            b.num_channels = 2;
            b.num_frames = n;
            b.sample_rate = sample_rate_;
            process(b);
            if (pos > sig.size() / 16) {
                for (uint32_t i = 0; i < n; ++i) meter.add(outL[i], outR[i]);
            }
        }
        calibrating_ = false;
        const float db = meter.meanSquareDb();
        reset();
        if (!(db > -90.0f)) return std::numeric_limits<float>::quiet_NaN();
        return kTargetLoudnessDb + (bass ? amp::kBassCalibrationTrimDb : 0.0f) - db; // unclamped
    }

    // buildFromPreset prepares the units whose prepare is independent (IComponent::preparesIndependently: the circuit
    // units, whose supply settle is most of a rig's load time) on worker threads. The rack is the same either way;
    // off by default so offline builds (tools, levelling renders) stay on one core.
    void setParallelPrepare(bool on) noexcept { parallel_prepare_ = on; }
    // Polled between units by buildFromPreset: once it returns true the remaining units are left unprepared (the caller
    // discards the rack: a newer preset load superseded it). Empty = never.
    void setCancelCheck(std::function<bool()> cancelled) { cancel_ = std::move(cancelled); }
    // buildFromPreset takes already-prepared units from PreparedUnitPool where it has them (the standalone app's loads).
    void setUseSpares(bool on) noexcept { use_spares_ = on; }
    // One entry per unit whose prepare is independent and expensive (PreparedUnitPool::want), right after
    // buildFromPreset. Amps prepared with their parameters (planDeferredCabFollow) are left out: a spare, prepared at
    // the defaults, would not save their settle.
    std::vector<int32_t> independentUnitTableIds() const {
        std::vector<int32_t> out;
        for (const auto* n : allNodesFlat())
            if (n->component && n->component->preparesIndependently() &&
                std::find(prepared_with_params_.begin(), prepared_with_params_.end(), n->component.get()) == prepared_with_params_.end())
                out.push_back(n->table_id);
        return out;
    }
    // Control thread, model lock held, before a plan is published (and after a preset build). Tells each amp
    // whether a cabinet follows it, by the same rule as coupleSpeakerLoad below. A unit whose rendering depends on it
    // (circuit amps: quality "hq" when no cab follows, docs/CIRCUIT_ENGINE.md §10.12) is re-prepared here, on a
    // detached copy when a published plan shares it, so the switch is immediate and never touches the audio thread.
    // Returns true when a unit was re-prepared (the plan's handoff is then not continuous).
    bool syncCabFollow() {
        bool reprepared = false;
        syncCabFollowList(nodes_, reprepared);
        return reprepared;
    }

    // Recompile only affected circuits, detached from any published plan. The normal
    // render-plan handoff adopts the audible unit's state at the next block boundary.
    bool syncCircuitNoise() { return syncCircuitNoiseList(nodes_); }

    void setDebugTaps(bool on) noexcept { debug_taps_ = on; }
    // Per-unit meters for the UI's hardware faces (UnitMeters.hpp); null = off. Control thread, before publishing.
    void setMeterBank(std::shared_ptr<UnitMeterBank> bank) noexcept { unit_meters_ = std::move(bank); }
    // Engine diagnostics (EngineDiag.hpp, /api/diag); null = off. Control thread, before publishing.
    void setDiagBank(std::shared_ptr<EngineDiagBank> bank) noexcept { diag_ = std::move(bank); }
    // Units buildFromPreset took ready-prepared from PreparedUnitPool (setUseSpares).
    std::size_t sparesUsed() const noexcept { return spares_used_; }
    void dumpDebugTaps() const {
        for (const auto& n : nodes_) dumpTapsRec(*n, 0);
    }

    void prepare(double sample_rate, uint32_t max_block_size) {
        (void)RtPool::instance(); // start the worker threads here, on the control thread, never on the audio path
        sample_rate_ = sample_rate;
        max_block_size_ = max_block_size;
        master_rms_env_ = 0.14f;
        master_peak_env_ = 0.0f;
        for (auto& n : nodes_) {
            prepareNode(*n, sample_rate, max_block_size);
        }
    }

    void reset() {
        master_rms_env_ = 0.14f;
        master_peak_env_ = 0.0f;
        for (auto& n : nodes_) {
            resetNode(*n);
        }
    }

    // The audio owner may also set this on the plan it renders (once per block): process() glides to it per sample.
    void setInputGainLinear(float g) noexcept { input_gain_ = std::clamp(g, 0.0f, 8.0f); }
    float inputGainLinear() const noexcept { return input_gain_; }

    // Set on a render plan by the publisher: true when the edits since the previous audible plan were continuous
    // parameter moves (knobs, faders), whose DSP history the handoff inherits, so no output bridge is needed.
    void setContinuousEdit(bool c) noexcept { continuous_edit_ = c; }
    bool continuousEdit() const noexcept { return continuous_edit_; }

    void setMasterBypass(bool b) noexcept { master_bypass_ = b; }
    bool masterBypass() const noexcept { return master_bypass_; }

    void setSlotToneScale(std::size_t slot_idx, float gain_scale, float mix_scale) noexcept {
        if (slot_idx < slot_gain_scale_.size()) {
            slot_gain_scale_[slot_idx] = std::clamp(gain_scale, 0.05f, 4.0f);
            slot_mix_scale_[slot_idx] = std::clamp(mix_scale, 0.0f, 2.0f);
        }
    }

    void setSlotBypassed(std::size_t slot_idx, bool bypassed) {
        if (slot_idx < flat_active_nodes_.size() && flat_active_nodes_[slot_idx]->component) {
            ComponentEdit transaction(*this, *flat_active_nodes_[slot_idx]);
            flat_active_nodes_[slot_idx]->component->setBypassed(bypassed);
            flat_active_nodes_[slot_idx]->component->setParameter("Pwr", bypassed ? 0.0f : 1.0f);
        }
    }

    bool isSlotBypassed(std::size_t slot_idx) const noexcept {
        if (slot_idx < flat_active_nodes_.size() && flat_active_nodes_[slot_idx]->component) {
            return flat_active_nodes_[slot_idx]->component->isBypassed();
        }
        return false;
    }

    void setComponentParameter(std::size_t active_slot_idx, std::string_view param_id, float val) {
        if (active_slot_idx < flat_active_nodes_.size() && flat_active_nodes_[active_slot_idx]->component) {
            ComponentEdit transaction(*this, *flat_active_nodes_[active_slot_idx]);
            flat_active_nodes_[active_slot_idx]->component->setParameter(param_id, val);
            flat_active_nodes_[active_slot_idx]->component->processControlRate();
        }
    }

    // Returns true when the value is continuous (a knob or fader), false for selectors, switches and steps.
    bool setComponentNormalizedParameter(std::size_t active_slot_idx, std::string_view param_id, float norm_val) {
        if (active_slot_idx >= flat_active_nodes_.size() || !flat_active_nodes_[active_slot_idx]->component) return true;
        ComponentEdit transaction(*this, *flat_active_nodes_[active_slot_idx]);
        auto* comp = flat_active_nodes_[active_slot_idx]->component.get();
        norm_val = std::clamp(norm_val, 0.0f, 1.0f);
        float raw_val = norm_val;
        bool continuous = true;
        for (const auto& ep : comp->extraParams()) {
            if (ep.id == param_id && !ep.labels.empty()) {
                raw_val = std::round(norm_val * static_cast<float>(ep.labels.size() - 1));
                continuous = false;
            }
        }
        for (const auto& p : comp->schema().params) {
            if (p.id == param_id) {
                float pmin = 0.0f, pmax = 1.0f;
                getParamRangeFor(comp->tableId(), p, pmin, pmax);
                raw_val = pmin + norm_val * (pmax - pmin);
                if (!isPluginTableId(comp->tableId()) && pmax - pmin > 1.5f && pmax - pmin <= 64.0f && std::floor(pmin) == pmin && std::floor(pmax) == pmax) {
                    raw_val = std::round(raw_val); // discrete selector
                    continuous = false;
                }
                if (p.kind == catalog::ParamKind::Int) {
                    raw_val = std::round(raw_val);
                    continuous = false;
                } else if (p.kind == catalog::ParamKind::Bool) {
                    raw_val = (norm_val >= 0.5f) ? 1.0f : 0.0f;
                    continuous = false;
                }
                break;
            }
        }
        comp->setParameter(param_id, raw_val);
        comp->processControlRate();
        flat_active_nodes_[active_slot_idx]->preset_modified = true;
        dirty_ = true;
        return continuous;
    }

    std::pair<std::string, bool> slotPresetLabel(std::size_t slot) const {
        if (slot >= flat_active_nodes_.size()) return {};
        return {flat_active_nodes_[slot]->preset_label, flat_active_nodes_[slot]->preset_modified};
    }

    // ───────────── Macros ─────────────
    struct MacroTargetView {
        std::size_t slot = 0;
        std::string param;
        float min = 0.0f, max = 1.0f;
        int split = -1; // >= 0 when the target is a split's mix balance
    };
    struct MacroView {
        int col = 0;
        std::string label;
        std::string source; // preset | auto | user | empty
        float value = 0.5f;
        std::vector<MacroTargetView> targets;
    };

    // Returns true when every target moved continuously (see setComponentNormalizedParameter).
    bool setMacroNormalized(int32_t col, float norm_val) {
        if (col < 0 || col >= kMacros) return true;
        bool continuous = true;
        auto& m = macros_[static_cast<std::size_t>(col)];
        struct PreviousTarget { GraphNode* node; std::shared_ptr<IComponent> component; bool modified; };
        std::vector<PreviousTarget> previous;
        previous.reserve(m.targets.size());
        for (const auto& t : m.targets)
            if (t.node) previous.push_back({t.node, t.node->component, t.node->preset_modified});
        const float old_value = m.value;
        const bool old_dirty = dirty_;
        try {
            m.value = std::clamp(norm_val, 0.0f, 1.0f);
            // Native candidates may reject preparation or a physical control
            // range. Validate them before touching shared hosted instances,
            // whose scalar setters intentionally retain native editor identity.
            // Preserve assignment order within each group (including repeats).
            for (bool hosted : {false, true}) {
                for (const auto& t : m.targets) {
                    if (!t.node || !t.node->component || isPluginTableId(t.node->table_id) != hosted) continue;
                    const float v = t.min + m.value * (t.max - t.min);
                    if (isSplitMixNode(t.node->table_id)) {
                        ComponentEdit transaction(*this, *t.node);
                        t.node->component->setParameter(t.param, std::clamp(v, 0.0f, 1.0f));
                    } else if (!setComponentNormalizedParameter(t.node->active_idx, t.param, v)) continuous = false;
                }
            }
        } catch (...) {
            for (auto& t : previous) {
                t.node->component = std::move(t.component);
                t.node->preset_modified = t.modified;
            }
            m.value = old_value;
            dirty_ = old_dirty;
            throw;
        }
        return continuous;
    }

    float getMacroNormalized(int32_t col) const noexcept {
        if (col >= 0 && col < kMacros) return macros_[static_cast<std::size_t>(col)].value;
        return 0.5f;
    }

    std::vector<MacroView> macroViews() const {
        std::vector<MacroView> out;
        for (int c = 0; c < kMacros; ++c) {
            const auto& m = macros_[static_cast<std::size_t>(c)];
            MacroView v;
            v.col = c;
            v.label = m.label;
            v.source = m.targets.empty() ? "empty" : m.source;
            v.value = m.value;
            for (const auto& t : m.targets) {
                if (!t.node) continue;
                if (isSplitMixNode(t.node->table_id)) v.targets.push_back({0, t.param, t.min, t.max, splitIdForMix(t.node)});
                else v.targets.push_back({t.node->active_idx, t.param, t.min, t.max, -1});
            }
            out.push_back(std::move(v));
        }
        return out;
    }

    bool assignMacro(int col, std::size_t slot, const std::string& param, float mn, float mx, const std::string& label, bool add) {
        if (col < 0 || col >= kMacros || slot >= flat_active_nodes_.size()) return false;
        auto& m = macros_[static_cast<std::size_t>(col)];
        if (!add || m.source != "user") m.targets.clear();
        m.targets.push_back({flat_active_nodes_[slot], param, mn, mx});
        m.source = "user";
        if (!label.empty()) m.label = label;
        else if (!add || m.label.empty()) m.label = paramDisplayName(flat_active_nodes_[slot], param);
        m.value = currentMacroValue(m);
        dirty_ = true;
        return true;
    }

    // For freshly built rigs: give the macro strip sensible controls if nothing is assigned yet.
    void autoMacrosIfEmpty() {
        for (const auto& m : macros_) if (!m.targets.empty()) return;
        autoAssignMacros();
        for (auto& m : macros_) m.value = currentMacroValue(m);
    }

    // Assign a macro to a split's A↔B balance.
    bool assignMacroSplit(int col, int uid, float mn, float mx, const std::string& label, bool add) {
        if (col < 0 || col >= kMacros) return false;
        std::vector<std::unique_ptr<GraphNode>>* vec = nullptr;
        std::size_t s = 0, mid = 0, mix = 0;
        if (!findSplit(nodes_, uid, vec, s, mid, mix)) return false;
        auto& m = macros_[static_cast<std::size_t>(col)];
        if (!add || m.source != "user") m.targets.clear();
        m.targets.push_back({(*vec)[mix].get(), "XFade", mn, mx});
        m.source = "user";
        m.label = label.empty() ? "Split Mix" : label;
        m.value = currentMacroValue(m);
        dirty_ = true;
        return true;
    }
    // Split id owning a mix node (for macro views), or -1.
    int splitIdForMix(const GraphNode* mixnode) const {
        std::function<int(const std::vector<std::unique_ptr<GraphNode>>&)> rec = [&](const auto& v) -> int {
            for (std::size_t i = 0; i < v.size(); ++i) {
                if (v[i].get() == mixnode) {
                    int depth = 0;
                    for (std::size_t j = i + 1; j-- > 0;) {
                        const int32_t t = v[j]->table_id;
                        if (isSplitMixNode(t) && j != i) ++depth;
                        else if (isSplitStartNode(t)) { if (depth == 0) return v[j]->uid; --depth; }
                    }
                }
                if (int r = rec(v[i]->children); r >= 0) return r;
            }
            return -1;
        };
        return rec(nodes_);
    }

    void clearMacro(int col) {
        if (col < 0 || col >= kMacros) return;
        macros_[static_cast<std::size_t>(col)] = Macro{};
        dirty_ = true;
    }

    void labelMacro(int col, const std::string& label) {
        if (col < 0 || col >= kMacros) return;
        macros_[static_cast<std::size_t>(col)].label = label;
        dirty_ = true;
    }

    // ───────────── Splits / lanes ─────────────
    // Inserts an empty parallel split (Split A → lane A | Split B → lane B | Split Mix) before the unit
    // at active position `pos` (or at the end). Returns the split id.
    int insertSplit(std::size_t pos, float xfade = 0.5f) {
        auto a = makeRawNode(7000), b = makeRawNode(8000), m = makeRawNode(9000);
        if (!a || !b || !m) return -1;
        m->component->setParameter("XFade", xfade);
        const int uid = a->uid;
        std::vector<std::unique_ptr<GraphNode>>* vec = &nodes_;
        std::size_t at = nodes_.size();
        if (pos < flat_active_nodes_.size()) locate(nodes_, flat_active_nodes_[pos], vec, at);
        vec->insert(vec->begin() + static_cast<std::ptrdiff_t>(at), std::move(m));
        vec->insert(vec->begin() + static_cast<std::ptrdiff_t>(at), std::move(b));
        vec->insert(vec->begin() + static_cast<std::ptrdiff_t>(at), std::move(a));
        collectActiveNodes();
        dirty_ = true;
        return uid;
    }

    // Inserts a unit into lane 'A' or 'B' of split `uid` at `index` within the lane. Returns its slot.
    int insertIntoLane(int uid, char lane, std::size_t index, int32_t table_id, const std::vector<std::pair<std::string, float>>& init) {
        std::vector<std::unique_ptr<GraphNode>>* vec = nullptr;
        std::size_t s = 0, mid = 0, mix = 0;
        if (!findSplit(nodes_, uid, vec, s, mid, mix)) return -1;
        const std::size_t begin = (lane == 'B' || lane == 'b') ? mid + 1 : s + 1;
        const std::size_t end = (lane == 'B' || lane == 'b') ? mix : mid;
        const std::size_t at = begin + std::min(index, end - begin);
        auto node = makeNode(table_id, init);
        if (!node) return -1;
        GraphNode* raw = node.get();
        vec->insert(vec->begin() + static_cast<std::ptrdiff_t>(at), std::move(node));
        collectActiveNodes();
        afterInsert(raw, table_id);
        return static_cast<int>(raw->active_idx);
    }

    bool moveIntoLane(std::size_t slot, int uid, char lane, std::size_t index) {
        if (slot >= flat_active_nodes_.size()) return false;
        GraphNode* target = flat_active_nodes_[slot];
        auto owned = detachNode(nodes_, target);
        if (!owned) return false;
        std::vector<std::unique_ptr<GraphNode>>* vec = nullptr;
        std::size_t s = 0, mid = 0, mix = 0;
        if (!findSplit(nodes_, uid, vec, s, mid, mix)) {
            nodes_.push_back(std::move(owned));
        } else {
            const std::size_t begin = (lane == 'B' || lane == 'b') ? mid + 1 : s + 1;
            const std::size_t end = (lane == 'B' || lane == 'b') ? mix : mid;
            vec->insert(vec->begin() + static_cast<std::ptrdiff_t>(begin + std::min(index, end - begin)), std::move(owned));
        }
        collectActiveNodes();
        dirty_ = true;
        return true;
    }

    // Removes a split. keep = "A" (lane B units deleted), "B", or "both" (lanes concatenated).
    bool removeSplit(int uid, const std::string& keep) {
        std::vector<std::unique_ptr<GraphNode>>* vec = nullptr;
        std::size_t s = 0, mid = 0, mix = 0;
        if (!findSplit(nodes_, uid, vec, s, mid, mix)) return false;
        std::vector<std::unique_ptr<GraphNode>> kept;
        for (std::size_t i = s + 1; i < mid; ++i) if (keep != "B") kept.push_back(std::move((*vec)[i]));
        for (std::size_t i = mid + 1; i < mix; ++i) if (keep != "A") kept.push_back(std::move((*vec)[i]));
        // macros pointing at deleted nodes are dropped below via collect
        std::vector<GraphNode*> alive;
        for (auto& k : kept) alive.push_back(k.get());
        vec->erase(vec->begin() + static_cast<std::ptrdiff_t>(s), vec->begin() + static_cast<std::ptrdiff_t>(mix + 1));
        vec->insert(vec->begin() + static_cast<std::ptrdiff_t>(s), std::make_move_iterator(kept.begin()), std::make_move_iterator(kept.end()));
        const auto all = allNodesFlat();
        for (auto& m : macros_) {
            m.targets.erase(std::remove_if(m.targets.begin(), m.targets.end(), [&](const MacroTarget& t) {
                return std::find(all.begin(), all.end(), t.node) == all.end();
            }), m.targets.end());
        }
        collectActiveNodes();
        dirty_ = true;
        return true;
    }

    bool setSplitMix(int uid, float xfade) {
        std::vector<std::unique_ptr<GraphNode>>* vec = nullptr;
        std::size_t s = 0, mid = 0, mix = 0;
        if (!findSplit(nodes_, uid, vec, s, mid, mix)) return false;
        if ((*vec)[mix]->component) {
            ComponentEdit transaction(*this, *(*vec)[mix]);
            (*vec)[mix]->component->setParameter("XFade", std::clamp(xfade, 0.0f, 1.0f));
        }
        dirty_ = true;
        return true;
    }

    // Signal-flow tree for the UI: units by slot index, splits with their two lanes.
    std::string layoutJson() const {
        std::string out;
        layoutList(nodes_, out);
        return out;
    }

    // ───────────── Rack editing ─────────────
    bool isDirty() const noexcept { return dirty_; }

    // ───────────── Rig tempo ─────────────
    // Model value, saved with the rig. Control thread; a published plan carries it (see process()).
    double tempo() const noexcept { return tempo_bpm_; }
    bool setTempo(double bpm) noexcept {
        const double t = clampTempo(bpm);
        if (std::abs(t - tempo_bpm_) < 1e-6) return false;
        tempo_bpm_ = t;
        dirty_ = true;
        return true;
    }
    void setDirty(bool d) noexcept { dirty_ = d; }
    const std::string& presetSource() const noexcept { return preset_source_; }
    void setPresetSource(std::string s) { preset_source_ = std::move(s); }

    bool addComponentSlot(int32_t table_id) { return insertComponent(table_id, flat_active_nodes_.size(), {}) >= 0; }

    // Inserts a new unit so that it appears at `pos` in the active slot list. Returns its slot index.
    // Amps get a matched cabinet automatically when nothing cab-like follows them; a matched cabinet
    // dropped after an amp picks that amp's cabinet.
    int insertComponent(int32_t table_id, std::size_t pos, const std::vector<std::pair<std::string, float>>& init_params) {
        auto node = makeNode(table_id, init_params);
        if (!node) return -1;
        GraphNode* raw = node.get();
        placeNode(std::move(node), pos);
        collectActiveNodes();
        afterInsert(raw, table_id);
        return static_cast<int>(raw->active_idx);
    }

private:
    // Post-insert conveniences: matched cab after amps, cab follows its amp, and MIDI-driven units get
    // a Pitch → MIDI detector in front of them if their lane has none upstream.
    void afterInsert(GraphNode* raw, int32_t table_id) {
        const bool is_amp = isAmpTable(table_id) ||
                            (table_id == 900005 && raw->component && namCaptureWantsCab(static_cast<int>(std::lround(raw->component->getParameter("capture")))));
        if (table_id == 156000 || table_id == 88000 || table_id == kCabinetUnitTable) autoMatchCab(raw);
        if ((table_id == 900002 || table_id == 900003 || table_id == 900006 || table_id == 900004 || plugins::isInstrumentTable(table_id)) && !detectorUpstream(raw)) {
            std::vector<std::unique_ptr<GraphNode>>* vec = nullptr;
            std::size_t at = 0;
            if (locate(nodes_, raw, vec, at)) {
                auto det = makeNode(900001, {});
                if (det) vec->insert(vec->begin() + static_cast<std::ptrdiff_t>(at), std::move(det));
                collectActiveNodes();
            }
        }
        // An amp whose circuit ends in speaker + cab (docs/CIRCUIT_ENGINE.md §10.21) already outputs a miked cabinet.
        if (is_amp && !(raw->component && raw->component->includesCabinet())) {
            // Look for a cabinet after the amp in the same lane (stop at lane/split boundaries).
            std::vector<std::unique_ptr<GraphNode>>* vec = nullptr;
            std::size_t at = 0;
            if (locate(nodes_, raw, vec, at)) {
                bool cab_follows = false;
                for (std::size_t j = at + 1; j < vec->size(); ++j) {
                    const int32_t t = (*vec)[j]->table_id;
                    if (isSplitStartNode(t) || isSplitMidNode(t) || isSplitMixNode(t) || isAmpTable(t)) break;
                    if (isCabTable(t)) { cab_follows = true; break; }
                }
                if (!cab_follows) {
                    // The Cabinet unit set to the amp's matched cabinet: identical sound to the matched
                    // cabinet, but any cabinet / mic / position can be chosen on it afterwards.
                    auto cab = makeNode(kCabinetUnitTable, {});
                    if (cab) {
                        GraphNode* craw = cab.get();
                        vec->insert(vec->begin() + static_cast<std::ptrdiff_t>(at + 1), std::move(cab));
                        collectActiveNodes();
                        autoMatchCab(craw);
                    }
                }
            }
        }
        dirty_ = true;
    }

public:
    bool moveSlot(std::size_t from, std::size_t to) {
        if (from >= flat_active_nodes_.size()) return false;
        GraphNode* target = flat_active_nodes_[from];
        std::unique_ptr<GraphNode> owned = detachNode(nodes_, target);
        if (!owned) return false;
        collectActiveNodes();
        placeNode(std::move(owned), to);
        collectActiveNodes();
        dirty_ = true;
        return true;
    }

    bool removeActiveSlot(std::size_t active_idx) {
        if (active_idx >= flat_active_nodes_.size()) return false;
        GraphNode* target = flat_active_nodes_[active_idx];
        for (auto& m : macros_) {
            m.targets.erase(std::remove_if(m.targets.begin(), m.targets.end(), [&](const MacroTarget& t) { return t.node == target; }),
                            m.targets.end());
        }
        if (removeNodeFromVec(nodes_, target)) {
            collectActiveNodes();
            dirty_ = true;
            return true;
        }
        return false;
    }

    // Applies a set of raw parameter values (e.g. a factory component preset) to a unit.
    bool applyParams(std::size_t slot, const std::vector<std::pair<std::string, float>>& params, const std::string& label = {}) {
        if (slot >= flat_active_nodes_.size() || !flat_active_nodes_[slot]->component) return false;
        ComponentEdit transaction(*this, *flat_active_nodes_[slot]);
        auto* c = flat_active_nodes_[slot]->component.get();
        for (const auto& kv : params) {
            if (kv.first == "Pwr" || kv.first == "CASSt") continue;
            c->setParameter(kv.first, kv.second);
        }
        c->processControlRate();
        flat_active_nodes_[slot]->preset_label = label;
        flat_active_nodes_[slot]->preset_modified = false;
        dirty_ = true;
        return true;
    }

    // Serialises the live rack (all parameters, nesting, macros) back into a preset structure.
    preset::RackPreset toPreset(const std::string& name, const std::string& author) const {
        preset::RackPreset p;
        p.info.name = name;
        p.info.author = author;
        p.info.instrument = active_preset_.info.instrument; // bass / guitar, for offline levelling (tools/rig_level)
        p.root_rack.table_id = 100;
        p.tempo_bpm = tempo_bpm_;
        int slot = 0;
        for (const auto& n : nodes_) p.root_rack.children.push_back(exportNode(*n, slot++));
        for (int c = 0; c < kMacros; ++c) {
            const auto& m = macros_[static_cast<std::size_t>(c)];
            for (const auto& t : m.targets) {
                if (!t.node) continue;
                preset::MacroTarget mt;
                mt.page = 0;
                mt.column = c;
                mt.control_name = m.label;
                mt.target_table_id = t.node->table_id;
                mt.target_index = instanceOrdinal(t.node);
                mt.target_param_id = t.param;
                mt.min_val = t.min;
                mt.max_val = t.max;
                p.macros.push_back(std::move(mt));
            }
        }
        return p;
    }

    std::vector<ActiveNodeView> activeNodeViews() const {
        std::vector<ActiveNodeView> out;
        out.reserve(flat_active_nodes_.size());
        for (std::size_t i = 0; i < flat_active_nodes_.size(); ++i) {
            auto* n = flat_active_nodes_[i];
            if (n && n->component) {
                out.push_back({i, n->table_id, n->instance_index, n->component.get(), n->uid});
            }
        }
        return out;
    }

    const preset::RackPreset& activePreset() const noexcept { return active_preset_; }

    void process(AudioBlock& block) {
        meters_on_ = unit_meters_ && !calibrating_ && unit_meters_->beginBlock(block.num_frames, sample_rate_) && !master_bypass_;
        diag_on_ = diag_ && !calibrating_ && !block.offline;
        if (diag_on_) diag_->beginGraph();
        // Input gain glides to its target (~10 ms) so IN knob moves never step the level. Calibration renders and a
        // fresh plan start on the target.
        if (input_gain_cur_ < 0.0f || calibrating_) input_gain_cur_ = input_gain_;
        const float in_glide = 1.0f - std::exp(-1.0f / (0.010f * static_cast<float>(sample_rate_)));
        const float in_g0 = input_gain_cur_;
        auto inGain = [&]() noexcept {
            input_gain_cur_ += (input_gain_ - input_gain_cur_) * in_glide;
            return input_gain_cur_;
        };
        if (nodes_.empty() || master_bypass_) {
            for (uint32_t i = 0; i < block.num_frames; ++i) {
                const float g = inGain();
                for (uint32_t c = 0; c < block.num_channels; ++c) {
                    const float* src = block.in[c] ? block.in[c] : block.in[0];
                    if (src && block.out[c]) block.out[c][i] = src[i] * g;
                }
            }
            return;
        }

        buf_a_.copyFrom(block.in, block.num_channels, block.num_frames);
        if (std::abs(input_gain_ - 1.0f) > 1e-4f || std::abs(in_g0 - 1.0f) > 1e-4f) {
            for (uint32_t i = 0; i < block.num_frames; ++i) {
                const float g = inGain();
                buf_a_.left[i] *= g;
                buf_a_.right[i] *= g;
            }
        }
        if (std::abs(input_gain_cur_ - input_gain_) < 1e-6f) input_gain_cur_ = input_gain_;

        midi_main_.clear();
        AudioBlock ctx = block;
        ctx.midi = &midi_main_;
        ctx.offline = block.offline || calibrating_; // calibration renders stay off the real-time pool
        // One tempo for the whole rig, read once per block (lock-free, no allocation): the host's when
        // the caller or the host clock provides it, otherwise the rig's own.
        if (!block.host_tempo) {
            const double host = hostTempo().bpm();
            ctx.host_tempo = host > 0.0;
            ctx.bpm = host > 0.0 ? host : tempo_bpm_;
        } else {
            ctx.bpm = clampTempo(block.bpm);
        }
        processNodeList(nodes_, buf_a_, buf_b_, ctx, true, sameChannels(buf_a_, block.num_frames));

        // Master section: a transparent peak limiter (no loudness gain: the rig plays at its units' own levels)
        // (fast attack, program-dependent release) and a soft safety ceiling at -0.5 dBFS.
        if (!calibrating_) {
            const float sr = static_cast<float>(sample_rate_);
            const float att = std::exp(-1.0f / (0.0008f * sr));
            const float rel = std::exp(-1.0f / (0.120f * sr));
            constexpr float kCeil = 0.891f; // -1 dBFS
            EngineDiagBank::MasterVals mv; // /api/diag: what the master section did to this block
            for (uint32_t i = 0; i < block.num_frames; ++i) {
                const float rl = buf_a_.left[i], rr = buf_a_.right[i];
                float l = rl;
                float r = rr;
                const float pk = std::max(std::abs(l), std::abs(r));
                const float over = pk > kCeil ? (pk / kCeil) : 1.0f;
                limiter_env_ = (over > limiter_env_) ? (att * limiter_env_ + (1.0f - att) * over)
                                                     : (rel * limiter_env_ + (1.0f - rel) * over);
                const float g = 1.0f / std::max(1.0f, limiter_env_);
                l *= g;
                r *= g;
                if (diag_on_) {
                    if (EngineDiagBank::nonFinite(rl) || EngineDiagBank::nonFinite(rr)) ++mv.nonfinite;
                    else {
                        mv.rig_peak = std::max(mv.rig_peak, std::max(std::abs(rl), std::abs(rr)));
                        mv.post_gain_peak = std::max(mv.post_gain_peak, pk);
                        mv.rig_sumsq += 0.5 * (double(rl) * rl + double(rr) * rr);
                    }
                    if (limiter_env_ > 1.0001f) ++mv.limiter;
                    mv.limiter_max_env = std::max(mv.limiter_max_env, limiter_env_);
                    if (std::abs(l) > 0.85f || std::abs(r) > 0.85f) ++mv.soft;
                }
                buf_a_.left[i] = softCeiling(l);
                buf_a_.right[i] = softCeiling(r);
            }
            if (diag_on_) {
                mv.samples = block.num_frames;
                diag_->master(mv, 1.0f);
            }
        }

        for (uint32_t c = 0; c < block.num_channels; ++c) {
            if (block.out[c]) {
                const float* src = (c == 0) ? buf_a_.left.data() : buf_a_.right.data();
                std::memcpy(block.out[c], src, sizeof(float) * block.num_frames);
            }
        }
    }

    std::size_t topLevelSlotCount() const noexcept { return nodes_.size(); }

    std::size_t totalComponentCount() const noexcept {
        std::size_t count = 0;
        for (const auto& n : nodes_) count += countNodes(*n);
        return count;
    }

private:
    static constexpr float kTargetLoudnessDb = -17.5f;

    static float softCeiling(float v) noexcept {
        const float a = std::abs(v);
        if (a <= 0.85f) return v;
        const float x = (a - 0.85f) / 0.12f;
        return std::copysign(0.85f + 0.12f * (x / (1.0f + x)), v);
    }

    // A CrossOver split's band split at the split's input: Linkwitz-Riley 4th order (two cascaded 2nd-order Butterworth
    // sections per band, bilinear with prewarping), so the two bands are in phase and their sum is an allpass (flat
    // magnitude). The low band feeds lane A ("Pan Low"), the high band lane B ("Pan High"). Audio thread; double state.
    struct CrossoverLR4 {
        float f = -1.0f;                        // the band edge now ("f", gliding to the knob; < 0: not started)
        float designed_f = -1.0f;
        double designed_sr = 0.0;
        double lp[5]{}, hp[5]{};                // b0 b1 b2 a1 a2
        double z[2][4][2]{};                    // [channel][low 1, low 2, high 1, high 2][transposed direct form II state]
        // Once per host block, before the block renders (the same in sequential and pipelined rendering).
        void update(float target, double sr, uint32_t frames) noexcept {
            target = std::clamp(target, 0.0f, 1.0f);
            if (f < 0.0f) {
                f = target;
            } else if (f != target) { // ~30 ms glide, one step per host block, so turning "f" never steps the filters
                f += (target - f) * (1.0f - std::exp(-static_cast<float>(frames) / (0.03f * static_cast<float>(sr))));
                if (std::abs(target - f) < 1e-4f) f = target;
            }
            if (f == designed_f && sr == designed_sr) return;
            designed_f = f, designed_sr = sr;
            const double hz = std::min(crossoverHz(f), 0.45 * sr);
            const double w0 = 2.0 * 3.14159265358979323846 * hz / sr, c = std::cos(w0), alpha = std::sin(w0) * 0.70710678118654752440;
            const double a0 = 1.0 + alpha;
            lp[0] = 0.5 * (1.0 - c) / a0, lp[1] = (1.0 - c) / a0, lp[2] = lp[0];
            hp[0] = 0.5 * (1.0 + c) / a0, hp[1] = -(1.0 + c) / a0, hp[2] = hp[0];
            lp[3] = hp[3] = -2.0 * c / a0;
            lp[4] = hp[4] = (1.0 - alpha) / a0;
        }
        static double section(double x, const double* k, double* zz) noexcept {
            const double y = k[0] * x + zz[0];
            zz[0] = k[1] * x - k[3] * y + zz[1];
            zz[1] = k[2] * x - k[4] * y;
            return y;
        }
        // n frames of the split's input into the two bands, in sample order (lo may be the input's own buffer).
        void process(const float* in_l, const float* in_r, float* lo_l, float* lo_r, float* hi_l, float* hi_r, uint32_t n) noexcept {
            for (uint32_t i = 0; i < n; ++i) {
                const double x[2] = {in_l[i], in_r[i]};
                float lo[2], hi[2];
                for (int ch = 0; ch < 2; ++ch) {
                    lo[ch] = static_cast<float>(section(section(x[ch], lp, z[ch][0]), lp, z[ch][1]));
                    hi[ch] = static_cast<float>(section(section(x[ch], hp, z[ch][2]), hp, z[ch][3]));
                }
                lo_l[i] = lo[0], lo_r[i] = lo[1], hi_l[i] = hi[0], hi_r[i] = hi[1];
            }
        }
    };
    struct GraphNode {
        int32_t table_id = 0;
        int32_t instance_index = 0;
        std::size_t active_idx = 0;
        std::shared_ptr<IComponent> component;
        std::vector<std::unique_ptr<GraphNode>> children;
        StereoScratchBuffer split_buf_a{};
        StereoScratchBuffer split_buf_b{};
        StereoScratchBuffer split_scratch_b{}; // lane B's own scratch (lane B may render on another core)
        float split_xfade_cur = -1.0f; // split start, audio thread: smoothed A/B mix (< 0 = not started)
        CrossoverLR4 xover{};          // CrossOver split start, audio thread: the band split (inherited across edits)
        std::vector<midi::MidiEvent> midi_a, midi_b;
        int uid = 0;
        std::string preset_label;   // last applied component preset
        bool preset_modified = false;
        bool cab_muted_by_amp = false; // cabinet bypassed by syncCabFollow because the amp before it includes its own cab
        double tap_in_e = 0.0, tap_out_e = 0.0;
        float tap_in_pk = 0.0f, tap_out_pk = 0.0f;
        std::size_t tap_n = 0;
        std::shared_ptr<void> pipe_a, pipe_b; // split start: each lane's series pipeline (PipeCtx; allocated on the control thread)
        UnitMeterBank::Acc macc{};  // series pipelining: this block's meter sums across sub-blocks
        double cost_us = 0.0;        // series pipelining: smoothed render cost per block (stage balancing only)
        double cost_acc_us = 0.0;
        bool mono_in = false;  // this block: the input is proved one signal on both channels (AudioBlock::mono_input)
        bool mono_out = false; // a container, this block: its children's output was (set while it renders)
        struct DiagAcc { uint64_t ns = 0; uint32_t frames = 0, nonfinite = 0; float peak = 0.0f; double sumsq = 0.0; } dacc{}; // this block, for /api/diag
    };


    void indexRenderNode(GraphNode& node) {
        render_node_index_.emplace(node.uid, &node);
        for (auto& c : node.children) indexRenderNode(*c);
    }

    static bool isCrossoverSplit(const GraphNode& split_node) noexcept { return split_node.table_id == 50000; }
    static float crossoverParam(const GraphNode& mix_node) noexcept {
        return mix_node.component ? mix_node.component->getParameter("f") : 0.5f;
    }

    static void inheritNodeState(GraphNode& node, const std::unordered_map<int, GraphNode*>& previous) noexcept {
        const auto found = previous.find(node.uid);
        auto* old = found == previous.end() ? nullptr : found->second;
        if (old && old->table_id == node.table_id) node.split_xfade_cur = old->split_xfade_cur, node.xover = old->xover;
        if (old && old->table_id == node.table_id && node.component && old->component &&
            node.component.get() != old->component.get())
            node.component->adoptRuntimeStateFrom(*old->component);
        for (auto& c : node.children) inheritNodeState(*c, previous);
    }

    static std::unique_ptr<GraphNode> snapshotNode(const GraphNode& n) {
        auto copy = std::make_unique<GraphNode>();
        copy->table_id = n.table_id;
        copy->instance_index = n.instance_index;
        copy->active_idx = n.active_idx;
        copy->component = n.component;
        copy->uid = n.uid;
        copy->preset_label = n.preset_label;
        copy->preset_modified = n.preset_modified;
        copy->cab_muted_by_amp = n.cab_muted_by_amp;
        if (isSplitStartNode(n.table_id)) {
            copy->midi_a.reserve(2048);
            copy->midi_b.reserve(2048);
        }
        for (const auto& c : n.children) copy->children.push_back(snapshotNode(*c));
        return copy;
    }

    class ComponentEdit {
    public:
        ComponentEdit(RackGraph& owner, GraphNode& node)
            : owner_(owner), node_(node), previous_(node.component),
              label_(node.preset_label), modified_(node.preset_modified),
              dirty_(owner.dirty_), exceptions_(std::uncaught_exceptions()) {
            // previous_ itself forces a detached candidate even when the model
            // has never been published. Preparation commits no partial candidate.
            owner_.detachForEdit(node_);
        }
        ~ComponentEdit() {
            if (std::uncaught_exceptions() > exceptions_) cancel();
        }
        void cancel() noexcept {
            node_.component = std::move(previous_);
            node_.preset_label.swap(label_);
            node_.preset_modified = modified_;
            owner_.dirty_ = dirty_;
        }
    private:
        RackGraph& owner_;
        GraphNode& node_;
        std::shared_ptr<IComponent> previous_;
        std::string label_;
        bool modified_, dirty_;
        int exceptions_;
    };

    // cab_override >= -1 gives the copy that cab-follow state instead of the original's (syncCabFollow).
    void detachForEdit(GraphNode& n, int cab_override = -2) {
        if (!n.component || n.component.use_count() == 1 || isPluginTableId(n.table_id)) return;
        auto replacement = createComponentByTableId(n.table_id);
        if (!replacement) throw std::runtime_error("Unable to prepare rack component");
        replacement->setCabFollows(cab_override >= -1 ? cab_override : n.component->cabFollows()); // before prepare
        tl_prepareForAdoption = true;
        replacement->prepare(sample_rate_, max_block_size_);
        tl_prepareForAdoption = false;
        for (const auto& p : n.component->exportParams()) {
            if (p.is_string) replacement->setStringParameter(p.id, p.str);
            else replacement->setParameter(p.id, p.num);
        }
        replacement->setBypassed(n.component->isBypassed());
        replacement->processControlRate();
        n.component = std::move(replacement);
    }

    void dumpTapsRec(const GraphNode& n, int depth) const {
        if (n.tap_n > 0 && n.component) {
            auto db = [](double e, std::size_t c) { return 10.0 * std::log10(std::max(1e-12, e / static_cast<double>(c))); };
            const double in_rms = db(n.tap_in_e, n.tap_n), out_rms = db(n.tap_out_e, n.tap_n);
            std::fprintf(stderr, "%*s[%6d] %-34s in %6.1f dB (crest %5.1f)  out %6.1f dB (crest %5.1f)%s\n", depth * 2, "",
                         n.table_id, std::string(n.component->displayName()).substr(0, 34).c_str(),
                         in_rms, 20.0 * std::log10(std::max(1e-9f, n.tap_in_pk)) - in_rms,
                         out_rms, 20.0 * std::log10(std::max(1e-9f, n.tap_out_pk)) - out_rms,
                         n.component->isBypassed() ? "  (bypassed)" : "");
        }
        for (const auto& c : n.children) dumpTapsRec(*c, depth + 1);
    }

    static bool removeNodeFromVec(std::vector<std::unique_ptr<GraphNode>>& vec, GraphNode* target) {
        for (auto it = vec.begin(); it != vec.end(); ++it) {
            if (it->get() == target) {
                vec.erase(it);
                return true;
            }
            if (removeNodeFromVec((*it)->children, target)) {
                return true;
            }
        }
        return false;
    }

    void collectActiveNodesRec(GraphNode& n) {
        if (!isUtilityOrModulatorNode(n.table_id) && n.component) {
            // Slot indices are installed by the audio owner just before rendering.
            // A control-side reorder can share this component with a running plan.
            n.active_idx = flat_active_nodes_.size();
            flat_active_nodes_.push_back(&n);
        }
        for (auto& c : n.children) {
            collectActiveNodesRec(*c);
        }
    }

    void collectActiveNodes() {
        allocLanePipes(nodes_);
        flat_active_nodes_.clear();
        for (auto& n : nodes_) {
            collectActiveNodesRec(*n);
        }
        slot_gain_scale_.assign(std::max<std::size_t>(16, flat_active_nodes_.size()), 1.0f);
        slot_mix_scale_.assign(std::max<std::size_t>(16, flat_active_nodes_.size()), 1.0f);
    }

    int next_uid_ = 1;

    std::unique_ptr<GraphNode> makeRawNode(int32_t table_id) {
        auto g = std::make_unique<GraphNode>();
        g->table_id = table_id;
        g->uid = next_uid_++;
        g->component = createComponentByTableId(table_id);
        if (!g->component) return nullptr;
        g->component->prepare(sample_rate_, max_block_size_);
        g->component->setParameter("Pwr", 1.0f);
        g->component->processControlRate();
        return g;
    }

    static bool locate(std::vector<std::unique_ptr<GraphNode>>& vec, GraphNode* target, std::vector<std::unique_ptr<GraphNode>>*& out_vec, std::size_t& out_idx) {
        for (std::size_t i = 0; i < vec.size(); ++i) {
            if (vec[i].get() == target) {
                out_vec = &vec;
                out_idx = i;
                return true;
            }
            if (locate(vec[i]->children, target, out_vec, out_idx)) return true;
        }
        return false;
    }

    // Finds split start `uid` and the indices of its matching B and Mix markers in the same list.
    static bool findSplit(std::vector<std::unique_ptr<GraphNode>>& vec, int uid, std::vector<std::unique_ptr<GraphNode>>*& out_vec,
                          std::size_t& s, std::size_t& mid, std::size_t& mix) {
        for (std::size_t i = 0; i < vec.size(); ++i) {
            if (vec[i]->uid == uid && isSplitStartNode(vec[i]->table_id)) {
                int depth = 1;
                std::size_t m = vec.size(), x = vec.size();
                for (std::size_t j = i + 1; j < vec.size(); ++j) {
                    const int32_t t = vec[j]->table_id;
                    if (isSplitStartNode(t)) ++depth;
                    else if (isSplitMidNode(t) && depth == 1) m = j;
                    else if (isSplitMixNode(t) && --depth == 0) { x = j; break; }
                }
                if (m < x && x < vec.size()) {
                    out_vec = &vec;
                    s = i;
                    mid = m;
                    mix = x;
                    return true;
                }
            }
            if (findSplit(vec[i]->children, uid, out_vec, s, mid, mix)) return true;
        }
        return false;
    }

    // Is there a Pitch → MIDI unit feeding this node's MIDI bus (same lane, or upstream of its split)?
    bool detectorUpstream(GraphNode* node) {
        std::vector<std::unique_ptr<GraphNode>>* vec = nullptr;
        std::size_t idx = 0;
        if (!locate(nodes_, node, vec, idx)) return false;
        auto findStartBack = [&](std::size_t from) -> std::size_t { // from a Split B/Mix marker back to its start
            int depth = 0;
            for (std::size_t j = from + 1; j-- > 0;) {
                const int32_t t = (*vec)[j]->table_id;
                if (isSplitMixNode(t) && j != from) ++depth;
                else if (isSplitStartNode(t)) {
                    if (depth == 0) return j;
                    --depth;
                }
            }
            return 0;
        };
        for (std::size_t j = idx; j-- > 0;) {
            const int32_t t = (*vec)[j]->table_id;
            if (t == 900001) return true;
            if (isSplitMidNode(t) || isSplitMixNode(t)) j = findStartBack(j);
        }
        return false;
    }

    void layoutList(const std::vector<std::unique_ptr<GraphNode>>& list, std::string& out) const {
        out += "[";
        bool first = true;
        auto unit = [&](const GraphNode& n) {
            if (isUtilityOrModulatorNode(n.table_id) || !n.component) return;
            if (!first) out += ",";
            first = false;
            out += "{\"type\":\"unit\",\"slot\":" + std::to_string(n.active_idx) + "}";
        };
        for (std::size_t i = 0; i < list.size(); ++i) {
            const auto& n = *list[i];
            if (isSplitStartNode(n.table_id)) {
                int depth = 1;
                std::size_t mid = list.size(), mix = list.size();
                for (std::size_t j = i + 1; j < list.size(); ++j) {
                    const int32_t t = list[j]->table_id;
                    if (isSplitStartNode(t)) ++depth;
                    else if (isSplitMidNode(t) && depth == 1) mid = j;
                    else if (isSplitMixNode(t) && --depth == 0) { mix = j; break; }
                }
                if (mid < mix && mix < list.size()) {
                    if (!first) out += ",";
                    first = false;
                    float xf = 0.5f;
                    if (list[mix]->component) xf = list[mix]->component->getParameter("XFade");
                    std::vector<std::unique_ptr<GraphNode>> dummy;
                    out += "{\"type\":\"split\",\"id\":" + std::to_string(n.uid) + ",\"kind\":\"" +
                           (n.table_id == 50000 ? std::string("crossover") : (n.table_id == 135000 ? std::string("ms") : std::string("parallel"))) +
                           "\",\"xfade\":" + std::to_string(xf) + ",\"lanes\":[";
                    auto lane = [&](std::size_t a, std::size_t b) {
                        std::string sub = "[";
                        bool f2 = true;
                        for (std::size_t k = a; k < b; ++k) {
                            const auto& c = *list[k];
                            if (isUtilityOrModulatorNode(c.table_id) || !c.component) continue;
                            if (!f2) sub += ",";
                            f2 = false;
                            sub += "{\"type\":\"unit\",\"slot\":" + std::to_string(c.active_idx) + "}";
                        }
                        return sub + "]";
                    };
                    out += lane(i + 1, mid) + "," + lane(mid + 1, mix) + "]}";
                    i = mix;
                    continue;
                }
            }
            if (!n.children.empty() && isContainerOrSplitNode(n.table_id)) {
                for (const auto& c : n.children) unit(*c);
                continue;
            }
            unit(n);
        }
        out += "]";
    }

    static constexpr int kMacros = 8;
    struct MacroTarget {
        GraphNode* node = nullptr;
        std::string param;
        float min = 0.0f, max = 1.0f;
    };
    struct Macro {
        std::string label;
        std::string source = "empty";
        float value = 0.5f;
        std::vector<MacroTarget> targets;
    };

public: // also used by the server's /api/rack/build
    static bool isAmpTable(int32_t t) noexcept {
        switch (t) {
            case 16000: case 30000: case 34000: case 38000: case 49000: case 54000: case 56000: case 62000:
            case 79000: case 80000: case 81000: case 87000: case 91000: case 92000: case 96000: case 101000:
            case 122000: case 134000: case 150000: case 151000: case 152000: case 162000: case 163000:
            case 172000: case 176000: case 177000: case 300000: return true;
            default: return false;
        }
    }
    static bool isCabTable(int32_t t) noexcept {
        return t == 156000 || t == 88000 || t == 119000 || t == 100000 || t == 168000 || t == 10000 || t == 11000 || t == kCabinetUnitTable;
    }
private:
    static int matchedCabIndexFor(int32_t amp) noexcept {
        switch (amp) {
            case 38000: return 0; case 54000: return 1; case 80000: return 2; case 92000: return 3;
            case 16000: return 4; case 87000: return 5; case 91000: return 6; case 122000: return 7;
            case 49000: return 8; case 101000: return 9; case 56000: return 10; case 34000: return 11;
            case 96000: return 12; case 30000: return 13; case 79000: return 14; case 62000: return 15;
            case 81000: return 16; case 134000: return 17; case 152000: return 18; case 150000: return 19;
            case 151000: return 20; case 162000: case 300000: return 21; case 163000: return 22;
            case 176000: return 23; case 172000: return 24; case 177000: return 25;
            case 900005: return 10; // NAM amp-head capture → a classic 4x12
            default: return -1;
        }
    }

    std::unique_ptr<GraphNode> makeNode(int32_t table_id, const std::vector<std::pair<std::string, float>>& init) {
        auto gnode = std::make_unique<GraphNode>();
        gnode->table_id = table_id;
        gnode->instance_index = 0;
        gnode->uid = next_uid_++;
        gnode->component = createComponentByTableId(table_id);
        if (!gnode->component) return nullptr;
        gnode->component->prepare(sample_rate_, max_block_size_);
        for (const auto& kv : init) {
            if (kv.first == "Pwr" || kv.first == "CASSt") continue;
            gnode->component->setParameter(kv.first, kv.second);
        }
        gnode->component->setParameter("Pwr", 1.0f);
        gnode->component->processControlRate();
        return gnode;
    }

    // Inserts before the unit currently at active position `pos` (inside the same branch), or appends.
    void placeNode(std::unique_ptr<GraphNode> node, std::size_t pos) {
        if (pos < flat_active_nodes_.size()) {
            GraphNode* anchor = flat_active_nodes_[pos];
            if (insertBefore(nodes_, anchor, node)) return;
        }
        nodes_.push_back(std::move(node));
    }

    static bool insertBefore(std::vector<std::unique_ptr<GraphNode>>& vec, GraphNode* anchor, std::unique_ptr<GraphNode>& node) {
        for (auto it = vec.begin(); it != vec.end(); ++it) {
            if (it->get() == anchor) {
                vec.insert(it, std::move(node));
                return true;
            }
            if (insertBefore((*it)->children, anchor, node)) return true;
        }
        return false;
    }

    static std::unique_ptr<GraphNode> detachNode(std::vector<std::unique_ptr<GraphNode>>& vec, GraphNode* target) {
        for (auto it = vec.begin(); it != vec.end(); ++it) {
            if (it->get() == target) {
                std::unique_ptr<GraphNode> out = std::move(*it);
                vec.erase(it);
                return out;
            }
            if (auto r = detachNode((*it)->children, target)) return r;
        }
        return nullptr;
    }

    void autoMatchCab(GraphNode* cab) {
        if (!cab || !cab->component) return;
        for (std::size_t i = cab->active_idx; i-- > 0;) {
            const int idx = matchedCabIndexFor(flat_active_nodes_[i]->table_id);
            if (idx >= 0) {
                if (cab->table_id == 156000) {
                    detachForEdit(*cab);
                    cab->component->setParameter("Cab", static_cast<float>(idx));
                    cab->component->processControlRate();
                } else if (cab->table_id == kCabinetUnitTable) {
                    detachForEdit(*cab);
                    cabinet::selectMatched(*cab->component, idx);
                    cab->component->processControlRate();
                }
                return;
            }
        }
    }

    int instanceOrdinal(const GraphNode* node) const {
        int k = 0;
        for (auto* n : allNodesFlat()) {
            if (n->table_id == node->table_id) {
                ++k;
                if (n == node) return k;
            }
        }
        return 1;
    }

    std::vector<GraphNode*> allNodesFlat() const {
        std::vector<GraphNode*> out;
        std::function<void(const std::vector<std::unique_ptr<GraphNode>>&)> rec = [&](const auto& v) {
            for (const auto& n : v) {
                out.push_back(n.get());
                rec(n->children);
            }
        };
        rec(nodes_);
        return out;
    }

    preset::PresetComponentNode exportNode(const GraphNode& n, int slot) const {
        preset::PresetComponentNode out;
        out.slot_index = slot;
        out.table_id = n.table_id;
        out.instance_index = slot;
        if (n.component) {
            for (const auto& e : n.component->exportParams()) {
                preset::PresetParam pp;
                pp.id = e.id;
                if (e.is_string) {
                    pp.type = "string";
                    pp.str_val = e.str;
                } else {
                    pp.type = "float";
                    pp.float_val = e.num;
                }
                out.params.push_back(std::move(pp));
            }
        }
        int ci = 0;
        for (const auto& c : n.children) out.children.push_back(exportNode(*c, ci++));
        return out;
    }

    static std::string paramDisplayName(const GraphNode* node, const std::string& param) {
        if (node && node->component) {
            for (const auto& p : node->component->schema().params) {
                if (p.id == param) return std::string(p.display_name.empty() ? p.id : p.display_name);
            }
        }
        return param;
    }

    float currentMacroValue(const Macro& m) const {
        if (m.targets.empty() || !m.targets[0].node || !m.targets[0].node->component) return 0.5f;
        const auto& t = m.targets[0];
        float pmin = 0.0f, pmax = 1.0f;
        for (const auto& p : t.node->component->schema().params) {
            if (p.id == t.param) {
                getParamRangeFor(t.node->table_id, p, pmin, pmax);
                break;
            }
        }
        const float norm = std::clamp((t.node->component->getParameter(t.param) - pmin) / std::max(1e-6f, pmax - pmin), 0.0f, 1.0f);
        const float span = t.max - t.min;
        return std::abs(span) > 1e-6f ? std::clamp((norm - t.min) / span, 0.0f, 1.0f) : norm;
    }

    // Macros from the preset's macro page; otherwise auto-pick the controls a player reaches for.
    void initMacros() {
        for (auto& m : macros_) m = Macro{};
        bool any = false;
        const auto all = allNodesFlat();
        for (const auto& pm : active_preset_.macros) {
            if (pm.page != 0 || pm.column < 0 || pm.column >= kMacros) continue;
            int k = 0;
            GraphNode* hit = nullptr;
            for (auto* n : all) {
                if (n->table_id == pm.target_table_id && ++k == std::max(1, pm.target_index)) { hit = n; break; }
            }
            if (!hit) {
                for (auto* n : all) if (n->table_id == pm.target_table_id) { hit = n; break; }
            }
            if (!hit || !hit->component) continue;
            auto& m = macros_[static_cast<std::size_t>(pm.column)];
            m.targets.push_back({hit, pm.target_param_id, pm.min_val, pm.max_val});
            m.source = "preset";
            if (m.label.empty()) m.label = pm.control_name.empty() ? paramDisplayName(hit, pm.target_param_id) : pm.control_name;
            any = true;
        }
        if (!any) autoAssignMacros();
        for (auto& m : macros_) m.value = currentMacroValue(m);
    }

    void autoAssignMacros() {
        struct Pick { GraphNode* node; std::string param; };
        std::vector<Pick> picks;
        auto findParam = [](GraphNode* n, std::initializer_list<std::string_view> names) -> std::string {
            if (!n->component) return {};
            for (auto want : names) {
                for (const auto& p : n->component->schema().params) {
                    std::string dn(p.display_name);
                    if (dn.size() == want.size() && std::equal(dn.begin(), dn.end(), want.begin(),
                            [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); }))
                        return std::string(p.id);
                }
            }
            return {};
        };
        auto add = [&](GraphNode* n, const std::string& id) {
            if (id.empty() || picks.size() >= static_cast<std::size_t>(kMacros)) return;
            for (const auto& p : picks) if (p.node == n && p.param == id) return;
            picks.push_back({n, id});
        };
        GraphNode* amp = nullptr;
        std::vector<GraphNode*> drives, spaces;
        for (auto* n : flat_active_nodes_) {
            if (!n->component || n->component->isBypassed()) continue;
            if (!amp && isAmpTable(n->table_id)) amp = n;
            const auto& nm = n->component->schema().display_name;
            const std::string ref(n->component->schema().reference_name);
            if (isDriveTable(n->table_id)) drives.push_back(n);
            else if (ref.find("Verb") != std::string::npos || ref.find("Lexicon") != std::string::npos || ref.find("EMT") != std::string::npos ||
                     ref.find("Cosmic") != std::string::npos || ref.find("Delay") != std::string::npos || ref.find("Memory Man") != std::string::npos ||
                     ref.find("Echo") != std::string::npos || ref.find("Reverb") != std::string::npos)
                spaces.push_back(n);
            (void)nm;
        }
        if (amp) add(amp, findParam(amp, {"Gain", "Preamp", "Lead", "Overdrive", "Volume I", "Volume", "N Volume", "Normal", "Bright Volume"}));
        for (auto* d : drives) add(d, findParam(d, {"Drive", "Overdrive", "Distortion", "Gain", "Fuzz", "Sustain", "Boost", "Harmonics"}));
        if (amp) {
            add(amp, findParam(amp, {"Bass", "Low"}));
            add(amp, findParam(amp, {"Mid", "Middle", "Midrange"}));
            add(amp, findParam(amp, {"Treble", "Tone"}));
        }
        for (auto* sp : spaces) add(sp, findParam(sp, {"Mix", "Wet", "Reverb", "Dry/Wet", "Level", "Intensity"}));
        if (amp) {
            add(amp, findParam(amp, {"Presence", "Tone Cut", "Cut"}));
            add(amp, findParam(amp, {"Master", "Post Gain", "Volume II", "Warm Volume"}));
        }
        for (std::size_t i = 0; i < picks.size(); ++i) {
            auto& m = macros_[i];
            m.targets.push_back({picks[i].node, picks[i].param, 0.0f, 1.0f});
            m.source = "auto";
            m.label = paramDisplayName(picks[i].node, picks[i].param);
        }
    }

    static bool isDriveTable(int32_t t) noexcept {
        switch (t) {
            case 15000: case 167000: case 40000: case 65000: case 17000: case 42000: case 41000: case 166000:
            case 169000: case 43000: case 174000: case 44000: case 46000: case 45000: case 170000: case 84000:
            case 111000: case 165000: case 173000: case 179000: case 180000: return true;
            default: return false;
        }
    }

    // A unit whose prepare + parameters buildFromPreset runs on a worker thread (setParallelPrepare).
    struct DeferredPrepare {
        IComponent* component;
        const preset::PresetComponentNode* pnode;
        int cab = -2; // >= -1: an amp syncCabFollow would re-prepare with this cab-follow state (planDeferredCabFollow)
    };

    std::unique_ptr<GraphNode> buildNode(const preset::PresetComponentNode& pnode, std::vector<DeferredPrepare>* deferred = nullptr) {
        auto gnode = std::make_unique<GraphNode>();
        gnode->table_id = pnode.table_id;
        gnode->instance_index = pnode.instance_index;
        gnode->uid = next_uid_++;
        // A spare (PreparedUnitPool) is a unit already prepared at this rate and block size: only its parameters remain.
        if (auto spare = use_spares_ ? PreparedUnitPool::instance().take(pnode.table_id, sample_rate_, max_block_size_) : nullptr) {
            gnode->component = std::move(spare);
            ++spares_used_;
            applyPresetParams(*gnode->component, pnode);
        } else if ((gnode->component = createComponentByTableId(pnode.table_id))) {
            if (deferred && gnode->component->preparesIndependently()) deferred->push_back({gnode->component.get(), &pnode});
            else if (!cancel_ || !cancel_()) prepareWithParams(*gnode->component, pnode);
        }
        for (const auto& c : pnode.children) {
            if (auto ch = buildNode(c, deferred)) {
                gnode->children.push_back(std::move(ch));
            }
        }
        return gnode;
    }

    void prepareWithParams(IComponent& component, const preset::PresetComponentNode& pnode) {
        component.prepare(sample_rate_, max_block_size_);
        applyPresetParams(component, pnode);
    }
    static void applyPresetParams(IComponent& component, const preset::PresetComponentNode& pnode) {
        for (const auto& p : pnode.params) {
            if (p.type == "string" || p.type == "filename") {
                component.setStringParameter(p.id, p.str_val);
            } else {
                component.setParameter(p.id, p.float_val);
            }
        }
        component.processControlRate();
    }

    // An amp whose quality follows the cabinet after it (circuit amps, CircuitUnit::cabFollowNeedsPrepare) is prepared
    // by buildNode at the default and then again by syncCabFollow with the preset's parameters and the cab-follow
    // state: the first settle is thrown away. For a deferred amp syncCabFollow would re-prepare, the job instead sets
    // the parameters and the cab-follow state first and prepares once, ending in the state the re-prepare gives (then
    // syncCabFollow finds it unchanged). The cab-follow state is found by syncCabFollowList's rule; the units it reads
    // after the amp (cabinets) are built and prepared already, deferred ones (circuit units) never have a speaker
    // profile. Presets with string parameters (a circuit model) keep the two prepares.
    void planDeferredCabFollow(std::vector<std::unique_ptr<GraphNode>>& list, std::vector<DeferredPrepare>& jobs) {
        if (jobs.empty()) return;
        for (std::size_t i = 0; i < list.size(); ++i) {
            GraphNode& n = *list[i];
            if (!n.children.empty()) planDeferredCabFollow(n.children, jobs);
            if (!isAmpTable(n.table_id) || !n.component) continue;
            auto job = std::find_if(jobs.begin(), jobs.end(), [&](const DeferredPrepare& d) { return d.component == n.component.get(); });
            if (job == jobs.end()) continue;
            if (std::any_of(job->pnode->params.begin(), job->pnode->params.end(), [](const preset::PresetParam& p) { return p.type == "string" || p.type == "filename"; })) continue;
            const bool own = n.component->includesCabinet();
            int cab = own ? 1 : 0;
            for (std::size_t j = i + 1; j < list.size() && !own; ++j) {
                const int32_t tid = list[j]->table_id;
                if (isAmpTable(tid) || isSplitStartNode(tid)) break;
                const IComponent* c = list[j]->component.get();
                if (!c || c->isBypassed()) continue;
                if (c->speakerImpedanceProfile().valid) { cab = 1; break; }
            }
            if (n.component->cabFollowNeedsPrepareBeforePrepare(cab)) {
                job->cab = cab;
                prepared_with_params_.push_back(n.component.get());
            }
        }
    }

    // The deferred units, each exactly as buildNode would have prepared it, spread over worker threads (the calling
    // thread takes a share). Each unit only touches itself, so the result does not depend on the order or the threads.
    void prepareDeferred(std::vector<DeferredPrepare>& jobs) {
        if (jobs.empty()) return;
        const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
        // Leave cores for the audio thread and its RtPool workers; circuit units also settle their two channels at once.
        const std::size_t workers = std::min<std::size_t>(jobs.size() - 1, std::max(1u, hw / 2 - 1));
        std::atomic<std::size_t> next{0};
        std::exception_ptr failure;
        std::mutex failure_mutex;
        auto run = [&] {
            for (std::size_t i; (i = next.fetch_add(1)) < jobs.size();) {
                if (cancel_ && cancel_()) break;
                try {
                    if (jobs[i].cab >= -1) { // what syncCabFollow's re-prepare would end with, in one prepare
                        applyPresetParams(*jobs[i].component, *jobs[i].pnode);
                        jobs[i].component->setCabFollows(jobs[i].cab);
                        jobs[i].component->prepare(sample_rate_, max_block_size_);
                        jobs[i].component->processControlRate();
                    } else
                        prepareWithParams(*jobs[i].component, *jobs[i].pnode);
                } catch (...) {
                    std::lock_guard<std::mutex> lock(failure_mutex);
                    if (!failure) failure = std::current_exception();
                }
            }
        };
        fenv_t env;
        std::fegetenv(&env); // workers use this thread's floating-point mode (FTZ/DAZ on x86)
        std::vector<std::thread> pool;
        pool.reserve(workers);
        for (std::size_t w = 0; w < workers; ++w)
            pool.emplace_back([&] {
                std::fesetenv(&env);
                run();
            });
        run();
        for (auto& t : pool) t.join();
        jobs.clear();
        if (failure) std::rethrow_exception(failure);
    }

    bool syncCircuitNoiseList(std::vector<std::unique_ptr<GraphNode>>& list) {
        bool changed = false;
        for (auto& n : list) {
            if (n->component && ce::circuitNoiseNeedsRefresh(*n->component)) {
                detachForEdit(*n);
                if (!ce::refreshCircuitNoise(*n->component)) throw std::runtime_error("Unable to rebuild circuit noise");
                changed = true;
            }
            changed = syncCircuitNoiseList(n->children) || changed;
        }
        return changed;
    }

    void prepareNode(GraphNode& n, double sr, uint32_t mbs) {
        if (n.component) { detachForEdit(n); n.component->prepare(sr, mbs); }
        for (auto& c : n.children) prepareNode(*c, sr, mbs);
    }

    void resetNode(GraphNode& n) {
        if (n.component) { detachForEdit(n); n.component->reset(); }
        for (auto& c : n.children) resetNode(*c);
    }

    std::size_t countNodes(const GraphNode& n) const noexcept {
        std::size_t total = 1;
        for (const auto& c : n.children) total += countNodes(*c);
        return total;
    }

    struct LaneJob {
        RackGraph* graph;
        std::vector<std::unique_ptr<GraphNode>>* list;
        std::size_t from, to;
        GraphNode* split;
        AudioBlock ctx;
        bool mono_in = false, mono_out = false; // the lane's input / output proved one signal on both channels
    };
    static void runLaneB(void* p) {
        auto& j = *static_cast<LaneJob*>(p);
        j.mono_out = j.graph->processLane(*j.list, j.from, j.to, j.split->split_buf_b, j.split->split_scratch_b, j.ctx,
                                          static_cast<PipeCtx*>(j.split->pipe_b.get()), j.mono_in);
    }

    // A split's Split B and Split Mix at its own nesting level (false: not a complete split).
    static bool splitBounds(const std::vector<std::unique_ptr<GraphNode>>& list, std::size_t i, std::size_t& mid_idx,
                          std::size_t& mix_idx) noexcept {
        int depth = 1;
        mid_idx = mix_idx = list.size();
        for (std::size_t j = i + 1; j < list.size(); ++j) {
            const int32_t jtid = list[j]->table_id;
            if (isSplitStartNode(jtid)) {
                ++depth;
            } else if (isSplitMidNode(jtid) && depth == 1) {
                mid_idx = j;
            } else if (isSplitMixNode(jtid)) {
                --depth;
                if (depth == 0) {
                    mix_idx = j;
                    break;
                }
            }
        }
        return mid_idx < mix_idx && mix_idx < list.size();
    }
    static bool splitEnabled(const GraphNode& split_node, const GraphNode& mix_node) noexcept {
        return (!split_node.component || !split_node.component->isBypassed()) && (!mix_node.component || !mix_node.component->isBypassed());
    }

    static uint32_t latencyList(const std::vector<std::unique_ptr<GraphNode>>& list, std::size_t from, std::size_t to) noexcept {
        uint32_t total = 0;
        for (std::size_t i = from; i < to; ++i) {
            const auto& n = *list[i];
            std::size_t mid, mix;
            if (isSplitStartNode(n.table_id) && splitBounds(list, i, mid, mix) && mix < to) {
                if (splitEnabled(n, *list[mix]))
                    total += std::max(latencyList(list, i + 1, mid), latencyList(list, mid + 1, mix));
                i = mix;
            } else if (n.component && !n.component->isBypassed()) {
                if (isContainerOrSplitNode(n.table_id)) total += latencyList(n.children, 0, n.children.size());
                else if (!isUtilityOrModulatorNode(n.table_id)) total += n.component->latencySamples();
            }
        }
        return total;
    }

    // Split Mix: lane A and lane B into out (which may be lane A's buffer), n frames. The one implementation for whole blocks
    // and pipelined sub-blocks, so both run the same arithmetic.
    void mixSplit(GraphNode& split_node, const GraphNode& mix_node, const float* a_l, const float* a_r, const float* b_l, const float* b_r,
                  float* out_l, float* out_r, uint32_t n) const noexcept {
        // Mix Path A and Path B using Split Mix XFade (0..1) and phase invert iP
        float xfade = 0.5f;
        float inv_b = 1.0f;
        if (mix_node.component) {
            xfade = std::clamp(mix_node.component->getParameter("XFade"), 0.0f, 1.0f);
            if (mix_node.table_id == 137000) {
                xfade = std::clamp(mix_node.component->getParameter("C2"), 0.0f, 1.0f);
            }
            // Invert is a catalogue switch whose untouched 0.5 is off (the parallel Split and the CrossOver alike, and
            // what the UI shows: IComponent::switchState), so lane B keeps its polarity unless Invert is switched on.
            const float ip = mix_node.component->getParameter("iP");
            if (ip > 0.5f) {
                inv_b = -1.0f;
            }
        }
        // Constant-power or linear crossfade where 0.5 is unity for both paths. The A/B balance glides
        // (~10 ms, inherited across edits) so dragging the split's mix never steps the level.
        if (split_node.split_xfade_cur < 0.0f) split_node.split_xfade_cur = xfade;
        const float xf_glide = 1.0f - std::exp(-1.0f / (0.010f * static_cast<float>(sample_rate_)));
        for (uint32_t f = 0; f < n; ++f) {
            split_node.split_xfade_cur += (xfade - split_node.split_xfade_cur) * xf_glide;
            const float xf = split_node.split_xfade_cur;
            const float gain_a = std::clamp(2.0f * (1.0f - xf), 0.0f, 1.4f);
            const float gain_b = std::clamp(2.0f * xf, 0.0f, 1.4f) * inv_b;
            out_l[f] = gain_a * a_l[f] + gain_b * b_l[f];
            out_r[f] = gain_a * a_r[f] + gain_b * b_r[f];
        }
    }

    // Whether node n's output is proved one signal on both channels, given that its input `in` is (AudioBlock::mono_input
    // of the units after it). A unit's trait is read before it renders, as a pipelined run reads every unit's up front;
    // a container answers after its children rendered (mono_out).
    bool monoAfter(const GraphNode& n, bool in) const noexcept {
        if (!in) return false;
        if (!n.component || n.component->isBypassed()) return true; // rendered as a copy
        if (isContainerOrSplitNode(n.table_id)) return n.children.empty() || n.mono_out;
        if (isUtilityOrModulatorNode(n.table_id)) return true; // a copy
        return n.component->monoPreserving();
    }

    // Renders `list` into io_buf. `mono`: its input is proved one signal on both channels; returns the same for its output.
    bool processNodeList(std::vector<std::unique_ptr<GraphNode>>& list,
                         StereoScratchBuffer& io_buf,
                         StereoScratchBuffer& scratch_buf,
                         const AudioBlock& host_ctx,
                         bool top_level,
                         bool mono) {
        std::size_t i = 0;
        while (i < list.size()) {
            int32_t tid = list[i]->table_id;
            if (top_level && pipe_on_) {
                const std::size_t e = pipeRunEnd(list, i, list.size(), true);
                if (e >= i + 2) {
                    const std::size_t done = processSeriesPipelined(list, i, e, io_buf, host_ctx, *pipe_top_, true, mono);
                    if (done > 0) {
                        i += done;
                        continue;
                    }
                }
            }
            if (isSplitStartNode(tid)) {
                // Find matching Split B and Split Mix at the same nesting level
                std::size_t mid_idx = list.size();
                std::size_t mix_idx = list.size();
                if (splitBounds(list, i, mid_idx, mix_idx)) {
                    auto& split_node = *list[i];
                    auto& mix_node = *list[mix_idx];
                    if (!splitEnabled(split_node, mix_node)) {
                        i = mix_idx + 1;
                        continue;
                    }
                    // Copy input into both Path A and Path B buffers (a CrossOver: its low band into A, its high band into
                    // B); each lane gets its own MIDI bus.
                    if (isCrossoverSplit(split_node)) {
                        split_node.xover.update(crossoverParam(mix_node), sample_rate_, host_ctx.num_frames);
                        split_node.xover.process(io_buf.left.data(), io_buf.right.data(), split_node.split_buf_a.left.data(),
                                                 split_node.split_buf_a.right.data(), split_node.split_buf_b.left.data(),
                                                 split_node.split_buf_b.right.data(), host_ctx.num_frames);
                    } else {
                        split_node.split_buf_a.copyFrom(io_buf.const_ptrs.data(), 2, host_ctx.num_frames);
                        split_node.split_buf_b.copyFrom(io_buf.const_ptrs.data(), 2, host_ctx.num_frames);
                    }
                    split_node.midi_a.clear();
                    split_node.midi_b.clear();
                    if (host_ctx.midi) {
                        split_node.midi_a = *host_ctx.midi;
                        split_node.midi_b = *host_ctx.midi;
                    }
                    AudioBlock ctx_a = host_ctx, ctx_b = host_ctx;
                    ctx_a.midi = &split_node.midi_a;
                    ctx_b.midi = &split_node.midi_b;

                    // Path B (mid_idx+1 .. mix_idx-1) renders on a pool core while Path A renders here; both
                    // lanes touch only their own nodes, buffers and MIDI bus. Inline when the worker is busy.
                    LaneJob job{this, &list, mid_idx + 1, mix_idx, &split_node, ctx_b, mono, false};
                    RtPool::Join lanes;
                    const bool parallel = mix_idx > mid_idx + 1 && mid_idx > i + 1 && !host_ctx.offline &&
                                          RtPool::instance().fork(lanes, &RackGraph::runLaneB, &job);
                    // Process Path A (i+1 .. mid_idx-1)
                    const bool mono_a = processLane(list, i + 1, mid_idx, split_node.split_buf_a, scratch_buf, ctx_a,
                                                    static_cast<PipeCtx*>(split_node.pipe_a.get()), mono);
                    if (parallel) RtPool::instance().join(lanes); // helps run queued work while waiting
                    else runLaneB(&job);

                    mixSplit(split_node, mix_node, split_node.split_buf_a.left.data(), split_node.split_buf_a.right.data(),
                             split_node.split_buf_b.left.data(), split_node.split_buf_b.right.data(), io_buf.left.data(),
                             io_buf.right.data(), host_ctx.num_frames);
                    mono = mono_a && job.mono_out;
                    i = mix_idx + 1;
                    continue;
                }
            }

            GraphNode& n = *list[i];
            if (isAmpTable(n.table_id)) coupleSpeakerLoad(list, i);
            n.mono_in = mono;
            const bool mono_next = monoAfter(n, mono);
            processNode(n, io_buf, scratch_buf, host_ctx);
            mono = isContainerOrSplitNode(n.table_id) ? monoAfter(n, mono) : mono_next;
            io_buf.copyFrom(scratch_buf.const_ptrs.data(), 2, host_ctx.num_frames);
            if (pipe_on_) {
                n.cost_us = 0.8 * n.cost_us + 0.2 * n.cost_acc_us;
                n.cost_acc_us = 0.0;
            }
            ++i;
        }
        return mono;
    }

    void syncCabFollowList(std::vector<std::unique_ptr<GraphNode>>& list, bool& reprepared) {
        for (std::size_t i = 0; i < list.size(); ++i) {
            GraphNode& n = *list[i];
            if (!n.children.empty()) syncCabFollowList(n.children, reprepared);
            if (!isAmpTable(n.table_id) || !n.component) continue;
            // An amp whose circuit ends in speaker + cab (docs/CIRCUIT_ENGINE.md §10.21) is its own cab: a cabinet unit after
            // it is bypassed once (remembered on that node, so one the user turns back on stays on) and turned back on
            // when the amp stops including one (its netlist edited back to a plain speaker load).
            const bool own = n.component->includesCabinet();
            for (std::size_t j = i + 1; j < list.size(); ++j) {
                GraphNode& cn = *list[j];
                if (isAmpTable(cn.table_id) || isSplitStartNode(cn.table_id)) break;
                if (!isCabTable(cn.table_id) || !cn.component) continue;
                if (own && !cn.cab_muted_by_amp && !cn.component->isBypassed()) {
                    bypassNode(cn, true);
                    cn.cab_muted_by_amp = true;
                    reprepared = true;
                } else if (!own && cn.cab_muted_by_amp) {
                    if (cn.component->isBypassed()) bypassNode(cn, false), reprepared = true;
                    cn.cab_muted_by_amp = false;
                }
            }
            int cab = own ? 1 : 0;
            for (std::size_t j = i + 1; j < list.size() && !own; ++j) {
                const int32_t tid = list[j]->table_id;
                if (isAmpTable(tid) || isSplitStartNode(tid)) break;
                const IComponent* c = list[j]->component.get();
                if (!c || c->isBypassed()) continue;
                if (c->speakerImpedanceProfile().valid) { cab = 1; break; }
            }
            const int had = n.component->cabFollows();
            if (had == cab || had < -1) continue; // unchanged, or the unit does not track it
            if (!n.component->cabFollowNeedsPrepare(cab)) {
                // Rendering is unchanged. The state is control-thread only (never read by the audio thread), so a
                // component shared with a published plan may record it in place.
                n.component->setCabFollows(cab);
                continue;
            }
            if (n.component.use_count() == 1) {
                n.component->setCabFollows(cab);
                n.component->prepare(sample_rate_, max_block_size_);
                n.component->processControlRate();
            } else {
                auto previous = n.component;
                try {
                    detachForEdit(n, cab); // a fresh copy prepared with `cab`, parameters and bypass copied
                } catch (...) {
                    n.component = std::move(previous);
                    throw;
                }
            }
            reprepared = true;
        }
    }

    // setSlotBypassed for a node (control thread): a detached copy when a published plan shares the component.
    void bypassNode(GraphNode& node, bool bypassed) {
        ComponentEdit transaction(*this, node);
        node.component->setBypassed(bypassed);
        node.component->setParameter("Pwr", bypassed ? 0.0f : 1.0f);
        dirty_ = true;
    }

    // Bi-directional amp ↔ cabinet coupling: the first active cabinet after an amp in the same serial
    // chain (before another amp or a split) loads its power stage with that speaker's impedance
    // profile (resonance fs / Q, voice-coil inductance). Audio thread; no allocation, a few virtual
    // calls per amp per block, and the amp only redesigns filters when the profile changes.
    static void coupleSpeakerLoad(std::vector<std::unique_ptr<GraphNode>>& list, std::size_t amp_idx) noexcept {
        IComponent* amp = list[amp_idx]->component.get();
        if (!amp || amp->isBypassed()) return;
        SpeakerImpedanceProfile load{};
        for (std::size_t j = amp_idx + 1; j < list.size(); ++j) {
            const int32_t tid = list[j]->table_id;
            if (isAmpTable(tid) || isSplitStartNode(tid)) break;
            const IComponent* c = list[j]->component.get();
            if (!c || c->isBypassed()) continue;
            if (const SpeakerImpedanceProfile p = c->speakerImpedanceProfile(); p.valid) {
                load = p;
                break;
            }
        }
        amp->setSpeakerLoadProfile(load);
    }

    static void allocLanePipes(std::vector<std::unique_ptr<GraphNode>>& list) { // control thread
        for (auto& n : list) {
            if (isSplitStartNode(n->table_id)) {
                if (!n->pipe_a) n->pipe_a = std::make_shared<PipeCtx>(false);
                if (!n->pipe_b) n->pipe_b = std::make_shared<PipeCtx>(false);
            }
            allocLanePipes(n->children);
        }
    }
    // ── series pipelining ──
    static constexpr int kPipeStages = 3;             // stages per chain: the series chain, and a crossed split's lane B
    static constexpr int kPipeJobs = 2 * kPipeStages; // stages of one run
    static constexpr std::size_t kPipeAtoms = 64;     // list entries one run may hold
    bool pipeable(const GraphNode& n) const noexcept {
        if (isSplitStartNode(n.table_id) || isSplitMidNode(n.table_id) || isSplitMixNode(n.table_id)) return false;
        if (isContainerOrSplitNode(n.table_id) || isPluginTableId(n.table_id)) return false; // MIDI units opt out by subBlockSafety
        // Safety 1 (a mono or a stereo path) is fine: the rack decides it per host block (AudioBlock::mono_input).
        return !n.component || n.component->subBlockSafety() != 2;
    }
    // The end (exclusive, <= to) of the run that may pipeline from i: pipelinable units and, with cross_splits (the
    // top-level chain), at most one enabled split whose two lanes hold only pipelinable units. A switched-off split is
    // passed over, as when rendered one unit at a time.
    std::size_t pipeRunEnd(const std::vector<std::unique_ptr<GraphNode>>& list, std::size_t i, std::size_t to, bool cross_splits) const noexcept {
        std::size_t e = i;
        bool split = false;
        while (e < to) {
            const GraphNode& n = *list[e];
            if (cross_splits && isSplitStartNode(n.table_id)) {
                std::size_t mid = 0, mix = 0;
                if (!splitBounds(list, e, mid, mix) || mix >= to) break;
                if (!splitEnabled(n, *list[mix])) {
                    e = mix + 1;
                    continue;
                }
                bool lanes = !split;
                for (std::size_t k = e + 1; lanes && k < mix; ++k) lanes = k == mid || pipeable(*list[k]);
                if (!lanes) break;
                split = true;
                e = mix + 1;
                continue;
            }
            if (!pipeable(n)) break;
            ++e;
        }
        return e;
    }
    // One stage's progress (frames of the block done), and the wait on it. Several stages may wait on one counter.
    struct PipeCounter {
        std::atomic<uint32_t> v{0};
        std::atomic<uint32_t> sleepers{0};
        void reset() noexcept {
            v.store(0, std::memory_order_relaxed);
            sleepers.store(0, std::memory_order_relaxed);
        }
        void publish(uint32_t x) noexcept {
            v.store(x, std::memory_order_seq_cst);
            if (sleepers.load(std::memory_order_seq_cst)) v.notify_all();
        }
        // Spin briefly (sub-blocks arrive every few µs), then sleep on the value so an oversubscribed machine never has a
        // waiting stage burning a core its upstream needs.
        void await(uint32_t need) noexcept {
            uint32_t x = v.load(std::memory_order_acquire);
            for (int spin = 0; x < need && spin < 2000; ++spin) {
                pipeRelax();
                x = v.load(std::memory_order_acquire);
            }
            if (x >= need) return;
            sleepers.fetch_add(1, std::memory_order_seq_cst);
            for (x = v.load(std::memory_order_seq_cst); x < need; x = v.load(std::memory_order_seq_cst)) v.wait(x, std::memory_order_seq_cst);
            sleepers.fetch_sub(1, std::memory_order_relaxed);
        }
    };
    // A run's entries in rendering order. The series chain holds units and a crossed split's tap (its input, kept for lane
    // B), lane A's units and mix; lane B's units render beside the chain.
    enum : uint8_t { kAtomUnit = 0, kAtomLaneA = 1, kAtomTap = 2, kAtomMix = 3 };
    struct PipeAtom {
        uint8_t kind = kAtomUnit;
        uint32_t idx = 0; // list index (the split start for a tap, the Split Mix for a mix)
        double cost = 0.0;
    };
    struct PipeCtx;
    struct PipeStageJob {
        RackGraph* g = nullptr;
        PipeCtx* pc = nullptr;
        std::vector<std::unique_ptr<GraphNode>>* list = nullptr;
        bool lane_b = false;                     // renders lane B's units pc->batom[from, to) (else the chain's pc->atom[from, to))
        uint32_t from = 0, to = 0;
        const StereoScratchBuffer* in = nullptr; // whole-block input of the stage
        StereoScratchBuffer* out = nullptr;      // whole-block output
        StereoScratchBuffer* a = nullptr;        // sub-block ping-pong
        StereoScratchBuffer* b = nullptr;
        PipeCounter* wait = nullptr; // the progress of this stage's input (null: ready)
        PipeCounter done;
        AudioBlock ctx{};
        uint32_t frames = 0, sub = 64;
        uint64_t fpmode = 0; // the caller's floating-point mode (denormal flush), set on whichever thread renders the stage
    };
    struct PipeCtx { // one series pipeline's stages and buffers (the top-level chain, or one lane of a split)
        explicit PipeCtx(bool crosses_splits = false)
            : cross(crosses_splits), a(crosses_splits ? kPipeJobs : kPipeStages), b(a.size()), out(a.size()),
              split_bufs(crosses_splits ? 3 : 0) {}
        const bool cross; // may hold a split (the top-level chain): kPipeJobs stages and the split buffers
        PipeStageJob jobs[kPipeJobs];
        std::vector<StereoScratchBuffer> a, b, out; // per stage (constructed in place: their pointers are their own)
        std::vector<StereoScratchBuffer> split_bufs; // a crossed split: its input (tap), lane B's ping-pong inside the mix stage
        std::array<PipeAtom, kPipeAtoms> atom{}, batom{};
        uint32_t n_atom = 0, n_batom = 0;
        // a crossed split, this block
        PipeCounter tap_done;
        GraphNode* split = nullptr;
        GraphNode* mix = nullptr;
        const StereoScratchBuffer* b_out = nullptr; // lane B's output (its last stage's, or the tap for an empty lane)
        PipeCounter* b_done = nullptr;              // its progress (null: ready by the time the mix runs)
        bool b_inline = false;                      // lane B renders inside the mix's stage
        AudioBlock ctx_a{}, ctx_b{};
    };
    static void pipeRelax() noexcept {
#if defined(__aarch64__)
        asm volatile("yield");
#elif defined(__x86_64__)
        __builtin_ia32_pause();
#endif
    }
    static uint64_t fpMode() noexcept {
#if defined(__aarch64__)
        uint64_t v;
        asm volatile("mrs %0, fpcr" : "=r"(v));
        return v;
#elif defined(__x86_64__)
        return _mm_getcsr();
#else
        return 0;
#endif
    }
    static void setFpMode(uint64_t v) noexcept {
#if defined(__aarch64__)
        asm volatile("msr fpcr, %0" ::"r"(v));
#elif defined(__x86_64__)
        _mm_setcsr(static_cast<unsigned>(v));
#else
        (void)v;
#endif
    }
    static void runPipeStage(void* p) noexcept {
        auto& j = *static_cast<PipeStageJob*>(p);
        PipeCtx& pc = *j.pc;
        auto& list = *j.list;
        const uint64_t saved_fp = fpMode();
        if (saved_fp != j.fpmode) setFpMode(j.fpmode);
        for (uint32_t off = 0; off < j.frames; off += j.sub) {
            const uint32_t n = std::min(j.sub, j.frames - off);
            if (j.wait) j.wait->await(off + n);
            std::memcpy(j.a->left.data(), j.in->left.data() + off, sizeof(float) * n);
            std::memcpy(j.a->right.data(), j.in->right.data() + off, sizeof(float) * n);
            auto unit = [&](uint32_t k, const AudioBlock& base, StereoScratchBuffer& x, StereoScratchBuffer& y) {
                AudioBlock c = base;
                c.num_frames = n;
                c.host_offset = off;
                c.host_frames = j.frames;
                j.g->processNode(*list[k], x, y, c, true);
                x.copyFrom(y.const_ptrs.data(), 2, n);
            };
            if (j.lane_b) {
                for (uint32_t q = j.from; q < j.to; ++q) unit(pc.batom[q].idx, pc.ctx_b, *j.a, *j.b);
            } else {
                for (uint32_t q = j.from; q < j.to; ++q) {
                    const PipeAtom& at = pc.atom[q];
                    if (at.kind == kAtomUnit) {
                        unit(at.idx, j.ctx, *j.a, *j.b);
                    } else if (at.kind == kAtomLaneA) {
                        unit(at.idx, pc.ctx_a, *j.a, *j.b);
                    } else if (at.kind == kAtomTap) { // the split's input for lane B; lane A goes on with it here
                        StereoScratchBuffer& tap = pc.split_bufs[0];
                        if (isCrossoverSplit(*pc.split)) { // a CrossOver: the high band for lane B, the low band for lane A
                            pc.split->xover.process(j.a->left.data(), j.a->right.data(), j.a->left.data(), j.a->right.data(),
                                                    tap.left.data() + off, tap.right.data() + off, n);
                        } else {
                            std::memcpy(tap.left.data() + off, j.a->left.data(), sizeof(float) * n);
                            std::memcpy(tap.right.data() + off, j.a->right.data(), sizeof(float) * n);
                        }
                        pc.tap_done.publish(off + n);
                    } else { // the Split Mix: lane A (here) with lane B
                        const float *bl, *br;
                        if (pc.b_inline) {
                            StereoScratchBuffer &x = pc.split_bufs[1], &y = pc.split_bufs[2];
                            std::memcpy(x.left.data(), pc.split_bufs[0].left.data() + off, sizeof(float) * n);
                            std::memcpy(x.right.data(), pc.split_bufs[0].right.data() + off, sizeof(float) * n);
                            for (uint32_t r = 0; r < pc.n_batom; ++r) unit(pc.batom[r].idx, pc.ctx_b, x, y);
                            bl = x.left.data(), br = x.right.data();
                        } else {
                            if (pc.b_done) pc.b_done->await(off + n);
                            bl = pc.b_out->left.data() + off, br = pc.b_out->right.data() + off;
                        }
                        j.g->mixSplit(*pc.split, *pc.mix, j.a->left.data(), j.a->right.data(), bl, br, j.a->left.data(), j.a->right.data(), n);
                    }
                }
            }
            std::memcpy(j.out->left.data() + off, j.a->left.data(), sizeof(float) * n);
            std::memcpy(j.out->right.data() + off, j.a->right.data(), sizeof(float) * n);
            j.done.publish(off + n);
        }
        if (saved_fp != j.fpmode) setFpMode(saved_fp);
    }
    static bool sameChannels(const StereoScratchBuffer& b, uint32_t n) noexcept {
        return std::memcmp(b.left.data(), b.right.data(), sizeof(float) * n) == 0;
    }
    // A serial run list[from, to) into io_buf (scratch as the per-unit ping-pong), pipelined where it can be. `mono`: the
    // input is proved one signal on both channels; returns the same for the output.
    bool processLane(std::vector<std::unique_ptr<GraphNode>>& list, std::size_t from, std::size_t to, StereoScratchBuffer& io_buf,
                     StereoScratchBuffer& scratch_buf, const AudioBlock& ctx, PipeCtx* pc, bool mono) {
        std::size_t i = from;
        while (i < to) {
            if (pc && pipe_on_) {
                const std::size_t e = pipeRunEnd(list, i, to, false);
                if (e >= i + 2) {
                    const std::size_t done = processSeriesPipelined(list, i, e, io_buf, ctx, *pc, false, mono);
                    if (done > 0) {
                        i += done;
                        continue;
                    }
                }
            }
            GraphNode& n = *list[i];
            n.mono_in = mono;
            const bool mono_next = monoAfter(n, mono);
            processNode(n, io_buf, scratch_buf, ctx);
            mono = isContainerOrSplitNode(n.table_id) ? monoAfter(n, mono) : mono_next;
            io_buf.copyFrom(scratch_buf.const_ptrs.data(), 2, ctx.num_frames);
            if (pipe_on_) {
                n.cost_us = 0.8 * n.cost_us + 0.2 * n.cost_acc_us;
                n.cost_acc_us = 0.0;
            }
            ++i;
        }
        return mono;
    }

    // Renders list[i, e) (a run from pipeRunEnd) and returns how many list entries it rendered (0: the caller renders entry
    // i itself). `mono`: the run's input is proved one signal on both channels; on return, whether its output is.
    //
    // The stages are cut by each unit's measured cost (any cut is bit-identical; this only balances the cores): up to
    // kPipeStages along the series chain and, for a crossed split, lane B either inside the stage that mixes or in up to
    // kPipeStages of its own, which take the split's input from a tap on the chain and hand their output to the mix.
    std::size_t processSeriesPipelined(std::vector<std::unique_ptr<GraphNode>>& list, std::size_t i, std::size_t e,
                                       StereoScratchBuffer& io_buf, const AudioBlock& ctx, PipeCtx& pc, bool couple, bool& mono) {
        const uint32_t frames = ctx.num_frames;
        const uint32_t sub = pipe_sub_ ? pipe_sub_ : std::max<uint32_t>(16, std::min<uint32_t>(64, (frames / 4) & ~15u));
        if (calibrating_ || frames < 2 * sub || (ctx.midi && !ctx.midi->empty()) || ctx.transport_playing) return 0; // offline renders stay on one core

        // ── the run as atoms ──
        uint32_t na = 0, nb = 0;
        std::size_t split_at = list.size(), mix_at = list.size();
        bool outside = false; // an active unit outside the split
        for (std::size_t k = i; k < e;) {
            if (isSplitStartNode(list[k]->table_id)) {
                std::size_t mid = 0, mix = 0;
                if (!splitBounds(list, k, mid, mix) || mix >= e) return 0;
                if (!splitEnabled(*list[k], *list[mix])) {
                    k = mix + 1;
                    continue;
                }
                if (!pc.cross || split_at < list.size() || na + (mid - k) + 1 > kPipeAtoms || mix - mid - 1 > kPipeAtoms) return 0;
                split_at = k, mix_at = mix;
                pc.atom[na++] = {kAtomTap, static_cast<uint32_t>(k), 0.0};
                for (std::size_t q = k + 1; q < mid; ++q) pc.atom[na++] = {kAtomLaneA, static_cast<uint32_t>(q), list[q]->cost_us};
                pc.atom[na++] = {kAtomMix, static_cast<uint32_t>(mix), 0.0};
                for (std::size_t q = mid + 1; q < mix; ++q) pc.batom[nb++] = {kAtomUnit, static_cast<uint32_t>(q), list[q]->cost_us};
                k = mix + 1;
                continue;
            }
            if (na >= kPipeAtoms) return 0;
            const GraphNode& n = *list[k];
            outside = outside || (n.component && !n.component->isBypassed());
            pc.atom[na++] = {kAtomUnit, static_cast<uint32_t>(k), n.cost_us};
            ++k;
        }
        const bool has_split = split_at < list.size();
        // A split with nothing active beside it renders as two lanes in parallel (processNodeList).
        if (na < 2 || (has_split && !outside)) return 0;
        pc.n_atom = na, pc.n_batom = nb;

        // ── stage cuts ──
        double S[kPipeAtoms + 1], SB[kPipeAtoms + 1];
        S[0] = SB[0] = 0.0;
        uint32_t tap_q = 0, mix_q = 0;
        for (uint32_t q = 0; q < na; ++q) {
            S[q + 1] = S[q] + pc.atom[q].cost;
            if (pc.atom[q].kind == kAtomTap) tap_q = q;
            if (pc.atom[q].kind == kAtomMix) mix_q = q;
        }
        for (uint32_t q = 0; q < nb; ++q) SB[q + 1] = SB[q] + pc.batom[q].cost;
        const double total_b = SB[nb], total = S[na] + total_b;
        if (has_split) {
            // Crossing the split pays when units beside it carry real work (they then overlap with the lanes). With next
            // to nothing beside it, the two lanes render as they did, in parallel with their own pipelines
            // (processNodeList), and don't wait on each other at every sub-block for the mix.
            double beside = 0.0;
            for (uint32_t q = 0; q < na; ++q)
                if (pc.atom[q].kind == kAtomUnit) beside += pc.atom[q].cost;
            if (beside < 0.1 * total) return 0;
        }
        // A plan is better with a smaller largest stage; on a tie, with fewer stages (fewer cores), then with more even
        // stages (smaller sum of squares: no near-empty first stage that leaves the caller waiting in join while the
        // forked stages wake up).
        auto better = [](double m, int n, double sq, double best_m, int best_n, double best_sq) {
            if (m < best_m - 1e-9 || m > best_m + 1e-9) return m < best_m;
            if (n != best_n) return n < best_n;
            return sq < best_sq - 1e-9;
        };
        // Lane B alone in kb stages: the best cut.
        double best_b[kPipeStages + 1] = {0.0, total_b, 1e300, 1e300}, best_b_sq[kPipeStages + 1] = {0.0, total_b * total_b, 1e300, 1e300};
        uint32_t bcut[kPipeStages + 1][kPipeStages + 1] = {};
        bcut[1][1] = nb;
        for (uint32_t c1 = 1; c1 < nb; ++c1) {
            const double a = SB[c1], b2 = SB[nb] - SB[c1];
            if (better(std::max(a, b2), 2, a * a + b2 * b2, best_b[2], 2, best_b_sq[2]))
                best_b[2] = std::max(a, b2), best_b_sq[2] = a * a + b2 * b2, bcut[2][1] = c1, bcut[2][2] = nb;
            for (uint32_t c2 = c1 + 1; c2 < nb; ++c2) {
                const double b = SB[c2] - SB[c1], c = SB[nb] - SB[c2];
                if (better(std::max({a, b, c}), 3, a * a + b * b + c * c, best_b[3], 3, best_b_sq[3]))
                    best_b[3] = std::max({a, b, c}), best_b_sq[3] = a * a + b * b + c * c, bcut[3][1] = c1, bcut[3][2] = c2, bcut[3][3] = nb;
            }
        }
        // The chain in k1 stages; lane B inside the mix's stage (kb 0) or in kb stages between the tap's and the mix's.
        double best = 1e300, best_sq = 1e300;
        int best_k1 = 1, best_kb = 0, best_n = 1 << 20;
        uint32_t best_c1 = na, best_c2 = na;
        const int k1_max = static_cast<int>(std::min<uint32_t>(kPipeStages, na));
        for (int k1 = 1; k1 <= k1_max; ++k1)
            for (uint32_t c1 = (k1 >= 2 ? 1 : na); c1 <= (k1 >= 2 ? na - 1 : na); ++c1)
                for (uint32_t c2 = (k1 == 3 ? c1 + 1 : na); c2 <= (k1 == 3 ? na - 1 : na); ++c2) {
                    double s[kPipeStages] = {S[std::min(c1, na)], S[c2] - S[c1], S[na] - S[c2]};
                    auto stageOf = [&](uint32_t q) { return q < c1 ? 0 : (q < c2 ? 1 : 2); };
                    auto consider = [&](double cost, int kb, double sq) {
                        const int stages = k1 + kb;
                        if (better(cost, stages, sq, best, best_n, best_sq))
                            best = cost, best_sq = sq, best_k1 = k1, best_kb = kb, best_n = stages, best_c1 = c1, best_c2 = c2;
                    };
                    const double chain_sq = s[0] * s[0] + s[1] * s[1] + s[2] * s[2];
                    if (has_split && nb > 0 && stageOf(tap_q) < stageOf(mix_q))
                        for (int kb = 1; kb <= static_cast<int>(std::min<uint32_t>(kPipeStages, nb)); ++kb)
                            consider(std::max({s[0], s[1], s[2], best_b[kb]}), kb, chain_sq + best_b_sq[kb]);
                    if (has_split) s[stageOf(mix_q)] += total_b;
                    consider(std::max({s[0], s[1], s[2]}), 0, s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
                }
        if (best_n < 2) return 0;                        // one stage: nothing runs beside anything
        if (total > 0 && best > 0.92 * total) return 0; // nothing to overlap (one unit is all the work)

        // ── what each unit's input is this block (AudioBlock::mono_input), read before anything renders ──
        bool m = mono, m_split = false;
        for (uint32_t q = 0; q < na; ++q) {
            const PipeAtom& at = pc.atom[q];
            if (at.kind == kAtomTap) {
                m_split = m;
            } else if (at.kind == kAtomMix) {
                bool mb = m_split;
                for (uint32_t r = 0; r < nb; ++r) {
                    GraphNode& n = *list[pc.batom[r].idx];
                    n.mono_in = mb;
                    mb = monoAfter(n, mb);
                }
                m = m && mb;
            } else {
                GraphNode& n = *list[at.idx];
                n.mono_in = m;
                m = monoAfter(n, m);
            }
        }

        if (couple)
            for (uint32_t q = 0; q < na; ++q)
                if (pc.atom[q].kind == kAtomUnit && isAmpTable(list[pc.atom[q].idx]->table_id)) coupleSpeakerLoad(list, pc.atom[q].idx);
        if (has_split) {
            GraphNode& sn = *list[split_at];
            sn.midi_a.clear(); // as processNodeList: each lane its own (here empty) MIDI bus
            sn.midi_b.clear();
            pc.split = &sn, pc.mix = list[mix_at].get();
            if (isCrossoverSplit(sn)) sn.xover.update(crossoverParam(*pc.mix), sample_rate_, frames); // as processNodeList does
            pc.ctx_a = ctx, pc.ctx_b = ctx;
            pc.ctx_a.midi = &sn.midi_a, pc.ctx_b.midi = &sn.midi_b;
            pc.tap_done.reset();
        }

        // ── stages, in an order where each waits only on earlier ones: the chain up to the tap's stage, lane B's, the rest ──
        const int k1 = best_k1, kb = has_split ? best_kb : 0, K = k1 + kb;
        const uint32_t ccut[kPipeStages + 1] = {0, k1 >= 2 ? best_c1 : na, k1 == 3 ? best_c2 : na, na};
        const int tap_stage = !has_split ? k1 - 1 : (tap_q < ccut[1] ? 0 : (tap_q < ccut[2] ? 1 : 2));
        int slot_chain[kPipeStages] = {}, slot_b[kPipeStages] = {};
        {
            int s = 0;
            for (int c = 0; c <= tap_stage; ++c) slot_chain[c] = s++;
            for (int b = 0; b < kb; ++b) slot_b[b] = s++;
            for (int c = tap_stage + 1; c < k1; ++c) slot_chain[c] = s++;
        }
        PipeStageJob* jobs = pc.jobs;
        const uint64_t fp = fpMode();
        auto setup = [&](int slot, bool lane_b, uint32_t from, uint32_t to, const StereoScratchBuffer* in, PipeCounter* wait) {
            auto& j = jobs[slot];
            j.g = this, j.pc = &pc, j.list = &list, j.lane_b = lane_b, j.from = from, j.to = to;
            j.in = in, j.out = &pc.out[static_cast<std::size_t>(slot)], j.wait = wait;
            j.a = &pc.a[static_cast<std::size_t>(slot)], j.b = &pc.b[static_cast<std::size_t>(slot)];
            j.done.reset();
            j.ctx = ctx, j.frames = frames, j.sub = sub, j.fpmode = fp;
        };
        for (int c = 0; c < k1; ++c)
            setup(slot_chain[c], false, ccut[c], ccut[c + 1], c == 0 ? &io_buf : &pc.out[static_cast<std::size_t>(slot_chain[c - 1])],
                  c == 0 ? nullptr : &jobs[slot_chain[c - 1]].done);
        for (int b = 0; b < kb; ++b)
            setup(slot_b[b], true, bcut[kb][b], bcut[kb][b + 1], b == 0 ? &pc.split_bufs[0] : &pc.out[static_cast<std::size_t>(slot_b[b - 1])],
                  b == 0 ? &pc.tap_done : &jobs[slot_b[b - 1]].done);
        if (has_split) {
            pc.b_inline = kb == 0 && nb > 0;
            pc.b_out = kb > 0 ? &pc.out[static_cast<std::size_t>(slot_b[kb - 1])] : &pc.split_bufs[0];
            pc.b_done = kb > 0 ? &jobs[slot_b[kb - 1]].done : nullptr;
        }
        if (diag_on_) diag_->notePipeline(K);
        // Later stages are forked last-first: join reclaims in reverse order, so an unstarted stage always runs before an
        // unstarted one that waits on it. A refused fork stops forking: that stage and the ones before it run here, in
        // order, before the join (the forked stages after them only wait on them).
        RtPool::Join jn;
        int inline_to = 0; // stages 0..inline_to run on this thread
        for (int s = K - 1; s >= 1; --s)
            if (!RtPool::instance().fork(jn, &RackGraph::runPipeStage, &jobs[s])) {
                inline_to = s;
                break;
            }
        for (int s = 0; s <= inline_to; ++s) runPipeStage(&jobs[s]);
        RtPool::instance().join(jn);
        io_buf.copyFrom(pc.out[static_cast<std::size_t>(slot_chain[k1 - 1])].const_ptrs.data(), 2, frames);

        auto commit = [&](uint32_t k) {
            GraphNode& n = *list[k];
            n.cost_us = 0.8 * n.cost_us + 0.2 * n.cost_acc_us;
            n.cost_acc_us = 0.0;
            if (meters_on_ && n.component && !n.component->isBypassed() && !isUtilityOrModulatorNode(n.table_id)) {
                UnitLiveReadout ro;
                n.component->liveReadout(ro);
                unit_meters_->commit(n.active_idx, n.macc, frames, ro);
            }
            n.macc = {};
            if (diag_on_) diagCommit(n);
        };
        for (uint32_t q = 0; q < na; ++q)
            if (pc.atom[q].kind == kAtomUnit || pc.atom[q].kind == kAtomLaneA) commit(pc.atom[q].idx);
        for (uint32_t r = 0; r < nb; ++r) commit(pc.batom[r].idx);
        mono = m;
        return e - i;
    }

    // /api/diag: the node's block (all its sub-blocks) to the bank. Whichever core rendered it; no locks or allocation.
    void diagCommit(GraphNode& n) noexcept {
        if (n.dacc.frames == 0 || !n.component) return;
        UnitSolverStats ss;
        const bool circuit = n.component->solverStats(ss);
        diag_->slot(n.active_idx, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(n.component.get())), n.table_id, n.dacc.ns, n.dacc.frames,
                    sample_rate_, n.dacc.peak, n.dacc.sumsq, n.dacc.nonfinite, circuit ? &ss : nullptr);
        n.dacc = {};
    }

    void processNode(GraphNode& n, StereoScratchBuffer& in_buf, StereoScratchBuffer& out_buf, const AudioBlock& host_ctx, bool sub = false) {
        if (!n.component || n.component->isBypassed()) {
            if (meters_on_ && n.component && !isUtilityOrModulatorNode(n.table_id)) unit_meters_->recordIdle(n.active_idx);
            out_buf.copyFrom(in_buf.const_ptrs.data(), 2, host_ctx.num_frames);
            return;
        }

        if (isContainerOrSplitNode(n.table_id)) {
            if (n.children.empty()) {
                out_buf.copyFrom(in_buf.const_ptrs.data(), 2, host_ctx.num_frames);
                return;
            }
            n.split_buf_a.copyFrom(in_buf.const_ptrs.data(), 2, host_ctx.num_frames);
            n.mono_out = processNodeList(n.children, n.split_buf_a, n.split_buf_b, host_ctx, false, n.mono_in);
            out_buf.copyFrom(n.split_buf_a.const_ptrs.data(), 2, host_ctx.num_frames);
            return;
        }

        if (isUtilityOrModulatorNode(n.table_id)) {
            out_buf.copyFrom(in_buf.const_ptrs.data(), 2, host_ctx.num_frames);
            return;
        }

        float g_scale = (n.active_idx < slot_gain_scale_.size()) ? slot_gain_scale_[n.active_idx] : 1.0f;
        if (std::abs(g_scale - 1.0f) > 1e-3f) {
            for (uint32_t i = 0; i < host_ctx.num_frames; ++i) {
                in_buf.left[i] *= g_scale;
                in_buf.right[i] *= g_scale;
            }
        }

        // Control-rate updates run on the control thread when parameters change (never per block on the
        // audio thread), so heavy work such as IR rebuilding stays off the render path.
        AudioBlock sub_block = host_ctx;
        sub_block.in = in_buf.const_ptrs.data();
        sub_block.out = out_buf.ptrs.data();
        sub_block.num_channels = 2;
        sub_block.mono_input = n.mono_in ? 1 : 0; // decided for the whole host block (processNodeList / processSeriesPipelined)
        n.component->setSlotIndex(static_cast<int>(n.active_idx));
        const bool timed = pipe_on_ || diag_on_;
        const auto t_cost = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        n.component->processAudio(sub_block);
        if (timed) {
            const auto spent = std::chrono::steady_clock::now() - t_cost;
            if (pipe_on_) n.cost_acc_us += std::chrono::duration<double, std::micro>(spent).count();
            if (diag_on_) {
                auto& d = n.dacc;
                d.ns += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(spent).count());
                d.frames += host_ctx.num_frames;
                float pk = d.peak;
                double e = 0.0;
                for (uint32_t i = 0; i < host_ctx.num_frames; ++i) {
                    const float a = out_buf.left[i], b = out_buf.right[i];
                    if (EngineDiagBank::nonFinite(a) || EngineDiagBank::nonFinite(b)) { ++d.nonfinite; continue; }
                    pk = std::max(pk, std::max(std::abs(a), std::abs(b)));
                    e += double(a) * a + double(b) * b;
                }
                d.peak = pk;
                d.sumsq += 0.5 * e;
                if (!sub) diagCommit(n);
            }
        }
        if (meters_on_ && sub) {
            unit_meters_->accumulate(n.active_idx, n.macc, in_buf.left.data(), in_buf.right.data(), out_buf.left.data(), out_buf.right.data(),
                                     host_ctx.num_frames);
        } else if (meters_on_) {
            UnitLiveReadout ro;
            n.component->liveReadout(ro);
            unit_meters_->record(n.active_idx, in_buf.left.data(), in_buf.right.data(), out_buf.left.data(), out_buf.right.data(),
                                 host_ctx.num_frames, ro);
        }
        if (debug_taps_ && !calibrating_) {
            for (uint32_t i = 0; i < host_ctx.num_frames; ++i) {
                const float a = in_buf.left[i], b = out_buf.left[i];
                n.tap_in_e += static_cast<double>(a) * a;
                n.tap_out_e += static_cast<double>(b) * b;
                n.tap_in_pk = std::max(n.tap_in_pk, std::abs(a));
                n.tap_out_pk = std::max(n.tap_out_pk, std::abs(b));
            }
            n.tap_n += host_ctx.num_frames;
        }
    }

    double sample_rate_ = 48000.0;
    uint32_t max_block_size_ = 512;
    float input_gain_ = 1.0f;
    float input_gain_cur_ = -1.0f; // audio thread: smoothed input gain (< 0 = not started; inherited on handoffs)
    bool continuous_edit_ = false; // render plan: see setContinuousEdit
    bool master_bypass_ = false;
    float master_rms_env_ = 0.14f;
    float master_peak_env_ = 0.0f;
    float limiter_env_ = 1.0f;
    bool calibrating_ = false;
    bool parallel_prepare_ = false; // buildFromPreset prepares independent units on worker threads
    std::function<bool()> cancel_;  // setCancelCheck
    bool use_spares_ = false;       // setUseSpares
    std::vector<const IComponent*> prepared_with_params_; // buildFromPreset: amps planDeferredCabFollow prepared once
    bool debug_taps_ = false;
    std::shared_ptr<UnitMeterBank> unit_meters_;
    bool meters_on_ = false; // audio thread: metering armed for this block
    std::shared_ptr<EngineDiagBank> diag_; // /api/diag (null = off)
    bool diag_on_ = false;   // audio thread: this block reports to diag_
    std::size_t spares_used_ = 0; // buildFromPreset: units taken from PreparedUnitPool
    double tempo_bpm_ = kTempoDefault; // rig tempo (saved with the rig)
    preset::RackPreset active_preset_{};
    std::array<Macro, kMacros> macros_{};
    bool dirty_ = false;
    std::string preset_source_ = "factory";
    std::vector<std::unique_ptr<GraphNode>> nodes_;
    std::vector<GraphNode*> flat_active_nodes_;
    std::unordered_map<int, GraphNode*> render_node_index_; // prepared before publication
    std::vector<float> slot_gain_scale_;
    std::vector<float> slot_mix_scale_;
    StereoScratchBuffer buf_a_{};
    StereoScratchBuffer buf_b_{};
    std::vector<midi::MidiEvent> midi_main_;
    bool pipe_on_ = !(std::getenv("HEADROOM_PIPE") && std::string_view(std::getenv("HEADROOM_PIPE")) == "0"); // HEADROOM_PIPE=0: off
    uint32_t pipe_sub_ = std::getenv("HEADROOM_PIPE_SUB") ? static_cast<uint32_t>(std::atoi(std::getenv("HEADROOM_PIPE_SUB"))) : 0;
    std::shared_ptr<PipeCtx> pipe_top_ = std::make_shared<PipeCtx>(true); // heap: ~340 kB of stage buffers (RackGraph is often a stack object)
};

} // namespace rig
