#pragma once
#include "AudioBlock.hpp"
#include "../generated/ComponentCatalog.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rig {

struct ParamValue {
    catalog::ParamKind kind = catalog::ParamKind::Float;
    float num_val = 0.0f;
    std::string str_val{};
};

inline void getParamEffectiveRange(const catalog::ParamSchema& p, float& out_min, float& out_max) noexcept {
    // Continuous float parameters in [0, 1] are stored normalized in [0.0, 1.0].
    // `p.min_value` and `p.max_value` record the default preset sub-range;
    // keep the full normalized [0.0, 1.0] domain so unity (0.5) stays centered.
    if (p.kind == catalog::ParamKind::Bool) {
        out_min = 0.0f;
        out_max = 1.0f;
        return;
    }
    if (p.min_value >= 0.0f && p.max_value <= 1.0001f) {
        out_min = 0.0f;
        out_max = 1.0f;
        return;
    }
    out_min = std::min(0.0f, p.min_value);
    out_max = std::max(1.0f, p.max_value);
    if (out_max - out_min < 1e-4f) {
        out_min = std::min(0.0f, p.default_value);
        out_max = std::max(1.0f, p.default_value);
    }
}

struct ExportedParam {
    std::string id;
    bool is_string = false;
    float num = 0.0f;
    std::string str{};
};

// Effective range plus known discrete-selector overrides (cabinet lists, mic/position selectors,
// amp modes) whose schema sub-ranges don't cover every option.
// Hosted third-party plugins (src/plugins/PluginHost.hpp) use table ids 910000..989999; their schemas
// carry exact ranges (min_value/max_value) and declared kinds, so no heuristics apply to them.
inline constexpr int32_t kPluginTableBase = 910000;
inline constexpr bool isPluginTableId(int32_t t) noexcept { return t >= kPluginTableBase && t < 990000; }

inline void getParamRangeFor(int32_t table_id, const catalog::ParamSchema& p, float& out_min, float& out_max) noexcept {
    if (isPluginTableId(table_id)) {
        out_min = p.min_value;
        out_max = p.max_value > p.min_value ? p.max_value : p.min_value + 1.0f;
        return;
    }
    getParamEffectiveRange(p, out_min, out_max);
    const std::string_view id = p.id;
    auto starts = [&](std::string_view pre) { return id.size() == pre.size() + 1 && id.substr(0, pre.size()) == pre && id.back() >= '1' && id.back() <= '8'; };
    auto set = [&](float a, float b) { out_min = a; out_max = b; };
    switch (table_id) {
        case 156000: if (id == "Cab") set(0, 25); break;
        case 88000: if (id == "Cab") set(0, 28); break;
        case 100000: if (id == "S") set(0, 28); break;
        case 119000: case 168000:
            if (starts("Cab")) set(0, 30);
            else if (starts("Mic")) set(0, 4);
            else if (starts("MPos")) set(0, 2);
            break;
        case 127000: if (id == "S1") set(0, 7); break;
        case 16000: case 96000: if (id == "M") set(0, 3); break;
        case 151000: case 162000: case 300000: if (id == "gainMode") set(0, 2); break;
        case 176000: if (id == "effectsSelector") set(0, 2); break;
        case 49000: if (id == "em") set(0, 2); break;
        default: break;
    }
}

// Selector parameters that exist in this engine but not in the factory schema (e.g. which IR file a
// loader uses). Raw value = option index.
struct ExtraParam {
    std::string id;
    std::string name;
    std::vector<std::string> labels;
    float def = 0.0f;
    std::vector<std::string> groups; // optional: group/family name per label (for grouped browsers)
};

// Physical speaker impedance profile reported by a cabinet component so the upstream
// power amplifier can couple its transconductance output stage & NFB loop to the cab load.
struct SpeakerImpedanceProfile {
    bool valid = false;
    float fs_hz = 90.0f;     // fundamental cone/enclosure resonance (Hz)
    float q_res = 1.4f;      // motional resonance Q (higher for closed-back 4x12, lower for open-back)
    float le_scale = 1.0f;   // relative voice-coil inductive HF rise scale
};

