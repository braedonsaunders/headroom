#include "HeadroomPluginCore.hpp"
#include "../../src/audio/RealtimeAudioServer.hpp"
#include "../../src/core/RtPool.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>

#ifndef HEADROOM_DEFAULT_RESEARCH_ROOT
#define HEADROOM_DEFAULT_RESEARCH_ROOT ""
#endif

namespace headroom::plug {
std::string PluginCore::apiToken() const { return server_ ? server_->apiToken() : std::string{}; }


namespace {
constexpr int kTrimRamp = 64;           // samples; fixed so renders don't depend on host block partitioning
constexpr double kBypassFadeSec = 0.010;

const ParamSpec kSpecs[] = {
    {kInputTrim, "Input", "In", "dB", -24.0f, 24.0f, 0.0f, false},
    {kOutputTrim, "Output", "Out", "dB", -36.0f, 12.0f, 0.0f, false},
    {kBypass, "Bypass", "Byp", "", 0.0f, 1.0f, 0.0f, true},
};

float dbToLin(float db) noexcept { return std::pow(10.0f, db / 20.0f); }
bool rateOk(double sr) noexcept { return std::isfinite(sr) && sr >= 8000.0 && sr <= 384000.0; }
std::string oneLine(std::string s) {
    for (char& c : s) if (c == '\n' || c == '\r') c = ' ';
    return s;
}
} // namespace

const ParamSpec& paramSpec(uint32_t id) {
    return kSpecs[std::clamp<uint32_t>(id, kFirstHostParam, kParamIdEnd - 1) - kFirstHostParam];
}

// ───────────── process-wide warm-up ownership ─────────────
// One live instance (one that has opened its editor) runs the amp / tube warm-up. When the owner goes
// away its worker is joined first (stopHosted), then ownership moves to another instance that has had its
// editor opened. Only this registry starts warm-ups, so they are never orphaned or double-started.
struct WarmupRegistry {
    static WarmupRegistry& get() { static WarmupRegistry r; return r; }
    std::mutex m;
    std::vector<std::pair<PluginCore*, rig::audio::RealtimeAudioServer*>> candidates;
    PluginCore* owner = nullptr;

    void editorOpened(PluginCore* c, rig::audio::RealtimeAudioServer* srv) {
        std::lock_guard<std::mutex> lk(m);
        if (std::none_of(candidates.begin(), candidates.end(), [&](const auto& e) { return e.first == c; })) candidates.push_back({c, srv});
        if (!owner) {
            owner = c;
            srv->startHostedWarmup();
        }
    }
    // Before the instance's server stops: it can no longer be handed ownership. Returns whether it owns it.
    bool withdraw(PluginCore* c) {
        std::lock_guard<std::mutex> lk(m);
        candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&](const auto& e) { return e.first == c; }), candidates.end());
        return owner == c;
    }
    // After the old owner's worker has been joined.
    void handOff(PluginCore* old) {
        std::lock_guard<std::mutex> lk(m);
        if (owner != old) return;
        owner = nullptr;
        if (!candidates.empty()) {
            owner = candidates.front().first;
            candidates.front().second->startHostedWarmup();
        }
    }
};

std::string resolveResearchRoot(std::string* settings_path) {
    namespace fs = std::filesystem;
    const std::string settings = rig::library::appSupportDir() + "/plugin-settings.txt";
    if (settings_path) *settings_path = settings;
    std::string root;
    if (const char* env = std::getenv("HEADROOM_RESEARCH_ROOT"); env && *env) root = env;
    std::error_code ec;
    if (root.empty()) {
        std::ifstream f(settings);
        std::string line;
        while (std::getline(f, line))
            if (line.rfind("research_root=", 0) == 0) root = line.substr(14);
        while (!root.empty() && (root.back() == ' ' || root.back() == '\r' || root.back() == '/')) root.pop_back();
    }
    if (root.empty()) {
        root = fs::path(HEADROOM_DEFAULT_RESEARCH_ROOT).lexically_normal().string();
        while (root.size() > 1 && root.back() == '/') root.pop_back();
        if (!fs::exists(settings, ec)) {
            std::ofstream f(settings);
            f << "# Headroom plugin (AU / VST3) settings.\n"
                 "# research_root: the folder that contains app/presets/Showcase\n"
                 "# (bundled rigs and resources). Reload the plugin after changing it.\n"
                 "research_root="
              << root << "\n";
        }
    }
    return root;
}

