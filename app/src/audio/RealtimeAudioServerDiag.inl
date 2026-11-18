// One-time level migration of user rigs (retired loudness cache → the rigs' own level knobs) and live engine
// diagnostics (GET /api/diag, HEADROOM_DIAG_LOG) for RealtimeAudioServer. Included INSIDE the class body (public
// section). Documented in docs/ENGINE_API.md → "Levels" and "Diagnostics".
//
// Real-time contract: nothing here runs on the audio thread. The render path only writes EngineDiagBank's atomics
// (core/EngineDiag.hpp) through renderBlock / renderChunk / RackGraph::process; everything below reads them, or keeps
// control-side history under its own mutex (lock order: graph_mutex_ → dg_mtx_; never the reverse).

    // ───────────── one-time level migration of user rigs (retired loudness cache → the rig's own knobs) ─────────────
    // Earlier builds matched every rig's loudness at run time, behind the player's back, and kept the gains in
    // <data dir>/loudness-cache-exact.txt (append-only lines "<key>\t<gain>", key "<rig path>|..."). The engine no
    // longer measures or re-levels anything. So that a user rig does not jump in level, the gain the cache last gave
    // it is folded once, on the rig's first load, into its own pure post-gain level knobs (core/LevelFold.hpp: a
    // cabinet's Volume, an EQ's Output, ...) and written into the rig file as ordinary knob positions. Before a file is
    // changed it is copied to <data dir>/Level Migration Backups/; the cache itself is copied once to
    // loudness-cache-exact.txt.pre-level-migration.bak and never deleted or written. Every rig handled is recorded in
    // <data dir>/level-migration.txt (so it is never migrated twice, and the cache is not read for it again) and in
    // /api/diag → level.migrations. A rig with no such knob keeps its raw level (recorded as "no_level_knob").
    static std::string levelCachePath() { return library::appSupportDir() + "/loudness-cache-exact.txt"; }
    static std::string levelLedgerPath() { return library::appSupportDir() + "/level-migration.txt"; }
    // lvl_mig_mtx_ held. Reads the cache and the ledger once.
    void levelMigrationLoadLocked() {
        if (lvl_cache_loaded_) return;
        lvl_cache_loaded_ = true;
        {
            std::ifstream f(levelCachePath());
            std::string line;
            while (std::getline(f, line)) {
                const auto tab = line.rfind('\t');
                if (tab == std::string::npos) continue;
                float g = 0.0f;
                try { g = std::stof(line.substr(tab + 1)); } catch (...) { continue; }
                if (!std::isfinite(g) || g <= 0.0f) continue;
                const std::string key = line.substr(0, tab);
                std::size_t cut = key.find("|lc1|");
                const bool content_keyed = cut != std::string::npos;
                if (!content_keyed) cut = key.find('|');
                auto& e = lvl_cache_[cut == std::string::npos ? key : key.substr(0, cut)];
                e.newest = g; // the old engine's "last known" gain: newest line wins
                if (content_keyed && key.size() >= cut + 5 + 16) e.by_content[key.substr(cut + 5, 16)] = g;
            }
        }
        std::ifstream l(levelLedgerPath());
        std::string line;
        while (std::getline(l, line)) {
            const auto tab = line.find('\t');
            lvl_done_.insert(tab == std::string::npos ? line : line.substr(0, tab));
        }
    }
    // Loader thread, before the rig is built: folds the cached gain into `p` and the file (see above). Returns true
    // when the rig's knobs were moved.
    bool migrateUserRigLevel(const std::string& path, const std::string& name, preset::RackPreset& p) {
        std::string bytes;
        float gain = 0.0f;
        {
            std::lock_guard<std::mutex> lk(lvl_mig_mtx_);
            levelMigrationLoadLocked();
            if (lvl_done_.count(path)) return false;
            const auto it = lvl_cache_.find(path);
            if (it == lvl_cache_.end()) return false; // the old engine never played it: nothing to keep
            std::ifstream f(path, std::ios::binary);
            std::ostringstream ss;
            ss << f.rdbuf();
            bytes = ss.str();
            char h[24];
            std::snprintf(h, sizeof h, "%016llx", static_cast<unsigned long long>(fnv1a64(bytes)));
            // prefer the gain measured for the file as it is now (an older measurement of an edited file never wins)
            const auto c = it->second.by_content.find(h);
            gain = c != it->second.by_content.end() ? c->second : it->second.newest;
        }
        DiagMigration m;
        m.preset = name;
        m.path = path;
        m.cache_gain = gain;
        m.cache_db = 20.0 * std::log10(static_cast<double>(gain));
        preset::RackPreset folded = p;
        // the cache was measured with the SSL / Lexicon / delay Outputs dead: fold the gain minus what they now add
        const auto mf = levelfold::foldLegacyGain(folded, m.cache_db);
        const auto& r = mf.fold;
        m.live_db = mf.live_db;
        m.change_db = mf.change_db;
        m.applied_db = r.applied_db;
        m.moves = r.moves;
        m.stop = r.stop;
        std::string status = "no_level_knob";
        if (!r.moves.empty()) {
            std::error_code ec;
            const std::string dir = library::appSupportDir() + "/Level Migration Backups";
            std::filesystem::create_directories(dir, ec);
            m.backup = dir + "/" + std::filesystem::path(path).filename().string();
            {
                std::lock_guard<std::mutex> lk(lvl_mig_mtx_);
                if (!lvl_cache_backed_up_) { // the cache, once, before the first rig file changes
                    lvl_cache_backed_up_ = true;
                    const std::string bak = levelCachePath() + ".pre-level-migration.bak";
                    if (!std::filesystem::exists(bak, ec)) std::filesystem::copy_file(levelCachePath(), bak, ec);
                }
            }
            std::string err;
            m.written = levelfold::applyMovesToFile(path, r.moves, m.backup, &err);
            m.error = err;
            status = m.written ? "migrated" : "write_failed";
            if (m.written) p = std::move(folded); // plays exactly as the file now reads
        }
        m.status = status;
        {
            std::lock_guard<std::mutex> lk(lvl_mig_mtx_);
            lvl_done_.insert(path);
            std::ofstream l(levelLedgerPath(), std::ios::app);
            char buf[200];
            std::snprintf(buf, sizeof buf, "\t%s\tcache_gain=%.6f\tcache_db=%+.2f\tlive_knob_db=%+.2f\tapplied_db=%+.2f\tchange_db=%+.2f\n",
                          status.c_str(), static_cast<double>(gain), m.cache_db, m.live_db, m.applied_db, m.change_db);
            l << path << buf;
        }
        std::fprintf(stderr, "Headroom: level of \"%s\": %s (cache %+.1f dB, Output knobs now live %+.1f dB, folded %+.1f dB into %zu knob(s), level change %+.1f dB)\n",
                     name.c_str(), status.c_str(), m.cache_db, m.live_db, m.applied_db, m.moves.size(), m.change_db);
        try {
            std::lock_guard<std::mutex> lk(dg_mtx_);
            diagPush(dg_migrations_, std::move(m));
        } catch (...) {}
        return status == "migrated";
    }

    // ───────────── diagnostics: control-side history ─────────────
    struct DiagMigration {
        uint64_t seq = 0; double t_unix = 0; uint64_t t_ns = 0;
        std::string preset, path, status, backup, error, stop;
        float cache_gain = 0;
        double cache_db = 0, applied_db = 0;
        double live_db = 0;   // what the newly live Output knobs add at the rig's stored values (levelfold::liveTrimDb)
        double change_db = 0; // level after the migration minus the level the old engine played it at (0 = no jump)
        std::vector<levelfold::Move> moves;
        bool written = false;
    };
    struct DiagLoad {
        uint64_t seq = 0; double t_unix = 0; uint64_t t_ns = 0;
        int idx = -1;
        std::string name, source, result = "error", error;
        double build_ms = -1, total_ms = 0;
        std::size_t units = 0, spares = 0;
        bool level_migrated = false;
    };
    static double diagUnixNow() {
        return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
    template <class T> void diagPush(std::deque<T>& q, T v) { // dg_mtx_ held
        v.seq = ++dg_seq_;
        v.t_unix = diagUnixNow();
        v.t_ns = diagNowNs();
        q.push_back(std::move(v));
        while (q.size() > 64) q.pop_front();
    }
    // One preset load (loadResolvedPreset), recorded when it returns or throws.
    struct DiagLoadScope {
        RealtimeAudioServer& s;
        DiagLoad e;
        uint64_t t0;
        DiagLoadScope(RealtimeAudioServer& srv, int idx, const ResolvedPreset& r) : s(srv), t0(diagNowNs()) {
            e.idx = idx;
            e.name = r.name;
            e.source = r.source;
        }
        ~DiagLoadScope() {
            try {
                e.total_ms = static_cast<double>(diagNowNs() - t0) / 1e6;
                std::lock_guard<std::mutex> lk(s.dg_mtx_);
                s.diagPush(s.dg_loads_, std::move(e));
            } catch (...) {}
        }
    };

    // ───────────── /api/diag ─────────────
    // full = the endpoint (cumulative + window + history); !full = one HEADROOM_DIAG_LOG line (window only, plus the
    // events since *since_seq).
    std::string buildDiagJson(bool full, uint64_t* since_seq = nullptr) {
        using J = nlohmann::ordered_json;
        const auto& B = *diag_bank_;
        const uint64_t now = diagNowNs();
        auto rd = [](double x, int d = 2) -> double {
            if (!std::isfinite(x)) return 0.0;
            const double m = std::pow(10.0, d);
            return std::round(x * m) / m;
        };
        auto db = [&](double lin) -> double { return lin > 1e-10 ? rd(20.0 * std::log10(lin), 1) : -200.0; };
        const double sr = sample_rate_.load();

        J o;
        o["ok"] = true;
        o["t_unix"] = rd(diagUnixNow(), 3);
        o["uptime_s"] = rd(static_cast<double>(now - dg_start_ns_) / 1e9, 1);
        if (full) {
            o["since_reset_s"] = B.resetNs() ? rd(static_cast<double>(now - B.resetNs()) / 1e9, 1) : 0.0;
            o["reset_pending"] = B.resetPending();
        }
        o["window_s"] = EngineDiagBank::kWindowBuckets * static_cast<double>(EngineDiagBank::kBucketNs) / 1e9;
        auto win = std::make_unique<EngineDiagBank::Window>();
        B.window(now, *win);
        const auto& w = *win;
        const double win_span_s = w.covered * static_cast<double>(EngineDiagBank::kBucketNs) / 1e9;
        o["window_covered_s"] = win_span_s;
        const uint64_t last = B.lastBlockNs();
        o["audio_rendering"] = last && now - last < 500'000'000ull;
        o["last_block_age_ms"] = last ? rd(static_cast<double>(now - last) / 1e6, 1) : -1.0;

        // render
        auto renderJ = [&](const EngineDiagBank::RenderVals& v, double p99_ns, double span_s) {
            J j;
            j["blocks"] = v.blocks;
            j["over_budget"] = v.over;
            j["over_ratio"] = v.blocks ? rd(double(v.over) / double(v.blocks), 3) : 0.0;
            j["mean_us"] = v.blocks ? rd(double(v.ns_sum) / double(v.blocks) / 1e3, 1) : 0.0;
            j["p99_us"] = rd(p99_ns / 1e3, 1);
            j["max_us"] = rd(double(v.ns_max) / 1e3, 1);
            j["budget_us"] = v.blocks ? rd(double(v.budget_sum) / double(v.blocks) / 1e3, 1) : 0.0;
            j["load"] = v.budget_sum ? rd(double(v.ns_sum) / double(v.budget_sum), 3) : 0.0;
            // audio the device pulled per wall-clock second: 1.0 when every cycle was rendered; less = skipped cycles
            j["realtime_ratio"] = span_s > 0 ? rd(double(v.budget_sum) / 1e9 / span_s, 3) : 0.0;
            j["late_callbacks"] = v.gaps;
            j["missing_ms"] = rd(double(v.gap_ns_sum) / 1e6, 1);
            j["max_gap_ms"] = rd(double(v.gap_max_ns) / 1e6, 1);
            j["output_bridges"] = v.bridges;
            return j;
        };
        J render;
        render["sample_rate"] = sr;
        render["callback_frames"] = B.lastFrames();
        render["callback_budget_us"] = B.lastFrames() && sr > 0 ? rd(1e6 * B.lastFrames() / sr, 1) : 0.0;
        if (!hosted_) {
            render["device"] = {{"running", audio_io_.running()}, {"buffer_frames", audio_io_.bufferSize()},
                                {"overloads", audio_io_.overloads()}, {"input_underruns", audio_io_.underruns()}};
        } else render["device"] = "host";
        {
            const auto pv = B.pipeline();
            bool on = true;
            uint32_t sub = 0;
            {
                std::lock_guard<std::mutex> lk(graph_mutex_);
                if (graph_) on = graph_->pipelining(), sub = graph_->pipeSubFrames();
            }
            render["pipelining"] = {{"on", on}, {"sub_frames", sub}, {"max_stages", RackGraph::pipeMaxStages()}, {"runs", pv.runs},
                                    {"avg_stages", pv.runs ? rd(double(pv.stages) / double(pv.runs), 2) : 0.0}, {"last_stages", pv.last_stages}};
            const auto ps = RtPool::instance().stats();
            render["rtpool"] = {{"workers", ps.workers}, {"active", ps.active}, {"forks", ps.forks}, {"ran_on_worker", ps.on_worker},
                                {"reclaimed_inline", ps.reclaimed}, {"refused", ps.refused}, {"in_device_workgroup", ps.joined_workgroup}};
        }
        if (full) {
            const double since = B.resetNs() ? static_cast<double>(now - B.resetNs()) / 1e9 : 0.0;
            render["total"] = renderJ(B.renderTotal(), B.totalPercentileNs(0.99), since);
        }
        render["window"] = renderJ(w.render, B.windowPercentileNs(now, EngineDiagBank::kWindowBuckets * EngineDiagBank::kBucketNs + EngineDiagBank::kBucketNs, 0.99),
                                   win_span_s);
        o["render"] = std::move(render);

        // slots
        auto slotJ = [&](const EngineDiagBank::SlotVals& v) {
            J j;
            j["blocks"] = v.blocks;
            j["mean_us"] = v.blocks ? rd(double(v.ns_sum) / double(v.blocks) / 1e3, 1) : 0.0;
            j["max_us"] = rd(double(v.ns_max) / 1e3, 1);
            j["max_load"] = rd(v.max_load, 3); // the unit's own time / the block's period, worst block
            j["over_share"] = v.over_share;    // blocks over its equal share of the period (period / units rendering)
            j["over_block"] = v.over_block;    // blocks where this unit alone took longer than the whole period
            j["out_peak_db"] = db(v.out_peak);
            j["out_rms_db"] = v.frames ? db(std::sqrt(v.out_sumsq / double(v.frames))) : -200.0;
            j["nonfinite"] = v.nonfinite;
            return j;
        };
        auto solverJ = [&](const EngineDiagBank::SlotVals& v) {
            J j;
            j["samples"] = v.c_samples;
            j["newton_failures"] = v.c_failures;
            j["homotopies"] = v.c_homotopies;
            j["ptc_rescues"] = v.c_ptc;
            j["half_steps"] = v.c_half;
            j["lte_substeps"] = v.c_lte;
            j["avg_newton_iters"] = v.c_samples ? rd(double(v.c_iters) / double(v.c_samples), 2) : 0.0;
            return j;
        };
        J slots = J::array();
        std::string rig_name;
        {
            std::lock_guard<std::mutex> lk(graph_mutex_);
            rig_name = cur_name_;
            if (graph_) {
                for (const auto& v : graph_->activeNodeViews()) {
                    J s;
                    s["slot"] = v.slot_idx;
                    s["table_id"] = v.table_id;
                    s["name"] = std::string(v.component->schema().display_name);
                    s["bypassed"] = v.component->isBypassed();
                    if (v.slot_idx >= static_cast<std::size_t>(EngineDiagBank::kSlots)) {
                        s["tracked"] = false;
                        slots.push_back(std::move(s));
                        continue;
                    }
                    const int i = static_cast<int>(v.slot_idx);
                    const auto info = B.slotInfo(i);
                    const auto tot = B.slotTotal(i);
                    const uint64_t key = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(v.component));
                    s["same_instance"] = tot.key == key; // false: the stats are of the unit this slot held before
                    s["rendering"] = info.last_ns && now - info.last_ns < 1'000'000'000ull;
                    if (full) s["total"] = slotJ(tot);
                    const auto& ws = w.slots[static_cast<std::size_t>(i)];
                    s["window"] = slotJ(ws.key == tot.key ? ws : EngineDiagBank::SlotVals{});
                    if (info.circuit) {
                        J c;
                        c["internal_rate_hz"] = info.internal_rate;
                        c["oversampling"] = info.oversampling;
                        c["multirate"] = info.multirate;
                        c["max_newton_iters"] = info.max_iterations;
                        if (full) c["total"] = solverJ(tot);
                        c["window"] = solverJ(ws.key == tot.key ? ws : EngineDiagBank::SlotVals{});
                        s["circuit"] = std::move(c);
                    }
                    slots.push_back(std::move(s));
                }
            }
        }
        o["preset"] = rig_name;
        o["slots"] = std::move(slots);

        // master section (RackGraph::process): peak limiter, soft ceiling (no loudness gain)
        auto masterJ = [&](const EngineDiagBank::MasterVals& v) {
            J j;
            j["samples"] = v.samples;
            j["rig_peak_db"] = db(v.rig_peak); // the rack's output, into the limiter
            j["rig_rms_db"] = v.samples ? db(std::sqrt(v.rig_sumsq / double(v.samples))) : -200.0;
            j["post_gain_peak_db"] = db(v.post_gain_peak);
            j["limiter_samples"] = v.limiter;
            j["limiter_pct"] = v.samples ? rd(100.0 * double(v.limiter) / double(v.samples), 2) : 0.0;
            j["limiter_max_gr_db"] = rd(20.0 * std::log10(std::max(1.0f, v.limiter_max_env)), 2);
            j["soft_ceiling_samples"] = v.soft; // above 0.85 after the limiter: the soft ceiling shapes them
            j["nonfinite"] = v.nonfinite;
            return j;
        };
        {
            J m;
            if (full) m["total"] = masterJ(B.masterTotal());
            m["window"] = masterJ(w.master);
            o["master"] = std::move(m);
        }
        // output stage (renderChunk): DC blocker, OUT gain, guard, then the backing track
        auto outJ = [&](const EngineDiagBank::OutputVals& v) {
            J j;
            j["samples"] = v.samples;
            j["clipped_l"] = v.clip_l; // |x| >= 1.0 at the device (0 dBFS)
            j["clipped_r"] = v.clip_r;
            j["guard_l"] = v.guard_l;  // above 0.95 before the last-resort guard (it bends them)
            j["guard_r"] = v.guard_r;
            j["guard_pct"] = v.samples ? rd(50.0 * double(v.guard_l + v.guard_r) / double(v.samples), 3) : 0.0;
            j["peak_l_db"] = db(v.peak_l);
            j["peak_r_db"] = db(v.peak_r);
            j["pre_guard_peak_db"] = db(v.pre_guard_peak);
            j["rms_db"] = v.samples ? db(std::sqrt(v.sumsq / double(v.samples))) : -200.0;
            j["nonfinite"] = v.nonfinite;
            j["input_peak_db"] = db(v.in_peak);
            j["input_clip_samples"] = v.in_clip; // |input| > 0.98 (after the input gain)
            return j;
        };
        {
            J out;
            out["guard"] = "soft above 0.95 FS (transparent below); limiter in the rack holds -1 dBFS";
            if (full) out["total"] = outJ(B.outputTotal());
            out["window"] = outJ(w.out);
            o["output"] = std::move(out);
        }

        // levels: no automatic levelling; the one-time user-rig migration log
        J lvl;
        lvl["automatic_leveling"] = false; // the engine never measures or re-levels a rig
        std::lock_guard<std::mutex> lk(dg_mtx_);
        auto migrationJ = [&](const DiagMigration& m) {
            J mv = J::array();
            for (const auto& x : m.moves)
                mv.push_back(J{{"unit", x.unit}, {"table_id", x.table_id}, {"param", x.param}, {"label", x.label}, {"from", rd(x.from, 4)},
                               {"to", rd(x.to, 4)}, {"db", rd(x.db, 2)}});
            J j{{"ago_s", rd(static_cast<double>(now - m.t_ns) / 1e9, 1)}, {"t_unix", rd(m.t_unix, 3)}, {"preset", m.preset}, {"path", m.path},
                {"status", m.status}, {"cache_gain", rd(m.cache_gain, 6)}, {"cache_db", rd(m.cache_db, 2)}, {"applied_db", rd(m.applied_db, 2)},
                {"live_knob_db", rd(m.live_db, 2)}, {"change_db", rd(m.change_db, 2)},
                {"residual_db", rd(m.cache_db - m.live_db - m.applied_db, 2)}, {"moves", std::move(mv)}, {"written", m.written}};
            if (!m.backup.empty()) j["backup"] = m.backup;
            if (!m.stop.empty()) j["knob_search_stopped_at"] = m.stop;
            if (!m.error.empty()) j["error"] = m.error;
            return j;
        };
        auto loadJ = [&](const DiagLoad& e) {
            J j{{"ago_s", rd(static_cast<double>(now - e.t_ns) / 1e9, 1)}, {"t_unix", rd(e.t_unix, 3)}, {"idx", e.idx}, {"name", e.name}, {"source", e.source},
                {"result", e.result}, {"build_ms", rd(e.build_ms, 1)}, {"total_ms", rd(e.total_ms, 1)}, {"units", e.units},
                {"spares_used", e.spares}, {"level_migrated", e.level_migrated}};
            if (!e.error.empty()) j["error"] = e.error;
            return j;
        };
        if (full) {
            J mg = J::array();
            for (auto it = dg_migrations_.rbegin(); it != dg_migrations_.rend(); ++it) mg.push_back(migrationJ(*it));
            lvl["migrations"] = std::move(mg);
            lvl["migration_source"] = levelCachePath();
            lvl["migration_ledger"] = levelLedgerPath();
        }
        o["level"] = std::move(lvl);
        {
            J wk;
            {
                std::lock_guard<std::mutex> jl(job_mtx_);
                wk["queued"] = jobs_.size();
            }
            wk["qos"] = "background";
            wk["thread_cpu_s"] = running_.load() ? rd(threadCpuSeconds(worker_), 3) : -1.0;
            o["worker"] = std::move(wk);
        }

        const bool live = running_.load(); // thread handles are only queried while the engine runs (stop() joins them)
        if (full) {
            J loads = J::array();
            for (auto it = dg_loads_.rbegin(); it != dg_loads_.rend() && loads.size() < 16; ++it) loads.push_back(loadJ(*it));
            o["preset_loads"] = std::move(loads);
            J th;
            auto cpu = [&](const std::thread& t) { return live ? rd(threadCpuSeconds(t), 3) : -1.0; };
            th["worker_cpu_s"] = cpu(worker_);
            {
                const auto pd = PreparedUnitPool::instance().diag();
                th["spares_pool"] = {{"cpu_s", rd(pd.cpu_s, 3)}, {"prepared", pd.prepared}, {"taken", pd.taken}, {"spares", pd.spares},
                                     {"preparing_table_id", pd.preparing}, {"held", pd.held}};
            }
            th["http_cpu_s"] = cpu(http_thread_);
            {
                std::lock_guard<std::mutex> pl(pl_mtx_);
                th["preset_loader_cpu_s"] = cpu(pl_thread_);
            }
            if (!hosted_) th["library_scan_cpu_s"] = cpu(library_thread_);
            th["diag_log_cpu_s"] = cpu(dg_log_thread_);
            th["process_cpu_s"] = rd(processCpuSeconds(), 3);
            th["note"] = "thread CPU clocks; -1 = thread not running";
            o["threads"] = std::move(th);
            o["engine_build"] = engineBuildFingerprint();
        } else if (since_seq) {
            J ev = J::array();
            for (const auto& e : dg_loads_) if (e.seq > *since_seq) ev.push_back(J{{"type", "preset_load"}, {"data", loadJ(e)}});
            for (const auto& m : dg_migrations_) if (m.seq > *since_seq) ev.push_back(J{{"type", "level_migration"}, {"data", migrationJ(m)}});
            if (!ev.empty()) o["events"] = std::move(ev);
            *since_seq = dg_seq_;
        }
        return o.dump();
    }

    // /api/diag?reset=1: the render thread clears the cumulative counters at its next block (wait briefly for it).
    void diagReset() {
        diag_bank_->requestReset();
        for (int i = 0; i < 40 && diag_bank_->resetPending(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // HEADROOM_DIAG_LOG=<path>: one JSON line per second (window stats + events since the last line), appended by this
    // thread (utility QoS), never by the audio thread. start() starts it when the variable is set; tools may call it.
    void startDiagLog(const std::string& path) {
        if (path.empty() || dg_log_thread_.joinable()) return;
        dg_log_stop_ = false;
        dg_log_thread_ = std::thread([this, path] {
#ifdef __APPLE__
            pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#endif
            uint64_t seq = 0;
            std::unique_lock<std::mutex> lk(dg_log_mtx_);
            auto next = std::chrono::steady_clock::now();
            while (!dg_log_stop_) {
                next += std::chrono::seconds(1);
                if (dg_log_cv_.wait_until(lk, next, [this] { return dg_log_stop_; })) break;
                lk.unlock();
                std::string line;
                try { line = buildDiagJson(false, &seq); } catch (...) {}
                if (!line.empty()) {
                    std::ofstream f(path, std::ios::app);
                    f << line << "\n";
                }
                lk.lock();
            }
        });
    }
    void stopDiagLog() {
        {
            std::lock_guard<std::mutex> lk(dg_log_mtx_);
            dg_log_stop_ = true;
        }
        dg_log_cv_.notify_all();
        if (dg_log_thread_.joinable()) dg_log_thread_.join();
    }

private:
    std::shared_ptr<EngineDiagBank> diag_bank_ = std::make_shared<EngineDiagBank>(); // shared with every plan
    // Level migration (lvl_mig_mtx_): the old cache, read once (per rig path: its newest gain, and the newest per
    // content hash), and the ledger of rigs already handled.
    struct LevelCacheEntry { float newest = 0; std::map<std::string, float> by_content; };
    std::mutex lvl_mig_mtx_;
    bool lvl_cache_loaded_ = false, lvl_cache_backed_up_ = false;
    std::map<std::string, LevelCacheEntry> lvl_cache_;
    std::set<std::string> lvl_done_;
    mutable std::mutex dg_mtx_;
    uint64_t dg_seq_ = 0;
    std::deque<DiagMigration> dg_migrations_;
    std::deque<DiagLoad> dg_loads_;
    const uint64_t dg_start_ns_ = diagNowNs();
    std::thread dg_log_thread_;
    std::mutex dg_log_mtx_;
    std::condition_variable dg_log_cv_;
    bool dg_log_stop_ = false;

public:
