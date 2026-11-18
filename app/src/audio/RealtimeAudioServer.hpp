#pragma once
#include "../core/RackGraph.hpp"
#include "../core/RealtimeGraphRuntime.hpp"
#include "../preset/PresetLoader.hpp"
#include "../dsp/TubeAmpComponents.hpp"
#include "../core/EngineFingerprint.hpp" // fnv1a64 (level migration: content hash of a rig file)
#include "../core/LevelFold.hpp"         // one-time level migration of user rigs into their own knobs
#include "../library/Library.hpp"
#include "WavIO.hpp"
#include "AudioIO.hpp"
#if defined(_WIN32) && RIG_WITH_ASIO
#include "AudioBackend.hpp"
#endif
#include "../midi/Synth.hpp"
#include "../midi/MidiOut.hpp"
#include "../midi/MidiIn.hpp"
#include "../analysis/ListenAnalyzer.hpp"
#include "../midi/MidiComponents.hpp"
#include "../dsp/NamComponent.hpp"
#include "../dsp/ComponentModel.hpp"
#include "../dsp/LooperUnit.hpp"
#include "../dsp/LoopStation.hpp"
#include "../dsp/PitchDoublerUnits.hpp"
#include "../dsp/ToneMatchAndCalibration.hpp"
#include "../core/RigScenes.hpp"
#include "../plugins/PluginHost.hpp"
#include "../dsp/ConvolutionEngine.hpp"
#include "BackingTracks.hpp" // Practice view: backing-track player + /api/backing (mixed in after the rig)
#include "json.hpp"

#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>
#else
#include <cstdint>
using UInt32 = std::uint32_t; // the render path uses CoreAudio's integer typedef
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <charconv>
#include <cerrno>
#include <cstdlib>
#include <random>
#include <type_traits>
#include <stdexcept>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <map>
#include <set>
#include <deque>
#include <functional>
#include <condition_variable>
#include <optional>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif
#include <vector>

#include <csignal>
#include "../platform/Denormals.hpp"
#include "../platform/Paths.hpp"
#include "../platform/Socket.hpp" // POSIX sockets / Winsock2

namespace rig::audio {

// DI player library entry (metadata only; audio is loaded when the track is selected).
struct DISampleTrack {
    std::string title;
    std::string group;   // sub-folder ("Built-in" for the top level)
    std::string path;
    double duration_sec = 0.0;
};
// Decoded, resampled, peak-normalised mono audio of the playing DI track.
struct TrackPCM {
    std::vector<int16_t> mono; // 16-bit keeps hour-long sessions small; the sources are 16-bit anyway
    static constexpr float kScale = 1.0f / 32767.0f;
};

// Sample rate and frame count from a WAV header, without reading the audio.
inline bool wavHeaderInfo(const std::string& path, uint32_t& sr, double& seconds) {
    std::ifstream f(path, std::ios::binary);
    char hdr[12];
    if (!f.read(hdr, 12) || std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0) return false;
    uint16_t ch = 1, bits = 16;
    sr = 0;
    char ck[8];
    while (f.read(ck, 8)) {
        uint32_t sz = 0;
        std::memcpy(&sz, ck + 4, 4);
        if (std::memcmp(ck, "fmt ", 4) == 0) {
            char fmt[16];
            if (!f.read(fmt, 16)) return false;
            std::memcpy(&ch, fmt + 2, 2);
            std::memcpy(&sr, fmt + 4, 4);
            std::memcpy(&bits, fmt + 14, 2);
            f.seekg(static_cast<std::streamoff>(sz - 16 + (sz & 1)), std::ios::cur);
        } else if (std::memcmp(ck, "data", 4) == 0) {
            const double frame = std::max(1, ch * (bits / 8));
            seconds = sr ? (sz / frame) / sr : 0.0;
            return sr > 0;
        } else {
            f.seekg(static_cast<std::streamoff>(sz + (sz & 1)), std::ios::cur);
        }
    }
    return false;
}

struct PresetEntry {
    std::string title;
    std::string author;
    std::string category;
    std::string tag_group;
    std::string rig_filename;
};

class RealtimeAudioServer {
    // Every model mutation is serialized and publishes a fully prepared plan
    // before releasing the control lock. The audio callback never sees this lock.
    class GraphEditGuard {
    public:
        explicit GraphEditGuard(RealtimeAudioServer& server, bool publish = true)
            : server_(server), lock_(server.graph_mutex_),
              previous_(publish && server.graph_ ? server.graph_->controlSnapshot() : nullptr),
              lineage_(server.graph_lineage_), exceptions_(std::uncaught_exceptions()), publish_(publish) {}
        void unchanged() noexcept { publish_ = false; }
        // The edit only moved continuous parameters: the plan's handoff needs no output bridge.
        void continuous(bool c = true) noexcept { continuous_ = c; }
        ~GraphEditGuard() {
            if (!publish_) return;
            if (std::uncaught_exceptions() > exceptions_) { rollback(); return; }
            try {
                server_.publishRenderPlanLocked(continuous_);
            } catch (const std::exception& e) {
                rollback();
                std::fprintf(stderr, "Headroom: preparing render plan failed: %s\n", e.what());
            } catch (...) {
                rollback();
                std::fprintf(stderr, "Headroom: preparing render plan failed\n");
            }
        }
    private:
        void rollback() noexcept {
            server_.graph_ = std::move(previous_);
            server_.graph_lineage_ = lineage_;
        }
        RealtimeAudioServer& server_;
        std::unique_lock<std::mutex> lock_;
        std::unique_ptr<RackGraph> previous_;
        uint64_t lineage_;
        int exceptions_;
        bool publish_;
        bool continuous_ = false;
    };
public:
    // Set before starting the hosted server; the owner outlives stopHosted().
    void setHostLatencySink(std::atomic<uint32_t>* sink) noexcept { host_latency_ = sink; }
    RealtimeAudioServer() {
        std::lock_guard<std::mutex> lock(noise_servers_mutex_);
        noise_servers_.push_back(this);
    }
    ~RealtimeAudioServer() {
        {
            std::lock_guard<std::mutex> lock(noise_servers_mutex_);
            noise_servers_.erase(std::remove(noise_servers_.begin(), noise_servers_.end(), this), noise_servers_.end());
        }
        stop();
    }
    const std::string& apiToken() const noexcept { return api_token_; }

    bool start(const std::string& repo_root, uint16_t http_port = 17770, bool enable_audio = true) {
        repo_root_ = repo_root;
        http_port_ = http_port;
#ifdef SIGPIPE
        ::signal(SIGPIPE, SIG_IGN); // a browser closing a socket mid-response must never kill the app
#endif

        loadCircuitNoiseSetting();
        loadPresetsCatalog();
        setNeuralGuitarModelPath(repo_root_ + "/app/resources/models/guitar_midi.hgnn");
        rig::platform::setAppRoot(repo_root_); // shipped-content root for engine lookups (NAM demo captures, …)
        global_looper_.prepare(sample_rate_.load());
        global_transpose_.prepare(sample_rate_.load());
        global_space_.prepare(sample_rate_.load());
        backing_.start(sample_rate_.load(), BackingTracks::defaultDir()); // Practice backing tracks
        rescanUserIrs();
        midi_out_.start();
        setGlobalMidiSink([](void* ctx, const midi::MidiEvent& e) { static_cast<midi::MidiOut*>(ctx)->push(e); }, &midi_out_);
        // Hardware controllers (foot switches, expression pedals): CCs through the controller map to their targets.
        midi_in_.start([this](int cc, int val, int ch) { dispatchMidiTarget(midi_map_.handleIncomingCc(cc, val, ch), val); });
        comp_bank_.scan();   // Schema Init presets only
        loadDISamples();
        loadActivePreset(0);
        running_.store(true);
        worker_ = std::thread([this] { workerLoop(); });
        startWarmup();
        if (const char* dl = std::getenv("HEADROOM_DIAG_LOG"); dl && *dl) startDiagLog(dl); // 1 JSON line / s of /api/diag
        library_thread_ = std::thread([this] {
            rig::platform::ScopedNoDenormalsX86 ftz; // x86: FTZ/DAZ (no-op on Apple Silicon)
            std::vector<library::FeaturedSpec> featured;
            for (const auto& pe : presets_) featured.push_back({pe.rig_filename, pe.title, pe.category});
            library_.scan(library::appSupportDir() + "/User Presets", featured, showcaseDir(),
                          libraryDir());
            library_ready_.store(true);
        });

        if (enable_audio) {
            loadAudioSettings();
            if (audio_settings_.source == "live" && AudioIO::micPermission() == "undetermined") AudioIO::requestMicPermission();
            if (!startAudio() && !asioRequested()) {
                audio_settings_ = AudioSettings{};
                if (!startAudio()) return false;
            }
        }
        if (!startHttpServer()) {
            return false;
        }
        return true;
    }

    void stop() {
        running_.store(false);
        ++preset_load_gen_; // a rig build in progress stops between units
        stopDiagLog();
        if (library_thread_.joinable()) library_thread_.join();
        { std::lock_guard<std::mutex> lk(job_mtx_); } // the idle worker may wait without a timeout: no lost wakeup
        job_cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        if (server_fd_ >= 0) {
            rig::net::shutdownListener(server_fd_); // wakes accept() (close alone doesn't on Linux)
            rig::net::closeSocket(server_fd_);
            server_fd_ = -1;
        }
        if (http_thread_.joinable()) {
            http_thread_.join();
        }
        // A final /api/listen request can start the analyzer until the HTTP thread exits.
        listen_.stop();
        stopPresetLoader();
        if (!hosted_) PreparedUnitPool::instance().stop();
        stopSettingsSaver();
        audio_io_.stop();
        graph_runtime_.stop(); // callback has synchronously stopped; reclaim off audio
    }

    // Bundled showcase rigs: next to the app bundle's resources when packaged, else the source tree.
    std::string showcaseDir() const {
        if (const char* env = std::getenv("HEADROOM_SHOWCASE_DIR")) return env;
        return repo_root_ + "/app/presets/Showcase";
    }

    std::string contentRoot() const {
        return showcaseDir();
    }

    // Headroom's shipped rig library (app/presets/Library/<Category>/*.rig), built by tools/library.
    std::string libraryDir() const {
        if (const char* env = std::getenv("HEADROOM_LIBRARY_DIR")) return env;
        return repo_root_ + "/app/presets/Library";
    }

    // A library / catalog preset, resolved to its file and labels.
    struct ResolvedPreset {
        std::string path, name, author, category, source = "factory";
    };
    bool resolvePreset(int idx, ResolvedPreset& r) const {
        library::LibraryEntry e;
        if (library_ready_.load() && library_.entry(idx, e)) {
            r.path = e.path;
            r.name = e.name;
            r.author = e.author;
            r.category = e.category;
            r.source = e.user ? "user" : e.showcase ? "showcase" : e.library ? "library" : "factory";
        } else if (idx >= 0 && idx < static_cast<int>(presets_.size())) {
            r.path = contentRoot() + "/" + presets_[idx].rig_filename;
            r.name = presets_[idx].title;
            r.author = presets_[idx].author;
            r.category = presets_[idx].category;
            r.source = "factory";
        } else {
            return false;
        }
        return true;
    }

    // Synchronous load (start-up, plug-in host state, tools). Supersedes any load the preset loader is running.
    bool loadActivePreset(int idx) {
        ResolvedPreset r;
        if (!resolvePreset(idx, r)) return false;
        return loadResolvedPreset(idx, r, ++preset_load_gen_, nullptr);
    }

    // Builds the rig off the model lock (the old rig keeps playing), then swaps it in. `gen` is this load's generation
    // (preset_load_gen_): a newer load supersedes it, which stops its build between units and skips the swap.
    // Returns true when the rig was installed; on a read failure `err` says why.
    bool loadResolvedPreset(int idx, const ResolvedPreset& r, uint64_t gen, std::string* err) {
        const std::string& path = r.path;
        const std::string& source = r.source;
        DiagLoadScope load_diag(*this, idx, r); // /api/diag preset_loads (recorded on return or throw)
        preset::RackPreset p;
        std::string perr;
        if (!preset::loadPreset(path, p, &perr)) {
            if (err) *err = perr.empty() ? "the rig file could not be read" : perr;
            load_diag.e.error = err ? *err : perr;
            return false;
        }
        // The rig plays at the level its units give: nothing measures or re-levels it. A user rig the old engine
        // levelled at run time gets that gain folded into its own level knobs once (migrateUserRigLevel).
        if (source == "user") load_diag.e.level_migrated = migrateUserRigLevel(path, r.name, p);
        auto superseded = [this, gen] { return preset_load_gen_.load() != gen; };
        auto new_graph = std::make_unique<RackGraph>();
        new_graph->setParallelPrepare(true); // circuit units settle on worker threads (RackGraph::buildFromPreset)
        new_graph->setCancelCheck(superseded);
        // The standalone app keeps prepared spares of the units of the rigs it loads (PreparedUnitPool): a rig whose
        // amps and pedals were loaded before builds without settling them again. The pool waits while this builds.
        const bool spares = !hosted_;
        new_graph->setUseSpares(spares);
        if (spares) PreparedUnitPool::instance().hold(true);
        try {
            new_graph->buildFromPreset(p, sample_rate_, 512);
        } catch (...) {
            if (spares) PreparedUnitPool::instance().hold(false);
            throw;
        }
        if (spares) PreparedUnitPool::instance().hold(false);
        new_graph->setCancelCheck(nullptr);
        load_diag.e.build_ms = static_cast<double>(diagNowNs() - load_diag.t0) / 1e6;
        load_diag.e.units = new_graph->totalComponentCount();
        load_diag.e.spares = new_graph->sparesUsed();
        if (superseded()) { load_diag.e.result = "superseded"; return false; }
        new_graph->setInputGainLinear(input_gain_.load());
        new_graph->setMasterBypass(master_bypass_.load());
        new_graph->setPresetSource(source);

        if (superseded()) { load_diag.e.result = "superseded"; return false; }
        GraphEditGuard lock(*this);
        if (superseded()) { // a newer load arrived while this one waited for the model lock
            lock.unchanged();
            load_diag.e.result = "superseded";
            return false;
        }
        swapInGraph(std::move(new_graph));
        if (graph_) scenes_.initForLoadedPreset(*graph_);
        if (spares && graph_) PreparedUnitPool::instance().want(graph_->independentUnitTableIds(), sample_rate_, 512);
        active_preset_idx_.store(idx);
        load_diag.e.result = "ok";
        cur_name_ = r.name;
        { std::lock_guard<std::mutex> lk(name_mutex_); cur_name_snapshot_ = cur_name_; } rack_rev_.fetch_add(1);
        cur_author_ = r.author;
        cur_category_ = r.category;
        if (source == "user") {
            cur_based_on_ = p.info.based_on;
            cur_based_on_path_ = p.info.based_on_path;
        } else {
            cur_based_on_ = r.name;
            cur_based_on_path_ = path;
        }
        return true;
    }

    // ───────────── preset loads off the HTTP thread ─────────────
    // A rig build prepares every unit, and circuit units settle their supplies (hundreds of ms each, seconds for the
    // biggest amps), so /api/preset/load (and /api/transport?action=preset) hand the build to the preset-loader thread:
    // the HTTP thread keeps serving telemetry, meters and edits, and the old rig keeps playing until the new one swaps
    // in. The newest request wins: an older one still waiting is answered at once with "superseded":true, and its
    // build stops between units. The reply is still the rack state once the rig is in (callers that render it keep
    // working); with ?async=1 it comes at once ({"ok":true,"loading":true}) and the client follows telemetry's
    // "preset_loading" (-1 when idle) and rack_rev. A rig that can't be read answers with "load_error".
    struct PresetLoadRequest {
        int idx = -1;
        uint64_t gen = 0;
        bool resolved = false;
        ResolvedPreset preset;
    };
    static std::string withJsonFields(std::string json, const std::string& fields) {
        if (!fields.empty() && !json.empty() && json.back() == '}') {
            json.pop_back();
            if (json.size() > 1) json += ",";
            json += fields + "}";
        }
        return json;
    }
    // HTTP thread. Answers `fd` now (async, superseded, bad index) or adopts it for the loader (http_fd_adopted_).
    void requestPresetLoad(int fd, int idx, bool async) {
        PresetLoadRequest rq;
        rq.idx = idx;
        // Resolve now when the library is up: a save or delete that lands before the loader gets to it would shift indices.
        if (library_ready_.load()) {
            rq.resolved = resolvePreset(idx, rq.preset);
            if (!rq.resolved) {
                sendJson(fd, withJsonFields(buildRackStateJson(), "\"load_error\":\"no rig at index " + std::to_string(idx) + "\""));
                return;
            }
        }
        std::vector<int> superseded;
        bool accepted = false;
        {
            std::lock_guard<std::mutex> lk(pl_mtx_);
            if (!pl_stopped_) {
                accepted = true;
                rq.gen = ++preset_load_gen_; // supersedes the load in progress (its build stops between units)
                for (const auto& w : pl_waiters_) superseded.push_back(w.first);
                pl_waiters_.clear();
                if (!async) {
                    pl_waiters_.push_back({fd, rq.gen});
                    http_fd_adopted_ = true;
                }
                pl_next_ = rq;
                preset_loading_idx_.store(idx);
                if (!pl_thread_.joinable()) pl_thread_ = std::thread([this] { presetLoaderLoop(); });
            }
        }
        pl_cv_.notify_one();
        if (!superseded.empty()) {
            const std::string body = withJsonFields(buildRackStateJson(), "\"superseded\":true");
            for (int w : superseded) {
                sendJson(w, body);
                rig::net::closeSocket(w);
            }
        }
        if (!accepted) sendJson(fd, withJsonFields(buildRackStateJson(), "\"load_error\":\"the engine is shutting down\""));
        else if (async) sendJson(fd, "{\"ok\":true,\"loading\":true,\"preset_idx\":" + std::to_string(idx) + "}");
    }
    void presetLoaderLoop() {
        rig::platform::ScopedNoDenormalsX86 ftz; // the HTTP thread's floating-point mode, so builds match a synchronous load
#ifdef __APPLE__
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0); // the user is waiting on it
#endif
        for (;;) {
            PresetLoadRequest rq;
            {
                std::unique_lock<std::mutex> lk(pl_mtx_);
                pl_cv_.wait(lk, [this] { return pl_next_.has_value() || pl_stopped_; });
                if (pl_stopped_) break;
                rq = std::move(*pl_next_);
                pl_next_.reset();
            }
            std::string err;
            bool ok = false;
            if (preset_load_gen_.load() == rq.gen) {
                // Library indices are only stable once the background scan has finished (see waitLibrary).
                for (int i = 0; i < 200 && !library_ready_.load() && preset_load_gen_.load() == rq.gen; ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (!rq.resolved) rq.resolved = resolvePreset(rq.idx, rq.preset);
                if (!rq.resolved) err = "no rig at index " + std::to_string(rq.idx);
                else {
                    try {
                        ok = loadResolvedPreset(rq.idx, rq.preset, rq.gen, &err);
                    } catch (const std::exception& e) {
                        err = std::string("the rig could not be built: ") + e.what();
                    } catch (...) {
                        err = "the rig could not be built";
                    }
                }
            }
            const bool superseded = !ok && preset_load_gen_.load() != rq.gen;
            if (!ok && !superseded) std::fprintf(stderr, "Headroom: loading rig %d failed: %s\n", rq.idx, err.c_str());
            std::vector<int> answer;
            {
                std::lock_guard<std::mutex> lk(pl_mtx_);
                for (auto it = pl_waiters_.begin(); it != pl_waiters_.end();) {
                    if (it->second <= rq.gen) {
                        answer.push_back(it->first);
                        it = pl_waiters_.erase(it);
                    } else ++it;
                }
                if (!pl_next_) preset_loading_idx_.store(-1);
            }
            if (!answer.empty()) {
                const std::string body = withJsonFields(buildRackStateJson(), ok ? std::string()
                                                                       : superseded ? std::string("\"superseded\":true")
                                                                       : "\"load_error\":\"" + jsonEscape(err) + "\"");
                for (int w : answer) {
                    sendJson(w, body);
                    rig::net::closeSocket(w);
                }
            }
        }
        std::vector<std::pair<int, uint64_t>> left;
        {
            std::lock_guard<std::mutex> lk(pl_mtx_);
            left.swap(pl_waiters_);
        }
        for (const auto& w : left) rig::net::closeSocket(w.first);
    }
    // stop(): cancels the build in progress and joins the loader. Requests after this are refused.
    void stopPresetLoader() {
        {
            std::lock_guard<std::mutex> lk(pl_mtx_);
            pl_stopped_ = true;
            ++preset_load_gen_;
        }
        pl_cv_.notify_all();
        if (pl_thread_.joinable()) pl_thread_.join();
    }

    // ───────────── instant preset switching ─────────────
    // A rig loads at the level its units give. Nothing measures or re-levels it, then or afterwards.

    // continuous: the edit only moved knobs / faders. Such a plan inherits the DSP history and renders without an
    // output bridge, unless an earlier non-continuous plan (switch, selector, structure) has not been heard yet.
    void publishRenderPlanLocked(bool continuous = false) {
        if (graph_) {
            // Amps learn whether a cab follows them; a circuit amp whose quality switches ("hq" with no cab) is
            // re-prepared here on a detached copy, so the plan below carries it (RackGraph::syncCabFollow).
            if (graph_->syncCircuitNoise()) continuous = false;
            if (graph_->syncCabFollow()) continuous = false;
            auto plan = graph_->renderSnapshot();
            const uint64_t rev = ++render_revision_;
            if (!continuous) bridge_revision_ = rev;
            plan->setContinuousEdit(continuous && graph_runtime_.audibleRevision() >= bridge_revision_);
            graph_runtime_.publish(std::move(plan), rev, graph_lineage_);
            if (host_latency_) host_latency_->store(graph_->latencySamples(), std::memory_order_release);
        }
        graph_runtime_.collect();
        // Idle CPU: wake the (possibly parked) worker so it collects the plan this replaces (see workerLoop).
        // Lock order graph_mutex_ → job_mtx_, as in enqueueJob.
        { std::lock_guard<std::mutex> lk(job_mtx_); worker_poll_ = true; }
        job_cv_.notify_one();
    }

    // Caller holds the model lock. Prepared plans are published by its guard.
    void swapInGraph(std::unique_ptr<RackGraph> g) {
        graph_ = std::move(g);
        if (graph_) graph_->setMeterBank(unit_meters_); // per-unit face meters (/api/telemetry?units=…)
        if (graph_) graph_->setDiagBank(diag_bank_);    // /api/diag (EngineDiag.hpp)
        ++graph_lineage_;
    }

    void enqueueJob(std::function<void()> fn, bool front = false) {
        {
            std::lock_guard<std::mutex> lk(job_mtx_);
            if (front) jobs_.push_front(std::move(fn));
            else jobs_.push_back(std::move(fn));
        }
        job_cv_.notify_one();
    }