// ───────────── PluginCore ─────────────
namespace {
rig::RtPool* retainPool() {
    auto& p = rig::RtPool::instance();
    p.retainUsers(); // workers run while any instance exists; the last instance joins them (binary unload)
    return &p;
}
} // namespace

PluginCore::PluginCore(Format f, std::string resource_dir)
    : format_(f), resource_dir_(std::move(resource_dir)), pool_(retainPool()), wg_slot_(pool_->acquireWorkgroupSlot()) {
    for (auto& v : host_value_) v.store(0.0f);
    for (uint32_t id : kHostParams) host_value_[id].store(paramSpec(id).def);
    for (auto& s : direct_seq_) s.store(0);
    direct_seen_.fill(0);
    in_gain_.reset(1.0f);
    out_gain_.reset(1.0f);
    wet_.reset(1.0f);
}

PluginCore::~PluginCore() {
    {
        std::lock_guard<std::mutex> lk(control_mutex_);
        stopEngineLocked();
    }
    pool_->releaseWorkgroupSlot(wg_slot_);
    pool_->releaseUsers();
}

void PluginCore::setRenderWorkgroup(void* os_workgroup) noexcept { pool_->setWorkgroupPtr(wg_slot_, os_workgroup); }

PluginCore::PoolStats PluginCore::poolStats() const noexcept {
    const auto s = pool_->stats();
    return {s.forks, s.on_worker, s.reclaimed, s.refused, s.workers, s.active, s.joined_workgroup, s.workgroup_max_parallel};
}

std::string PluginCore::engineError() const {
    std::lock_guard<std::mutex> lk(control_mutex_);
    return engine_error_;
}

std::string PluginCore::settingsFilePath() const { return rig::library::appSupportDir() + "/plugin-settings.txt"; }

bool PluginCore::startEngineLocked(double sr) {
    if (server_) return true;
    if (engine_failed_) return false;
    const std::string root = resolveResearchRoot();
    auto s = std::make_unique<rig::audio::RealtimeAudioServer>();
    s->setHostLatencySink(&latency_samples_);
    std::string err;
    if (!s->startHosted(root, sr, &err)) { // startHosted joins whatever it started before returning false
        engine_error_ = err.empty() ? std::string("engine failed to start") : err;
        engine_failed_ = true;
        std::fprintf(stderr, "Headroom plugin: %s (research root: %s)\n", engine_error_.c_str(), root.c_str());
        return false;
    }
    server_ = std::move(s);
    return true;
}

void PluginCore::stopEngineLocked() {
    server_live_.store(nullptr, std::memory_order_release);
    if (!server_) return;
    const bool owned = WarmupRegistry::get().withdraw(this);
    server_->stopHosted(); // joins HTTP, library and worker threads, then reclaims the render runtime
    if (owned) WarmupRegistry::get().handOff(this);
    server_.reset();
}

