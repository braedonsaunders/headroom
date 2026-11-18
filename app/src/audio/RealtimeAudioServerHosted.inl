// Hosted (DAW plugin) mode for RealtimeAudioServer. Included INSIDE the class body (public section), so it
// can call the server's existing internals; it adds methods and a few hosted-only members and does not
// change the render, swap, snapshot or graph-mutation code. Used by the AU / VST3 wrappers in app/plugin.
//
// Differences from start():
//   * the host drives audio through renderHost(); no AudioIO device, no DI-player decode, no mic request;
//   * no CoreMIDI port and no setGlobalMidiSink(). That sink is a process-wide singleton (per binary image);
//     hosted mode never installs one, so MIDI units' external output is dropped (MidiComponents.cpp checks
//     for null) and can never reach another instance's or another plugin's sink. External MIDI out is
//     therefore unavailable in the plugin (v1); MIDI → Headroom Poly Synth inside the rig still works;
//   * no nested AudioComponents: hosted third-party plugin units and the AU-backed MIDI Instrument (900003,
//     DLS / installed AU instruments) are rejected recursively on every preset / state / template load and
//     insert, before any graph is constructed (their create/dispose may need the host's main thread);
//   * the HTTP API binds an ephemeral loopback port per instance (boundHttpPort()), never 17770;
//   * no warm-up and no library scan until the wrapper asks (editor opened); the preset-library index and
//     the component-preset bank are scanned once per process image and copied into each instance;
//   * device / source / DI-player routes and plugin-hosting routes are refused;
//   * no process-wide signal(SIGPIPE) change (accepted client sockets get SO_NOSIGPIPE instead).
// Threading contract: every method except renderHost()/setHostTransport() runs on a control thread, never on
// the host's render callback. prepareHost() additionally requires the callback to be synchronously quiescent.

    struct HostedState {
        std::string rig_xml;            // .rig XML (library::userPresetXml)
        std::string name, author, category, source = "factory";
        int preset_idx = -1;
        float input_gain = 1.0f;        // the rack's own input knob (linear)
        // State saved by a build that still levelled rigs at run time: that loudness gain (linear; < 0 = none). A
        // restore folds it into the rig's own level knobs once (core/LevelFold.hpp), so the project keeps its level.
        float legacy_preset_gain = -1.0f;
        bool master_bypass = false;
        bool dirty = false;
        std::array<float, 8> macros{};  // macro knob values, captured under the same lock as the rig
        bool has_macros = false;
    };

    static bool hostedRateSupported(double sr) noexcept { return std::isfinite(sr) && sr >= 8000.0 && sr <= 384000.0; }
    // Units that create nested AudioComponents: not available inside the plugin (v1).
    static bool hostedUnsupportedTable(int32_t tid) noexcept { return plugins::isPluginTable(tid) || tid == 900003; }
    static bool hostedSupported(const preset::PresetComponentNode& n, std::string* why) {
        if (hostedUnsupportedTable(n.table_id)) {
            if (why) *why = n.table_id == 900003 ? "uses the AU-backed MIDI Instrument" : "hosts a third-party plugin";
            return false;
        }
        for (const auto& c : n.children)
            if (!hostedSupported(c, why)) return false;
        return true;
    }
    static bool hostedSupported(const preset::RackPreset& p, std::string* why) { return hostedSupported(p.root_rack, why); }

    // Starts the engine for a plugin instance. On failure every thread it started is joined again.
    bool startHosted(const std::string& repo_root, double sample_rate, std::string* err = nullptr) {
        if (!hostedRateSupported(sample_rate)) { if (err) *err = "unsupported sample rate"; return false; }
        hosted_ = true;
        loadCircuitNoiseSetting();
        repo_root_ = repo_root;
        http_port_ = 0; // ephemeral
        sample_rate_.store(sample_rate);
        live_.store(true);       // renderChunk's live-input branch: the host's input is the guitar
        live_gain_.store(1.0f);  // plugin-level input trim lives in the wrapper
        monitor_.store(true);
        playing_.store(false);
        const bool trace = std::getenv("HEADROOM_PLUGIN_TRACE") != nullptr;
        auto t0 = std::chrono::steady_clock::now();
        auto lap = [&](const char* what) {
            if (!trace) return;
            const auto now = std::chrono::steady_clock::now();
            std::fprintf(stderr, "Headroom plugin: %s %.0f ms\n", what, std::chrono::duration<double, std::milli>(now - t0).count());
            t0 = now;
        };
        loadPresetsCatalog();
        setNeuralGuitarModelPath(repo_root_ + "/app/resources/models/guitar_midi.hgnn");
        rig::platform::setAppRoot(repo_root_); // shipped-content root for engine lookups (NAM demo captures, …)
        global_looper_.prepare(sample_rate_.load());
        global_transpose_.prepare(sample_rate_.load());
        global_space_.prepare(sample_rate_.load());
        rescanUserIrs();
        // The component-preset bank only serves the editor's HTTP API,
        // so it is loaded with the editor services (startHostedEditor), not here.
        std::string why;
        const bool loaded = hostedLoadPreset(0, &why);
        lap("initial rig");
        if (!loaded) {
            if (err) *err = "shipped rigs not found under " + contentRoot() + (why.empty() ? "" : " (" + why + ")");
            stopHosted();
            return false;
        }
        running_.store(true);
        worker_ = std::thread([this] { workerLoop(); });
        // Bind + listen now (cheap; the port is fixed for the instance's lifetime). Requests queue in the
        // listen backlog until the editor services start accepting.
        if (!hostedBindHttp()) {
            if (err) *err = "could not bind the loopback API port";
            stopHosted();
            return false;
        }
        hosted_prepared_ok_ = true;
        hosted_fresh_ = true;
        lap("engine threads + port");
        return true;
    }

    // Editor services, started when an editor first attaches: load (or copy the process-wide) component-
    // preset bank, then serve the HTTP API. Runs on the thread stop() joins as http_thread_, so the bank
    // is written before any HTTP request can read it. Idempotent.
    void startHostedEditor() {
        if (hosted_editor_started_.exchange(true) || server_fd_ < 0) return;
        http_thread_ = std::thread([this] {
            const auto bank = sharedComponentBank(); // Schema Init presets only
            if (!running_.load()) return;
            comp_bank_ = *bank;
            httpLoop();
        });
    }

    // Joins every producer (HTTP, library, worker/calibration) and then reclaims the render runtime. Call
    // only after the host has stopped calling renderHost(). Safe to call more than once.
    void stopHosted() {
        running_.store(false);
        // close() does not interrupt a blocked accept() on macOS: poke the listener so httpLoop sees running_.
        if (server_fd_ >= 0 && hosted_port_ != 0) {
            const int s = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
            if (s >= 0) {
                sockaddr_in a{};
                a.sin_family = AF_INET;
                a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                a.sin_port = htons(hosted_port_);
                ::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
                rig::net::closeSocket(s);
            }
        }
        stop();
        hosted_port_ = 0;
    }

    bool isHosted() const noexcept { return hosted_; }
    uint16_t boundHttpPort() const noexcept { return hosted_port_; }

    // Host (re)preparation: AU Initialize / VST3 setActive(true), with the host callback synchronously
    // quiescent. Always a full prepare (new lineage → fresh DSP history), then the render-owned output-stage
    // history is zeroed. Success is decided inside the serialised operation. Levels are untouched (the
    // real graph is never rendered here, and nothing re-levels a rig). On failure sample_rate_ is restored and false is returned: the
    // wrapper stays suspended until a later call succeeds.
    bool prepareHost(double sr) {
        if (!hostedRateSupported(sr)) return false;
        bool ok = false;
        GraphEditGuard lock(*this, /*publish=*/false);
        const double old = sample_rate_.load();
        try {
            // A graph built moments ago by startHosted at this rate has no history yet: don't prepare twice.
            const bool fresh = hosted_fresh_ && std::abs(sr - old) < 0.5;
            hosted_fresh_ = false;
            sample_rate_.store(sr);
            global_looper_.prepare(sr);
            global_transpose_.prepare(sr);
            global_space_.prepare(sr);
            ++graph_lineage_; // never inherit DSP history across a host prepare
            if (graph_ && !fresh) graph_->prepare(sr, 512);
            publishRenderPlanLocked();
            ok = true;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "Headroom plugin: prepare at %.0f Hz failed: %s\n", sr, e.what());
        } catch (...) {
            std::fprintf(stderr, "Headroom plugin: prepare at %.0f Hz failed\n", sr);
        }
        if (!ok) sample_rate_.store(old);
        hosted_prepared_ok_ = ok;
        if (ok && hosted_reset_history_on_prepare_) resetHostRenderHistory(); // callback quiescent (caller contract)
        return ok;
    }

    // Host render callback. No allocation, locks or blocking (it is the same path AudioIO's callback uses).
    // All four pointers must be valid planar buffers of n frames; in == out aliasing is allowed.
    void renderHost(const float* inL, const float* inR, float* outL, float* outR, uint32_t n) {
        rig::platform::ScopedNoDenormalsX86 ftz; // x86: FTZ/DAZ (no-op on Apple Silicon)
        renderBlock(inL, inR, outL, outR, n);
    }

    // Host transport, from the process callback (atomics only). v1 scope: BPM only — the host tempo drives
    // every synced unit through rig::hostTempo() (core/Tempo.hpp: lock-free, read once per block by
    // RackGraph). That clock is process-wide per plugin binary, which matches a DAW's single project tempo;
    // beat position / seek / loop are recorded here but not used for phase sync yet.
    void setHostTransport(double bpm, double beat, bool playing) noexcept {
        host_bpm_.store(bpm, std::memory_order_relaxed);
        host_beat_.store(beat, std::memory_order_relaxed);
        host_playing_.store(playing, std::memory_order_relaxed);
        if (bpm > 0.0 && std::isfinite(bpm)) hostTempo().setTempo(bpm);
    }
    double hostBpm() const noexcept { return host_bpm_.load(std::memory_order_relaxed); }

    void macroValuesHosted(float out[8]) const {
        std::lock_guard<std::mutex> lock(graph_mutex_);
        for (int c = 0; c < 8; ++c) out[c] = graph_ ? graph_->getMacroNormalized(c) : 0.5f;
    }

    HostedState captureHostedState() const {
        HostedState s;
        preset::RackPreset p;
        {
            std::lock_guard<std::mutex> lock(graph_mutex_);
            if (!graph_) return s;
            p = graph_->toPreset(cur_name_, cur_author_);
            p.info.based_on = cur_based_on_;
            p.info.based_on_path = cur_based_on_path_;
            s.dirty = graph_->isDirty();
            s.source = graph_->presetSource();
            s.name = cur_name_;
            s.author = cur_author_;
            s.category = cur_category_;
            for (int c = 0; c < 8; ++c) s.macros[static_cast<std::size_t>(c)] = graph_->getMacroNormalized(c);
            s.has_macros = true;
            s.preset_idx = active_preset_idx_.load();
            s.input_gain = input_gain_.load();
            s.master_bypass = master_bypass_.load();
        }
        s.rig_xml = library::userPresetXml(p);
        return s;
    }

    // Rebuilds the rig from saved state (off the callback; the new plan is published like a preset load).
    bool restoreHostedState(const HostedState& s, std::string* err = nullptr) {
        preset::RackPreset p;
        if (!preset::parsePresetData(s.rig_xml, "plugin state", p, err)) return false;
        std::string why;
        if (!hostedSupported(p, &why)) { if (err) *err = "rig " + why + " (not available in the plugin)"; return false; }
        const float ig = std::clamp(std::isfinite(s.input_gain) ? s.input_gain : 1.0f, 0.0f, 4.0f);
        if (std::isfinite(s.legacy_preset_gain) && s.legacy_preset_gain > 0.0f && std::abs(s.legacy_preset_gain - 1.0f) > 1e-4f) {
            // measured with the SSL / Lexicon / delay Outputs dead: fold the gain minus what they now add
            const auto mf = levelfold::foldLegacyGain(p, 20.0 * std::log10(static_cast<double>(s.legacy_preset_gain)));
            const auto& fr = mf.fold;
            DiagMigration m;
            m.live_db = mf.live_db;
            m.change_db = mf.change_db;
            m.preset = s.name;
            m.path = "plugin state";
            m.status = fr.moves.empty() ? "no_level_knob" : "migrated";
            m.cache_gain = s.legacy_preset_gain;
            m.cache_db = 20.0 * std::log10(static_cast<double>(s.legacy_preset_gain));
            m.applied_db = fr.applied_db;
            m.moves = fr.moves;
            m.stop = fr.stop;
            std::lock_guard<std::mutex> lk(dg_mtx_);
            diagPush(dg_migrations_, std::move(m));
        }
        auto g = std::make_unique<RackGraph>();
        g->setParallelPrepare(true); // circuit units settle on worker threads, as in a preset load
        g->buildFromPreset(p, sample_rate_, 512);
        g->setInputGainLinear(ig);
        g->setMasterBypass(s.master_bypass);
        g->setPresetSource(s.source.empty() ? std::string("user") : s.source);
        if (s.has_macros) // on the private, unpublished graph: no plan publication involved
            for (int c = 0; c < 8; ++c) {
                const float v = s.macros[static_cast<std::size_t>(c)];
                if (std::isfinite(v) && std::abs(g->getMacroNormalized(c) - v) > 1e-6f) g->setMacroNormalized(c, std::clamp(v, 0.0f, 1.0f));
            }
        g->setDirty(s.dirty);
        ++preset_load_gen_; // replaces the rig: a preset load still building is superseded
        GraphEditGuard lock(*this);
        swapInGraph(std::move(g));
        input_gain_.store(ig);
        master_bypass_.store(s.master_bypass);
        active_preset_idx_.store(s.preset_idx);
        cur_name_ = s.name.empty() ? std::string("Untitled Rig") : s.name;
        cur_author_ = s.author;
        cur_category_ = s.category;
        cur_based_on_ = p.info.based_on;
        cur_based_on_path_ = p.info.based_on_path;
        { std::lock_guard<std::mutex> lk(name_mutex_); cur_name_snapshot_ = cur_name_; }
        rack_rev_.fetch_add(1);
        return true;
    }

    // Host reset while the callback may still run (AU Reset): rebuild the rig from its own state on the
    // worker (fresh components and lineage) and swap it in through the normal publication path. Coalesced.
    void requestHostedReset() {
        if (hosted_reset_pending_.exchange(true)) return;
        enqueueJob([this] {
            hosted_reset_pending_.store(false);
            if (!running_.load()) return;
            const HostedState s = captureHostedState();
            if (!s.rig_xml.empty()) restoreHostedState(s);
        }, /*front=*/true);
    }

    // Background preset-library index for the editor's browser (scanned once per process image).
    void startHostedLibrary() {
        if (hosted_library_started_.exchange(true)) return;
        std::vector<library::FeaturedSpec> featured;
        for (const auto& pe : presets_) featured.push_back({pe.rig_filename, pe.title, pe.category});
        const std::string user = library::appSupportDir() + "/User Presets", showcase = showcaseDir();
        library_thread_ = std::thread([this, featured, user, showcase] {
            rig::platform::ScopedNoDenormalsX86 ftz; // x86: FTZ/DAZ (no-op on Apple Silicon)
#ifdef __APPLE__
            pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#endif
            const auto shared = sharedLibraryIndex(user, featured, showcase);
            if (!running_.load()) return;
            library_.assignFrom(*shared);
            library_ready_.store(true);
        });
    }

    // Tube-table / amp-trim warm-up for hosted mode (the wrapper's process-wide registry picks ONE instance), as
    // startWarmup(). Rigs are never measured or re-levelled.
    void startHostedWarmup() {
        enqueueJob([this] {
            for (const auto& sc : catalog::kAllComponents) {
                if (!running_.load()) return;
                if (isTubeAmpTable(sc.table_id))
                    if (auto c = createComponentByTableId(sc.table_id)) c->prepare(sample_rate_, 512);
            }
        });
    }

    // Render-time telemetry (renderBlock's own counters), for the offline budget measurement.
    struct HostRenderStats { uint64_t blocks, over_budget, elapsed_ns, budget_ns, peak_ns; };
    HostRenderStats hostRenderStats() const noexcept {
        return {render_blocks_.load(), render_over_budget_.load(), render_time_ns_.load(), render_budget_ns_.load(), render_peak_ns_.load()};
    }

    // Template racks for hosted mode: same as newRack(), with the AU-free Poly Synth (900004) in place of
    // the AU-backed Instrument (900003).
    void newRackHosted(const std::string& tmpl) {
        if (tmpl != "guitar_synth" && tmpl != "instrument") { newRack(tmpl); return; }
        auto g = std::make_unique<RackGraph>();
        preset::RackPreset empty;
        empty.root_rack.table_id = 100;
        g->buildFromPreset(empty, sample_rate_, 512);
        auto init = [this](int32_t t) { return initParamsFor(t); };
        const int sid = g->insertSplit(9999, 0.45f);
        if (tmpl == "guitar_synth") {
            g->insertIntoLane(sid, 'A', 99, 15000, init(15000));
            g->insertIntoLane(sid, 'A', 99, 56000, init(56000));
        }
        for (int32_t t : {900001, 900002, 900004, 127000}) g->insertIntoLane(sid, 'B', 99, t, init(t));
        g->setInputGainLinear(input_gain_.load());
        g->setMasterBypass(master_bypass_.load());
        g->setPresetSource("new");
        g->autoMacrosIfEmpty();
        g->setDirty(true);
        ++preset_load_gen_; // replaces the rig: a preset load still building is superseded
        GraphEditGuard lock(*this);
        swapInGraph(std::move(g));
        active_preset_idx_.store(-1);
        cur_name_ = "New Rig";
        { std::lock_guard<std::mutex> lk(name_mutex_); cur_name_snapshot_ = cur_name_; }
        rack_rev_.fetch_add(1);
        cur_author_ = "";
        cur_category_ = "User";
        cur_based_on_.clear();
        cur_based_on_path_.clear();
    }