// Live readout a unit can publish for its face's meters (UnitMeters.hpp). Negative = not reported.
struct UnitLiveReadout {
    float gain_reduction_db = -1.0f; // dynamics: current gain reduction, dB >= 0
    float lfo_phase = -1.0f;         // modulation: LFO phase 0..1; -2 = the oscillator is not running (switched off)
    float lfo2_phase = -1.0f;        // a second rotor / oscillator, same convention (a rotary cabinet's bass drum)
};

// Solver counters a circuit-level unit reports for /api/diag (EngineDiag.hpp). Cumulative since the unit's circuit was
// built (a rebuild may restart them); the diagnostics bank takes per-block deltas. Plain values, read on the thread
// that just rendered the unit.
struct UnitSolverStats {
    uint64_t samples = 0;           // circuit samples solved (internal rate)
    uint64_t newton_iterations = 0;
    uint64_t homotopies = 0;        // Newton failed at the full step: source-stepping homotopy tried
    uint64_t failures = 0;          // every fallback failed: the last solution was kept (audible as a glitch)
    uint64_t ptc_rescues = 0;       // pseudo-transient continuation solved what Newton (+ homotopy) could not
    uint64_t half_steps = 0;        // samples re-solved as two half steps after Newton failed
    uint64_t lte_substeps = 0;      // group-samples substepped by the truncation-error control
    int max_iterations = 0;
    double internal_rate = 0.0;     // Hz
    int oversampling = 1;
    bool multirate = false;
};

// Set (on the control thread) while the rack prepares an edit copy of a live unit: the copy's whole runtime state is
// about to be replaced by adoptRuntimeStateFrom() at the plan swap, so expensive start-up work such as settling a
// mains-fed circuit's supply can be skipped. A knob edit on a circuit amp took 0.5-0.8 s of control-thread time.
inline thread_local bool tl_prepareForAdoption = false;

class IComponent {
public:
    virtual ~IComponent() = default;
    // Audio thread, right after processAudio, only while the UI polls per-unit meters. Must be cheap (no locks).
    virtual void liveReadout(UnitLiveReadout&) const noexcept {}
    // Audio thread (or the core that rendered the unit), right after processAudio, while a diagnostics bank is attached.
    // Circuit units fill in their solver counters and return true. Must be cheap (no locks, no allocation).
    virtual bool solverStats(UnitSolverStats&) const noexcept { return false; }
    virtual std::vector<ExtraParam> extraParams() const { return {}; }
    // The rack tells each unit its position (used by units that publish per-slot live data).
    virtual void setSlotIndex(int) {}

    // Algorithmic latency in host samples (used by SplitNode parallel branch alignment).
    virtual uint32_t latencySamples() const noexcept { return 0; }
    // Series-unit pipelining (RackGraph): may this unit's block be processed as consecutive sub-blocks with output bit
    // for bit the same as one block? 0 = yes; 1 = yes when the rack gives it AudioBlock::mono_input (it picks a mono or a
    // stereo path, from that decision, or else by comparing the channels of each call); 2 = no (block-level state, own
    // partitioning, or untested).
    virtual int subBlockSafety() const noexcept { return 0; }
    // Audio thread, once per host block, before the unit renders: given an input that is one signal on both channels,
    // the output is too, bit for bit (both channels run the same arithmetic on the same state, or the unit renders one
    // channel and copies it). RackGraph propagates this from the rack input to decide AudioBlock::mono_input for the
    // units that pick a mono or a stereo path. False (the default) only costs CPU: such a unit after it renders stereo.
    virtual bool monoPreserving() const noexcept { return false; }

    // Bi-directional cabinet → power-amp impedance coupling hooks.
    virtual SpeakerImpedanceProfile speakerImpedanceProfile() const noexcept { return {}; }
    virtual void setSpeakerLoadProfile(const SpeakerImpedanceProfile&) noexcept {}