bool PluginCore::prepare(double sample_rate, uint32_t max_frames) {
    if (!std::isfinite(sample_rate) || sample_rate <= 0.0 || max_frames == 0 || max_frames > 65536) return false;
    pool_->collectRetired(); // workgroups the render thread swapped out are released here, off the callback
    std::string pending;
    {
        std::lock_guard<std::mutex> lk(control_mutex_);
        server_live_.store(nullptr, std::memory_order_release);
        sample_rate_ = sample_rate;
        max_frames_ = max_frames;
        dry_l_.assign(max_frames, 0.0f);
        dry_r_.assign(max_frames, 0.0f);
        trim_l_.assign(max_frames, 0.0f);
        trim_r_.assign(max_frames, 0.0f);
        bypass_l_.assign(kDryRingSize, 0.0f);
        bypass_r_.assign(kDryRingSize, 0.0f);
        bypass_pos_ = 0;
        latency_samples_.store(0, std::memory_order_release);
        in_gain_.reset(dbToLin(host_value_[kInputTrim].load()));
        out_gain_.reset(dbToLin(host_value_[kOutputTrim].load()));
        param_bypass_ = host_value_[kBypass].load() >= 0.5f;
        last_bypass_ = param_bypass_ || host_bypass_prop_.load();
        wet_.reset(last_bypass_ ? 0.0f : 1.0f);
        for (uint32_t i = 0; i < kParamIdEnd; ++i) direct_seen_[i] = direct_seq_[i].load();
        faulted_ = false;
        // Rates the engine isn't built for (validators probe e.g. 1234.5678 Hz): stay suspended, pass dry.
        if (!rateOk(sample_rate)) return true;
        if (!startEngineLocked(sample_rate)) return true;    // dry pass-through; the editor shows engine_error_
        if (!server_->prepareHost(sample_rate)) return false; // stays suspended (server_live_ == nullptr)
        pending = std::move(pending_state_);                 // state that arrived before the engine existed
        pending_state_.clear();
        if (pending.empty()) {
            server_live_.store(server_.get(), std::memory_order_release);
            return true;
        }
    }
    const bool ok = loadState(pending);
    std::lock_guard<std::mutex> lk(control_mutex_);
    if (!ok) std::fprintf(stderr, "Headroom plugin: saved state could not be restored; keeping the default rig\n");
    if (server_) server_live_.store(server_.get(), std::memory_order_release);
    return true;
}

void PluginCore::suspend() {
    {
        std::lock_guard<std::mutex> lk(control_mutex_);
        server_live_.store(nullptr, std::memory_order_release);
    }
    pool_->collectRetired();
}

void PluginCore::requestReset() {
    std::lock_guard<std::mutex> lk(control_mutex_);
    if (server_) server_->requestHostedReset();
}

uint16_t PluginCore::attachEditor() {
    std::string pending;
    {
        std::lock_guard<std::mutex> lk(control_mutex_);
        if (!server_ && !engine_failed_ && startEngineLocked(sample_rate_)) {
            pending = std::move(pending_state_); // nothing renders on a just-started engine yet
            pending_state_.clear();
        }
    }
    if (!pending.empty()) loadState(pending);
    std::lock_guard<std::mutex> lk(control_mutex_);
    if (!server_) return 0;
    server_->startHostedEditor();  // component bank + HTTP accept loop (background)
    server_->startHostedLibrary(); // preset browser index (background, shared per process)
    WarmupRegistry::get().editorOpened(this, server_.get());
    return server_->boundHttpPort();
}

float PluginCore::paramValue(uint32_t id) const noexcept {
    return isHostParam(id) ? host_value_[id].load(std::memory_order_relaxed) : 0.0f;
}

void PluginCore::setParamDirect(uint32_t id, float value) noexcept {
    if (!isHostParam(id) || !std::isfinite(value)) return;
    const auto& sp = paramSpec(id);
    host_value_[id].store(std::clamp(value, sp.min, sp.max), std::memory_order_relaxed);
    direct_seq_[id].fetch_add(1, std::memory_order_release);
}

void PluginCore::setTransport(const Transport& t) noexcept {
    if (!t.valid) return;
    if (auto* s = server_live_.load(std::memory_order_acquire)) s->setHostTransport(t.bpm, t.beat, t.playing);
}

// ───────────── render ─────────────
void PluginCore::applyEventRT(const ParamEvent& e) noexcept {
    if (!isHostParam(e.id) || !std::isfinite(e.value)) return;
    const auto& sp = paramSpec(e.id);
    const float v = std::clamp(e.value, sp.min, sp.max);
    host_value_[e.id].store(v, std::memory_order_relaxed);
    switch (e.id) {
        case kInputTrim: in_gain_.set(dbToLin(v), kTrimRamp); break;
        case kOutputTrim: out_gain_.set(dbToLin(v), kTrimRamp); break;
        case kBypass: param_bypass_ = v >= 0.5f; break;
        default: break;
    }
}