    // Background worker (background QoS: efficiency cores, never competing with the audio work): the one-time
    // amp warm-up, plan collection, hosted resets.
    void workerLoop() {
        rig::platform::ScopedNoDenormalsX86 ftz; // x86: FTZ/DAZ (no-op on Apple Silicon)
#ifdef __APPLE__
        pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0);
#endif
        // Idle CPU: the audio thread retires replaced plans without signalling, so collect() is polled every
        // 250 ms — but only for a few seconds after a job or a publish. Otherwise the worker parks until
        // enqueueJob / publishRenderPlanLocked / stop wakes it: zero wakeups per idle instance. A plan retired
        // while parked (host not rendering) is reclaimed by the next publish's collect() or by stop().
        auto busy_until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (running_.load()) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lk(job_mtx_);
                const auto ready = [this] { return !jobs_.empty() || !running_.load() || worker_poll_; };
                if (std::chrono::steady_clock::now() < busy_until) job_cv_.wait_for(lk, std::chrono::milliseconds(250), ready);
                else job_cv_.wait(lk, ready);
                if (worker_poll_ || !jobs_.empty()) busy_until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                worker_poll_ = false;
                if (!running_.load()) break;
                if (!jobs_.empty()) {
                    job = std::move(jobs_.front());
                    jobs_.pop_front();
                }
            }
            if (job) job();
            {
                std::lock_guard<std::mutex> lock(graph_mutex_);
                graph_runtime_.collect();
            }
        }
    }

    void startWarmup() {
        // Every amp model's trims + the shared tube tables (persisted per level version, so a one-time cost). Rigs are
        // never measured at run time: each plays at its own knobs' level.
        enqueueJob([this] {
            for (const auto& sc : catalog::kAllComponents) {
                if (!running_.load()) return;
                if (!isTubeAmpTable(sc.table_id)) continue;
                if (auto c = createComponentByTableId(sc.table_id)) c->prepare(sample_rate_, 512);
            }
        });
    }

    void newRack(const std::string& tmpl) {
        auto g = std::make_unique<RackGraph>();
        preset::RackPreset empty;
        empty.root_rack.table_id = 100;
        g->buildFromPreset(empty, sample_rate_, 512);
        auto init = [this](int32_t t) { return initParamsFor(t); };
        std::vector<int32_t> chain;
        if (tmpl == "amp") chain = {56000};
        else if (tmpl == "pedalboard") chain = {15000, 56000, 158000, 127000};
        for (int32_t t : chain) g->insertComponent(t, 9999, init(t));
        if (tmpl == "guitar_synth" || tmpl == "instrument") {
            // Lane A: the guitar chain. Lane B: Pitch → MIDI → MIDI FX → Instrument → reverb.
            const int sid = g->insertSplit(9999, 0.45f);
            if (tmpl == "guitar_synth") {
                g->insertIntoLane(sid, 'A', 99, 15000, init(15000));
                g->insertIntoLane(sid, 'A', 99, 56000, init(56000));
            }
            g->insertIntoLane(sid, 'B', 99, 900001, init(900001));
            g->insertIntoLane(sid, 'B', 99, 900002, init(900002));
            g->insertIntoLane(sid, 'B', 99, 900003, init(900003));
            g->insertIntoLane(sid, 'B', 99, 127000, init(127000));
        }
        installNewRig(std::move(g), "New Rig");
    }

    // A rig built from scratch (template or /api/rack/build) replaces the live one in a single swap.
    void installNewRig(std::unique_ptr<RackGraph> g, const std::string& name) {
        g->setInputGainLinear(input_gain_.load());
        g->setMasterBypass(master_bypass_.load());
        g->setPresetSource("new");
        g->autoMacrosIfEmpty();
        g->setDirty(true);
        ++preset_load_gen_; // replaces the rig: a preset load still building is superseded
        GraphEditGuard lock(*this);
        swapInGraph(std::move(g));
        if (graph_) scenes_.initForLoadedPreset(*graph_);
        active_preset_idx_.store(-1);
        cur_name_ = name.empty() ? std::string("New Rig") : name;
        { std::lock_guard<std::mutex> lk(name_mutex_); cur_name_snapshot_ = cur_name_; } rack_rev_.fetch_add(1);
        cur_author_ = "";
        cur_category_ = "User";
        cur_based_on_.clear();
        cur_based_on_path_.clear();
    }

    // /api/rack/build: a whole new rig from a unit list, built off the live graph and swapped in once (no
    // half-built chains on the way). spec = {"name", "tempo"?, "units": [{"table_id", "norms"?: {param_id: 0..1},
    // "bypassed"?}]}. Norms map exactly like /api/param. When an amp is followed by a cabinet in the list, the
    // cabinet the rack adds after amps by itself is dropped so the rig gets the requested one. Unknown table ids
    // are skipped; returns the number of units placed (-1 = bad spec).
    int buildRack(const std::string& spec_json) {
        const auto spec = nlohmann::json::parse(spec_json, nullptr, false);
        if (spec.is_discarded() || !spec.is_object() || !spec.contains("units") || !spec["units"].is_array()) return -1;
        auto g = std::make_unique<RackGraph>();
        preset::RackPreset empty;
        empty.root_rack.table_id = 100;
        g->buildFromPreset(empty, sample_rate_, 512);
        int placed = 0;
        int auto_cab = -1; // slot of the cabinet the rack added after the previous amp
        for (const auto& u : spec["units"]) {
            if (!u.is_object() || !u.contains("table_id") || !u["table_id"].is_number_integer()) continue;
            const auto tid = u["table_id"].get<int32_t>();
            if (auto_cab >= 0 && RackGraph::isCabTable(tid)) g->removeActiveSlot(static_cast<std::size_t>(auto_cab));
            auto_cab = -1;
            if (plugins::isPluginTable(tid)) plugins::preinstantiate(tid, sample_rate_, 512);
            const std::size_t before = g->activeNodeViews().size();
            const int idx = g->insertComponent(tid, 99999, initParamsFor(tid));
            if (idx < 0) continue;
            ++placed;
            if (u.contains("norms") && u["norms"].is_object())
                for (const auto& [pid, v] : u["norms"].items())
                    if (v.is_number()) g->setComponentNormalizedParameter(static_cast<std::size_t>(idx), pid, v.get<float>());
            if (u.value("bypassed", false)) g->setSlotBypassed(static_cast<std::size_t>(idx), true);
            const auto views = g->activeNodeViews();
            if (views.size() == before + 2 && static_cast<std::size_t>(idx) + 1 < views.size() && RackGraph::isCabTable(views[idx + 1].table_id))
                auto_cab = idx + 1;
        }
        if (spec.contains("tempo") && spec["tempo"].is_number()) g->setTempo(spec["tempo"].get<double>());
        std::string name = spec.value("name", std::string());
        if (name.size() > 120) name.resize(120);
        installNewRig(std::move(g), name);
        return placed;
    }

    // Schema INIT values; switches default off and selectors to their first option.
    std::vector<std::pair<std::string, float>> initParamsFor(int32_t table_id) const {
        std::vector<std::pair<std::string, float>> v;
        if (plugins::isPluginTable(table_id)) return v; // hosted plugins start from their own defaults
        if (const auto* cp = comp_bank_.initFor(table_id)) v = cp->params;
        if (const auto* sc = catalog::findByTableId(table_id)) {
            for (const auto& p : sc->params) {
                if (p.id == "Pwr" || p.id == "CASSt") continue;
                bool have = false;
                for (const auto& kv : v) if (kv.first == p.id) { have = true; break; }
                if (have) continue;
                float mn = 0.0f, mx = 1.0f;
                getParamRangeFor(table_id, p, mn, mx);
                if (isBoolParam(table_id, p)) v.emplace_back(std::string(p.id), 0.0f);
                else if (!paramLabels(table_id, p.id).empty() || (mx - mn > 1.5f && mx - mn <= 64.0f)) v.emplace_back(std::string(p.id), std::round(std::clamp(p.default_value, mn, mx)));
            }
        }
        return v;
    }

    int insertUnit(int32_t table_id, std::size_t pos) {
        std::vector<std::pair<std::string, float>> v = initParamsFor(table_id);
        if (plugins::isPluginTable(table_id)) plugins::preinstantiate(table_id, sample_rate_, 512); // instantiate outside the rack lock
        GraphEditGuard lock(*this);
        return graph_ ? graph_->insertComponent(table_id, pos, v) : -1;
    }

    bool moveUnit(std::size_t from, std::size_t to) {
        GraphEditGuard lock(*this);
        return graph_ && graph_->moveSlot(from, to);
    }

    bool applyComponentPreset(std::size_t slot, const std::string& name) {
        GraphEditGuard lock(*this);
        if (!graph_) return false;
        auto views = graph_->activeNodeViews();
        if (slot >= views.size()) return false;
        if (views[slot].table_id == 900004) {
            const auto params = polySynthPresetParams(name);
            return !params.empty() && graph_->applyParams(slot, params, name);
        }
        const auto* bank = comp_bank_.presetsFor(views[slot].table_id);
        if (!bank) return false;
        for (const auto& cp : *bank) {
            if (cp.name == name) return graph_->applyParams(slot, cp.params, name);
        }
        return false;
    }

    int savePreset(const std::string& name, const std::string& author, std::string* err,
                   const std::string& override_based_on = {}, const std::string& override_based_on_path = {},
                   bool has_override_lineage = false) {
        preset::RackPreset p;
        {
            std::lock_guard<std::mutex> lock(graph_mutex_);
            if (!graph_) return -1;
            p = graph_->toPreset(name, author);
            p.info.based_on = has_override_lineage ? override_based_on : cur_based_on_;
            p.info.based_on_path = has_override_lineage ? override_based_on_path : cur_based_on_path_;
        }

        const int idx = library_.saveUser(p, err);
        if (idx >= 0) {
            GraphEditGuard lock(*this);
            graph_->setDirty(false);
            graph_->setPresetSource("user");
            active_preset_idx_.store(idx);
            cur_name_ = name;
            { std::lock_guard<std::mutex> lk(name_mutex_); cur_name_snapshot_ = cur_name_; } rack_rev_.fetch_add(1);
            cur_author_ = author;
            cur_category_ = "User";
            cur_based_on_ = p.info.based_on;
            cur_based_on_path_ = p.info.based_on_path;
        }
        return idx;
    }

    void setPlayState(bool play) { playing_.store(play); }
    bool isPlaying() const { return playing_.load(); }

    void selectTrack(int idx, bool auto_play = true) {
        const bool ok = (active_track_idx_.load() == idx && active_pcm_.load()) ? true : activateTrack(idx);
        if (ok && auto_play) playing_.store(true);
    }

    void seekNormalized(double norm) {
        std::lock_guard<std::mutex> lk(pcm_mtx_);
        if (const TrackPCM* pcm = active_pcm_.load()) {
            const std::size_t total = pcm->mono.size();
            if (total > 0) playhead_frame_.store(static_cast<std::size_t>(std::clamp(norm, 0.0, 0.999) * double(total)));
        }
    }

    // The audio thread applies input_gain_ to the plan it renders every block (smoothed per sample in
    // RackGraph::process), so a gain move publishes no plan; the model keeps the value for snapshots and saves.
    void setInputGain(float g) {
        g = std::clamp(g, 0.0f, 4.0f);
        GraphEditGuard lock(*this, /*publish=*/false);
        input_gain_.store(g);
        if (graph_) graph_->setInputGainLinear(g);
    }

    // Top-bar IN / OUT (persisted in audio settings): IN is the rig input gain, OUT the master output level.
    void applyTopbarGains() {
        setInputGain(std::pow(10.0f, audio_settings_.rig_input_db / 20.0f));
        out_gain_target_.store(audio_settings_.output_gain_db <= -59.9f ? 0.0f : std::pow(10.0f, audio_settings_.output_gain_db / 20.0f), std::memory_order_relaxed);
    }

    // /api/audio/set: apply the given settings, return the response body. Gain-only requests (the IN / OUT knobs
    // and the input-gain slider while dragging) get a tiny reply: the full one enumerates every audio device.
    std::string handleAudioSet(const std::string& query) {
        applyAudioSettings(query);
        if (queryKeysWithin(query, {"input_gain_db", "rig_input_db", "output_gain_db", "lite"})) {
            const auto& a = audio_settings_;
            std::ostringstream o;
            o << "{\"ok\":true,\"lite\":true,\"gains\":{\"input_gain_db\":" << a.input_gain_db << ",\"rig_input_db\":" << a.rig_input_db
              << ",\"output_gain_db\":" << a.output_gain_db << "}}";
            return o.str();
        }
        return buildAudioJson();
    }

    void setMasterBypass(bool b) {
        master_bypass_.store(b);
        GraphEditGuard lock(*this);
        if (graph_) graph_->setMasterBypass(b);
    }

    void setSlotBypass(std::size_t slot_idx, bool b) {
        GraphEditGuard lock(*this);
        if (graph_) graph_->setSlotBypassed(slot_idx, b);
    }

    void setSlotGainScale(std::size_t slot_idx, float scale) {
        GraphEditGuard lock(*this);
        lock.continuous();
        if (graph_) graph_->setSlotToneScale(slot_idx, scale, 1.0f);
    }

    void setSlotNormalizedParameter(std::size_t slot_idx, const std::string& param_id, float norm_val) {
        GraphEditGuard lock(*this);
        if (!graph_) return;
        bool continuous = graph_->setComponentNormalizedParameter(slot_idx, param_id, norm_val);
        // Float-typed switches and labelled selectors (see isBoolParam / paramLabels) are steps too.
        for (const auto& v : graph_->activeNodeViews())
            if (v.slot_idx == slot_idx && v.component) {
                const int32_t tid = v.component->tableId();
                if (!paramLabels(tid, param_id).empty()) continuous = false;
                for (const auto& p : v.component->schema().params)
                    if (p.id == param_id && isBoolParam(tid, p)) continuous = false;
            }
        lock.continuous(continuous);
    }

    void setMacroNormalized(int32_t col, float norm_val) {
        GraphEditGuard lock(*this);
        if (graph_) lock.continuous(graph_->setMacroNormalized(col, norm_val));
    }

    // ───────────── rig tempo ─────────────
    // The single entry point for the rig tempo (UI, tap tempo, MIDI later). Saved with the rig and
    // published like any edit. A plugin host drives tempo through rig::hostTempo().setTempo() (or
    // AudioBlock::host_tempo) instead, which needs no lock and overrides this while set.
    bool setTempo(double bpm) {
        GraphEditGuard lock(*this);
        lock.continuous(); // synced times glide to the new tempo
        if (!graph_ || !graph_->setTempo(bpm)) { lock.unchanged(); return false; }
        rack_rev_.fetch_add(1);
        return true;
    }
    double tempo() const {
        std::lock_guard<std::mutex> lock(graph_mutex_);
        return graph_ ? graph_->tempo() : kTempoDefault;
    }
    // Server-side tap (for footswitches / MIDI): average of the recent intervals, reset after 2 s.
    double tapTempo() {
        using clk = std::chrono::steady_clock;
        const auto now = clk::now();
        std::lock_guard<std::mutex> lk(tap_mtx_);
        if (!taps_.empty() && now - taps_.back() > std::chrono::seconds(2)) taps_.clear();
        taps_.push_back(now);
        if (taps_.size() > 6) taps_.erase(taps_.begin());
        if (taps_.size() < 2) return 0.0;
        const double secs = std::chrono::duration<double>(taps_.back() - taps_.front()).count() / static_cast<double>(taps_.size() - 1);
        return secs > 0.0 ? 60.0 / secs : 0.0;
    }
    std::string tempoJson() const {
        const double host = hostTempo().bpm();
        std::ostringstream o;
        o << "{\"bpm\":" << (host > 0.0 ? host : tempo()) << ",\"rig_bpm\":" << tempo() << ",\"source\":\"" << (host > 0.0 ? "host" : "rig")
          << "\",\"min\":" << kTempoMin << ",\"max\":" << kTempoMax << "}";
        return o.str();
    }

    // Swap a legacy cabinet unit (156000 / 88000 / 119000 / 168000) for an equivalent Cabinet unit in the same
    // place, so mics and positions can be chosen on an existing rig. Returns the new slot or -1.
    int convertToCabinet(std::size_t slot) {
        GraphEditGuard lock(*this);
        cabinet::Equivalent eq;
        bool bypassed = false, found = false;
        if (graph_)
            for (const auto& v : graph_->activeNodeViews())
                if (v.slot_idx == slot) {
                    found = cabinet::equivalentOf(*v.component, eq);
                    bypassed = v.component->isBypassed();
                }
        if (!found || !graph_->removeActiveSlot(slot)) { lock.unchanged(); return -1; }
        const int ns = graph_->insertComponent(kCabinetUnitTable, slot, {});
        if (ns < 0) throw std::runtime_error("Cabinet unit unavailable"); // guard rolls the removal back
        graph_->editComponent(static_cast<std::size_t>(ns), [&](IComponent* c) {
            if (!c) return false;
            cabinet::apply(*c, eq);
            c->processControlRate();
            return true;
        });
        if (bypassed) graph_->setSlotBypassed(static_cast<std::size_t>(ns), true);
        rack_rev_.fetch_add(1);
        return ns;
    }

    // Cabinet unit: choose cabinet / mics / positions by stable id (see /api/cabinets).
    bool selectCabinet(std::size_t slot, const std::string& cab, const std::string& mic_a, const std::string& pos_a,
                       const std::string& mic_b, const std::string& pos_b) {
        GraphEditGuard lock(*this);
        bool ok = false;
        if (graph_)
            ok = graph_->editComponent(slot, [&](IComponent* c) {
                if (!c || !cabinet::isCabinetUnit(*c)) return false;
                cabinet::select(*c, cab, mic_a, pos_a, mic_b, pos_b);
                c->processControlRate();
                return true;
            });
        if (ok) { graph_->setDirty(true); rack_rev_.fetch_add(1); }
        else lock.unchanged();
        return ok;
    }

    bool addComponentToRack(int32_t table_id) {
        GraphEditGuard lock(*this);
        if (graph_) return graph_->addComponentSlot(table_id);
        return false;
    }

    bool removeSlotFromRack(std::size_t slot_idx) {
        GraphEditGuard lock(*this);
        if (graph_) return graph_->removeActiveSlot(slot_idx);
        return false;
    }

    std::string buildRackStateJson() const {
        std::lock_guard<std::mutex> lock(graph_mutex_);
        int pidx = active_preset_idx_.load();
        const auto& pmeta = presets_[std::clamp(pidx, 0, static_cast<int>(presets_.size()) - 1)];

        std::ostringstream oss;
        oss.setf(std::ios::fixed);
        oss.precision(4);

        oss << "{";
        oss << "\"preset_idx\":" << pidx << ",";
        oss << "\"preset_name\":\"" << jsonEscape(cur_name_) << "\",";
        oss << "\"author\":\"" << jsonEscape(cur_author_) << "\",";
        oss << "\"category\":\"" << jsonEscape(cur_category_) << "\",";
        oss << "\"tag_group\":\"" << jsonEscape(pidx >= 0 && pidx < static_cast<int>(presets_.size()) ? pmeta.tag_group : cur_category_) << "\",";
        oss << "\"dirty\":" << ((graph_ && graph_->isDirty()) ? "true" : "false") << ",";
        oss << "\"preset_source\":\"" << (graph_ ? graph_->presetSource() : std::string("factory")) << "\",";
        oss << "\"based_on\":\"" << jsonEscape(cur_based_on_) << "\",";
        oss << "\"based_on_path\":\"" << jsonEscape(cur_based_on_path_) << "\",";
        oss << "\"rack_rev\":" << rack_rev_.load() << ",";
        oss << "\"render_revision\":" << render_revision_ << ",";
        oss << "\"audible_revision\":" << graph_runtime_.audibleRevision() << ",";
        oss << "\"graph_lineage\":" << graph_lineage_ << ",";
        oss << "\"source\":\"" << (live_.load() ? "live" : "player") << "\",";
        oss << "\"input_clip\":" << (clip_hold_.load() > 0 ? "true" : "false") << ",";
        oss << "\"midi\":" << liveMidiJson() << ",";
        oss << "\"looper\":" << global_looper_.toJson() << ",";
        oss << "\"library_ready\":" << (library_ready_.load() ? "true" : "false") << ",";
        {
            const double host = hostTempo().bpm();
            const double rig_bpm = graph_ ? graph_->tempo() : kTempoDefault;
            oss << "\"tempo\":{\"bpm\":" << (host > 0.0 ? host : rig_bpm) << ",\"rig_bpm\":" << rig_bpm << ",\"source\":\"" << (host > 0.0 ? "host" : "rig") << "\"},";
        }

        oss << "\"presets\":[";
        for (std::size_t i = 0; i < presets_.size(); ++i) {
            if (i) oss << ",";
            oss << "{\"idx\":" << i
                << ",\"preset_name\":\"" << jsonEscape(presets_[i].title) << "\""
                << ",\"author\":\"" << jsonEscape(presets_[i].author) << "\""
                << ",\"category\":\"" << jsonEscape(presets_[i].category) << "\""
                << ",\"tag_group\":\"" << jsonEscape(presets_[i].tag_group) << "\"}";
        }
        oss << "],";

        oss << "\"slots\":[";
        if (graph_) {
            auto views = graph_->activeNodeViews();
            for (std::size_t s = 0; s < views.size(); ++s) {
                if (s) oss << ",";
                const auto& v = views[s];
                const auto& schema = v.component->schema();
                std::string arch = classifyArchetype(schema);
                int oversamp = schema.fixed_4x_oversampling
                                   ? 4
                                   : (schema.target_sample_rate_k >= 2
                                          ? 4
                                          : (schema.target_sample_rate_k == 1
                                                 ? 2
                                                 : (schema.family == catalog::EngineFamily::Neural ? 2 : 1)));
                if (int eo = engineOversampling(schema.table_id, sample_rate_); eo > 0) oversamp = eo;
                if (schema.family == catalog::EngineFamily::Convolution) oversamp = 1;
                double isr_khz = (sample_rate_ * oversamp) / 1000.0;
                const std::string engine_desc = plugins::isPluginTable(schema.table_id) ? plugins::engineDescription(schema.table_id) : engineDescription(schema.table_id);
                std::string nmodel;

                oss << "{";
                oss << "\"slot_idx\":" << v.slot_idx << ",";
                oss << "\"node_id\":" << v.stable_id << ",";
                oss << "\"table_id\":" << v.table_id << ",";
                oss << "\"display_name\":\"" << jsonEscape(schema.display_name) << "\",";
                oss << "\"reference_name\":\"" << jsonEscape(schema.reference_name) << "\",";
                oss << "\"family\":\"" << jsonEscape(familyLabel(schema.family)) << "\",";
                oss << "\"archetype\":\"" << arch << "\",";
                oss << "\"bypassed\":" << (v.component->isBypassed() ? "true" : "false") << ",";
                oss << "\"oversampling\":" << oversamp << ",";
                oss << "\"internal_sr_khz\":" << isr_khz << ",";
                oss << "\"neural_model\":\"" << jsonEscape(nmodel) << "\",";
                oss << "\"engine\":\"" << jsonEscape(engine_desc) << "\",";
                {
                    const auto pl = graph_->slotPresetLabel(v.slot_idx);
                    oss << "\"component_preset\":\"" << jsonEscape(pl.first) << "\",\"component_preset_modified\":" << (pl.second ? "true" : "false") << ",";
                }
                if (plugins::isPluginTable(v.table_id)) oss << plugins::slotJson(*v.component) << ",";
                if (const std::string cn = model::customName(*v.component); !cn.empty()) oss << "\"custom_name\":\"" << jsonEscape(cn) << "\",";
                if (v.table_id == kCabinetUnitTable) oss << cabinet::slotJson(*v.component) << ",";
                if (isLooperUnit(*v.component)) oss << looperSlotJson(*v.component) << ",";

                // Collect & rank parameters so primary tone/drive/mix controls come first and utility/bypass params are skipped
                std::vector<const catalog::ParamSchema*> ranked_params;
                for (const auto& p : schema.params) {
                    if (isHiddenUtilityParam(p)) continue;
                    ranked_params.push_back(&p);
                }
                if (!plugins::isPluginTable(schema.table_id) && schema.table_id != kCabinetUnitTable) // plugins and Cabinet keep their own parameter order
                std::stable_sort(ranked_params.begin(), ranked_params.end(), [](const catalog::ParamSchema* a, const catalog::ParamSchema* b) {
                    return paramPriorityScore(*a) > paramPriorityScore(*b);
                });

                oss << "\"params\":[";
                std::size_t p_emitted = 0;
                const auto extras = v.component->extraParams();
                auto secondary = [](const std::string& id) { return id == "IR2" || id == "IR3" || id == "IR4"; };
                for (const auto& ep : extras) {
                    if (secondary(ep.id)) continue;
                    if (p_emitted > 0) oss << ",";
                    appendExtraParamJson(oss, ep, v.component->getParameter(ep.id), true);
                    ++p_emitted;
                }
                for (const auto* pp : ranked_params) {
                    if (p_emitted > 0) oss << ",";
                    appendParamJson(oss, schema.table_id, *pp, v.component->getParameter(pp->id), v.component);
                    ++p_emitted;
                }
                for (const auto& ep : extras) {
                    if (!secondary(ep.id)) continue;
                    if (p_emitted > 0) oss << ",";
                    appendExtraParamJson(oss, ep, v.component->getParameter(ep.id), true);
                    ++p_emitted;
                }
                oss << "],\"primary\":" << std::min<std::size_t>(9, p_emitted);
                {
                    oss << ",\"factory_presets\":[";
                    if (const auto* bank = comp_bank_.presetsFor(schema.table_id)) {
                        for (std::size_t k = 0; k < bank->size(); ++k) oss << (k ? "," : "") << "\"" << jsonEscape((*bank)[k].name) << "\"";
                    } else if (schema.table_id == 900004) {
                        const auto& ps = midi::synthPresets();
                        for (std::size_t k = 0; k < ps.size(); ++k) oss << (k ? "," : "") << "\"" << jsonEscape(ps[k].name) << "\"";
                    }
                    oss << "]";
                }
                oss << "}";
            }
        }
        oss << "],";

        oss << "\"layout\":" << (graph_ ? graph_->layoutJson() : std::string("[]")) << ",";
        oss << "\"macros\":[";
        if (graph_) {
            auto views = graph_->activeNodeViews();
            auto mviews = graph_->macroViews();
            for (std::size_t c = 0; c < mviews.size(); ++c) {
                const auto& m = mviews[c];
                if (c) oss << ",";
                std::string unit = "", label = m.label;
                std::size_t first_slot = 0;
                if (!m.targets.empty() && m.targets[0].slot < views.size()) {
                    unit = std::string(views[m.targets[0].slot].component->schema().reference_name);
                    first_slot = m.targets[0].slot;
                }
                if (label.empty()) label = (m.source == "empty") ? "" : "Macro " + std::to_string(c + 1);
                oss << "{\"col\":" << c << ",\"slot\":" << (c + 1) << ",\"name\":\"" << jsonEscape(label) << "\",\"source\":\"" << m.source
                    << "\",\"target\":\"" << jsonEscape(unit) << "\",\"target_slot\":" << first_slot << ",\"val\":" << m.value << ",\"targets\":[";
                for (std::size_t t = 0; t < m.targets.size(); ++t) {
                    const auto& tv = m.targets[t];
                    std::string pname = tv.param, uname;
                    if (tv.split >= 0) {
                        pname = "A↔B Mix";
                        uname = "Parallel Split";
                    } else if (tv.slot < views.size()) {
                        const auto& sc = views[tv.slot].component->schema();
                        uname = std::string(sc.reference_name);
                        for (const auto& p : sc.params)
                            if (p.id == tv.param) pname = prettifyParamName(p.display_name.empty() ? p.id : p.display_name);
                    }
                    oss << (t ? "," : "") << "{\"slot\":" << (tv.split >= 0 ? -1 : static_cast<long>(tv.slot)) << ",\"split\":" << tv.split << ",\"param\":\"" << jsonEscape(tv.param) << "\",\"param_name\":\""
                        << jsonEscape(pname) << "\",\"unit\":\"" << jsonEscape(uname) << "\",\"min\":" << tv.min << ",\"max\":" << tv.max << "}";
                }
                oss << "]}";
            }
        }
        oss << "],\"scenes\":" << scenes_.toJson()
            << ",\"global_fx\":" << buildGlobalFxJson()
            << ",\"trainer\":" << buildTrainerJson() << "}";
        return oss.str();
    }

    std::string buildTelemetryJson() const {
        int tidx = active_track_idx_.load();
        int pidx = active_preset_idx_.load();
        std::size_t pos = playhead_frame_.load();
        double dur = 1.0;
        std::string tname = "None";
        {
            std::lock_guard<std::mutex> lk(tracks_mtx_);
            if (tidx >= 0 && tidx < static_cast<int>(tracks_.size())) {
                dur = tracks_[static_cast<std::size_t>(tidx)].duration_sec;
                tname = tracks_[static_cast<std::size_t>(tidx)].title;
            }
        }
        double cur_sec = double(pos) / sample_rate_;
        std::string pname;
        {
            std::lock_guard<std::mutex> lk(name_mutex_);
            pname = cur_name_snapshot_;
        }

        std::ostringstream oss;
        oss.setf(std::ios::fixed);
        oss.precision(4);
        oss << "{";
        oss << "\"playing\":" << (playing_.load() ? "true" : "false") << ",";
        oss << "\"loop\":" << (loop_.load() ? "true" : "false") << ",";
        oss << "\"track_idx\":" << tidx << ",";
        oss << "\"track_name\":\"" << jsonEscape(tname) << "\",";
        oss << "\"position_sec\":" << cur_sec << ",";
        oss << "\"duration_sec\":" << dur << ",";
        oss << "\"input_gain\":" << input_gain_.load() << ",";
        oss << "\"master_bypass\":" << (master_bypass_.load() ? "true" : "false") << ",";
        oss << "\"preset_idx\":" << pidx << ",";
        oss << "\"preset_name\":\"" << jsonEscape(pname) << "\",";
        oss << "\"rack_rev\":" << rack_rev_.load() << ",";
        oss << "\"preset_loading\":" << preset_loading_idx_.load() << ","; // rig being built (-1: none), see requestPresetLoad
        const auto renderBlocks = render_blocks_.load(std::memory_order_acquire);
        const auto renderBudget = render_budget_ns_.load(std::memory_order_relaxed);
        oss << "\"render_budget\":{\"blocks\":" << renderBlocks
            << ",\"over_budget\":" << render_over_budget_.load(std::memory_order_relaxed)
            << ",\"elapsed_ns\":" << render_time_ns_.load(std::memory_order_relaxed)
            << ",\"budget_ns\":" << renderBudget
            << ",\"mean_ratio\":" << double(render_time_ns_.load(std::memory_order_relaxed)) / std::max<uint64_t>(1, renderBudget)
            << ",\"last_ratio\":" << render_last_ratio_.load(std::memory_order_relaxed)
            << ",\"peak_ms\":" << double(render_peak_ns_.load(std::memory_order_relaxed)) / 1e6
            << ",\"scope\":\"renderBlock wall time; not device xruns\"},";
        oss << "\"render_revision\":" << render_revision_ << ",";
        oss << "\"audible_revision\":" << graph_runtime_.audibleRevision() << ",";
        oss << "\"graph_lineage\":" << graph_lineage_ << ",";
        oss << "\"source\":\"" << (live_.load() ? "live" : "player") << "\",";
        oss << "\"input_clip\":" << (clip_hold_.load() > 0 ? "true" : "false") << ",";
        oss << "\"midi\":" << liveMidiJson() << ",";
        oss << "\"backing\":" << backing_.telemetryJson() << ","; // Practice backing track transport
        { // Looper summary (relaxed atomics, no lock) so the UI's tab LED needs no separate /api/looper poll.
            static constexpr const char* kLoopNames[] = {"empty", "recording", "playing", "overdubbing", "stopped", "armed"};
            const auto nm = [](LoopTrackState st) { return kLoopNames[std::clamp(static_cast<int>(st), 0, 5)]; };
            const int sel = std::clamp(global_looper_.selected(), 0, LoopStation::kTracks - 1);
            const LoopTrackState cur = global_looper_.trackState(sel);
            oss << "\"looper\":{\"state\":\"" << nm(cur) << "\",\"state_id\":" << static_cast<int>(cur) << ",\"tracks\":[";
            for (int t = 0; t < LoopStation::kTracks; ++t) oss << (t ? ",\"" : "\"") << nm(global_looper_.trackState(t)) << "\"";
            oss << "]},";
        }
        oss << "\"in_rms\":" << in_rms_.load() << ",";
        oss << "\"out_rms_l\":" << out_rms_l_.load() << ",";
        oss << "\"out_rms_r\":" << out_rms_r_.load() << ",";
        oss << "\"waveform_in\":[";
        for (int i = 0; i < 64; ++i) {
            if (i) oss << ",";
            oss << wave_in_[i].load(std::memory_order_relaxed);
        }
        oss << "],\"waveform_out\":[";
        for (int i = 0; i < 64; ++i) {
            if (i) oss << ",";
            oss << wave_out_[i].load(std::memory_order_relaxed);
        }
        oss << "]}";
        return oss.str();
    }

#include "RealtimeAudioServerDiag.inl"   // one-time level migration of user rigs + /api/diag, HEADROOM_DIAG_LOG
#include "RealtimeAudioServerHosted.inl" // DAW-plugin (hosted) mode: additive methods, see app/plugin

private:
    static std::string_view familyLabel(catalog::EngineFamily f) noexcept {
        switch (f) {
            case catalog::EngineFamily::Neural: return "State-Space Tube Model";
            case catalog::EngineFamily::CircuitWaveshaper: return "4× Tube Circuit LUT";
            case catalog::EngineFamily::Convolution: return "Zero-Latency IR Convolver";
            case catalog::EngineFamily::ModernDSP: return "Modern Oversampled DSP";
            case catalog::EngineFamily::ClassicDSP: return "Studio Classic DSP";
            case catalog::EngineFamily::StudioHardware: return "Vintage Studio Hardware";
            case catalog::EngineFamily::MasteringLimiter: return "Mastering Dynamics";
            default: return "Modular Signal Processor";
        }
    }

    static bool containsCI(std::string_view hay, std::string_view needle) noexcept {
        if (needle.empty() || needle.size() > hay.size()) return false;
        for (std::size_t i = 0; i <= hay.size() - needle.size(); ++i) {
            bool match = true;
            for (std::size_t j = 0; j < needle.size(); ++j) {
                if (std::tolower(static_cast<unsigned char>(hay[i + j])) !=
                    std::tolower(static_cast<unsigned char>(needle[j]))) {
                    match = false;
                    break;
                }
            }
            if (match) return true;
        }
        return false;
    }

    static bool isHiddenUtilityParam(const catalog::ParamSchema& p) noexcept {
        if (p.kind == catalog::ParamKind::String) return true;
        std::string_view id = p.id;
        std::string_view dn = p.display_name;
        if (id == "Pwr" || id == "on_off" || id == "bypass" ||
            containsCI(id, "on_off") || containsCI(id, "bypass") || containsCI(id, "stereo") ||
            containsCI(id, "learn") || containsCI(id, "expert") || containsCI(id, "variac") ||
            containsCI(dn, "on/off") || containsCI(dn, "bypass") || containsCI(dn, "stereo") ||
            containsCI(dn, "learn")) {
            return true;
        }
        return false;
    }

    static int paramPriorityScore(const catalog::ParamSchema& p) noexcept {
        std::string_view id = p.id;
        std::string_view dn = p.display_name;
        auto match = [&](std::initializer_list<std::string_view> kw) {
            for (auto k : kw) {
                if (containsCI(id, k) || containsCI(dn, k)) return true;
            }
            return false;
        };
        if (match({"gain", "drive", "overdrive", "distortion", "fuzz", "sust", "thresh", "size", "time", "rate", "speed"})) return 100;
        if (match({"bass", "low", "mid", "treble", "high", "tone", "tilt", "filter", "freq", "cutoff", "decay", "feedback", "depth", "character", "band"})) return 90;
        if (match({"presence", "resonance", "air", "room", "diffusion", "mod", "ratio", "attack", "release", "xfade", "x-fade", "ab", "a/b", "cab"})) return 80;
        if (match({"master", "volume", "level", "output", "mix", "dry", "wet"})) return 70;
        if (p.kind == catalog::ParamKind::Float) return 50;
        return 25;
    }

    static std::string classifyArchetype(const catalog::ComponentSchema& s) {
        if (plugins::isPluginTable(s.table_id)) return plugins::isInstrumentTable(s.table_id) ? "plugin_instrument" : "plugin";
        switch (s.table_id) {
            case 900001: return "midi_detector";
            case 900002: return "midi_fx";
            case 900003: return "instrument";
            case 900004: return "synth";
            case 900005: return "nam_capture";
            case 900006: return "arpeggiator";
            case 900200: case 53000: return "looper";
            case 900201: return "pitch";
            case 900202: return "spatial";
            case 900203: return "tonematch";
            default: break;
        }
        auto has = [&](std::initializer_list<std::string_view> kw) {
            for (auto k : kw) {
                if (containsCI(s.reference_name, k) || containsCI(s.display_name, k)) return true;
            }
            return false;
        };

        if (s.family == catalog::EngineFamily::Convolution) {
            if (has({"Convolution", "Reverb", "Spring", "Plate"})) return "reverb";
            return "cabinet";
        }
        if (s.family == catalog::EngineFamily::Neural) {
            if (has({"Multi-Mode", "Character", "Saturator"})) return "saturator_matrix";
            if (has({"Screamer", "TS808", "HM-2", "Heavy Metal", "IVP", "Surveyor", "Fuzz", "RAT", "Pedal", "Boost", "Stomp", "Percolator"})) return "pedal_drive";
            if (has({"Vox", "AC15", "AC30", "Top Boost", "Supro", "1624T", "Fender", "Deluxe", "Twin", "Tweed"})) return "amp_boutique";
            return "amp_neural";
        }
        if (s.family == catalog::EngineFamily::CircuitWaveshaper) {
            if (s.fixed_4x_oversampling ||
                has({"Marshall", "1959SLP", "JCM800", "Soldano", "SLO", "Mesa", "Rectifier", "Bogner", "Uberschall", "Peavey", "5150", "Fender", "Tweed", "Bassman", "Deluxe", "Twin", "Vox", "AC30", "Roland", "JC-120", "Orange", "OR120", "Hiwatt", "DR103", "Ampeg", "SVT"})) {
                if (has({"Vox", "AC30", "Tweed", "Bassman", "Deluxe", "Twin", "Roland", "JC-120", "JTM45"})) return "amp_boutique";
                return "amp_tube";
            }
            return "pedal_drive";
        }
        if (has({"Reverb", "Cosmic", "Verb", "Spring", "Plate", "Lexicon", "224", "480", "EMT 140"})) return "reverb";
        if (has({"Delay", "Echo", "Memory Man"})) return "delay";
        if (has({"Chorus", "Dimension", "Flanger", "Mistress", "Phaser", "Phase 90", "Small Stone", "Ensemble", "Rotary", "Leslie", "Tremolo", "Vibrato", "Uni-Vibe", "Pitch"})) return "modulation";
        if (has({"Comp", "1176", "LA-2A", "160VU", "Limiter", "Limiting", "Maximizer", "Gate", "Noise", "Distressor", "Transient", "Ross", "Fairchild"})) return "compressor";
        if (has({"Screamer", "TS808", "TS9", "Overdrive", "HM-2", "Heavy Metal", "IVP", "Surveyor", "Distortion", "DS-1", "RAT", "Fuzz", "Big Muff", "Metal Zone", "SansAmp", "Crusher", "Wavefolder"})) return "pedal_drive";
        return "eq";
    }

    static std::string prettifyParamName(std::string_view raw) {
        std::string s(raw);
        for (char& c : s) {
            if (c == '_') c = ' ';
        }
        bool cap_next = true;
        for (char& c : s) {
            if (std::isspace(static_cast<unsigned char>(c)) || c == '/' || c == '-') {
                cap_next = true;
            } else if (cap_next) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                cap_next = false;
            }
        }
        return s;
    }

    static std::string formatParamDisplay(const catalog::ParamSchema& p, float raw, float norm) {
        std::ostringstream oss;
        oss.setf(std::ios::fixed);
        if (p.kind == catalog::ParamKind::Bool) {
            return (norm >= 0.5f) ? "ON" : "OFF";
        }
        if (p.kind == catalog::ParamKind::Int) {
            oss << static_cast<int>(std::round(raw));
            if (!p.unit.empty()) oss << " " << p.unit;
            return oss.str();
        }
        if (!p.unit.empty()) {
            oss.precision(1);
            oss << raw << " " << p.unit;
            return oss.str();
        }
        float pmin = 0.0f, pmax = 1.0f;
        getParamEffectiveRange(p, pmin, pmax);
        if (std::abs((pmax - pmin) - 1.0f) < 1e-2f) {
            oss.precision(1);
            oss << (norm * 10.0f);
            return oss.str();
        }
        oss.precision(1);
        oss << raw;
        return oss.str();
    }

    // ───────────── parameter metadata (kind, option labels, defaults) ─────────────
    static std::vector<std::string> paramLabels(int32_t tid, std::string_view id) {
        static const std::vector<std::string> kMatched = {
            "Vox AC30 2x12", "Ampeg SVT 8x10", "Orange PPC412", "Marshall JTM45 4x12", "Mesa Recto 4x12", "Hiwatt SE4123",
            "Marshall Modified 4x12", "Soldano 4x12", "Roland JC-120 2x12", "Marshall 1960A JCM800", "Marshall 1960A Lead",
            "Marshall 1960AX Greenback", "Mesa Recto 4x12 V30", "Fender Twin 2x12", "Fender Tweed Deluxe 1x12", "Fender Bassman 4x10",
            "Bogner Uberkab 4x12", "Peavey 5150 4x12", "Supro 1624T 1x12", "Gallien-Krueger 800RB 4x10", "Bogner Ecstasy 4x12",
            "Bogner Shiva 2x12", "Friedman BE-100 4x12", "Vox AC15 Alnico Blue", "Fender Deluxe Reverb 1x12", "Ampeg SVT-VR 8x10"};
        static const std::vector<std::string> kRoom = {
            "Vox AC30 2x12 Silver", "Vox AC30 2x12 Alnico Blue", "Orange PPC412 4x12", "Mesa Rectifier 4x12", "Hiwatt SE4123 4x12",
            "Marshall 1960AX 60s Greenback", "Marshall 1960A 70s Blackback", "Marshall 1960B 70s G12H", "Marshall 1960A 80s G12T-75",
            "Marshall 1960TV Tall 4x12", "Soldano 4x12 V30", "Soldano 4x12 Eminence", "Roland JC-120 2x12", "Fender Twin 2x12 Jensen",
            "Fender Showman 2x15 JBL", "Fender Bassman 4x10 Jensen", "Fender Tweed Deluxe 1x12", "Bogner Uberkab 4x12 V30",
            "Bogner Uberkab 4x12 T75", "Peavey 5150 4x12 Sheffield", "Ampeg SVT-810E 8x10", "Hartke XL 4x10 Aluminum",
            "Ampeg B-15N 1x15", "Mesa Lone Star 2x12", "Marshall 1936 2x12", "Marshall 1960B Vintage 4x12",
            "Engl Pro XXL 4x12", "Mesa Oversized 4x12 Ch A", "Mesa Oversized 4x12 Ch B"};
        auto sel = [&](std::string_view pre) { return id.size() == pre.size() + 1 && id.substr(0, pre.size()) == pre; };
        switch (tid) {
            case 156000: if (id == "Cab") return kMatched; break;
            case 88000:
                if (id == "Cab") {
                    std::vector<std::string> v = {"Bogner Uberkab 4x12 V30", "Mesa Rectifier 4x12", "Marshall 1960A 80s G12T-75",
                                                  "Marshall 1960A 70s Blackback", "Marshall 1960AX 60s Greenback", "Marshall 1960B Vintage 4x12",
                                                  "Engl Pro XXL 4x12", "Marshall 1936 2x12", "Mesa Lone Star 2x12", "Fender Bassman 4x10 Jensen",
                                                  "Fender Twin 2x12 Jensen"};
                    for (const char* n : {"Ampeg SVT 8x10", "Marshall Modified 4x12", "Bogner Uberkab 4x12", "Mesa Recto 4x12",
                                          "Marshall 1960A Lead", "Marshall 1960AX Greenback", "Orange PPC412", "Hiwatt SE4123",
                                          "Fender Tweed Deluxe 1x12", "Vox AC30 2x12", "Fender Twin 2x12", "Fender Bassman 4x10",
                                          "Roland JC-120 2x12", "Marshall 1960A JCM800", "Marshall JTM45 4x12", "Mesa Recto 4x12 V30",
                                          "Peavey 5150 4x12", "Soldano 4x12"})
                        v.push_back(n);
                    return v;
                }
                break;
            case 100000: if (id == "S") return kRoom; break;
            case 119000: case 168000:
                if (sel("Cab")) {
                    std::vector<std::string> v = {"DI Box", "No Cabinet"};
                    v.insert(v.end(), kRoom.begin(), kRoom.end());
                    return v;
                }
                if (sel("Mic")) return {"Shure SM57", "Sennheiser MD 421", "Neumann U87", "Royer R-121", "Beyerdynamic M 160"};
                if (sel("MPos")) return {"Cap", "Cap Edge", "Cone"};
                break;
            case 127000: if (id == "S1") return {"Gold Plate Short", "Gold Plate Long", "Silver Plate Short", "Silver Plate Long", "Studio Spring", "Dual Spring", "Amp Spring", "Dub Spring"}; break;
            case 16000: case 96000: if (id == "M") return {"Clean", "Raw", "Vintage", "Modern"}; break;
            case 151000: case 162000: case 300000: if (id == "gainMode") return {"Low", "Mid", "High"}; break;
            case 176000: if (id == "effectsSelector") return {"Off", "Tremolo", "Vibrato"}; break;
            case 49000: if (id == "em") return {"Off", "Chorus", "Vibrato"}; break;
            case 167000: if (id == "mode") return {"Turbo", "Hot", "+", "TS9"}; break; // raw 0..3, the face's MODE knob left to right
            case 900001:
                if (id == "mode") return {"Poly", "Mono"};
                if (id == "engine") return {"Neural", "Classic"};
                if (id == "low" || id == "high") {
                    std::vector<std::string> v;
                    static const char* kN[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
                    for (int k = 0; k <= 108; ++k) v.push_back(std::string(kN[k % 12]) + std::to_string(k / 12 - 1));
                    return v;
                }
                break;
            case 900002:
                if (id == "harmony") return {"Off", "Octave", "Fifth", "Power", "Third", "Major", "Minor", "Sus4"};
                if (id == "scale") return {"Chromatic", "Major", "Minor", "Dorian", "Mixolydian", "Pent. Minor", "Pent. Major", "Blues"};
                if (id == "root") return {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
                if (id == "arp") return {"Off", "Up", "Down", "Up/Down", "Random"};
                if (id == "rate") return {"1/4", "1/8", "1/16", "1/8T", "1/16T", "1/32"};
                if (id == "octave") return {"-3", "-2", "-1", "0", "+1", "+2", "+3"};
                break;
            case 900006:
                if (id == "trigger") return {"Bloom", "Always", "Engage", "Freeze"};
                if (id == "pattern") return {"Up", "Down", "Up/Down", "Converge", "Walk", "Melody", "Pedal", "Random"};
                if (id == "rate") return {"1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"};
                if (id == "range") return {"1 oct", "2 oct", "3 oct", "4 oct"};
                if (id == "extend") return {"As Played", "Triad", "7th", "9th", "11th"};
                if (id == "key") return {"Auto", "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
                if (id == "scale") return {"Major", "Minor", "Dorian", "Mixolydian", "Phrygian", "Lydian", "Harm. Minor"};
                if (id == "clock") return {"Rig", "Feel"};
                if (id == "voice") return {"Synth", "Strings", "Both"};
                if (id == "retrig") return {"Chord", "Pick"};
                break;
            case 900004:
                if (id == "osc1_wave" || id == "osc2_wave") return {"Saw", "Square", "Triangle", "Sine"};
                if (id == "unison") return {"1", "2", "3"};
                break;
            default: break;
        }
        return {};
    }

    static bool isBoolParam(int32_t tid, const catalog::ParamSchema& p) {
        if (p.kind == catalog::ParamKind::Bool) return true;
        const std::string_view id = p.id, dn = p.display_name;
        static const std::vector<std::pair<int32_t, std::string_view>> kBools = {
            {101000, "Hg"}, {56000, "Hg"}, {134000, "L13"}, {134000, "H14"}, {134000, "B15"}, {134000, "C16"}, {122000, "O2"},
            {163000, "channel"}, {163000, "normalMode"}, {163000, "bright"}, {81000, "c"}, {30000, "Br"}, {30000, "RvOn"}, {30000, "ViOn"},
            {172000, "bright"}, {172000, "reverb on"}, {172000, "reverb mode"}, {172000, "tremolo on"}, {172000, "tempo sync"}, {152000, "bright"},
            {54000, "uL"}, {54000, "uH"}, {54000, "br"}, {54000, "gE"}, {177000, "ultra low"}, {177000, "ultra high"}, {177000, "output selector"},
            {150000, "boost"}, {150000, "loCut"}, {150000, "midContour"}, {150000, "hiBoost"}, {49000, "B"}, {40000, "Br"}, {900006, "engage"},
            {45000, "CM"}, // 45000 A (AMP) and C (MOD) are 3-position selectors in the SansAmp GT2 netlist, not switches
            {156000, "V"}, {127000, "M3"}, {173000, "gainboost"}, {173000, "basssaver"},
            {110000, "K4"}, {114000, "F7"}}; // Peak Filter Kill (notch), Trance Gate Free (clock unsynced)
        for (const auto& kv : kBools)
            if (kv.first == tid && kv.second == id) return true;
        if (tid == 119000 || tid == 168000) {
            if ((id.size() == 2 && (id[0] == 'm' || id[0] == 's')) || id.substr(0, 2) == "pi" || id.substr(0, 2) == "rm") return true;
        }
        return containsCI(dn, "on/off") || containsCI(dn, " on") || dn == "Mute" || containsCI(dn, "mute") || containsCI(dn, "solo") ||
               containsCI(dn, "invert") || dn == "Sync" || containsCI(dn, "tempo sync") || containsCI(dn, "freeze");
    }

    void appendParamJson(std::ostringstream& oss, int32_t tid, const catalog::ParamSchema& p, float raw, const IComponent* comp = nullptr) const {
        if (plugins::isPluginTable(tid)) { oss << plugins::paramJson(tid, p, raw); return; } // AU names, units, value strings
        float pmin = 0.0f, pmax = 1.0f;
        getParamRangeFor(tid, p, pmin, pmax);
        const float norm = std::clamp((raw - pmin) / std::max(1e-6f, pmax - pmin), 0.0f, 1.0f);
        const auto labels = paramLabels(tid, p.id);
        std::string kind = "continuous";
        if (!labels.empty()) kind = "stepped";
        else if (isBoolParam(tid, p)) kind = "bool";
        else if (pmax - pmin > 1.5f && pmax - pmin <= 64.0f && std::floor(pmin) == pmin && std::floor(pmax) == pmax) kind = "stepped";
        float def_raw = p.default_value;
        if (const auto* init = comp_bank_.initFor(tid)) {
            for (const auto& kv : init->params)
                if (kv.first == p.id) def_raw = kv.second;
        }
        const float def_norm = std::clamp((def_raw - pmin) / std::max(1e-6f, pmax - pmin), 0.0f, 1.0f);
        std::string fmt = formatParamDisplay(p, raw, norm);
        if (!labels.empty()) {
            const int li = std::clamp(static_cast<int>(std::lround(raw - pmin)), 0, static_cast<int>(labels.size()) - 1);
            fmt = labels[static_cast<std::size_t>(li)];
        }
        // A switch's state as the unit's DSP reads it (IComponent::switchState): an untouched catalogue switch (raw
        // 0.5) is off on most units and on for some, so the UI shows "on" rather than guessing from the value.
        int sw = -1;
        if (kind == "bool") {
            if (comp) sw = comp->switchState(p.id);
            if (sw < 0) sw = switchStateFor(tid, p.id, raw);
            fmt = sw ? "On" : "Off";
        }
        if (comp) { std::string live; if (comp->paramDisplay(p.id, live)) fmt = live; } // e.g. synced "1/8 D"
        const std::string label = prettifyParamName(p.display_name.empty() ? p.id : p.display_name);
        oss << "{\"id\":\"" << jsonEscape(p.id) << "\",\"name\":\"" << jsonEscape(label) << "\",\"unit\":\"" << jsonEscape(p.unit)
            << "\",\"min\":" << pmin << ",\"max\":" << pmax << ",\"val\":" << raw << ",\"norm\":" << norm << ",\"default\":" << def_norm
            << ",\"kind\":\"" << kind << "\",\"fmt\":\"" << jsonEscape(fmt) << "\"";
        if (sw >= 0) oss << ",\"on\":" << (sw ? "true" : "false");
        if (!p.role.empty()) {
            oss << ",\"section\":\"" << jsonEscape(p.role) << "\"";
        }
        if (!labels.empty()) {
            oss << ",\"labels\":[";
            for (std::size_t i = 0; i < labels.size(); ++i) oss << (i ? "," : "") << "\"" << jsonEscape(labels[i]) << "\"";
            oss << "]";
        }
        oss << "}";
    }

    void appendExtraParamJson(std::ostringstream& oss, const ExtraParam& ep, float raw, bool use_raw) const {
        const float mx = static_cast<float>(std::max<std::size_t>(1, ep.labels.size()) - 1);
        const float v = use_raw ? raw : ep.def;
        const int li = std::clamp(static_cast<int>(std::lround(v)), 0, static_cast<int>(std::max<std::size_t>(1, ep.labels.size())) - 1);
        oss << "{\"id\":\"" << jsonEscape(ep.id) << "\",\"name\":\"" << jsonEscape(ep.name) << "\",\"unit\":\"\",\"min\":0,\"max\":" << mx
            << ",\"val\":" << li << ",\"norm\":" << (mx > 0 ? static_cast<float>(li) / mx : 0.0f) << ",\"default\":" << (mx > 0 ? ep.def / mx : 0.0f)
            << ",\"kind\":\"stepped\",\"fmt\":\"" << jsonEscape(ep.labels.empty() ? std::string() : ep.labels[static_cast<std::size_t>(li)]) << "\",\"labels\":[";
        for (std::size_t i = 0; i < ep.labels.size(); ++i) oss << (i ? "," : "") << "\"" << jsonEscape(ep.labels[i]) << "\"";
        oss << "]";
        if (!ep.groups.empty()) {
            oss << ",\"groups\":[";
            for (std::size_t i = 0; i < ep.groups.size(); ++i) oss << (i ? "," : "") << "\"" << jsonEscape(ep.groups[i]) << "\"";
            oss << "]";
        }
        oss << "}";
    }

    static std::string componentCategory(const catalog::ComponentSchema& s, const std::string& impl) {
        if (impl == "plugin") return "Plugins";
        if (impl == "midi" || impl == "instrument") return "MIDI & Instruments";
        if (impl == "nam") return "Amps";
        if (impl == "amp") return "Amps";
        if (impl == "cab") return "Cabinets";
        if (impl == "drive") return containsCI(s.reference_name, "Crusher") || containsCI(s.reference_name, "Wavefolder") || containsCI(s.reference_name, "Moog") ? "Creative" : "Drive & Fuzz";
        if (impl == "dynamics") return "Dynamics";
        if (impl == "eq") return "EQ & Filter";
        if (impl == "delay") return "Delay";
        if (impl == "ir_reverb" || impl == "fdn_reverb") return "Reverb";
        if (impl == "mod") {
            if (s.table_id == kLooperUnitTable || s.table_id == kLegacyLoopStationTable) return "Creative";
            if (s.table_id == kTransposeUnitTable || containsCI(s.reference_name, "Pitch") || containsCI(s.reference_name, "Whammy") || containsCI(s.reference_name, "OC-2") || containsCI(s.reference_name, "Octaver") || containsCI(s.reference_name, "Transpose")) return "Pitch & Synth";
            if (s.table_id == kDoublerRoomUnitTable) return "Modulation";
            return "Modulation";
        }
        return "Utility";
    }

    std::string buildComponentsJson() const {
        std::ostringstream oss;
        oss.setf(std::ios::fixed);
        oss.precision(4);
        oss << "{\"categories\":[\"Plugins\",\"MIDI & Instruments\",\"Amps\",\"Cabinets\",\"Drive & Fuzz\",\"Dynamics\",\"EQ & Filter\",\"Modulation\",\"Delay\",\"Reverb\",\"Pitch & Synth\",\"Creative\",\"Utility\"],\"components\":[";
        bool first = true;
        std::vector<const catalog::ComponentSchema*> all;
        for (const auto& c : catalog::extraComponents()) all.push_back(&c);
        for (const auto& c : catalog::studioComponents()) all.push_back(&c);
        for (const auto& c : catalog::kAllComponents) all.push_back(&c);
        for (const auto* c : plugins::importedSchemas()) all.push_back(c);
        for (const auto* sp : all) {
            const auto& s = *sp;
            const std::string impl = implementationKind(s.table_id);
            if (impl == "none" || impl == "routing" || impl == "fallback") continue;
            if (containsCI(s.reference_name, "Deprecated") || s.table_id == 10000 || s.table_id == 11000 || s.table_id == 100 ||
                s.table_id == 100000)
                continue;
            if (!first) oss << ",";
            first = false;
            std::vector<const catalog::ParamSchema*> ranked;
            for (const auto& p : s.params)
                if (!isHiddenUtilityParam(p)) ranked.push_back(&p);
            if (!plugins::isPluginTable(s.table_id) && s.table_id != kCabinetUnitTable) // plugins and Cabinet keep their own parameter order
            std::stable_sort(ranked.begin(), ranked.end(), [](const catalog::ParamSchema* a, const catalog::ParamSchema* b) {
                return paramPriorityScore(*a) > paramPriorityScore(*b);
            });
            oss << "{\"table_id\":" << s.table_id << ",\"name\":\"" << jsonEscape(s.display_name) << "\",\"ref\":\"" << jsonEscape(s.reference_name)
                << "\",\"archetype\":\"" << classifyArchetype(s) << "\",\"category\":\"" << componentCategory(s, impl) << "\",\"engine\":\""
                << jsonEscape(plugins::isPluginTable(s.table_id) ? plugins::engineDescription(s.table_id) : engineDescription(s.table_id)) << "\",\"params\":[";
            const auto* init = comp_bank_.initFor(s.table_id);
            bool any_extra = false;
            if (s.table_id == 168000 || s.table_id == 118000 || s.table_id == 900003 || s.table_id == 900005) {
                if (auto tmp = createComponentByTableId(s.table_id)) {
                    for (const auto& ep : tmp->extraParams()) {
                        if (any_extra) oss << ",";
                        appendExtraParamJson(oss, ep, ep.def, false);
                        any_extra = true;
                    }
                }
            }
            if (any_extra && !ranked.empty()) oss << ",";
            for (std::size_t i = 0; i < ranked.size(); ++i) {
                const auto& p = *ranked[i];
                float raw = p.default_value;
                if (init)
                    for (const auto& kv : init->params)
                        if (kv.first == p.id) raw = kv.second;
                if (i) oss << ",";
                appendParamJson(oss, s.table_id, p, raw);
            }
            oss << "],\"primary\":" << std::min<std::size_t>(9, ranked.size()) << ",\"factory_presets\":[";
            if (const auto* bank = comp_bank_.presetsFor(s.table_id)) {
                for (std::size_t k = 0; k < bank->size(); ++k) oss << (k ? "," : "") << "\"" << jsonEscape((*bank)[k].name) << "\"";
            }
            oss << "]}";
        }
        oss << "]}";
        return oss.str();
    }

    std::string buildTracksJson() const {
        std::ostringstream oss;
        std::lock_guard<std::mutex> lk(tracks_mtx_);
        oss << "{\"tracks\":[";
        for (std::size_t i = 0; i < tracks_.size(); ++i) {
            oss << (i ? "," : "") << "{\"idx\":" << i << ",\"title\":\"" << jsonEscape(tracks_[i].title) << "\",\"group\":\""
                << jsonEscape(tracks_[i].group) << "\",\"duration\":" << tracks_[i].duration_sec << "}";
        }
        oss << "],\"user_dir\":\"" << jsonEscape(userDITrackDir()) << "\"}";
        return oss.str();
    }

    static std::string jsonEscape(std::string_view s) {
        std::string out;
        out.reserve(s.size() + 4);
        for (char c : s) {
            if (c == '"') out += "\\\"";
            else if (c == '\\') out += "\\\\";
            else if (c == '\n') out += " ";
            else if (c == '\r') continue;
            else out.push_back(c);
        }
        return out;
    }

    void loadPresetsCatalog() {
        presets_ = {
            {"JCM800 British Crunch", "Headroom", "British Crunch", "Crunch", "00 JCM800 British Crunch.rig"},
            {"Sub Drop Metal", "Headroom", "Modern High-Gain", "High Gain", "18 Sub Drop Metal.rig"},
            {"Lead & Brass Harmony", "Headroom", "Lead & Harmony", "High Gain", "20 Lead & Brass Harmony.rig"},
            {"Crunch & Analog Pad", "Headroom", "Plexi + Synth Pad", "Guitar + MIDI", "17 Crunch & Analog Pad.rig"},
            {"Welded Sub", "Headroom", "Sub Synth Welded Into The Amp", "Guitar + MIDI", "../Library/Guitar + MIDI/Welded Sub.rig"},
            {"Vowel Weld", "Headroom", "Synth + Guitar Through One Vowel", "Guitar + MIDI", "../Library/Guitar + MIDI/Vowel Weld.rig"},
            {"Synth Body", "Headroom", "Synth Lows + Amp Grind", "Guitar + MIDI", "../Library/Guitar + MIDI/Synth Body.rig"},
            {"Locked Groove", "Headroom", "Arp + Guitar On One Gate", "Guitar + MIDI", "../Library/Guitar + MIDI/Locked Groove.rig"},
            {"Chime & Strings", "Headroom", "Top-Boost + Strings", "Clean", "19 Chime & Strings.rig"},
            {"Clean & Grand", "Headroom", "Blackface + Grand Piano", "Clean", "24 Clean & Grand.rig"},
            {"Twang & Tines", "Headroom", "Tweed + Tine Keys", "Clean", "26 Twang & Tines.rig"},
            {"Funk Clav", "Headroom", "Funk Envelope + Clav", "Clean", "25 Funk Clav.rig"},
            {"Octave Fuzz Synth", "Headroom", "Fuzz + Octave Lead", "High Gain", "27 Octave Fuzz Synth.rig"},
            {"Shoegaze Organ Wall", "Headroom", "Fuzz + Drawbar Organ", "Ambient & FX", "21 Shoegaze Organ Wall.rig"},
            {"Stadium Synth Double", "Headroom", "High-Gain + Synth Lead", "High Gain", "22 Stadium Synth Double.rig"},
            {"Neural Choir", "Headroom", "NAM Capture + Choir", "Guitar + MIDI", "23 Neural Choir.rig"},
            {"Ambient Bell Swells", "Headroom", "Shimmer + Glass Bells", "Ambient & FX", "28 Ambient Bell Swells.rig"},
            {"Metal Orchestra", "Headroom", "Recto + Strings & Choir", "High Gain", "29 Metal Orchestra.rig"},
            {"Concert Grand", "Headroom", "Poly MIDI Grand Piano", "MIDI", "01 Concert Grand.rig"},
            {"Analog Poly Pad", "Headroom", "Poly Synth Pad", "MIDI", "10 Analog Poly Pad.rig"}
        };
    }

    // DI player library: every WAV under app/samples/di (sub-folders become groups) plus the user's own
    // folder ~/Library/Application Support/Headroom/DI Tracks. Only metadata is indexed here.
    std::string userDITrackDir() const { return library::appSupportDir() + "/DI Tracks"; }

    void scanDITracks() {
        static const std::map<std::string, std::string> kTitles = {
            {"Lead DI.wav", "Lead DI (80s Shred & Lead)"},
            {"Rock DI.wav", "Rock DI (Classic Hard Rock, 120 bpm)"},
            {"Metal DI.wav", "Metal DI (Drop-Tuned Metal Chug)"},
            {"Crunch DI.wav", "Crunch DI (Alt-Rock Punchy Riff)"},
            {"Clean Strumming.wav", "Clean Strumming (Boutique Chime DI)"},
            {"Dream Acoustic.wav", "Dream Acoustic (Fingerpicked Clean DI)"}};
        static const char* kOrder[] = {"Lead DI.wav", "Rock DI.wav", "Metal DI.wav", "Crunch DI.wav", "Clean Strumming.wav", "Dream Acoustic.wav"};
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(userDITrackDir(), ec);
        std::vector<DISampleTrack> found;
        auto scanRoot = [&](const std::string& root, const std::string& top_group) {
            std::vector<fs::path> files;
            for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) break;
                if (!it->is_regular_file(ec)) continue;
                std::string ext = it->path().extension().string();
                for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (ext == ".wav" && it->path().filename().string()[0] != '.') files.push_back(it->path());
            }
            for (const auto& f : files) {
                DISampleTrack t;
                t.path = f.string();
                const fs::path rel = f.lexically_relative(root);
                t.group = rel.has_parent_path() && !rel.parent_path().empty() ? rel.parent_path().string() : top_group;
                const auto kt = kTitles.find(f.filename().string());
                if (kt != kTitles.end() && t.group == "Built-in") t.title = kt->second;
                else {
                    std::string stem = f.stem().string();
                    // "07 Metal Session" → "Metal Session": the number only orders the folder.
                    std::size_t i = 0;
                    while (i < stem.size() && std::isdigit(static_cast<unsigned char>(stem[i]))) ++i;
                    if (i > 0 && i < stem.size() && stem[i] == ' ') stem = stem.substr(i + 1);
                    for (auto& c : stem) if (c == '_') c = ' ';
                    t.title = stem;
                }
                uint32_t sr = 0;
                if (!wavHeaderInfo(t.path, sr, t.duration_sec)) continue;
                found.push_back(std::move(t));
            }
        };
        scanRoot(repo_root_ + "/app/samples/di", "Built-in");
        scanRoot(userDITrackDir(), "Your tracks");
        auto rank = [&](const DISampleTrack& t) -> int {
            if (t.group == "Built-in") {
                const std::string fn = fs::path(t.path).filename().string();
                for (int i = 0; i < 6; ++i) if (fn == kOrder[i]) return i;
                return 6;
            }
            return 100;
        };
        std::stable_sort(found.begin(), found.end(), [&](const DISampleTrack& a, const DISampleTrack& b) {
            const int ra = rank(a), rb = rank(b);
            if (ra != rb) return ra < rb;
            if (a.group != b.group) {
                // Your own recordings right after the built-ins, then the long-form collections.
                static const char* kGroupOrder[] = {"Your tracks", "Long Sessions", "Technique Drills", "Guitar-TECHS Excerpts", "GuitarSet Comps & Solos"};
                auto gr = [](const std::string& g) { for (int i = 0; i < 5; ++i) if (g == kGroupOrder[i]) return i; return 5; };
                if (gr(a.group) != gr(b.group)) return gr(a.group) < gr(b.group);
                return a.group < b.group;
            }
            return a.path < b.path;
        });
        std::lock_guard<std::mutex> lk(tracks_mtx_);
        tracks_ = std::move(found);
    }

    // Decodes a track to mono at the engine rate, peak-normalised to a realistic guitar level (see below).
    std::unique_ptr<TrackPCM> decodeTrack(const std::string& path) const {
        WavData wav;
        if (!readWavFile(path, wav) || wav.left.empty()) return nullptr;
        auto pcm = std::make_unique<TrackPCM>();
        const double ratio = double(wav.sample_rate) / sample_rate_;
        const std::size_t out_frames = static_cast<std::size_t>(double(wav.left.size()) / ratio);
        std::vector<float> tmp(out_frames);
        float peak = 0.0f;
        const std::size_t last = wav.left.size() - 1;
        for (std::size_t i = 0; i < out_frames; ++i) {
            const double src = double(i) * ratio;
            const std::size_t i0 = std::min(static_cast<std::size_t>(src), last), i1 = std::min(i0 + 1, last);
            const float fr = static_cast<float>(src - double(i0));
            const float a = 0.5f * (wav.left[i0] + wav.right[i0]), b = 0.5f * (wav.left[i1] + wav.right[i1]);
            const float v = a + fr * (b - a);
            tmp[i] = v;
            peak = std::max(peak, std::abs(v));
        }
        // Test DI tracks are mastered near full scale, but a real guitar peaks around 0.3-0.4 V at the amp input. The
        // circuit units are calibrated in volts (1.0 FS = 1 V at the input jack), so play the tracks at a single-coil
        // level: 0.35 FS peaks. At 0.85 they drove every amp about 8 dB harder than a Strat would.
        const float g = peak > 1e-4f ? 0.35f / peak : 1.0f;
        pcm->mono.resize(out_frames);
        for (std::size_t i = 0; i < out_frames; ++i)
            pcm->mono[i] = static_cast<int16_t>(std::lround(std::clamp(tmp[i] * g, -1.0f, 1.0f) * 32767.0f));
        return pcm;
    }

    // Makes `idx` the playing track. Decoding happens on the calling thread; the audio thread only ever
    // sees a finished buffer, and retired buffers are freed here (never on the audio thread).
    bool activateTrack(int idx) {
        std::string path;
        {
            std::lock_guard<std::mutex> lk(tracks_mtx_);
            if (idx < 0 || idx >= static_cast<int>(tracks_.size())) return false;
            path = tracks_[static_cast<std::size_t>(idx)].path;
        }
        auto pcm = decodeTrack(path);
        if (!pcm) return false;
        std::lock_guard<std::mutex> lk(pcm_mtx_);
        const auto now = std::chrono::steady_clock::now();
        const TrackPCM* old = active_pcm_.load();
        playhead_frame_.store(0);
        active_pcm_.store(pcm.get(), std::memory_order_release);
        active_track_idx_.store(idx);
        for (auto& e : pcm_pool_) if (e.first.get() == old) e.second = now;
        pcm_pool_.emplace_back(std::move(pcm), std::chrono::steady_clock::time_point::max());
        // Free buffers that have been off the audio thread for a while.
        pcm_pool_.erase(std::remove_if(pcm_pool_.begin(), pcm_pool_.end(), [&](const auto& e) {
            return e.first.get() != active_pcm_.load() && e.second != std::chrono::steady_clock::time_point::max() &&
                   now - e.second > std::chrono::seconds(2);
        }), pcm_pool_.end());
        return true;
    }

    // Frees DI buffers the audio thread stopped using more than 2 s ago (called from the HTTP thread).
    void purgeRetiredTracks() {
        std::lock_guard<std::mutex> lk(pcm_mtx_);
        const auto now = std::chrono::steady_clock::now();
        pcm_pool_.erase(std::remove_if(pcm_pool_.begin(), pcm_pool_.end(), [&](const auto& e) {
            return e.first.get() != active_pcm_.load() && e.second != std::chrono::steady_clock::time_point::max() &&
                   now - e.second > std::chrono::seconds(2);
        }), pcm_pool_.end());
    }

    // User-rig operations re-sort the user list, so the loaded rig's index can change: re-find it by file path.
    void remapActiveIndex(const std::string& path) {
        if (path.empty()) return;
        for (const auto& e : library_.entries())
            if (e.path == path) { active_preset_idx_.store(e.idx); return; }
    }

    void loadDISamples() {
        scanDITracks();
        int idx = active_track_idx_.load();
        {
            std::lock_guard<std::mutex> lk(tracks_mtx_);
            if (idx < 0 || idx >= static_cast<int>(tracks_.size())) idx = 0;
        }
        activateTrack(idx);
    }

    // ───────────── audio device management ─────────────
    inline static std::mutex noise_servers_mutex_;
    inline static std::vector<RealtimeAudioServer*> noise_servers_;
    inline static std::once_flag noise_settings_loaded_;

    void loadCircuitNoiseSetting() {
        // New plugin instances must not reset a value changed by an existing instance.
        std::call_once(noise_settings_loaded_, [this] {
            std::ifstream f(hosted_ ? library::appSupportDir() + "/plugin-settings.txt" : settingsPath());
            std::string line;
            while (std::getline(f, line)) {
                if (line == "circuit_noise=1") ce::setCircuitNoiseEnabled(true);
                else if (line == "circuit_noise=0") ce::setCircuitNoiseEnabled(false);
            }
        });
    }
    std::string soundSettingsJson() const {
        return std::string("{\"circuit_noise\":") + (ce::circuitNoiseEnabled() ? "true}" : "false}");
    }
    void setCircuitNoiseSetting(bool enabled) {
        std::lock_guard<std::mutex> lock(noise_servers_mutex_);
        ce::setCircuitNoiseEnabled(enabled);
        if (hosted_) {
            // Preserve research_root and any other plugin settings.
            const auto path = library::appSupportDir() + "/plugin-settings.txt";
            std::ifstream in(path);
            std::string text, line;
            while (std::getline(in, line)) if (line.rfind("circuit_noise=", 0) != 0) text += line + "\n";
            in.close();
            std::ofstream out(path, std::ios::trunc);
            out << text << "circuit_noise=" << (enabled ? 1 : 0) << "\n";
        } else saveAudioSettings();
        for (auto* server : noise_servers_) {
            if (!server->running_.load()) continue;
            server->enqueueJob([server] {
                GraphEditGuard edit(*server); // syncCircuitNoise + publish, entirely off the callback
            });
        }
    }

    std::string settingsPath() const { return library::appSupportDir() + "/audio-settings.txt"; }

    bool asioRequested() const {
#if defined(_WIN32) && RIG_WITH_ASIO
        return audio_settings_.backend == "asio" || preferredBackend() == AudioBackend::Asio;
#else
        return false;
#endif
    }

    void loadAudioSettings() {
        std::ifstream f(settingsPath());
        std::string line;
        while (std::getline(f, line)) {
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            try {
                if (k == "source") audio_settings_.source = v;
#if defined(_WIN32) && RIG_WITH_ASIO
                else if (k == "backend") audio_settings_.backend = v == "asio" ? "asio" : "default";
                else if (k == "asio_driver") audio_settings_.asio_driver = v;
#endif
                else if (k == "input_id") audio_settings_.input_id = static_cast<uint32_t>(safeParse<unsigned long>(v, audio_settings_.input_id));
                else if (k == "output_id") audio_settings_.output_id = static_cast<uint32_t>(safeParse<unsigned long>(v, audio_settings_.output_id));
                else if (k == "input_channel") audio_settings_.input_channel = safeParse<int>(v, audio_settings_.input_channel);
                else if (k == "input_mode") audio_settings_.input_mode = v;
                else if (k == "sample_rate") audio_settings_.sample_rate = safeParse<double>(v, audio_settings_.sample_rate);
                else if (k == "buffer_size") audio_settings_.buffer_size = safeParse<int>(v, audio_settings_.buffer_size);
                else if (k == "input_gain_db") audio_settings_.input_gain_db = safeParse<float>(v, audio_settings_.input_gain_db);
                else if (k == "rig_input_db") audio_settings_.rig_input_db = std::clamp(safeParse<float>(v, audio_settings_.rig_input_db), -24.0f, 12.0f);
                else if (k == "output_gain_db") audio_settings_.output_gain_db = std::clamp(safeParse<float>(v, audio_settings_.output_gain_db), -60.0f, 12.0f);
                else if (k == "monitor") audio_settings_.monitor = v == "1";
            } catch (...) {}
        }
        if (asioRequested()) return; // ASIO opens a single driver by name.
        // Device ids are not stable across reboots/replugs: drop ids that no longer exist.
        auto devs = AudioIO::devices();
        auto exists = [&](uint32_t id) { return id == 0 || std::any_of(devs.begin(), devs.end(), [&](const DeviceInfo& d) { return d.id == id; }); };
        if (!exists(audio_settings_.input_id)) audio_settings_.input_id = 0;
        if (!exists(audio_settings_.output_id)) audio_settings_.output_id = 0;
    }

    std::string audioSettingsText() const {
        std::ostringstream f;
        const auto& a = audio_settings_;
#if defined(_WIN32) && RIG_WITH_ASIO
        f << "backend=" << a.backend << "\nasio_driver=" << a.asio_driver << "\n";
#endif
        f << "source=" << a.source << "\ninput_id=" << a.input_id << "\noutput_id=" << a.output_id << "\ninput_channel=" << a.input_channel
          << "\ninput_mode=" << a.input_mode << "\nsample_rate=" << a.sample_rate << "\nbuffer_size=" << a.buffer_size
          << "\ninput_gain_db=" << a.input_gain_db << "\nrig_input_db=" << a.rig_input_db << "\noutput_gain_db=" << a.output_gain_db
          << "\nmonitor=" << (a.monitor ? 1 : 0) << "\ncircuit_noise=" << (ce::circuitNoiseEnabled() ? 1 : 0) << "\n";
        return f.str();
    }
    // Writes `text` unless a newer version was already written (the debounced saver and an immediate save race).
    void writeSettingsVersion(const std::string& text, uint64_t version) {
        std::lock_guard<std::mutex> lk(save_file_mtx_);
        if (version <= settings_written_version_) return;
        settings_written_version_ = version;
        std::ofstream f(settingsPath(), std::ios::trunc);
        f << text;
    }
    void saveAudioSettings() {
        uint64_t v;
        {
            std::lock_guard<std::mutex> lk(save_mtx_);
            v = ++settings_version_;
            save_pending_ = false; // this write supersedes a pending debounced one
        }
        writeSettingsVersion(audioSettingsText(), v);
    }
    // Knob drags (IN / OUT / input gain) save 500 ms after the last move, off the HTTP thread: no disk write per tick.
    void scheduleAudioSettingsSave() {
        std::string text = audioSettingsText();
        {
            std::lock_guard<std::mutex> lk(save_mtx_);
            save_text_ = std::move(text);
            save_text_version_ = ++settings_version_;
            save_due_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            save_pending_ = true;
            if (!settings_saver_.joinable()) {
                saver_stop_ = false;
                settings_saver_ = std::thread([this] { settingsSaverLoop(); });
            }
        }
        save_cv_.notify_one();
    }
    void settingsSaverLoop() {
        std::unique_lock<std::mutex> lk(save_mtx_);
        for (;;) {
            if (!save_pending_) {
                if (saver_stop_) return;
                save_cv_.wait(lk);
                continue;
            }
            if (!saver_stop_ && std::chrono::steady_clock::now() < save_due_) {
                save_cv_.wait_until(lk, save_due_);
                continue;
            }
            std::string text = std::move(save_text_);
            const uint64_t v = save_text_version_;
            save_pending_ = false;
            lk.unlock();
            writeSettingsVersion(text, v);
            lk.lock();
        }
    }
    void stopSettingsSaver() { // flushes a pending save
        {
            std::lock_guard<std::mutex> lk(save_mtx_);
            saver_stop_ = true;
        }
        save_cv_.notify_all();
        if (settings_saver_.joinable()) settings_saver_.join();
    }
    // Query names only (no values): true when every key is one of `allowed`.
    static bool queryKeysWithin(const std::string& query, std::initializer_list<std::string_view> allowed) {
        std::size_t pos = 0;
        bool any = false;
        while (pos <= query.size()) {
            const std::size_t amp = std::min(query.find('&', pos), query.size());
            const std::string_view kv(query.data() + pos, amp - pos);
            const std::string_view key = kv.substr(0, std::min(kv.find('='), kv.size()));
            if (!key.empty()) {
                if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) return false;
                any = true;
            }
            pos = amp + 1;
        }
        return any;
    }

    bool startAudio() {
        const bool live = audio_settings_.source == "live";
        live_gain_.store(std::pow(10.0f, audio_settings_.input_gain_db / 20.0f));
        monitor_.store(audio_settings_.monitor);
        applyTopbarGains();
        auto fn = [this](const float* inL, const float* inR, float* outL, float* outR, uint32_t n) {
            rig::platform::ScopedNoDenormalsX86 ftz; // Intel Macs (universal build); miniaudio callbacks set their own
            renderBlock(inL, inR, outL, outR, n);
        };
        audio_error_.clear();
        if (!audio_io_.start(audio_settings_, live, fn, &audio_error_)) { live_.store(false); return false; }
        if (std::abs(audio_io_.sampleRate() - sample_rate_) > 0.5) {
            // Device runs at a different rate: re-prepare the DSP and DI tracks, then restart.
            const double device_rate = audio_io_.sampleRate();
            audio_io_.stop();
            sample_rate_ = device_rate;
            global_looper_.prepare(sample_rate_.load());
            global_transpose_.prepare(sample_rate_.load());
            global_space_.prepare(sample_rate_.load());
            backing_.setSampleRate(sample_rate_.load()); // Practice backing track: re-decode at the device rate
            loadDISamples();

            {
                GraphEditGuard lock(*this);
                ++graph_lineage_; // sample-rate changes cannot inherit incompatible DSP histories
                if (graph_) graph_->prepare(sample_rate_, 512); // never re-levelled at a new rate
            }
            if (!audio_io_.start(audio_settings_, live, fn, &audio_error_)) { live_.store(false); return false; }
        }
        if (asioRequested() && std::abs(audio_io_.sampleRate() - sample_rate_) > 0.5) {
            audio_io_.stop();
            live_.store(false);
            audio_error_ = "ASIO sample rate changed while starting. Retry after the driver clock settles.";
            return false;
        }
#if defined(_WIN32) && RIG_WITH_ASIO
        if (audio_settings_.backend == "asio" && audio_settings_.asio_driver.empty())
            audio_settings_.asio_driver = audio_io_.activeAsioDriver();
#endif
        live_.store(live && audio_io_.inputRunning());
        return true;
    }

    std::string buildAudioJson() const {
        std::ostringstream o;
        const bool asio = asioRequested();
        auto devs = asio ? std::vector<DeviceInfo>{} : AudioIO::devices();
        auto list = [&](bool inputs) {
            o << "[";
            bool first = true;
            for (const auto& d : devs) {
                if ((inputs ? d.input_channels : d.output_channels) <= 0) continue;
                o << (first ? "" : ",") << "{\"id\":" << d.id << ",\"name\":\"" << jsonEscape(d.name) << "\",\"channels\":"
                  << (inputs ? d.input_channels : d.output_channels) << ",\"sample_rates\":[";
                for (std::size_t i = 0; i < d.sample_rates.size(); ++i) o << (i ? "," : "") << static_cast<int>(d.sample_rates[i]);
                o << "]}";
                first = false;
            }
            o << "]";
        };
        const auto& a = audio_settings_;
        o << "{\"inputs\":";
        list(true);
        o << ",\"outputs\":";
        list(false);
        std::string error = audio_error_;
#if defined(_WIN32) && RIG_WITH_ASIO
        const auto drivers = AudioIO::asioDrivers();
        o << ",\"asio_requested\":" << (asio ? "true" : "false") << ",\"asio_drivers\":[";
        for (std::size_t i = 0; i < drivers.size(); ++i)
            o << (i ? "," : "") << "\"" << jsonEscape(drivers[i]) << "\"";
        o << "]";
        const auto driver = audio_io_.activeAsioDriver();
        const auto driver_error = audio_io_.asioError();
        if (!driver_error.empty()) error = driver_error;
#endif
        o << ",\"current\":{";
#if defined(_WIN32) && RIG_WITH_ASIO
        o << "\"backend\":\"" << a.backend << "\",\"asio_driver\":\"" << jsonEscape(a.asio_driver)
          << "\",\"requested_sample_rate\":" << a.sample_rate << ",\"requested_buffer_size\":" << a.buffer_size << ",";
#endif
        const bool stopped_asio = asio && !audio_io_.running();
        const int rate = asio ? (stopped_asio ? 0 : static_cast<int>(audio_io_.sampleRate())) : static_cast<int>(sample_rate_);
        const uint32_t input = asio ? 0 : (a.input_id ? a.input_id : AudioIO::defaultInputDevice());
        o << "\"source\":\"" << a.source << "\",\"input_id\":" << input
          << ",\"output_id\":" << audio_io_.outputDevice() << ",\"input_channel\":" << a.input_channel << ",\"input_mode\":\"" << a.input_mode
          << "\",\"sample_rate\":" << rate << ",\"buffer_size\":" << (stopped_asio ? 0 : audio_io_.bufferSize())
          << ",\"input_gain_db\":" << a.input_gain_db << ",\"rig_input_db\":" << a.rig_input_db << ",\"output_gain_db\":" << a.output_gain_db << ",\"monitor\":" << (a.monitor ? "true" : "false") << "}";
        o << ",\"buffer_sizes\":[32,64,128,256,512,1024]";
        o << ",\"status\":{";
#if defined(_WIN32) && RIG_WITH_ASIO
        const char* backend = !audio_io_.running() ? "stopped" : driver.empty() ? backendName(activeBackend()) : "asio";
        o << "\"active_backend\":\"" << backend << "\",\"active_asio_driver\":\"" << jsonEscape(driver)
          << "\",\"asio_input_channels\":" << audio_io_.asioInputChannels() << ",";
#endif
        o << "\"running\":" << (audio_io_.running() ? "true" : "false") << ",\"input_ok\":" << (audio_io_.inputRunning() ? "true" : "false")
          << ",\"latency_ms\":" << audio_io_.latencyMs() << ",\"underruns\":" << audio_io_.underruns() << ",\"error\":\"" << jsonEscape(error)
          << "\",\"mic_permission\":\"" << AudioIO::micPermission() << "\"}}";
        return o.str();
    }

    void applyAudioSettings(const std::string& query) {
        auto& a = audio_settings_;
        auto has = [&](const char* k) { return !getQueryParam(query, k).empty(); };
        bool backend_changed = false;
#if defined(_WIN32) && RIG_WITH_ASIO
        if (has("backend")) {
            const auto backend = getQueryParam(query, "backend");
            if (backend != "asio" && backend != "default") { audio_error_ = "Unknown audio backend."; return; }
        }
        if (has("asio_driver")) {
            const auto driver = getQueryParam(query, "asio_driver");
            if (driver.find_first_of("\r\n\0", 0, 3) != std::string::npos) { audio_error_ = "Invalid ASIO driver name."; return; }
            a.asio_driver = driver;
        }
        if (has("backend")) a.backend = getQueryParam(query, "backend");
        backend_changed = has("backend") || has("asio_driver");
#endif
        try {
            if (has("source")) a.source = getQueryParam(query, "source") == "live" ? "live" : "player";
            if (has("input_id")) a.input_id = static_cast<uint32_t>(safeParse<unsigned long>(getQueryParam(query, "input_id"), a.input_id));
            if (has("output_id")) a.output_id = static_cast<uint32_t>(safeParse<unsigned long>(getQueryParam(query, "output_id"), a.output_id));
            if (has("input_channel")) a.input_channel = std::max(0, safeParse<int>(getQueryParam(query, "input_channel"), a.input_channel));
            if (has("input_mode")) a.input_mode = getQueryParam(query, "input_mode") == "stereo" ? "stereo" : "mono";
            if (has("sample_rate")) a.sample_rate = safeParse<double>(getQueryParam(query, "sample_rate"), a.sample_rate);
            if (has("buffer_size")) a.buffer_size = safeParse<int>(getQueryParam(query, "buffer_size"), a.buffer_size);
            if (has("input_gain_db")) a.input_gain_db = std::clamp(safeParse<float>(getQueryParam(query, "input_gain_db"), a.input_gain_db), -24.0f, 36.0f);
            if (has("monitor")) a.monitor = getQueryParam(query, "monitor") == "1";
            if (has("rig_input_db")) a.rig_input_db = std::clamp(safeParse<float>(getQueryParam(query, "rig_input_db"), a.rig_input_db), -24.0f, 12.0f);
            if (has("output_gain_db")) a.output_gain_db = std::clamp(safeParse<float>(getQueryParam(query, "output_gain_db"), a.output_gain_db), -60.0f, 12.0f);
        } catch (...) {}
        if (has("rig_input_db") || has("output_gain_db")) applyTopbarGains();
        // Gain / monitor changes don't need a device restart.
        const bool needs_restart = backend_changed || has("source") || has("input_id") || has("output_id") || has("input_channel") || has("input_mode") ||
                                   has("sample_rate") || has("buffer_size");
        live_gain_.store(std::pow(10.0f, a.input_gain_db / 20.0f));
        monitor_.store(a.monitor);
        if (a.source == "live" && AudioIO::micPermission() == "undetermined") AudioIO::requestMicPermission();
        if (queryKeysWithin(query, {"input_gain_db", "rig_input_db", "output_gain_db", "lite"})) { // a knob drag
            scheduleAudioSettingsSave();
            return;
        }
        if (needs_restart) {
            if (!startAudio() && !asioRequested()) {
                const std::string e = audio_error_;
                AudioSettings fallback = a;
                fallback.source = "player";
                fallback.output_id = 0;
                audio_settings_ = fallback;
                startAudio();
                audio_error_ = e;
                audio_settings_.source = a.source;
            }
        }
        saveAudioSettings();
    }

    void renderBlock(const float* liveL, const float* liveR, float* out_l_all, float* out_r_all, uint32_t total) {
        const auto started = std::chrono::steady_clock::now();
        const uint64_t started_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(started.time_since_epoch()).count());
        diag_bank_->beginRender(started_ns); // /api/diag: atomics only
        const double rate = sample_rate_.load(std::memory_order_relaxed);
        for (uint32_t off = 0; off < total; off += 1024) {
            const uint32_t n = std::min<uint32_t>(1024, total - off);
            renderChunk(liveL + off, liveR + off, out_l_all + off, out_r_all + off, n);
        }
        // Bounded single-writer telemetry: no allocation, mutex or retry loop.
        // This times our render work, not AudioUnit/device scheduling or xruns.
        if (total && rate > 0 && std::isfinite(rate)) {
            const auto elapsed = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - started).count());
            const auto budget = std::max<uint64_t>(1, static_cast<uint64_t>(1e9 * total / rate));
            render_time_ns_.fetch_add(elapsed, std::memory_order_relaxed);
            render_budget_ns_.fetch_add(budget, std::memory_order_relaxed);
            render_last_ratio_.store(static_cast<float>(double(elapsed) / budget), std::memory_order_relaxed);
            if (elapsed > budget) render_over_budget_.fetch_add(1, std::memory_order_relaxed);
            if (elapsed > render_peak_ns_.load(std::memory_order_relaxed)) render_peak_ns_.store(elapsed, std::memory_order_relaxed);
            render_blocks_.fetch_add(1, std::memory_order_release);
            diag_bank_->endRender(started_ns + elapsed, elapsed, budget, total);
        }
    }

    void renderChunk(const float* liveL, const float* liveR, float* out_l, float* out_r, UInt32 num_frames) {
        std::array<float, 1024> in_l{};
        std::array<float, 1024> in_r{};
        UInt32 frames = std::min<UInt32>(num_frames, 1024);

        const bool live = live_.load(std::memory_order_relaxed);
        bool is_playing = playing_.load(std::memory_order_relaxed);
        if (live) {
            const float g_target = live_gain_.load(std::memory_order_relaxed);
            if (live_gain_cur_ < 0.0f) live_gain_cur_ = g_target;
            const float glide = 1.0f - std::exp(-1.0f / (0.010f * static_cast<float>(sample_rate_.load(std::memory_order_relaxed))));
            float pk = 0.0f;
            for (UInt32 i = 0; i < frames; ++i) {
                live_gain_cur_ += (g_target - live_gain_cur_) * glide; // input-gain slider: ~10 ms glide, no zipper
                const float g = live_gain_cur_;
                in_l[i] = liveL[i] * g;
                in_r[i] = liveR[i] * g;
                pk = std::max(pk, std::max(std::abs(liveL[i]), std::abs(liveR[i])));
            }
            if (pk > 0.98f) clip_hold_.store(static_cast<int>(sample_rate_ * 0.6), std::memory_order_relaxed);
            else clip_hold_.store(std::max(0, clip_hold_.load(std::memory_order_relaxed) - static_cast<int>(frames)), std::memory_order_relaxed);
        } else if (is_playing) {
            if (const TrackPCM* trk = active_pcm_.load(std::memory_order_acquire)) {
                std::size_t pos = playhead_frame_.load(std::memory_order_relaxed);
                std::size_t len = trk->mono.size();
                if (len > 0) {
                    const bool ab_on = trainer_ab_loop_.load(std::memory_order_relaxed);
                    const std::size_t loop_a_idx = static_cast<std::size_t>(std::clamp(trainer_loop_a_.load(std::memory_order_relaxed), 0.0f, 0.98f) * double(len));
                    const std::size_t loop_b_idx = std::max(loop_a_idx + 1, static_cast<std::size_t>(std::clamp(trainer_loop_b_.load(std::memory_order_relaxed), 0.02f, 1.0f) * double(len)));
                    const float speed = std::clamp(trainer_speed_.load(std::memory_order_relaxed), 0.50f, 1.50f);
                    const float stem_cut = std::clamp(trainer_stem_cut_.load(std::memory_order_relaxed), 0.0f, 1.0f);
                    double fpos = static_cast<double>(pos) + trainer_frac_pos_;
                    for (UInt32 i = 0; i < frames; ++i) {
                        std::size_t ipos = static_cast<std::size_t>(fpos);
                        if (ab_on && ipos >= loop_b_idx) {
                            fpos = static_cast<double>(loop_a_idx);
                            ipos = loop_a_idx;
                        } else if (ipos >= len) {
                            if (loop_.load(std::memory_order_relaxed)) {
                                fpos = ab_on ? static_cast<double>(loop_a_idx) : 0.0;
                                ipos = static_cast<std::size_t>(fpos);
                            } else {
                                playing_.store(false, std::memory_order_relaxed);
                                is_playing = false;
                                break;
                            }
                        }
                        const std::size_t ipos1 = std::min(ipos + 1, len - 1);
                        const float frac = static_cast<float>(fpos - std::floor(fpos));
                        const float s0 = trk->mono[ipos] * TrackPCM::kScale;
                        const float s1 = trk->mono[ipos1] * TrackPCM::kScale;
                        float s = s0 + frac * (s1 - s0);
                        if (stem_cut > 1e-3f) {
                            // Mid-band guitar stem cut filter (attenuates 250 Hz - 3.6 kHz fundamental/presence)
                            trainer_lp_z_ += 0.08f * (s - trainer_lp_z_);
                            const float mid_band = s - trainer_lp_z_;
                            s -= stem_cut * 0.65f * mid_band;
                        }
                        in_l[i] = in_r[i] = s;
                        fpos += static_cast<double>(speed);
                    }
                    const std::size_t final_pos = static_cast<std::size_t>(fpos);
                    trainer_frac_pos_ = fpos - static_cast<double>(final_pos);
                    playhead_frame_.store(final_pos, std::memory_order_relaxed);
                }
            }
        }

        // Observe input for 1-click Auto Input Gain Calibration
        input_calibrator_.observeBlock(in_l.data(), in_r.data(), frames, sample_rate_.load(std::memory_order_relaxed));

        // Global Polyphonic Transpose / Drop-Tuner (pre-rig input)
        if (global_transpose_.isActive()) {
            global_transpose_.processStereo(in_l.data(), in_r.data(), frames);
        }

        // Global Looper (LoopStation): runs here on the DI when set to Pre-rig (re-amp: the loop plays through the
        // rig), otherwise after the rig (processPost below). Tempo = host clock, else the rig tempo (last block's).
        const double cur_bpm = hostTempo().bpm() > 0.0 ? hostTempo().bpm() : looper_rig_bpm_;
        global_looper_.processPre(in_l.data(), in_r.data(), frames, cur_bpm);
        const bool looper_active = global_looper_.active();

        std::array<float, 1024> proc_l{};
        std::array<float, 1024> proc_r{};

        if (auto* audible = graph_runtime_.beginBlock()) {
            looper_rig_bpm_ = audible->tempo();   // looper: the rig tempo (audio-thread copy of the published plan)
            const auto revision = graph_runtime_.audibleRevision();
            audible->setInputGainLinear(input_gain_.load(std::memory_order_relaxed)); // IN knob: no plan per move
            // Knob / fader edits of the same rack inherit the DSP history and are already continuous: bridging
            // them from a held sample dipped the output on every request while dragging (choppy audio).
            if (revision != audio_revision_ && graph_runtime_.lastHandoffContinuous() && audible->continuousEdit()) {
                audio_revision_ = revision;
            } else if (revision != audio_revision_) {
                audio_revision_ = revision;
                transition_remaining_ = std::max(1, static_cast<int>(sample_rate_.load() * 0.005));
                transition_total_ = transition_remaining_;
                diag_bank_->noteBridge();
                transition_l_ = last_graph_l_;
                transition_r_ = last_graph_r_;
            }
            UInt32 offset = 0;
            while (offset < frames) {
                const UInt32 chunk = std::min<UInt32>(512, frames - offset);
                const float* ins[2] = {in_l.data() + offset, in_r.data() + offset};
                float* outs[2] = {proc_l.data() + offset, proc_r.data() + offset};
                AudioBlock block{};
                block.in = ins;
                block.out = outs;
                block.num_channels = 2;
                block.num_frames = chunk;
                block.sample_rate = sample_rate_.load();
                audible->process(block);
                offset += chunk;
            }
            // A short bounded bridge from the previous output avoids a hard step
            // without rendering shared component state twice during an edit.
            for (UInt32 i = 0; i < frames; ++i) {
                if (transition_remaining_ > 0) {
                    const float t = 1.0f - static_cast<float>(transition_remaining_) / transition_total_;
                    proc_l[i] = transition_l_ + t * (proc_l[i] - transition_l_);
                    proc_r[i] = transition_r_ + t * (proc_r[i] - transition_r_);
                    --transition_remaining_;
                }
                last_graph_l_ = proc_l[i];
                last_graph_r_ = proc_r[i];
            }
        }

        // Global Looper, Post-rig: records / plays the rig output; always adds the click (never recorded)
        global_looper_.processPost(proc_l.data(), proc_r.data(), frames, cur_bpm);

        // Global Stereo Doubler & Amp-in-the-Room Binaural Spatializer (post-rig output)
        if (global_space_.doublerEnabled() || global_space_.roomEnabled()) {
            global_space_.processStereo(proc_l.data(), proc_r.data(), frames);
        }

        constexpr float kDcR = 0.998f;
        const float out_gain_tgt_now = out_gain_target_.load(std::memory_order_relaxed);
        double sum_in = 0.0;
        double sum_l = 0.0;
        double sum_r = 0.0;
        EngineDiagBank::OutputVals odiag; // /api/diag output stage (plain locals; published once below)
        for (UInt32 i = 0; i < frames; ++i) {
            float xl = proc_l[i];
            float xr = proc_r[i];
            float yl = xl - dc_x1_l_ + kDcR * dc_y1_l_;
            float yr = xr - dc_x1_r_ + kDcR * dc_y1_r_;
            dc_x1_l_ = xl;
            dc_y1_l_ = yl;
            dc_x1_r_ = xr;
            dc_y1_r_ = yr;

            // The rack's master limiter already holds peaks under -1 dBFS; this is only a last-resort
            // guard (transparent below 0.95) instead of a full-range tanh that dulled every preset.
            auto guard = [](float v) noexcept {
                const float a = std::abs(v);
                if (a <= 0.95f) return v;
                const float x = (a - 0.95f) / 0.05f;
                return std::copysign(0.95f + 0.05f * (x / (1.0f + x)), v);
            };
            const bool audible = (is_playing || live || looper_active) && monitor_.load(std::memory_order_relaxed);
            // master output level (top-bar OUT), ~10 ms smoothing so knob moves don't zipper
            out_gain_cur_ += (out_gain_tgt_now - out_gain_cur_) * 0.002f;
            const float pre_l = yl * out_gain_cur_, pre_r = yr * out_gain_cur_;
            float sl = audible ? guard(pre_l) : 0.0f;
            float sr = audible ? guard(pre_r) : 0.0f;
            if (audible) {
                const float al = std::abs(pre_l), ar = std::abs(pre_r);
                if (al > 0.95f) ++odiag.guard_l;
                if (ar > 0.95f) ++odiag.guard_r;
                odiag.pre_guard_peak = std::max(odiag.pre_guard_peak, std::max(al, ar));
            }
            {
                const float il = std::abs(in_l[i]), ir = std::abs(in_r[i]);
                odiag.in_peak = std::max(odiag.in_peak, std::max(il, ir));
                if (il > 0.98f || ir > 0.98f) ++odiag.in_clip;
            }
            out_l[i] = sl;
            out_r[i] = sr;
            sum_in += double(in_l[i]) * double(in_l[i]);
            sum_l += double(sl) * double(sl);
            sum_r += double(sr) * double(sr);
        }
        for (UInt32 i = frames; i < num_frames; ++i) {
            out_l[i] = 0.0f;
            out_r[i] = 0.0f;
        }

        if (frames > 0) {
            float cur_in = static_cast<float>(std::sqrt(sum_in / double(frames)));
            float cur_l = static_cast<float>(std::sqrt(sum_l / double(frames)));
            float cur_r = static_cast<float>(std::sqrt(sum_r / double(frames)));
            in_rms_.store(0.8f * in_rms_.load(std::memory_order_relaxed) + 0.2f * cur_in, std::memory_order_relaxed);
            (void)cur_l; (void)cur_r;   // the top-bar OUT meter is measured after the backing track is mixed in (below)

            for (int w = 0; w < 64; ++w) {
                UInt32 idx = (w * frames) / 64;
                wave_in_[w].store(in_l[idx] * input_gain_.load(std::memory_order_relaxed), std::memory_order_relaxed);
                wave_out_[w].store(out_l[idx], std::memory_order_relaxed);
            }
            // Scope history: peak-preserving 4:1 decimation into a lock-free ring (wait-free relaxed stores;
            // display data only, so a torn read across a wrap is harmless). Read by /api/scope.
            {
                const float ig = input_gain_.load(std::memory_order_relaxed);
                uint64_t pos = scope_pos_.load(std::memory_order_relaxed);
                for (UInt32 i = 0; i + kScopeDecim <= frames; i += kScopeDecim) {
                    float bi = 0.0f, bo = 0.0f;
                    for (UInt32 k = 0; k < kScopeDecim; ++k) {
                        const float vi = in_l[i + k] * ig, vo = out_l[i + k];
                        if (std::fabs(vi) > std::fabs(bi)) bi = vi;
                        if (std::fabs(vo) > std::fabs(bo)) bo = vo;
                    }
                    scope_in_[pos & (kScopeLen - 1)].store(bi, std::memory_order_relaxed);
                    scope_out_[pos & (kScopeLen - 1)].store(bo, std::memory_order_relaxed);
                    ++pos;
                }
                scope_pos_.store(pos, std::memory_order_release);
            }
            // Listen pane (/api/listen): full-rate dry input (post input gain) + rig output into the analysis
            // ring. Only while the pane is polling; wait-free relaxed stores, no locks or allocation.
            if (listen_.wanted()) listen_.write(in_l.data(), input_gain_.load(std::memory_order_relaxed), out_l, frames);
        }
        // Practice backing track: added after the rig, global FX, output guard and the meters/scope above (so those
        // still show the guitar alone). The pre-rig input drives its optional duck. Lock-free: see BackingTrackPlayer.
        backing_.player().process(out_l, out_r, frames, in_l.data(), in_r.data());
        // Top-bar OUT meter = what you hear: rig + backing track (the scope / Listen pane above stay guitar-only).
        if (frames > 0) {
            double ol = 0.0, orr = 0.0;
            for (UInt32 i = 0; i < frames; ++i) { ol += double(out_l[i]) * double(out_l[i]); orr += double(out_r[i]) * double(out_r[i]); }
            for (UInt32 i = 0; i < frames; ++i) { // what reaches the device: 0 dBFS clips, non-finite samples
                const float a = out_l[i], b = out_r[i];
                if (EngineDiagBank::nonFinite(a) || EngineDiagBank::nonFinite(b)) { ++odiag.nonfinite; continue; }
                const float aa = std::abs(a), ab = std::abs(b);
                if (aa >= 1.0f) ++odiag.clip_l;
                if (ab >= 1.0f) ++odiag.clip_r;
                odiag.peak_l = std::max(odiag.peak_l, aa);
                odiag.peak_r = std::max(odiag.peak_r, ab);
            }
            odiag.samples = frames;
            odiag.sumsq = 0.5 * (ol + orr);
            diag_bank_->output(odiag);
            const float cl = static_cast<float>(std::sqrt(ol / double(frames))), cr = static_cast<float>(std::sqrt(orr / double(frames)));
            out_rms_l_.store(0.8f * out_rms_l_.load(std::memory_order_relaxed) + 0.2f * cl, std::memory_order_relaxed);
            out_rms_r_.store(0.8f * out_rms_r_.load(std::memory_order_relaxed) + 0.2f * cr, std::memory_order_relaxed);
        }
    }

    bool startHttpServer() {
        if (!createApiToken()) return false;
        rig::net::ensureInit(); // WSAStartup on Windows
        server_fd_ = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
        if (server_fd_ < 0) return false;

        int opt = 1;
        ::setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(http_port_);

        if (::bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            rig::net::closeSocket(server_fd_);
            server_fd_ = -1;
            return false;
        }
        if (::listen(server_fd_, 16) < 0) {
            rig::net::closeSocket(server_fd_);
            server_fd_ = -1;
            return false;
        }

        http_port_ = queryBoundPort();
        running_.store(true);
        http_thread_ = std::thread([this]() { httpLoop(); });
        return true;
    }

    template<class T> static T safeParse(const std::string& text, T fallback = T{}) noexcept {
        T value{};
        if constexpr (std::is_floating_point_v<T>) {
            char* end = nullptr;
            errno = 0;
            if constexpr (std::is_same_v<T, float>) value = std::strtof(text.c_str(), &end);
            else value = std::strtod(text.c_str(), &end);
            if (errno != 0 || end == text.c_str() || end != text.c_str() + text.size() || !std::isfinite(value))
                return fallback;
        } else {
            const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (ec != std::errc{} || end != text.data() + text.size()) return fallback;
        }
        return value;
    }

    bool createApiToken() {
        // random_device uses the OS entropy source on supported platforms; fail closed if unavailable.
        try {
            std::random_device random;
            static constexpr char hex[] = "0123456789abcdef";
            api_token_.clear();
            for (int i = 0; i < 4; ++i) {
                const uint32_t word = random();
                for (int shift = 28; shift >= 0; shift -= 4) api_token_ += hex[(word >> shift) & 15];
            }
            return true;
        } catch (...) { return false; }
    }

    static std::string urlDecode(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '%' && i + 2 < s.size()) {
                const auto hex = [](char c) {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    return -1;
                };
                const int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
                if (hi >= 0 && lo >= 0) { out.push_back(static_cast<char>((hi << 4) | lo)); i += 2; }
                else out.push_back('%');
            } else if (s[i] == '+') {
                out.push_back(' ');
            } else {
                out.push_back(s[i]);
            }
        }
        return out;
    }

    static std::string getQueryParam(const std::string& query, const std::string& key) {
        std::string needle = key + "=";
        std::size_t pos = 0;
        while (pos < query.size()) {
            std::size_t amp = query.find('&', pos);
            std::string pair = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
            if (pair.rfind(needle, 0) == 0) {
                return urlDecode(pair.substr(needle.size()));
            }
            if (amp == std::string::npos) break;
            pos = amp + 1;
        }
        return "";
    }

    void httpLoop() {
        rig::platform::ScopedNoDenormalsX86 ftz; // x86: FTZ/DAZ (no-op on Apple Silicon)
        while (running_.load()) {
            sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            int client_fd = ::accept(server_fd_, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
            if (client_fd < 0) {
                if (!running_.load()) break;
                continue;
            }
            rig::net::setRecvTimeout(client_fd, 5000);
            rig::net::setSendTimeout(client_fd, 5000);
            rig::net::noSigPipe(client_fd); // no SIGPIPE inside a DAW (SO_NOSIGPIPE on Apple)
            try { handleHttpClient(client_fd); }
            catch (...) {
                // Last-resort containment: no request exception may escape into the app/DAW.
                static constexpr char bad[] = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
                rig::net::sendBytes(client_fd, bad, sizeof(bad) - 1);
            }
            if (http_fd_adopted_) http_fd_adopted_ = false; // the preset loader answers and closes it
            else {
                { std::lock_guard<std::mutex> lock(cors_mutex_); cors_origins_.erase(client_fd); }
                rig::net::closeSocket(client_fd);
            }
        }
    }

    // POST body (Content-Length bytes after the headers), reading past the first recv when needed. Capped at
    // 8 MB and bounded by a receive timeout so a short body can't stall the HTTP thread.
    static std::string readRequestBody(int fd, const std::string& first,
                                       std::chrono::steady_clock::time_point deadline) {
        const auto hdr_end = first.find("\r\n\r\n");
        if (hdr_end == std::string::npos) return {};
        std::size_t want = 0;
        {
            std::string headers = first.substr(0, hdr_end);
            for (auto& c : headers) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            const auto cl = headers.find("\r\ncontent-length:");
            if (cl == std::string::npos) return {};
            const auto begin = headers.find_first_not_of(" \t", cl + 17);
            if (begin == std::string::npos) return {};
            want = safeParse<unsigned long>(headers.substr(begin, headers.find("\r\n", begin) - begin));
        }
        want = std::min<std::size_t>(want, 8u << 20);
        std::string body = first.substr(hdr_end + 4);
        if (body.size() < want) {
            char chunk[16384];
            while (body.size() < want) {
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
                if (remaining <= 0) throw std::runtime_error("request timeout");
                rig::net::setRecvTimeout(fd, static_cast<int>(remaining));
                const ssize_t got = ::recv(fd, chunk, std::min(sizeof(chunk), want - body.size()), 0);
                if (got <= 0) throw std::runtime_error("short request body");
                body.append(chunk, static_cast<std::size_t>(got));
            }
        }
        if (body.size() > want) body.resize(want);
        return body;
    }

    void handleHttpClient(int fd) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        std::string req;
        char buf[8192];
        while (req.find("\r\n\r\n") == std::string::npos) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0 || req.size() >= 32768) return;
            rig::net::setRecvTimeout(fd, static_cast<int>(remaining));
            const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) return;
            req.append(buf, static_cast<std::size_t>(n));
        }
        std::istringstream iss(req.substr(0, req.find("\r\n\r\n")));
        std::string method, full_path, version, line;
        iss >> method >> full_path >> version;
        std::getline(iss, line);
        std::map<std::string, std::string> headers;
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto colon = line.find(':');
            if (colon == std::string::npos) throw std::runtime_error("bad header");
            std::string key = line.substr(0, colon);
            for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            const auto begin = line.find_first_not_of(" \t", colon + 1);
            std::string value = begin == std::string::npos ? "" : line.substr(begin);
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
            if (!headers.emplace(key, value).second) throw std::runtime_error("duplicate header");
        }
        const std::string ip = "127.0.0.1:" + std::to_string(http_port_);
        const std::string local = "localhost:" + std::to_string(http_port_);
        if (headers["host"] != ip && headers["host"] != local) { sendJson(fd, "{}", "403 Forbidden"); return; }
        const std::string origin = headers["origin"];
        // WKWebView loads our bundled file:// UI, whose serialized origin is null.
        if (!origin.empty() && origin != "null" && origin != "http://" + ip && origin != "http://" + local) {
            sendJson(fd, "{}", "403 Forbidden"); return;
        }
        { std::lock_guard<std::mutex> lock(cors_mutex_); cors_origins_[fd] = origin; }
        if (method == "OPTIONS") { sendJson(fd, "", "204 No Content"); return; }
        if ((method != "GET" && method != "POST") || version != "HTTP/1.1" || full_path.empty() || full_path[0] != '/')
            throw std::runtime_error("bad request line");

        std::string path = full_path;
        std::string query;
        std::size_t qpos = full_path.find('?');
        if (qpos != std::string::npos) {
            path = full_path.substr(0, qpos);
            query = full_path.substr(qpos + 1);
        }

        // All routes are API routes. Authenticate before reading bodies or dispatching mutations.
        if (api_token_.empty() || (headers["x-headroom-token"] != api_token_ && getQueryParam(query, "token") != api_token_)) {
            sendJson(fd, "{}", "403 Forbidden"); return;
        }
        const std::string body = readRequestBody(fd, req, deadline);

        if (hosted_ && hostedHttpOverride(fd, path, query)) return; // plugin: host owns devices; no nested plugin import
        if (path == "/api/rack") {
            sendJson(fd, buildRackStateJson());
            return;
        }

        if (path.rfind("/api/plugins", 0) == 0) { // hosted AU plugins: list / import / remove / editor
            int inst = -1;
            if (path == "/api/plugins/editor" && !getQueryParam(query, "slot").empty()) {
                std::lock_guard<std::mutex> lock(graph_mutex_);
                try {
                    const auto slot = static_cast<std::size_t>(safeParse<unsigned long>(getQueryParam(query, "slot"), static_cast<unsigned long>(-1)));
                    if (graph_)
                        for (const auto& v : graph_->activeNodeViews())
                            if (v.slot_idx == slot) inst = plugins::instanceIdOf(*v.component);
                } catch (...) {}
            }
            sendJson(fd, plugins::handleApi(path, [&](const char* k) { return getQueryParam(query, k); }, inst));
            return;
        }

        if (model::handlesPath(path)) { // component detail page: exact model export, curves, user components
            bool modified = false;
            const std::string out = model::handleApi(
                path, [&](const std::string& k) { return getQueryParam(query, k); },
                [&](long slot, const model::SlotFn& fn) {
                    const bool editing = path == "/api/component/apply";
                    GraphEditGuard lock(*this, editing);
                    bool changed = false;
                    if (editing && graph_ && slot >= 0)
                        changed = graph_->editComponent(static_cast<std::size_t>(slot), fn);
                    else {
                        IComponent* c = nullptr;
                        if (graph_ && slot >= 0)
                            for (const auto& v : graph_->activeNodeViews())
                                if (static_cast<long>(v.slot_idx) == slot) c = v.component;
                        changed = fn(c);
                    }
                    if (changed && graph_) graph_->setDirty(true);
                    else lock.unchanged();
                },
                sample_rate_, &modified);
            if (modified) rack_rev_.fetch_add(1);
            sendJson(fd, out);
            return;
        }

        if (path == "/api/tempo" || path == "/api/tempo/tap") {
            double bpm = 0.0;
            if (path == "/api/tempo/tap") bpm = tapTempo();
            else if (const std::string b = getQueryParam(query, "bpm"); !b.empty()) {
                try { bpm = safeParse<double>(b); } catch (...) { bpm = 0.0; }
            } else if (const std::string n = getQueryParam(query, "nudge"); !n.empty()) {
                try { bpm = tempo() + safeParse<double>(n); } catch (...) { bpm = 0.0; }
            }
            if (bpm > 0.0) setTempo(bpm);
            sendJson(fd, tempoJson());
            return;
        }
        if (path == "/api/cabinets") {
            sendJson(fd, cabinet::catalogJson());
            return;
        }
        if (path == "/api/cabinet/convert") {
            int ns = -1;
            try { ns = convertToCabinet(static_cast<std::size_t>(safeParse<unsigned long>(getQueryParam(query, "slot"), static_cast<unsigned long>(-1)))); } catch (...) { ns = -1; }
            if (ns < 0) { sendJson(fd, "{\"ok\":false,\"error\":\"slot is not a convertible cabinet\"}"); return; }
            sendJson(fd, buildRackStateJson());
            return;
        }
        if (path == "/api/cabinet") {
            bool ok = false;
            try {
                const auto slot = static_cast<std::size_t>(safeParse<unsigned long>(getQueryParam(query, "slot"), static_cast<unsigned long>(-1)));
                ok = selectCabinet(slot, getQueryParam(query, "cab"), getQueryParam(query, "mic_a"), getQueryParam(query, "pos_a"),
                                   getQueryParam(query, "mic_b"), getQueryParam(query, "pos_b"));
            } catch (...) { ok = false; }
            if (!ok) { sendJson(fd, "{\"ok\":false,\"error\":\"slot is not a Cabinet unit\"}"); return; }
            sendJson(fd, buildRackStateJson());
            return;
        }

        if (path == "/api/listen") {
            // Listen pane: tuner (mono + polyphonic), key, chord, spectra, input health. See ENGINE_API.md.
            if (!listen_model_set_) { listen_model_set_ = true; listen_.setModelPath(repo_root_ + "/app/resources/models/guitar_midi.hgnn"); }
            listen_.start([this] { return static_cast<double>(sample_rate_.load()); });
            std::string j = listen_.poll();
            if (j.size() > 1 && j.back() == '}') {
                char b[160];
                std::snprintf(b, sizeof b, ",\"source\":\"%s\",\"latency_ms\":%.2f,\"cpu\":%.4f}", live_.load() ? "live" : "player",
                              audio_io_.latencyMs(), static_cast<double>(render_last_ratio_.load(std::memory_order_relaxed)));
                j.pop_back();
                j += b;
            }
            sendJson(fd, j);
            return;
        }
        if (path == "/api/listen/set") {
            // ?a4=440&strings=40,45,50,55,59,64 (MIDI, low→high; empty = chromatic) &reset_key=1
            const std::string a4 = getQueryParam(query, "a4"), st = getQueryParam(query, "strings");
            try { if (!a4.empty()) listen_.setReference(safeParse<double>(a4)); } catch (...) {}
            if (query.find("strings=") != std::string::npos) {
                std::vector<double> v;
                std::stringstream ss(st);
                std::string tok;
                while (std::getline(ss, tok, ',')) { try { if (!tok.empty()) v.push_back(safeParse<double>(tok)); } catch (...) {} }
                listen_.setStrings(std::move(v));
            }
            if (getQueryParam(query, "reset_key") == "1") listen_.resetKey();
            sendJson(fd, "{\"ok\":true}");
            return;
        }
        if (path == "/api/scope") {
            // New decimated scope samples since `since` (newest <= 2048): {"pos","rate","in":[..],"out":[..]}
            const uint64_t pos = scope_pos_.load(std::memory_order_acquire);
            uint64_t since = 0;
            try { since = safeParse<unsigned long long>(getQueryParam(query, "since").empty() ? "0" : getQueryParam(query, "since")); } catch (...) {}
            uint64_t from = (since == 0 || since > pos || pos - since > 2048) ? (pos > 2048 ? pos - 2048 : 0) : since;
            std::string o; o.reserve(32 * (pos - from) + 96);
            char b[32];
            o += "{\"pos\":" + std::to_string(pos) + ",\"rate\":" + std::to_string(sample_rate_ / kScopeDecim) + ",\"in\":[";
            for (uint64_t k = from; k < pos; ++k) { std::snprintf(b, sizeof b, "%s%.4f", k == from ? "" : ",", scope_in_[k & (kScopeLen - 1)].load(std::memory_order_relaxed)); o += b; }
            o += "],\"out\":[";
            for (uint64_t k = from; k < pos; ++k) { std::snprintf(b, sizeof b, "%s%.4f", k == from ? "" : ",", scope_out_[k & (kScopeLen - 1)].load(std::memory_order_relaxed)); o += b; }
            o += "]}";
            sendJson(fd, o);
            return;
        }
        if (BackingTracks::handlesPath(path)) { // Practice backing tracks (app/docs/ENGINE_API.md → Backing tracks)
            sendJson(fd, backing_.handleApi(path, [&](const char* k) { return getQueryParam(query, k); }, body));
            return;
        }
        if (BackingTracks::isYoutubeHostPath(path)) { // http origin for the Practice view's YouTube embed
            BackingTracks::sendHtml(fd, BackingTracks::youtubeHostHtml());
            return;
        }
        if (path == "/api/diag") { // engine diagnostics (RealtimeAudioServerDiag.inl); ?reset=1 clears the cumulative stats
            if (getQueryParam(query, "reset") == "1") diagReset();
            sendJson(fd, buildDiagJson(true));
            return;
        }
        if (path == "/api/telemetry") {
            purgeRetiredTracks();
            std::string tj = buildTelemetryJson();
            if (const std::string u = getQueryParam(query, "units"); !u.empty() && tj.size() > 1 && tj.back() == '}') {
                // Per-unit face meters (UnitMeters.hpp); arms metering for ~1.5 s per poll.
                tj.pop_back();
                tj += ",\"units\":" + unit_meters_->json(UnitMeterBank::parseList(u), UnitMeterBank::parseList(getQueryParam(query, "spec")),
                                                        UnitMeterBank::parseList(getQueryParam(query, "tune")), UnitMeterBank::parseList(getQueryParam(query, "wave"))) + "}";
            }
            sendJson(fd, tj);
            return;
        }

        if (path == "/api/transport") {
            std::string action = getQueryParam(query, "action");
            if (action == "play") {
                playing_.store(true);
            } else if (action == "pause") {
                playing_.store(false);
            } else if (action == "toggle") {
                playing_.store(!playing_.load());
            } else if (action == "track") {
                int tidx = safeParse<int>(getQueryParam(query, "idx"), static_cast<int>(-1));
                bool auto_play = (getQueryParam(query, "play") != "0");
                selectTrack(tidx, auto_play);
            } else if (action == "seek") {
                double norm = safeParse<double>(getQueryParam(query, "pos"));
                seekNormalized(norm);
            } else if (action == "loop") {
                loop_.store(getQueryParam(query, "val") == "1");
            } else if (action == "gain") {
                setInputGain(safeParse<float>(getQueryParam(query, "val")));
                audio_settings_.rig_input_db = std::clamp(20.0f * std::log10(std::max(1e-4f, input_gain_.load())), -24.0f, 12.0f);
            } else if (action == "bypass") {
                setMasterBypass(getQueryParam(query, "val") == "1");
            } else if (action == "preset") {
                requestPresetLoad(fd, safeParse<int>(getQueryParam(query, "idx"), static_cast<int>(-1)), getQueryParam(query, "async") == "1");
                return;
            }
            sendJson(fd, buildTelemetryJson());
            return;
        }

        if (path == "/api/param") {
            std::size_t slot = static_cast<std::size_t>(safeParse<unsigned long>(getQueryParam(query, "slot"), static_cast<unsigned long>(-1)));
            std::string bypass_str = getQueryParam(query, "bypass");
            if (!bypass_str.empty()) {
                setSlotBypass(slot, bypass_str == "1");
            }
            std::string scale_str = getQueryParam(query, "scale");
            if (!scale_str.empty()) {
                setSlotGainScale(slot, safeParse<float>(scale_str));
            }
            std::string pid = getQueryParam(query, "id");
            std::string norm_str = getQueryParam(query, "norm");
            if (!pid.empty() && !norm_str.empty()) {
                setSlotNormalizedParameter(slot, pid, safeParse<float>(norm_str));
            }
            sendJson(fd, "{\"ok\":true}");
            return;
        }

        if (path == "/api/macro") {
            int col = safeParse<int>(getQueryParam(query, "col"), static_cast<int>(-1));
            float norm = safeParse<float>(getQueryParam(query, "norm"));
            setMacroNormalized(col, norm);
            // A macro drag sends this per move: reply small (the UI paints the targets itself). full=1: the rack.
            if (getQueryParam(query, "full") == "1") sendJson(fd, buildRackStateJson());
            else sendJson(fd, "{\"ok\":true,\"col\":" + std::to_string(col) + ",\"val\":" + std::to_string(std::clamp(norm, 0.0f, 1.0f)) + "}");
            return;
        }

        if (path == "/api/rack/add") {
            int32_t tid = safeParse<int>(getQueryParam(query, "table_id"), static_cast<int>(-1));
            addComponentToRack(tid);
            sendJson(fd, buildRackStateJson());
            return;
        }

        if (path == "/api/rack/remove") {
            std::size_t slot = static_cast<std::size_t>(safeParse<unsigned long>(getQueryParam(query, "slot"), static_cast<unsigned long>(-1)));
            rack_rev_.fetch_add(1);
            removeSlotFromRack(slot);
            sendJson(fd, buildRackStateJson());
            return;
        }

        auto qi = [&](const char* k, long def) -> long {
            const std::string v = getQueryParam(query, k);
            if (v.empty()) return def;
            try { return safeParse<long>(v, def); } catch (...) { return def; }
        };
        auto qf = [&](const char* k, float def) -> float {
            const std::string v = getQueryParam(query, k);
            if (v.empty()) return def;
            try { return safeParse<float>(v, def); } catch (...) { return def; }
        };

        if (path == "/api/nam") {
            if (getQueryParam(query, "rescan") == "1") rescanNamCaptures();
            sendJson(fd, "{\"folder\":\"" + jsonEscape(namCaptureFolder()) + "\"}");
            return;
        }
        if (path == "/api/settings/sound" || path == "/api/settings/sound/set") {
            if (path == "/api/settings/sound/set") {
                const auto v = getQueryParam(query, "circuit_noise");
                if (v != "0" && v != "1") { sendJson(fd, "{}", "400 Bad Request"); return; }
                setCircuitNoiseSetting(v == "1");
            }
            sendJson(fd, soundSettingsJson());
            return;
        }