    // Control thread. Whether a cabinet follows this amp in its serial chain: -2 not tracked, -1 unknown, 0 none, 1 a cab
    // (RackGraph::syncCabFollow, before every plan publish; carried to detached copies). Circuit amps default to
    // quality "hq" (whole circuit oversampled) when none does. cabFollowNeedsPrepare: setting `cab` would change how
    // the unit renders, so the rack re-prepares a copy that no published plan shares.
    virtual int cabFollows() const noexcept { return -2; }
    // Control thread. The unit's output is already a miked cabinet (a circuit amp whose netlist ends in speaker + cab,
    // docs/CIRCUIT_ENGINE.md §10.21): the rack adds no cabinet after it and bypasses one that follows it (once).
    virtual bool includesCabinet() const noexcept { return false; }
    virtual void setCabFollows(int /*cab*/) noexcept {}
    virtual bool cabFollowNeedsPrepare(int /*cab*/) const noexcept { return false; }
    // The same question for a unit not prepared yet and not given its parameters (a rack build): whether preparing it
    // and then setting `cab` would make the rack prepare it again. The rack then sets `cab` and the parameters first and
    // prepares once, which ends in the same state (RackGraph::planDeferredCabFollow).
    virtual bool cabFollowNeedsPrepareBeforePrepare(int /*cab*/) const noexcept { return false; }

    // Snapshot of every stored parameter (schema order first), for preset saving.
    virtual std::vector<ExportedParam> exportParams() const { return {}; }
    // Optional live readout for a parameter whose meaning depends on state (e.g. a time knob shows
    // "1/8 D" while the unit is synced to the rig tempo). Control thread only.
    virtual bool paramDisplay(std::string_view /*param_id*/, std::string& /*out*/) const { return false; }
    // A two-position switch parameter as this unit's DSP reads its current value: 1 on, 0 off, -1 not a switch this
    // unit knows. The UI shows this rather than guessing from the value: catalogue switches rest at the midpoint
    // (raw 0.5, "untouched"), which most units read as off (> 0.5) and some as on (>= 0.5, or a circuit selector).
    virtual int switchState(std::string_view /*param_id*/) const { return -1; }

    virtual int32_t tableId() const noexcept = 0;
    virtual std::string_view referenceName() const noexcept = 0;
    virtual std::string_view displayName() const noexcept = 0;
    virtual catalog::EngineFamily family() const noexcept = 0;
    virtual const catalog::ComponentSchema& schema() const noexcept = 0;

    virtual void prepare(double sample_rate, uint32_t max_block_size) = 0;
    virtual void reset() = 0;
    // prepare(), parameter setting and processControlRate() touch only this instance (no shared caches, files or host
    // services), so a rack build may run them on a worker thread alongside other units (RackGraph::buildFromPreset).
    // Circuit units, whose prepare settles the supply for hundreds of milliseconds, say so; everything else stays serial.
    virtual bool preparesIndependently() const noexcept { return false; }

    // Audio-thread handoff only: transfer compatible DSP history without allocation.
    // The previous component will never render again; parameter maps stay untouched.
    virtual void adoptRuntimeStateFrom(IComponent&) noexcept {}

    virtual void setParameter(std::string_view param_id, float value) = 0;
    virtual void setStringParameter(std::string_view param_id, std::string_view value) = 0;
    virtual float getParameter(std::string_view param_id) const = 0;
    virtual std::string getStringParameter(std::string_view param_id) const = 0;

    virtual void applyModulationDelta(std::string_view param_id, float delta) = 0;
    virtual void clearModulationDeltas() = 0;

    virtual void processControlRate() = 0;
    virtual void processAudio(AudioBlock& block) = 0;

    bool isBypassed() const noexcept { return bypassed_.load(std::memory_order_relaxed); }
    void setBypassed(bool b) noexcept { bypassed_ = b; }

protected:
    std::atomic<bool> bypassed_{false};
};

bool switchReadsMidpointAsOn(int32_t table_id, std::string_view param_id) noexcept; // ComponentFactory.cpp (see below)

/**
 * Base component implementing full schema-backed parameter storage, modulation offsets,
 * `Pwr` / `on_off` / `bypass` handling, and clean sample-rate preparation for any of the 166 components.
 */
class SchemaComponentBase : public IComponent {
public:
    explicit SchemaComponentBase(const catalog::ComponentSchema& schema)
        : schema_(schema) {
        for (const auto& p : schema_.params) {
            ParamValue pv{};
            pv.kind = p.kind;
            pv.num_val = p.default_value;
            // Ensure power switch defaults to ON (1.0f) unless preset explicitly sets Pwr="0"
            if (p.id == "Pwr" || p.id == "on_off") {
                pv.num_val = 1.0f;
            }
            params_.emplace(std::string(p.id), std::move(pv));
        }
    }

    std::vector<ExportedParam> exportParams() const override {
        std::vector<ExportedParam> out;
        std::vector<std::string> seen;
        auto emit = [&](const std::string& id, const ParamValue& pv) {
            ExportedParam e;
            e.id = id;
            e.is_string = pv.kind == catalog::ParamKind::String;
            e.num = pv.num_val;
            e.str = pv.str_val;
            out.push_back(std::move(e));
            seen.push_back(id);
        };
        for (const auto& p : schema_.params) {
            if (auto it = params_.find(std::string(p.id)); it != params_.end()) emit(it->first, it->second);
        }
        for (const auto& kv : params_) {
            if (std::find(seen.begin(), seen.end(), kv.first) == seen.end()) emit(kv.first, kv.second);
        }
        return out;
    }

    int32_t tableId() const noexcept override { return schema_.table_id; }
    std::string_view referenceName() const noexcept override { return schema_.reference_name; }
    std::string_view displayName() const noexcept override { return schema_.display_name; }
    catalog::EngineFamily family() const noexcept override { return schema_.family; }
    const catalog::ComponentSchema& schema() const noexcept override { return schema_; }

    void prepare(double sample_rate, uint32_t max_block_size) override {
        host_sr_ = sample_rate > 1000.0 ? sample_rate : 48000.0;
        max_block_size_ = max_block_size;
        onPrepare(host_sr_, max_block_size_);
    }

    void reset() override {
        onReset();
    }

    void setParameter(std::string_view param_id, float value) override {
        auto& pv = params_[std::string(param_id)];
        pv.num_val = value;
        if (param_id == "Pwr" || param_id == "on_off") {
            bypassed_ = (value < 0.5f);
        } else if (param_id == "bypass") {
            bypassed_ = (value >= 0.5f);
        }
        onParameterChanged(param_id, value);
    }

    void setStringParameter(std::string_view param_id, std::string_view value) override {
        auto& pv = params_[std::string(param_id)];
        pv.kind = catalog::ParamKind::String;
        pv.str_val = std::string(value);
        onStringParameterChanged(param_id, value);
    }

    float getParameter(std::string_view param_id) const override {
        float base = 0.0f;
        if (auto it = params_.find(std::string(param_id)); it != params_.end()) {
            base = it->second.num_val;
        }
        if (auto mit = mod_deltas_.find(std::string(param_id)); mit != mod_deltas_.end()) {
            base += mit->second;
        }
        return base;
    }

    float getParamOr(std::string_view param_id, float fallback) const noexcept {
        float base = fallback;
        if (auto it = params_.find(std::string(param_id)); it != params_.end()) {
            base = it->second.num_val;
        }
        if (auto mit = mod_deltas_.find(std::string(param_id)); mit != mod_deltas_.end()) {
            base += mit->second;
        }
        return base;
    }

    // Behavioural units: every value away from the midpoint reads alike (> 0.5 on); at exactly 0.5 the unit's own
    // test decides (switchReadsMidpointAsOn, next to the DSP classes in ComponentFactory.cpp).
    int switchState(std::string_view param_id) const override {
        if (!params_.count(std::string(param_id))) return -1;
        const float raw = getParamOr(param_id, 0.0f);
        return raw > 0.5f || (raw == 0.5f && switchReadsMidpointAsOn(schema_.table_id, param_id)) ? 1 : 0;
    }