void PluginCore::renderSegment(uint32_t from, uint32_t to, float* outL, float* outR) noexcept {
    if (to <= from) return;
    const bool bypass = param_bypass_ || host_bypass_prop_.load(std::memory_order_relaxed);
    if (bypass != last_bypass_) {
        last_bypass_ = bypass;
        wet_.set(bypass ? 0.0f : 1.0f, std::max(1, static_cast<int>(sample_rate_ * kBypassFadeSec)));
    }
    auto* srv = server_live_.load(std::memory_order_acquire);
    if (srv && !faulted_) {
        for (uint32_t i = from; i < to; ++i) {
            const float g = in_gain_.next();
            trim_l_[i] = dry_l_[i] * g;
            trim_r_[i] = dry_r_[i] * g;
        }
        try {
            srv->renderHost(trim_l_.data() + from, trim_r_.data() + from, outL + from, outR + from, to - from);
        } catch (...) { // never let an exception unwind into the host; dry until the host prepares again
            faulted_ = true;
            faults_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (!srv || faulted_) {
        std::memcpy(outL + from, dry_l_.data() + from, sizeof(float) * (to - from));
        std::memcpy(outR + from, dry_r_.data() + from, sizeof(float) * (to - from));
        return;
    }
    for (uint32_t i = from; i < to; ++i) {
        const float o = out_gain_.next(), w = wet_.next();
        const uint32_t delay = std::min(latencySamples(), kDryRingSize - 1);
        bypass_l_[bypass_pos_] = dry_l_[i];
        bypass_r_[bypass_pos_] = dry_r_[i];
        const uint32_t read = (bypass_pos_ - delay) & (kDryRingSize - 1);
        const float dl = bypass_l_[read], dr = bypass_r_[read];
        bypass_pos_ = (bypass_pos_ + 1) & (kDryRingSize - 1);
        outL[i] = dl + w * (outL[i] * o - dl);
        outR[i] = dr + w * (outR[i] * o - dr);
    }
}

void PluginCore::process(const float* inL, const float* inR, float* outL, float* outR, uint32_t n,
                         const ParamEvent* events, uint32_t num_events) noexcept {
    // Block-start values set through setParamDirect (AU SetParameter, restored state).
    for (uint32_t id : kHostParams) {
        const uint32_t s = direct_seq_[id].load(std::memory_order_acquire);
        if (s != direct_seen_[id]) {
            direct_seen_[id] = s;
            applyEventRT({0, id, host_value_[id].load(std::memory_order_relaxed)});
        }
    }
    if (n == 0 || !outL || !outR || max_frames_ == 0) { // parameter flush, or never prepared
        for (uint32_t i = 0; events && i < num_events; ++i) applyEventRT(events[i]);
        if (n && outL) std::memset(outL, 0, sizeof(float) * n);
        if (n && outR) std::memset(outR, 0, sizeof(float) * n);
        return;
    }
    if (!inR) inR = inL;
    uint32_t ev = 0;
    for (uint32_t pos = 0; pos < n; pos += max_frames_) {
        const uint32_t len = std::min(max_frames_, n - pos);
        // Copy the input first: the host may process in place (out == in).
        if (inL) {
            std::memcpy(dry_l_.data(), inL + pos, sizeof(float) * len);
            std::memcpy(dry_r_.data(), inR + pos, sizeof(float) * len);
        } else {
            std::memset(dry_l_.data(), 0, sizeof(float) * len);
            std::memset(dry_r_.data(), 0, sizeof(float) * len);
        }
        float* oL = outL + pos;
        float* oR = outR + pos;
        uint32_t seg = 0;
        while (events && ev < num_events && events[ev].offset < pos + len) {
            const uint32_t at = std::max(seg, events[ev].offset > pos ? events[ev].offset - pos : 0u);
            renderSegment(seg, at, oL, oR);
            seg = at;
            applyEventRT(events[ev]);
            ++ev;
        }
        renderSegment(seg, len, oL, oR);
    }
    while (events && ev < num_events) applyEventRT(events[ev++]); // offsets ≥ n: apply at block end
}

// ───────────── state ─────────────
// Text blob: "HEADROOM-STATE 1\n", key=value lines (host params, rig metadata, the 8 macro values captured
// under the same engine lock as the rig), then "rig_bytes=N\n" followed by exactly N bytes of .rig XML (the
// user-preset format). Trims/bypass are included so formats whose host doesn't keep parameter values
// separately (VST3 component state) restore everything.
std::string PluginCore::saveState() {
    std::lock_guard<std::mutex> lk(control_mutex_);
    if (!server_ && !pending_state_.empty()) return pending_state_;
    std::ostringstream o;
    o.precision(9);
    o << "HEADROOM-STATE 1\n";
    o << "in_trim=" << host_value_[kInputTrim].load() << "\nout_trim=" << host_value_[kOutputTrim].load()
      << "\nbypass=" << (host_value_[kBypass].load() >= 0.5f ? 1 : 0) << "\n";
    if (server_) {
        const auto st = server_->captureHostedState();
        o << "name=" << oneLine(st.name) << "\nauthor=" << oneLine(st.author) << "\ncategory=" << oneLine(st.category)
          << "\nsource=" << oneLine(st.source) << "\npreset_idx=" << st.preset_idx << "\ninput_gain=" << st.input_gain
          << "\nmaster_bypass=" << (st.master_bypass ? 1 : 0) << "\ndirty=" << (st.dirty ? 1 : 0)
          << "\nmacros=";
        for (std::size_t c = 0; c < st.macros.size(); ++c) o << (c ? "," : "") << st.macros[c];
        o << "\nrig_bytes=" << st.rig_xml.size() << "\n" << st.rig_xml;
    }
    return o.str();
}

bool PluginCore::loadState(const std::string& blob) {
    if (blob.rfind("HEADROOM-STATE 1\n", 0) != 0) return false;
    rig::audio::RealtimeAudioServer::HostedState st;
    bool has_rig = false;
    float in_trim = 0.0f, out_trim = 0.0f, byp = 0.0f;
    std::size_t pos = blob.find('\n') + 1;
    while (pos < blob.size()) {
        const std::size_t nl = blob.find('\n', pos);
        if (nl == std::string::npos) break;
        const std::string line = blob.substr(pos, nl - pos);
        pos = nl + 1;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        try {
            if (k == "in_trim") in_trim = std::stof(v);
            else if (k == "out_trim") out_trim = std::stof(v);
            else if (k == "bypass") byp = std::stof(v);
            else if (k == "name") st.name = v;
            else if (k == "author") st.author = v;
            else if (k == "category") st.category = v;
            else if (k == "source") st.source = v;
            else if (k == "preset_idx") st.preset_idx = std::stoi(v);
            else if (k == "input_gain") st.input_gain = std::stof(v);
            else if (k == "preset_gain") st.legacy_preset_gain = std::stof(v); // older builds: folded into knobs on restore
            else if (k == "master_bypass") st.master_bypass = v == "1";
            else if (k == "dirty") st.dirty = v == "1";
            else if (k == "macros") {
                std::istringstream ms(v);
                std::string tok;
                for (std::size_t c = 0; c < st.macros.size() && std::getline(ms, tok, ','); ++c) st.macros[c] = std::stof(tok);
                st.has_macros = true;
            } else if (k == "rig_bytes") {
                const std::size_t nbytes = static_cast<std::size_t>(std::stoull(v));
                if (pos + nbytes > blob.size()) return false;
                st.rig_xml = blob.substr(pos, nbytes);
                has_rig = true;
                break;
            }
        } catch (...) {
            return false;
        }
    }
    std::lock_guard<std::mutex> lk(control_mutex_);
    if (has_rig && server_) {
        std::string err;
        if (!server_->restoreHostedState(st, &err)) {
            std::fprintf(stderr, "Headroom plugin: state restore failed: %s\n", err.c_str());
            return false; // host params untouched too: a state applies all-or-nothing
        }
    } else if (has_rig) {
        pending_state_ = blob; // applied when the engine starts
    }
    setParamDirect(kInputTrim, in_trim);
    setParamDirect(kOutputTrim, out_trim);
    setParamDirect(kBypass, byp);
    return true;
}

} // namespace headroom::plug