#if defined(_WIN32) && RIG_WITH_ASIO
        if (path == "/api/audio/asio/control-panel") {
            std::string err;
            const bool ok = audio_io_.openAsioControlPanel(&err);
            sendJson(fd, std::string("{\"ok\":") + (ok ? "true" : "false") + ",\"error\":\"" + jsonEscape(err) + "\"}");
            return;
        }
#endif
        if (path == "/api/audio") {
            sendJson(fd, buildAudioJson());
            return;
        }
        if (path == "/api/audio/set") {
            sendJson(fd, handleAudioSet(query));
            return;
        }
        if (path == "/api/library") {
            if (!library_ready_.load()) { sendJson(fd, "{\"loading\":true,\"presets\":[],\"count\":0}"); return; }
            sendJson(fd, library_.json());
            return;
        }
        // Library indices only become stable once the background scan has finished: a save/delete/load-by-index
        // issued during the first seconds after launch would otherwise resolve against a list that then shifts.
        auto waitLibrary = [&] { for (int i = 0; i < 200 && !library_ready_.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100)); };
        if (path == "/api/preset/load") { // off the HTTP thread (requestPresetLoad)
            requestPresetLoad(fd, static_cast<int>(qi("idx", 0)), getQueryParam(query, "async") == "1");
            return;
        }
        if (path == "/api/preset/save") {
            waitLibrary();
            std::string name = getQueryParam(query, "name");
            if (name.empty()) name = "My Rig";
            const bool has_bo = query.find("based_on=") != std::string::npos;
            std::string err;
            const int idx = savePreset(name, getQueryParam(query, "author"), &err,
                                       getQueryParam(query, "based_on"), getQueryParam(query, "based_on_path"), has_bo);
            sendJson(fd, idx >= 0 ? "{\"ok\":true,\"idx\":" + std::to_string(idx) + "}" : "{\"ok\":false,\"error\":\"" + jsonEscape(err) + "\"}");
            return;
        }
        if (path == "/api/preset/delete") {
            waitLibrary();
            const int del_idx = static_cast<int>(qi("idx", -1));
            library::LibraryEntry active_before;
            const bool had_active = library_.entry(active_preset_idx_.load(), active_before);
            const bool ok = library_.deleteUser(del_idx);
            if (ok) {
                if (active_preset_idx_.load() == del_idx) active_preset_idx_.store(-1);
                else if (had_active) remapActiveIndex(active_before.path);
                rack_rev_.fetch_add(1);
            }
            sendJson(fd, ok ? "{\"ok\":true}" : "{\"ok\":false}");
            return;
        }
        if (path == "/api/preset/rename") {
            waitLibrary();
            const int target_idx = static_cast<int>(qi("idx", -1));
            const std::string new_name = getQueryParam(query, "name");
            library::LibraryEntry old_e;
            const bool had_old = library_.entry(target_idx, old_e);
            library::LibraryEntry active_before;
            const bool had_active = library_.entry(active_preset_idx_.load(), active_before);
            // Decide BEFORE the list re-sorts whether the renamed rig is the loaded one (indices shift afterwards).
            const bool renamed_is_active = had_active && had_old && active_before.path == old_e.path;
            std::string err;
            const int new_idx = library_.renameUser(target_idx, new_name, &err);
            if (new_idx >= 0 && had_active && !renamed_is_active) { remapActiveIndex(active_before.path); rack_rev_.fetch_add(1); }
            if (new_idx >= 0) {
                std::lock_guard<std::mutex> lock(graph_mutex_);
                if (renamed_is_active) {
                    active_preset_idx_.store(new_idx);
                    cur_name_ = new_name;
                    { std::lock_guard<std::mutex> lk(name_mutex_); cur_name_snapshot_ = cur_name_; }
                    rack_rev_.fetch_add(1);
                }
            }
            sendJson(fd, new_idx >= 0 ? "{\"ok\":true,\"idx\":" + std::to_string(new_idx) + ",\"name\":\"" + jsonEscape(new_name) + "\"}"
                                      : "{\"ok\":false,\"error\":\"" + jsonEscape(err) + "\"}");
            return;
        }
        if (path == "/api/preset/duplicate") {
            waitLibrary();
            const int target_idx = static_cast<int>(qi("idx", -1));
            std::string err;
            library::LibraryEntry active_before;
            const bool had_active = library_.entry(active_preset_idx_.load(), active_before);
            const int new_idx = library_.duplicateUser(target_idx, &err);
            if (new_idx >= 0 && had_active) { remapActiveIndex(active_before.path); rack_rev_.fetch_add(1); }
            library::LibraryEntry dup_e;
            if (new_idx >= 0) library_.entry(new_idx, dup_e);
            sendJson(fd, new_idx >= 0 ? "{\"ok\":true,\"idx\":" + std::to_string(new_idx) + ",\"name\":\"" + jsonEscape(dup_e.name) + "\"}"
                                      : "{\"ok\":false,\"error\":\"" + jsonEscape(err) + "\"}");
            return;
        }
        if (path == "/api/preset/reveal") {
            waitLibrary();
            const int target_idx = static_cast<int>(qi("idx", -1));
            library::LibraryEntry e;
            const bool ok = library_.entry(target_idx, e);
            sendJson(fd, ok ? "{\"ok\":true,\"path\":\"" + jsonEscape(e.path) + "\"}" : "{\"ok\":false}");
            return;
        }
        if (path == "/api/rack/new") {
            newRack(getQueryParam(query, "template"));
            sendJson(fd, buildRackStateJson());
            return;
        }
        if (path == "/api/rack/build") { // POST body (or ?spec=) = JSON spec, see buildRack()
            const int placed = buildRack(!body.empty() ? body : getQueryParam(query, "spec"));
            if (placed < 0) { sendJson(fd, "{\"ok\":false,\"error\":\"bad spec\"}"); return; }
            std::string out = buildRackStateJson();
            out.insert(out.size() - 1, ",\"built_units\":" + std::to_string(placed));
            sendJson(fd, out);
            return;
        }
        if (path == "/api/rack/insert" && getQueryParam(query, "split").empty()) {
            rack_rev_.fetch_add(1);
            insertUnit(static_cast<int32_t>(qi("table_id", 0)), static_cast<std::size_t>(std::max(0L, qi("pos", 99999))));
            sendJson(fd, buildRackStateJson());
            return;
        }
        if (path == "/api/rack/split") {
            int sid = -1;
            {
                GraphEditGuard lock(*this);
                rack_rev_.fetch_add(1);
                if (graph_) sid = graph_->insertSplit(static_cast<std::size_t>(std::max(0L, qi("pos", 99999))), qf("xfade", 0.5f));
            }
            (void)sid;
            sendJson(fd, buildRackStateJson());
            return;
        }
        if (path == "/api/rack/split/remove") {
            {
                GraphEditGuard lock(*this);
                rack_rev_.fetch_add(1);
                if (graph_) graph_->removeSplit(static_cast<int>(qi("split", -1)), getQueryParam(query, "keep").empty() ? "A" : getQueryParam(query, "keep"));
            }
            sendJson(fd, buildRackStateJson());
            return;
        }
        if (path == "/api/rack/split/mix") {
            {
                GraphEditGuard lock(*this);
                lock.continuous(); // dragged like a knob; the mix glides (RackGraph::processNodeList)
                if (graph_) graph_->setSplitMix(static_cast<int>(qi("split", -1)), qf("xfade", 0.5f));
            }
            sendJson(fd, "{\"ok\":true}");
            return;
        }
        if (path == "/api/rack/insert" && !getQueryParam(query, "split").empty()) {
            const int32_t tid = static_cast<int32_t>(qi("table_id", 0));
            const auto init = initParamsFor(tid);
            if (plugins::isPluginTable(tid)) plugins::preinstantiate(tid, sample_rate_, 512); // instantiate outside the rack lock
            {
                GraphEditGuard lock(*this);
                rack_rev_.fetch_add(1);
                const std::string lane = getQueryParam(query, "lane");
                if (graph_) graph_->insertIntoLane(static_cast<int>(qi("split", -1)), lane.empty() ? 'A' : lane[0], static_cast<std::size_t>(std::max(0L, qi("index", 99999))), tid, init);
            }
            sendJson(fd, buildRackStateJson());
            return;
        }
        if (path == "/api/rack/move" && !getQueryParam(query, "split").empty()) {
            {
                GraphEditGuard lock(*this);
                rack_rev_.fetch_add(1);
                const std::string lane = getQueryParam(query, "lane");
                if (graph_) graph_->moveIntoLane(static_cast<std::size_t>(std::max(0L, qi("from", 0))), static_cast<int>(qi("split", -1)), lane.empty() ? 'A' : lane[0],
                                                 static_cast<std::size_t>(std::max(0L, qi("index", 99999))));
            }
            sendJson(fd, buildRackStateJson());
            return;
        }
        if (path == "/api/rack/move" && getQueryParam(query, "split").empty()) {
            rack_rev_.fetch_add(1);
            moveUnit(static_cast<std::size_t>(std::max(0L, qi("from", 0))), static_cast<std::size_t>(std::max(0L, qi("to", 0))));
            sendJson(fd, buildRackStateJson());
            return;
        }
        if (path == "/api/rack/component-preset") {
            rack_rev_.fetch_add(1);
            applyComponentPreset(static_cast<std::size_t>(std::max(0L, qi("slot", 0))), getQueryParam(query, "name"));
            sendJson(fd, buildRackStateJson());
            return;
        }
        if (path == "/api/components") {
            sendJson(fd, buildComponentsJson());
            return;
        }
        if (path == "/api/tracks/envelope") {
            // Whole-track RMS envelope for the Practice timeline: {"idx","bins":[0..1 x n]} (normalised to the peak bin).
            const int idx = static_cast<int>(qi("idx", 0));
            const int n = static_cast<int>(std::clamp(qi("bins", 900), 16L, 4096L));
            std::string p;
            { std::lock_guard<std::mutex> lk(tracks_mtx_); if (idx >= 0 && idx < static_cast<int>(tracks_.size())) p = tracks_[static_cast<std::size_t>(idx)].path; }
            WavData w;
            if (p.empty() || !readWavFile(p, w) || w.left.empty()) { sendJson(fd, "{\"ok\":false}"); return; }
            std::vector<double> e(static_cast<std::size_t>(n), 0.0);
            const std::size_t N = w.left.size();
            for (int b = 0; b < n; ++b) {
                const std::size_t a = N * static_cast<std::size_t>(b) / n, z = std::max(a + 1, N * static_cast<std::size_t>(b + 1) / n);
                double acc = 0;
                for (std::size_t i = a; i < z && i < N; ++i) { const double v = 0.5 * (w.left[i] + w.right[i]); acc += v * v; }
                e[static_cast<std::size_t>(b)] = std::sqrt(acc / double(z - a));
            }
            const double mx = std::max(1e-9, *std::max_element(e.begin(), e.end()));
            std::string o = "{\"ok\":true,\"idx\":" + std::to_string(idx) + ",\"bins\":[";
            char buf[16];
            for (int b = 0; b < n; ++b) { std::snprintf(buf, sizeof buf, "%s%.3f", b ? "," : "", e[static_cast<std::size_t>(b)] / mx); o += buf; }
            o += "]}";
            sendJson(fd, o);
            return;
        }
        if (path == "/api/tracks") {
            if (getQueryParam(query, "rescan") == "1") {
                // Keep the playing track selected across a rescan (indices may shift).
                std::string cur;
                {
                    std::lock_guard<std::mutex> lk(tracks_mtx_);
                    const int t = active_track_idx_.load();
                    if (t >= 0 && t < static_cast<int>(tracks_.size())) cur = tracks_[static_cast<std::size_t>(t)].path;
                }
                scanDITracks();
                std::lock_guard<std::mutex> lk(tracks_mtx_);
                for (std::size_t i = 0; i < tracks_.size(); ++i)
                    if (tracks_[i].path == cur) active_track_idx_.store(static_cast<int>(i));
            }
            sendJson(fd, buildTracksJson());
            return;
        }
        if (path == "/api/macro/assign") {
            {
                GraphEditGuard lock(*this);
                rack_rev_.fetch_add(1);
                if (graph_ && !getQueryParam(query, "split").empty())
                    graph_->assignMacroSplit(static_cast<int>(qi("col", 0)), static_cast<int>(qi("split", -1)), qf("min", 0.0f), qf("max", 1.0f), getQueryParam(query, "label"), qi("add", 0) == 1);
                else if (graph_) graph_->assignMacro(static_cast<int>(qi("col", 0)), static_cast<std::size_t>(std::max(0L, qi("slot", 0))), getQueryParam(query, "param"),
                                                qf("min", 0.0f), qf("max", 1.0f), getQueryParam(query, "label"), qi("add", 0) == 1);
            }
            sendJson(fd, buildRackStateJson());
            return;
        }
        if (path == "/api/macro/clear") {
            {
                GraphEditGuard lock(*this);
                rack_rev_.fetch_add(1);
                if (graph_) graph_->clearMacro(static_cast<int>(qi("col", 0)));
            }
            sendJson(fd, buildRackStateJson());
            return;
        }
        if (path == "/api/macro/label") {
            {
                GraphEditGuard lock(*this);
                rack_rev_.fetch_add(1);
                if (graph_) graph_->labelMacro(static_cast<int>(qi("col", 0)), getQueryParam(query, "label"));
            }
            sendJson(fd, buildRackStateJson());
            return;
        }

        // ───────────── Pro Features HTTP API ─────────────
        // Global looper — the whole surface lives in LoopStation::handleApi (see ENGINE_API.md "Looper").
        if (path == "/api/looper" || path == "/api/looper/wave") {
            sendJson(fd, global_looper_.handleApi(path, [&](const char* k) { return getQueryParam(query, k); },
                                                  library::appSupportDir() + "/Loops"));
            return;
        }

        if (path == "/api/global-fx") {
            if (!getQueryParam(query, "transpose").empty()) global_transpose_.setSemitones(qf("transpose", 0.0f));
            if (!getQueryParam(query, "cents").empty()) global_transpose_.setCents(qf("cents", 0.0f));
            if (!getQueryParam(query, "doubler").empty()) global_space_.setDoublerEnabled(getQueryParam(query, "doubler") == "1");
            if (!getQueryParam(query, "doubler_width").empty()) global_space_.setDoublerWidth(qf("doubler_width", 0.75f));
            if (!getQueryParam(query, "doubler_delay").empty()) global_space_.setDoublerDelayMs(qf("doubler_delay", 16.5f));
            if (!getQueryParam(query, "doubler_detune").empty()) global_space_.setDoublerDetune(qf("doubler_detune", 4.5f));
            if (!getQueryParam(query, "room").empty()) global_space_.setRoomEnabled(getQueryParam(query, "room") == "1");
            if (!getQueryParam(query, "room_amt").empty()) global_space_.setRoomAmount(qf("room_amt", 0.35f));
            if (!getQueryParam(query, "demic").empty()) global_space_.setDeMicAmount(qf("demic", 0.50f));
            sendJson(fd, buildGlobalFxJson());
            return;
        }

        if (path == "/api/scenes") {
            const std::string action = getQueryParam(query, "action");
            if (action == "select") {
                GraphEditGuard lock(*this);
                if (graph_) {
                    scenes_.selectScene(static_cast<int>(qi("idx", 0)), *graph_);
                    rack_rev_.fetch_add(1);
                }
            } else if (action == "save") {
                std::lock_guard<std::mutex> lock(graph_mutex_);
                if (graph_) scenes_.saveScene(static_cast<int>(qi("idx", scenes_.activeScene())), *graph_, getQueryParam(query, "label"));
            } else if (action == "ab") {
                GraphEditGuard lock(*this);
                if (graph_) {
                    scenes_.toggleAbCompare(*graph_);
                    rack_rev_.fetch_add(1);
                }
            } else if (action == "ab_capture") {
                std::lock_guard<std::mutex> lock(graph_mutex_);
                if (graph_) scenes_.captureAbBaseline(*graph_);
            }
            sendJson(fd, buildRackStateJson());
            return;
        }

        if (path == "/api/audio/calibrate") {
            const std::string action = getQueryParam(query, "action");
            if (action == "reset") {
                input_calibrator_.resetWindow();
                sendJson(fd, input_calibrator_.toJson(audio_settings_.input_gain_db));
                return;
            }
            // The level is measured after the device input gain, so the recommendation is current + delta.
            // Report THIS measurement (not one recomputed against the new gain, which double-counted it), change
            // only the device input gain (the rig's own input-gain knob is left alone), and restart the window.
            const float before_db = audio_settings_.input_gain_db;
            const auto rec = input_calibrator_.computeRecommendation(before_db);
            const bool applied = action == "apply" && rec.ok;
            if (applied) {
                audio_settings_.input_gain_db = rec.recommended_gain_db;
                live_gain_.store(std::pow(10.0f, audio_settings_.input_gain_db / 20.0f));
                saveAudioSettings();
                input_calibrator_.resetWindow();
            }
            std::ostringstream o;
            o.setf(std::ios::fixed); o.precision(2);
            o << "{\"ok\":" << (rec.ok ? "true" : "false") << ",\"applied\":" << (applied ? "true" : "false")
              << ",\"peak_dbfs\":" << rec.measured_peak_dbfs << ",\"rms_dbfs\":" << rec.measured_rms_dbfs
              << ",\"crest_db\":" << rec.crest_db << ",\"previous_gain_db\":" << before_db
              << ",\"recommended_gain_db\":" << rec.recommended_gain_db << ",\"delta_db\":" << (rec.recommended_gain_db - before_db)
              << ",\"input_gain_db\":" << audio_settings_.input_gain_db
              << ",\"pickup_type\":\"" << jsonEscape(rec.pickup_profile) << "\""
              << ",\"measured_peak_dbfs\":" << rec.measured_peak_dbfs << ",\"pickup_profile\":\"" << jsonEscape(rec.pickup_profile) << "\""
              << ",\"message\":\"" << jsonEscape(rec.message) << "\"}";
            sendJson(fd, o.str());
            return;
        }

        if (path == "/api/irs") {
            if (getQueryParam(query, "rescan") == "1") rescanUserIrs();
            sendJson(fd, userIrsJson());
            return;
        }

        if (path == "/api/cabinet/bake-ir") {
            const auto slot = static_cast<std::size_t>(std::max(0L, qi("slot", 0)));
            const std::string name = getQueryParam(query, "name");
            std::string out_path, out_err;
            bool ok = false;
            {
                std::lock_guard<std::mutex> lock(graph_mutex_);
                if (graph_) {
                    const auto views = graph_->activeNodeViews();
                    if (slot < views.size() && views[slot].component) {
                        auto clone = createComponentByTableId(views[slot].table_id);
                        if (clone) {
                            clone->prepare(sample_rate_.load(), 512);
                            for (const auto& ep : views[slot].component->exportParams()) {
                                if (ep.is_string) clone->setStringParameter(ep.id, ep.str);
                                else clone->setParameter(ep.id, ep.num);
                            }
                            clone->processControlRate();
                            ok = bakeUnitIrToWav(*clone, sample_rate_.load(), name, true, out_path, out_err);
                        }
                    } else {
                        out_err = "Cabinet slot not found";
                    }
                }
            }
            sendJson(fd, ok ? ("{\"ok\":true,\"path\":\"" + jsonEscape(out_path) + "\",\"folder\":\"" + jsonEscape(exportedIrFolder()) + "\"}")
                            : ("{\"ok\":false,\"error\":\"" + jsonEscape(out_err) + "\"}"));
            return;
        }

        if (path == "/api/tonematch") {
            const int preset_idx = static_cast<int>(qi("preset", 0));
            const ToneMatchCurve curve = builtinReferenceTarget(preset_idx);
            int target_slot = -1;
            {
                GraphEditGuard lock(*this);
                if (graph_) {
                    auto views = graph_->activeNodeViews();
                    for (const auto& v : views) {
                        if (v.table_id == kToneMatchUnitTable) {
                            target_slot = static_cast<int>(v.slot_idx);
                            break;
                        }
                    }
                    if (target_slot < 0) {
                        target_slot = graph_->insertComponent(kToneMatchUnitTable, views.size(), initParamsFor(kToneMatchUnitTable));
                    }
                    if (target_slot >= 0) {
                        graph_->editComponent(static_cast<std::size_t>(target_slot), [&](IComponent* c) {
                            if (!c) return false;
                            return applyToneMatchCurve(*c, curve);
                        });
                        rack_rev_.fetch_add(1);
                    }
                }
            }
            sendJson(fd, buildRackStateJson());
            return;
        }

        if (path == "/api/trainer") {
            const std::string action = getQueryParam(query, "action");
            if (!getQueryParam(query, "track").empty()) activateTrack(static_cast<int>(qi("track", 0)));
            std::size_t total = 1;
            if (const TrackPCM* trk = active_pcm_.load()) total = std::max<std::size_t>(1, trk->mono.size());
            if (!getQueryParam(query, "seek").empty()) {
                const float sk = std::clamp(qf("seek", 0.0f), 0.0f, 1.0f);
                playhead_frame_.store(static_cast<std::size_t>(double(sk) * double(total)));
                trainer_frac_pos_ = 0.0;
            }
            const float cur_norm = std::clamp(static_cast<float>(double(playhead_frame_.load()) / double(total)), 0.0f, 1.0f);
            if (action == "play") playing_.store(true);
            else if (action == "pause" || action == "stop") playing_.store(false);
            else if (action == "toggle") playing_.store(!playing_.load());
            else if (action == "set_a") {
                trainer_loop_a_.store(std::min(cur_norm, trainer_loop_b_.load() - 0.02f));
            } else if (action == "set_b") {
                trainer_loop_b_.store(std::max(cur_norm, trainer_loop_a_.load() + 0.02f));
                trainer_ab_loop_.store(true);
            } else if (action == "clear_ab") {
                trainer_ab_loop_.store(false);
                trainer_loop_a_.store(0.0f);
                trainer_loop_b_.store(1.0f);
            }
            if (!getQueryParam(query, "ab").empty()) trainer_ab_loop_.store(getQueryParam(query, "ab") == "1");
            if (!getQueryParam(query, "loop_a").empty()) trainer_loop_a_.store(std::clamp(qf("loop_a", 0.0f), 0.0f, 0.98f));
            if (!getQueryParam(query, "loop_b").empty()) trainer_loop_b_.store(std::clamp(qf("loop_b", 1.0f), 0.02f, 1.0f));
            if (!getQueryParam(query, "speed").empty()) trainer_speed_.store(std::clamp(qf("speed", 1.0f), 0.50f, 1.50f));
            if (!getQueryParam(query, "stem_cut").empty()) trainer_stem_cut_.store(std::clamp(qf("stem_cut", 0.0f), 0.0f, 1.0f));
            sendJson(fd, buildTrainerJson());
            return;
        }

        if (path == "/api/midi-learn") {
            const std::string action = getQueryParam(query, "action");
            if (action == "arm") midi_map_.armLearn(getQueryParam(query, "target"));
            else if (action == "cancel") midi_map_.cancelLearn();
            else if (action == "bind") midi_map_.bind(static_cast<int>(qi("cc", 1)), getQueryParam(query, "target"), static_cast<int>(qi("ch", 0)));
            else if (action == "unbind") midi_map_.unbind(getQueryParam(query, "target"));
            else if (action == "cc") {
                const int cc = static_cast<int>(qi("cc", 0));
                const int val = static_cast<int>(qi("val", 127));
                const std::string target = midi_map_.handleIncomingCc(cc, val, static_cast<int>(qi("ch", 0)));
                dispatchMidiTarget(target, val);
            }
            sendJson(fd, midi_map_.toJson());
            return;
        }

        sendJson(fd, "{\"error\":\"not_found\"}");
    }

    void sendJson(int fd, const std::string& json, const char* status = "200 OK") {
        std::string origin;
        {
            std::lock_guard<std::mutex> lock(cors_mutex_);
            if (const auto it = cors_origins_.find(fd); it != cors_origins_.end()) {
                origin = it->second;
                cors_origins_.erase(it);
            }
        }
        std::ostringstream oss;
        oss << "HTTP/1.1 " << status << "\r\n"
            << "Content-Type: application/json\r\n"
            << (origin.empty() ? "" : "Access-Control-Allow-Origin: " + origin + "\r\n")
            << "Vary: Origin\r\n"
            << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            << "Access-Control-Allow-Headers: Content-Type, X-Headroom-Token\r\n"
            << "Connection: close\r\n"
            << "Cache-Control: no-cache\r\n"
            << "Content-Length: " << json.size() << "\r\n\r\n"
            << json;
        std::string s = oss.str();
        std::size_t sent = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (sent < s.size()) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0) break;
            rig::net::setSendTimeout(fd, static_cast<int>(remaining));
            const ssize_t n = rig::net::sendBytes(fd, s.data() + sent, s.size() - sent);
            if (n <= 0) break;
            sent += static_cast<std::size_t>(n);
        }
    }

    std::string repo_root_;
    uint16_t http_port_ = 17770;
    std::string api_token_;
    std::mutex cors_mutex_;
    std::map<int, std::string> cors_origins_; // includes sockets adopted by the preset loader
    std::atomic<double> sample_rate_{48000.0};

    midi::MidiOut midi_out_;
    AudioIO audio_io_;
    AudioSettings audio_settings_{};
    std::string audio_error_;
    std::atomic<bool> live_{false};
    std::atomic<bool> monitor_{true};
    std::atomic<float> out_gain_target_{1.0f};  // top-bar OUT (linear); render thread smooths into out_gain_cur_
    float out_gain_cur_ = 1.0f;
    std::atomic<float> live_gain_{1.0f};
    std::atomic<int> clip_hold_{0};
    mutable std::mutex graph_mutex_;
    std::atomic<uint32_t>* host_latency_ = nullptr;
    std::unique_ptr<RackGraph> graph_; // control model, never rendered
    std::shared_ptr<UnitMeterBank> unit_meters_ = std::make_shared<UnitMeterBank>(); // per-unit face meters
    std::mutex tap_mtx_;
    std::vector<std::chrono::steady_clock::time_point> taps_;
    RealtimeGraphRuntime<RackGraph> graph_runtime_;
    // Written during control/calibration publication, observed by HTTP telemetry.
    std::atomic<uint64_t> render_revision_{0};
    std::atomic<uint64_t> graph_lineage_{0};
    uint64_t bridge_revision_ = 0; // model lock: latest plan published with an output bridge
    // Debounced audio-settings save (scheduleAudioSettingsSave)
    std::mutex save_mtx_, save_file_mtx_;
    std::condition_variable save_cv_;
    std::thread settings_saver_;
    std::string save_text_;
    uint64_t settings_version_ = 0, save_text_version_ = 0, settings_written_version_ = 0;
    std::chrono::steady_clock::time_point save_due_{};
    bool save_pending_ = false, saver_stop_ = false;
    uint64_t audio_revision_ = 0; // fields below are audio-thread-only
    float live_gain_cur_ = -1.0f; // smoothed live input gain (< 0 = not started)
    int transition_remaining_ = 0, transition_total_ = 1;
    float transition_l_ = 0.0f, transition_r_ = 0.0f;
    float last_graph_l_ = 0.0f, last_graph_r_ = 0.0f;

    std::vector<PresetEntry> presets_;
    std::vector<DISampleTrack> tracks_;
    mutable std::mutex tracks_mtx_;
    std::atomic<const TrackPCM*> active_pcm_{nullptr};
    std::mutex pcm_mtx_;
    std::deque<std::pair<std::unique_ptr<TrackPCM>, std::chrono::steady_clock::time_point>> pcm_pool_;
    library::PresetLibrary library_;
    library::ComponentPresetBank comp_bank_;
    std::atomic<bool> library_ready_{false};
    std::thread library_thread_;
    std::string cur_name_, cur_author_, cur_category_, cur_based_on_, cur_based_on_path_;
    mutable std::mutex name_mutex_;
    std::string cur_name_snapshot_;
    std::atomic<uint64_t> rack_rev_{1};
    std::mutex loud_mtx_;
    std::map<std::string, float> loud_cache_;
    bool loud_loaded_ = false;
    std::mutex job_mtx_;
    std::condition_variable job_cv_;
    bool worker_poll_ = false; // job_mtx_: a plan was published, the worker should collect (workerLoop)
    std::deque<std::function<void()>> jobs_;
    std::thread worker_;

    std::atomic<bool> running_{false};
    int server_fd_ = -1;
    std::thread http_thread_;
    bool http_fd_adopted_ = false; // HTTP thread: the handler handed the client socket on (preset loader), don't close it
    // Preset loader (requestPresetLoad): the newest request, sockets waiting for their load, the thread.
    std::atomic<uint64_t> preset_load_gen_{0}; // bumped by every load request; a build commits only while it is current
    std::atomic<int> preset_loading_idx_{-1};  // telemetry "preset_loading"
    std::mutex pl_mtx_;
    std::condition_variable pl_cv_;
    std::optional<PresetLoadRequest> pl_next_;
    std::vector<std::pair<int, uint64_t>> pl_waiters_; // (socket, generation)
    bool pl_stopped_ = false;
    std::thread pl_thread_;

    std::atomic<bool> playing_{false};
    std::atomic<bool> loop_{true};
    std::atomic<int> active_track_idx_{0};
    std::atomic<int> active_preset_idx_{0};
    std::atomic<std::size_t> playhead_frame_{0};
    std::atomic<float> input_gain_{1.0f};
    std::atomic<bool> master_bypass_{false};

    float dc_x1_l_ = 0.0f;
    float dc_y1_l_ = 0.0f;
    float dc_x1_r_ = 0.0f;
    float dc_y1_r_ = 0.0f;

    static_assert(std::atomic<uint64_t>::is_always_lock_free && std::atomic<float>::is_always_lock_free);
    std::atomic<uint64_t> render_blocks_{0}, render_over_budget_{0}, render_time_ns_{0}, render_budget_ns_{0}, render_peak_ns_{0};
    std::atomic<float> render_last_ratio_{0};

    std::atomic<float> in_rms_{0.0f};
    std::atomic<float> out_rms_l_{0.0f};
    std::atomic<float> out_rms_r_{0.0f};
    static constexpr uint32_t kScopeLen = 8192, kScopeDecim = 4;
    std::array<std::atomic<float>, kScopeLen> scope_in_{}, scope_out_{};
    std::atomic<uint64_t> scope_pos_{0};
    bool listen_model_set_ = false;      // HTTP thread only
    rig::listen::ListenService listen_;  // Listen pane analysis (tuner, key, chord, spectra); thread starts on first /api/listen
    std::array<std::atomic<float>, 64> wave_in_{};
    std::array<std::atomic<float>, 64> wave_out_{};

    // Pro Studio Features state
    LoopStation global_looper_{};
    double looper_rig_bpm_ = 120.0;   // audio thread only
    PolyPitchShifter global_transpose_{};
    StereoDoublerRoomProcessor global_space_{};
    InputCalibrator input_calibrator_{};
    RigSceneManager scenes_{};
    MidiControllerMap midi_map_{};
    midi::MidiIn midi_in_;   // after midi_map_: destroyed first, so its callback never outlives the map

    std::atomic<bool> trainer_ab_loop_{false};
    std::atomic<float> trainer_loop_a_{0.0f};
    std::atomic<float> trainer_loop_b_{1.0f};
    std::atomic<float> trainer_speed_{1.0f};
    std::atomic<float> trainer_stem_cut_{0.0f};
    double trainer_frac_pos_ = 0.0;
    float trainer_lp_z_ = 0.0f;
    mutable BackingTracks backing_; // Practice view backing tracks (own files: BackingTracks / BackingTrackPlayer)

    std::string buildGlobalFxJson() const {
        std::ostringstream oss;
        oss.setf(std::ios::fixed);
        oss.precision(2);
        oss << "{\"transpose\":" << global_transpose_.semitones()
            << ",\"cents\":" << global_transpose_.cents()
            << ",\"doubler\":" << (global_space_.doublerEnabled() ? "true" : "false")
            << ",\"doubler_width\":" << global_space_.doublerWidth()
            << ",\"doubler_delay\":" << global_space_.doublerDelayMs()
            << ",\"doubler_detune\":" << global_space_.doublerDetune()
            << ",\"room\":" << (global_space_.roomEnabled() ? "true" : "false")
            << ",\"room_amt\":" << global_space_.roomAmount()
            << ",\"demic\":" << global_space_.deMicAmount() << "}";
        return oss.str();
    }

    std::string buildTrainerJson() const {
        const int tidx = active_track_idx_.load();
        const std::size_t pos = playhead_frame_.load();
        std::size_t total = 1;
        if (const TrackPCM* trk = active_pcm_.load()) total = std::max<std::size_t>(1, trk->mono.size());
        double dur = double(total) / std::max(1.0, sample_rate_.load());
        std::string tname = "None";
        {
            std::lock_guard<std::mutex> lk(tracks_mtx_);
            if (tidx >= 0 && tidx < static_cast<int>(tracks_.size())) {
                dur = tracks_[static_cast<std::size_t>(tidx)].duration_sec;
                tname = tracks_[static_cast<std::size_t>(tidx)].title;
            }
        }
        const double pos_sec = double(pos) / std::max(1.0, sample_rate_.load());
        const double pos_norm = std::clamp(double(pos) / double(total), 0.0, 1.0);
        std::ostringstream oss;
        oss.setf(std::ios::fixed);
        oss.precision(3);
        oss << "{\"playing\":" << (playing_.load() ? "true" : "false")
            << ",\"loop\":" << (loop_.load() ? "true" : "false")
            << ",\"track_idx\":" << tidx
            << ",\"track_name\":\"" << jsonEscape(tname) << "\""
            << ",\"position_sec\":" << pos_sec
            << ",\"duration_sec\":" << dur
            << ",\"position_norm\":" << pos_norm
            << ",\"ab_loop\":" << (trainer_ab_loop_.load() ? "true" : "false")
            << ",\"loop_a\":" << trainer_loop_a_.load()
            << ",\"loop_b\":" << trainer_loop_b_.load()
            << ",\"speed\":" << trainer_speed_.load()
            << ",\"stem_cut\":" << trainer_stem_cut_.load() << "}";
        return oss.str();
    }

    void dispatchMidiTarget(const std::string& target, int val) {
        if (target.empty()) return;
        const float norm = std::clamp(static_cast<float>(val) / 127.0f, 0.0f, 1.0f);
        if (target.rfind("macro:", 0) == 0) {
            try { setMacroNormalized(safeParse<int>(target.substr(6)), norm); rack_rev_.fetch_add(1); } catch (...) {}
        } else if (target.rfind("scene:", 0) == 0 && val >= 64) {
            try {
                GraphEditGuard lock(*this);
                if (graph_) { scenes_.selectScene(safeParse<int>(target.substr(6)), *graph_); rack_rev_.fetch_add(1); }
            } catch (...) {}
        } else if (target == "looper:toggle" && val >= 64) {   // Rec/Play/Dub on the selected track
            global_looper_.post(LoopStation::kPress);
        } else if (target == "looper:stop" && val >= 64) {      // All Start / Stop
            global_looper_.post(LoopStation::kAllStartStop);
        } else if (target == "looper:clear" && val >= 64) {
            global_looper_.post(LoopStation::kClearAll);
        } else if (target == "looper:undo" && val >= 64) {
            global_looper_.post(LoopStation::kUndo);
        } else if (target == "looper:redo" && val >= 64) {
            global_looper_.post(LoopStation::kRedo);
        } else if (target.rfind("looper:track:", 0) == 0 && val >= 64) {   // looper:track:1..3 = that track's footswitch
            try { global_looper_.post(LoopStation::kPress, safeParse<int>(target.substr(13)) - 1); } catch (...) {}
        } else if (target == "tap_tempo" && val >= 64) {
            const double bpm = tapTempo();
            if (bpm > 0.0) setTempo(bpm);
        } else if (target == "transpose:down" && val >= 64) {
            global_transpose_.setSemitones(std::clamp(global_transpose_.semitones() - 1.0f, -12.0f, 12.0f));
        } else if (target == "transpose:up" && val >= 64) {
            global_transpose_.setSemitones(std::clamp(global_transpose_.semitones() + 1.0f, -12.0f, 12.0f));
        } else if (target.rfind("bloom:", 0) == 0) {
            // Every Bloom unit (900006) in the rig, whichever preset is loaded. bloom:engage follows the switch
            // (a momentary pedal = hold, a latching one = latch), bloom:toggle flips on each press, bloom:motion
            // is the expression pedal.
            std::vector<std::size_t> slots;
            float engaged = 0.0f;
            {
                GraphEditGuard lock(*this, false);
                if (graph_)
                    for (const auto& v : graph_->activeNodeViews())
                        if (v.table_id == 900006 && v.component) {
                            if (slots.empty()) engaged = v.component->getParameter("engage");
                            slots.push_back(v.slot_idx);
                        }
            }
            const std::string what = target.substr(6);
            for (std::size_t s : slots) {
                if (what == "engage") setSlotNormalizedParameter(s, "engage", val >= 64 ? 1.0f : 0.0f);
                else if (what == "toggle" && val >= 64) setSlotNormalizedParameter(s, "engage", engaged >= 0.5f ? 0.0f : 1.0f);
                else if (what == "motion") setSlotNormalizedParameter(s, "motion", norm);
            }
            if (!slots.empty()) rack_rev_.fetch_add(1);
        }
    }
};

} // namespace rig::audio