    float getNormalizedParam(std::initializer_list<std::string_view> keys, float fallback) const noexcept {
        auto isIgnored = [](const catalog::ParamSchema& p) noexcept {
            return p.id == "Pwr" || p.id == "on_off" || p.id == "bypass" ||
                   p.id == "CASSt" || p.id == "st" || p.id == "LL";
        };
        // Pass 1: exact match on id or display_name
        for (auto key : keys) {
            for (const auto& p : schema_.params) {
                if (isIgnored(p)) continue;
                if (iequals(p.id, key) || iequals(p.display_name, key)) {
                    float raw = getParamOr(p.id, p.default_value);
                    float pmin = 0.0f, pmax = 1.0f;
                    getParamEffectiveRange(p, pmin, pmax);
                    return std::clamp((raw - pmin) / (pmax - pmin), 0.0f, 1.0f);
                }
            }
        }
        // Pass 2: substring match (only for keys of length >= 3, skipping frequency/mode switches when looking for gain/eq)
        for (auto key : keys) {
            if (key.size() < 3) continue;
            for (const auto& p : schema_.params) {
                if (isIgnored(p)) continue;
                if (icontains(p.id, "freq") && !icontains(key, "freq")) continue;
                if (icontains(p.id, "mode") && !icontains(key, "mode")) continue;
                if (icontains(p.id, "boost") && !icontains(key, "boost")) continue;
                if (icontains(p.id, "saver") && !icontains(key, "saver")) continue;
                if (icontains(p.id, key) || icontains(p.display_name, key)) {
                    float raw = getParamOr(p.id, p.default_value);
                    float pmin = 0.0f, pmax = 1.0f;
                    getParamEffectiveRange(p, pmin, pmax);
                    return std::clamp((raw - pmin) / (pmax - pmin), 0.0f, 1.0f);
                }
            }
        }
        return fallback;
    }

    std::string getStringParameter(std::string_view param_id) const override {
        if (auto it = params_.find(std::string(param_id)); it != params_.end()) {
            return it->second.str_val;
        }
        return {};
    }

    void applyModulationDelta(std::string_view param_id, float delta) override {
        mod_deltas_[std::string(param_id)] += delta;
    }

    void clearModulationDeltas() override {
        for (auto& kv : mod_deltas_) {
            kv.second = 0.0f;
        }
    }

    void processControlRate() override {
        onProcessControlRate();
    }

    void processAudio(AudioBlock& block) override {
        if (bypassed_) {
            block.copyInToOut();
            return;
        }
        onProcessAudio(block);
    }

protected:
    static bool iequals(std::string_view a, std::string_view b) noexcept {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i]))) {
                return false;
            }
        }
        return true;
    }

    static bool icontains(std::string_view hay, std::string_view needle) noexcept {
        if (needle.empty() || needle.size() > hay.size()) return false;
        for (std::size_t i = 0; i <= hay.size() - needle.size(); ++i) {
            if (iequals(hay.substr(i, needle.size()), needle)) return true;
        }
        return false;
    }

    virtual void onPrepare(double /*sample_rate*/, uint32_t /*max_block_size*/) {}
    virtual void onReset() {}
    virtual void onParameterChanged(std::string_view /*param_id*/, float /*value*/) {}
    virtual void onStringParameterChanged(std::string_view /*param_id*/, std::string_view /*value*/) {}
    virtual void onProcessControlRate() {}
    virtual void onProcessAudio(AudioBlock& block) {
        block.copyInToOut();
    }

    const catalog::ComponentSchema& schema_;
    double host_sr_ = 48000.0;
    uint32_t max_block_size_ = 512;
    std::unordered_map<std::string, ParamValue> params_;
    std::unordered_map<std::string, float> mod_deltas_;
};

std::unique_ptr<IComponent> createComponentByTableId(int32_t table_id);
// Two-position switches (IComponent::switchState): whether the behavioural model of `table_id` reads a raw value of
// exactly 0.5 as on (`>= 0.5`) rather than off (`> 0.5`, the catalogue convention for an untouched switch).
bool switchReadsMidpointAsOn(int32_t table_id, std::string_view param_id) noexcept;
// The same answer without a live instance (the component catalogue's defaults): 1 on, 0 off.
int switchStateFor(int32_t table_id, std::string_view param_id, float raw);
// Which DSP implementation backs a component: amp, drive, cab, ir_reverb, fdn_reverb, delay, mod,
// dynamics, eq, routing, fallback (generic EQ stand-in) or none.
const char* implementationKind(int32_t table_id);

} // namespace rig