private:
    // startHttpServer() without the accept thread (see startHostedEditor).
    bool hostedBindHttp() {
        if (!createApiToken()) return false;
        rig::net::ensureInit(); // WSAStartup on Windows
        server_fd_ = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
        if (server_fd_ < 0) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(0); // ephemeral, per instance
        if (::bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 || ::listen(server_fd_, 16) < 0) {
            rig::net::closeSocket(server_fd_);
            server_fd_ = -1;
            return false;
        }
        hosted_port_ = queryBoundPort();
        http_port_ = hosted_port_;
        return hosted_port_ != 0;
    }

    uint16_t queryBoundPort() const noexcept {
        if (server_fd_ < 0) return 0;
        sockaddr_in a{};
        socklen_t len = sizeof(a);
        if (::getsockname(server_fd_, reinterpret_cast<sockaddr*>(&a), &len) != 0) return 0;
        return ntohs(a.sin_port);
    }

    // Audio-thread-only output history; written here only while the host callback is quiescent.
    void resetHostRenderHistory() noexcept {
        dc_x1_l_ = dc_y1_l_ = dc_x1_r_ = dc_y1_r_ = 0.0f;
        last_graph_l_ = last_graph_r_ = 0.0f;
        transition_l_ = transition_r_ = 0.0f;
        transition_remaining_ = 0;
        transition_total_ = 0; // only read while transition_remaining_ > 0, which renderChunk re-arms with it
    }

    // Path of library / catalog preset `idx`, resolved exactly like loadActivePreset().
    std::string hostedPresetPath(int idx) const {
        library::LibraryEntry e;
        if (library_ready_.load() && library_.entry(idx, e)) return e.path;
        if (idx >= 0 && idx < static_cast<int>(presets_.size())) return contentRoot() + "/" + presets_[static_cast<std::size_t>(idx)].rig_filename;
        return {};
    }
    bool hostedPresetAllowed(int idx, std::string* why) const {
        const std::string path = hostedPresetPath(idx);
        preset::RackPreset p;
        if (path.empty() || !preset::loadPreset(path, p, why)) return false;
        return hostedSupported(p, why);
    }
    bool hostedLoadPreset(int idx, std::string* why = nullptr) {
        return hostedPresetAllowed(idx, why) && loadActivePreset(idx);
    }

    void sendHostedRefusal(int fd, const char* what) { sendJson(fd, std::string("{\"ok\":false,\"hosted\":true,\"error\":\"") + what + "\"}"); }

    // Routes a hosted engine must not serve as-is. Returns true when it answered.
    bool hostedHttpOverride(int fd, const std::string& path, const std::string& query) {
        auto qi = [&](const char* k, long def) -> long {
            const std::string v = getQueryParam(query, k);
            if (v.empty()) return def;
            try { return std::stol(v); } catch (...) { return def; }
        };
        if (path == "/api/audio") { // no device enumeration: the DAW owns audio I/O
            std::ostringstream o;
            o << "{\"hosted\":true,\"inputs\":[],\"outputs\":[],\"current\":{\"source\":\"live\",\"sample_rate\":" << static_cast<int>(sample_rate_)
              << ",\"monitor\":true},\"buffer_sizes\":[],\"status\":{\"running\":true,\"input_ok\":true,\"latency_ms\":0,\"underruns\":0,"
                 "\"error\":\"\",\"mic_permission\":\"granted\"}}";
            sendJson(fd, o.str());
            return true;
        }
        if (path.rfind("/api/audio/", 0) == 0) { sendHostedRefusal(fd, "audio devices are owned by the host (DAW)"); return true; }
        if (path.rfind("/api/plugins", 0) == 0) { sendHostedRefusal(fd, "plugin hosting is only available in the Headroom app"); return true; }
        if (path == "/api/tracks") { sendJson(fd, "{\"tracks\":[],\"user_dir\":\"\",\"hosted\":true}"); return true; }
        if (path == "/api/transport") {
            const std::string action = getQueryParam(query, "action");
            if (action == "play" || action == "pause" || action == "toggle" || action == "track" || action == "seek" || action == "loop") {
                sendJson(fd, buildTelemetryJson()); // DI player / source controls: the host's input is the source
                return true;
            }
            if (action == "preset") {
                std::string why;
                if (!hostedPresetAllowed(static_cast<int>(qi("idx", 0)), &why)) { sendJson(fd, buildRackStateJson()); return true; }
            }
            return false;
        }
        if (path == "/api/preset/load") {
            std::string why;
            if (!hostedPresetAllowed(static_cast<int>(qi("idx", 0)), &why)) { sendJson(fd, buildRackStateJson()); return true; }
            return false;
        }
        if (path == "/api/rack/new") {
            newRackHosted(getQueryParam(query, "template"));
            sendJson(fd, buildRackStateJson());
            return true;
        }
        if (path == "/api/rack/insert" || path == "/api/rack/add") {
            if (hostedUnsupportedTable(static_cast<int32_t>(qi("table_id", 0)))) { sendJson(fd, buildRackStateJson()); return true; }
            return false;
        }
        return false;
    }

    static std::shared_ptr<const library::ComponentPresetBank> sharedComponentBank() {
        static std::mutex m;
        static std::shared_ptr<library::ComponentPresetBank> slot;
        std::lock_guard<std::mutex> lk(m);
        if (!slot) {
            slot = std::make_shared<library::ComponentPresetBank>();
            slot->scan();
        }
        return slot;
    }
    static std::shared_ptr<const library::PresetLibrary> sharedLibraryIndex(const std::string& user,
                                                                            const std::vector<library::FeaturedSpec>& featured,
                                                                            const std::string& showcase) {
        static std::mutex m;
        static std::map<std::string, std::shared_ptr<library::PresetLibrary>> cache;
        std::lock_guard<std::mutex> lk(m);
        auto& slot = cache[user + "|" + showcase];
        if (!slot) {
            slot = std::make_shared<library::PresetLibrary>();
            slot->scan(user, featured, showcase);
        }
        return slot;
    }

    bool hosted_ = false;
    bool hosted_prepared_ok_ = true; // model lock
    bool hosted_fresh_ = false;      // model lock: graph built by startHosted, not yet host-prepared
    std::atomic<bool> hosted_editor_started_{false};
    uint16_t hosted_port_ = 0;
    std::atomic<bool> hosted_library_started_{false};
    std::atomic<bool> hosted_reset_pending_{false};
    std::atomic<double> host_bpm_{0.0}, host_beat_{0.0};
    std::atomic<bool> host_playing_{false};

public:
    bool hosted_reset_history_on_prepare_ = true; // approved: zero output-stage history at quiescent prepare
