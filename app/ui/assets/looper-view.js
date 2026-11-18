/* Headroom — Looper view: a three-track loop station  (assets/looper-view.js + looper-view.css → #view-looper)
 *
 *   Engine: GET /api/looper (status, settings, actions) and /api/looper/wave?track=N&bins=B (per-layer envelopes),
 *   both answered by LoopStation (app/src/dsp/LoopStation.*; contract in app/docs/ENGINE_API.md "Looper").
 *
 *   Layout (fits the centre pane without scrolling from 1100×720 up):
 *     - display: the selected track's state, a bar·beat (Tempo sync) or time counter, tempo; one lane per track
 *       with its layers stacked (oldest at the core, newest outside), undone layers as a ghost, beat / bar grid
 *       and the playhead;
 *     - setup strip: Source (post-rig / pre-rig re-amp), Input monitoring, Sync (Free / Loop / Tempo), Length
 *       (Auto or N bars), Count-in, Click, Stop mode, After rec (play / overdub);
 *     - deck: a channel per track (level, pan, mute, reverse, ½× / 1× / 2×, stop, clear and its Rec/Play/Dub
 *       footswitch with a position ring), and a master channel (loop level, feedback, undo / redo, All Start/Stop).
 *   Keys (Looper tab only, never while typing) double as footswitches: 1 2 3 that track's footswitch, R the
 *   selected track's, S All Start/Stop (twice = stop now), ⇧1–3 stop a track, U undo, ⇧U redo, ⌫ clear the
 *   selected track (press twice), ⇧⌫ clear all (twice), M mute, V reverse, - / = speed, ↑ ↓ select, K click.
 *   The tab LED (#vt-looper-led) follows the engine from any tab (1 s poll while hidden).
 *
 * Registered with HRViews (assets/studio-views.js). Uses index.html globals at call time only: API_BASE, knobCore,
 * scaleSVG, queueSend, showMenu, closeMenu, toast, ui.
 */
(function () {
  "use strict";
  const $ = (id) => document.getElementById(id);
  const clamp = (v, a, b) => Math.max(a, Math.min(b, v));
  const now = () => performance.now();
  const api = async (path) => { const r = await fetch(API_BASE + path, { cache: "no-store" }); if (!r.ok) throw new Error(r.status); return r.json(); };
  const say = (m) => { if (typeof toast === "function") toast(m); };
  const setText = (el, v) => { if (el && el._v !== v) { el._v = v; el.textContent = v; } };
  const setHTML = (el, v) => { if (el && el._h !== v) { el._h = v; el.innerHTML = v; } };
  const typingIn = (a) => a && (a.tagName === "INPUT" && a.type !== "range" || a.tagName === "SELECT" || a.tagName === "TEXTAREA" || a.isContentEditable);
  const fmtT = (s) => { s = Math.max(0, s || 0); const m = Math.floor(s / 60), r = s - m * 60; return `${m}:${r.toFixed(1).padStart(4, "0")}`; };
  const fmtDb = (g) => (g <= 0.001 ? "−∞ dB" : `${g >= 0.9995 ? "+" : ""}${(20 * Math.log10(g)).toFixed(1)} dB`.replace("+-", "−").replace("-", "−"));
  const fmtPan = (p) => (Math.abs(p) < 0.02 ? "C" : `${p < 0 ? "L" : "R"}${Math.round(Math.abs(p) * 100)}`);

  const NT = 3;
  const HUES = ["#e3a04b", "#4cc7b5", "#a58cf2"];               // track identity (waveform)
  const STATE_COL = { recording: "#ff5246", overdubbing: "#ffad3a", playing: "#43d68f", armed: "#f2f2f2", stopped: "#7f8a96", empty: "#59616b" };
  const STATE_LBL = { empty: "Empty", armed: "Armed", recording: "Recording", playing: "Playing", overdubbing: "Overdub", stopped: "Stopped" };
  const SYNC_LBL = { free: "Free", loop: "Loop", tempo: "Tempo" };
  const STOP_LBL = { instant: "Instant", loop: "Loop end", fade: "Fade" };
  const BAR_OPTS = [0, 1, 2, 4, 8, 16];

  const S = {
    built: false, visible: false, st: null, stAt: 0, supported: true,
    pollT: 0, seq: 0, raf: 0, inflight: false,
    waves: Array.from({ length: NT }, () => ({ rev: -1, at: 0, data: null, busy: false })),
    clearArm: { t: -1, at: 0 }, lastExport: "", dragging: null,
  };

  /* =====================================================================
     ENGINE
     ===================================================================== */
  /* Idle CPU (assets/activity.js): no polling at all while the window/editor is hidden (HRActivity restarts it).
     Open: 20 Hz while a track records/plays/is armed, 4 Hz while the looper is idle. Other tab: the LED follows
     /api/telemetry's "looper" summary when the engine sends one (onTelemetry below), else a 1 Hz poll. */
  const busyState = (st) => !!(st && st.tracks && st.tracks.some((t) => t.state && t.state !== "empty" && t.state !== "stopped" && t.state !== "paused"));
  async function poll() {
    clearTimeout(S.pollT); S.pollT = 0;
    if (window.HRActivity && !HRActivity.awake()) return;
    if (!S.visible && S.telLed) return;   // the LED rides on telemetry
    // API_BASE is only settled once index.html has found its engine (it may be a test instance on another port)
    if (typeof ui === "undefined" || !ui.everConnected) { S.pollT = setTimeout(poll, 300); return; }
    if (!S.inflight) {
      S.inflight = true;
      const seq = ++S.seq;
      try {
        const d = await api("/api/looper");
        if (seq === S.seq) ingest(d);
      } catch (e) { /* engine offline */ }
      S.inflight = false;
    }
    if (window.HRActivity && !HRActivity.awake()) return;
    if (S.visible) { paint(); fetchWaves(); drawSoon(); S.pollT = setTimeout(poll, busyState(S.st) ? 50 : 250); }
    else { paintLed(); if (!S.telLed) S.pollT = setTimeout(poll, 1000); }
  }
  if (window.HRActivity) HRActivity.on((e) => { if (e.awake && !S.pollT && !S.inflight) poll(); });
  // Telemetry (index.html → HRViews.onTelemetry): a "looper" summary drives the tab LED from any tab.
  function onTelemetry(d) {
    const lp = d && d.looper;
    if (!lp || typeof lp.state !== "string") return;
    S.telLed = true;
    if (S.visible) return;
    // "tracks": every track's state (engine ENGINE_API.md); same rule as paintLed()
    const ss = Array.isArray(lp.tracks) ? lp.tracks : [lp.state], l = $("vt-looper-led");
    const cls = ss.includes("recording") || ss.includes("armed") ? "rec" : ss.includes("overdubbing") ? "dub" : ss.includes("playing") ? "play" : "";
    if (l && l.className !== `vt-led ${cls}`) l.className = `vt-led ${cls}`;
  }
  function ingest(d) {
    if (d && typeof d.state === "string") {
      S.supported = Array.isArray(d.tracks);
      S.st = d;
      S.stAt = now();
    } else S.supported = false;
  }
  async function cmd(q) {
    try {
      const d = await api(`/api/looper?${q}`);
      if (d && typeof d.state === "string") { ++S.seq; ingest(d); paint(); drawSoon(); }
      return d;
    } catch (e) { say("Couldn’t reach the engine."); return null; }
  }
  // Waveforms: fetched per track when its content revision moves (throttled while it's being written).
  function fetchWaves() {
    const st = S.st;
    if (!st || !st.tracks) return;
    st.tracks.forEach((tr, t) => {
      const w = S.waves[t];
      const cv = document.querySelector(`.lx-lane[data-t="${t}"] canvas`);
      const bins = clamp(Math.round((cv ? cv.clientWidth : 400) / 2.5), 48, 480);
      const resized = w.data && Math.abs(w.data.bins - bins) > bins * 0.15;
      if (w.busy || (tr.env_rev === w.rev && !resized)) return;
      const writing = tr.state === "recording" || tr.state === "overdubbing";
      if (writing && now() - w.at < 140) return;
      w.busy = true;
      const rev = tr.env_rev;
      api(`/api/looper/wave?track=${t}&bins=${bins}`).then((d) => { if (d && d.layers) { w.data = d; w.rev = rev; } })
        .catch(() => {}).finally(() => { w.busy = false; w.at = now(); });
    });
  }

  /* =====================================================================
     BUILD
     ===================================================================== */
  const icon = {
    stop: `<svg viewBox="0 0 12 12"><rect x="2.5" y="2.5" width="7" height="7" rx="1.2" fill="currentColor"/></svg>`,
    clear: `<svg viewBox="0 0 12 12" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"><path d="M3 3l6 6M9 3l-6 6"/></svg>`,
    export: `<svg viewBox="0 0 14 14" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"><path d="M7 2v7M4.2 6.4 7 9.2l2.8-2.8M2.5 11.5h9"/></svg>`,
  };
  function knobHTML(k, label, t) {
    const core = typeof knobCore === "function" ? knobCore("alu", 32, -135, typeof scaleSVG === "function" ? scaleSVG(k === "pan" ? "ticks" : "arc", 0, {}, "cont") : "") : "";
    return `<div class="lx-knob k-alu" data-k="${k}"${t != null ? ` data-t="${t}"` : ""} role="slider" tabindex="0" aria-label="${label}${t != null ? ` track ${t + 1}` : ""}">
      ${core}<span class="lx-kl">${label}</span><span class="lx-kv">—</span></div>`;
  }
  function chip(id, label, title) {
    return `<button class="lx-chip" data-chip="${id}" title="${title}"><span class="lx-cl">${label}<i class="lx-cled"></i></span><span class="lx-cv"><b>—</b></span></button>`;
  }
  function build() {
    S.built = true;
    const box = $("view-looper");
    const lanes = [0, 1, 2].map((t) => `
      <div class="lx-lane" data-t="${t}" style="--hue:${HUES[t]}" title="Track ${t + 1} — click to select">
        <div class="lx-lnum"><b>${t + 1}</b><i class="lx-lled"></i></div>
        <div class="lx-lwave"><canvas></canvas><span class="lx-lmsg"></span></div>
        <div class="lx-lmeta"><b class="num"></b><small></small></div>
      </div>`).join("");
    const strips = [0, 1, 2].map((t) => `
      <div class="lx-strip" data-t="${t}" style="--hue:${HUES[t]}">
        <div class="lx-sh"><span class="lx-sname"><i></i>Track ${t + 1}</span>
          <span class="lx-sbtns"><button class="lx-ib" data-act="stop" data-t="${t}" title="Stop track ${t + 1} (⇧${t + 1})" aria-label="Stop track ${t + 1}">${icon.stop}</button><button class="lx-ib" data-act="clear" data-t="${t}" title="Clear track ${t + 1} — press twice (⌫ clears the selected track)" aria-label="Clear track ${t + 1}">${icon.clear}</button></span></div>
        <div class="lx-knobs">${knobHTML("level", "Level", t)}${knobHTML("pan", "Pan", t)}</div>
        <div class="lx-tgls">
          <button class="lx-tg" data-tg="mute" data-t="${t}" title="Mute — keeps playing in time (M)">M</button>
          <button class="lx-tg" data-tg="reverse" data-t="${t}" title="Reverse (V)">REV</button>
          <span class="lx-spd" role="group" aria-label="Speed">
            <button data-spd="0.5" data-t="${t}" title="Half speed — an octave down">½×</button><button data-spd="1" data-t="${t}" title="Normal speed">1×</button><button data-spd="2" data-t="${t}" title="Double speed — an octave up">2×</button>
          </span>
        </div>
        <div class="lx-fs">
          <div class="lx-ring"><svg viewBox="0 0 100 100" aria-hidden="true"><circle class="bg" cx="50" cy="50" r="44"/><circle class="arc" cx="50" cy="50" r="44" pathLength="100" stroke-dasharray="0 100"/></svg>
            <button class="footswitch" data-act="press" data-t="${t}" aria-label="Track ${t + 1}: record, play, overdub"></button></div>
          <div class="lx-fl"><b>REC</b><kbd>${t + 1}</kbd></div>
        </div>
      </div>`).join("");
    box.innerHTML = `<div class="lx-page">
      <section class="lx-unit" id="lx-unit">
        <header class="lx-top">
          <div class="lx-brand"><span class="lx-logo">HEADROOM</span><span class="lx-model">LOOP STATION</span></div>
          <div class="lx-in" title="Input level going into the looper"><span>IN</span><div class="lx-inbar"><i id="lx-inmeter"></i></div></div>
          <button class="lx-export" id="lx-export" title="Export the loop as 24-bit WAV (mix, stems or one track)">${icon.export}<span>Export</span></button>
        </header>
        <div class="lx-display" id="lx-display">
          <div class="lx-dhead">
            <div class="lx-dstate"><i class="lx-sled" id="lx-sled"></i><div><b id="lx-slbl">EMPTY</b><small id="lx-ssub">TRACK 1</small></div></div>
            <div class="lx-counter num"><span id="lx-cmain">0:00.0</span><small id="lx-csub"></small></div>
            <div class="lx-tempo"><b class="num" id="lx-bpm">120</b><small id="lx-tsub">BPM · FREE</small></div>
          </div>
          <div class="lx-lanes">${lanes}</div>
        </div>
        <div class="lx-setup" id="lx-setup">
          ${chip("source", "Source", "Post-rig records what the rig sounds like. Pre-rig records your dry DI and plays it back through the rig (re-amp), so you can change the sound while it loops.")}
          ${chip("monitor", "Input", "Hear your live playing through the looper. Off: only the loops are heard (handy when re-amping).")}
          ${chip("sync", "Sync", "Free: every track has its own length. Loop: the first loop is the master, others snap to ¼, ½, 1, 2, 3… of it. Tempo: bars of the rig tempo, starting on the bar line.")}
          ${chip("bars", "Length", "Loop length in bars (Tempo sync). Auto: set by when you close the loop, rounded to the nearest bar.")}
          ${chip("count_in", "Count-in", "One bar of clicks before the first recording (Tempo sync).")}
          ${chip("click", "Click", "Metronome while the loops run (Tempo sync). Never recorded.")}
          ${chip("stop_mode", "Stop", "How Stop ends a loop: at once, at the end of the loop, or with a fade.")}
          ${chip("after_rec", "After rec", "What closing a recording does: play it back, or keep layering (overdub).")}
        </div>
        <div class="lx-deck">${strips}
          <div class="lx-strip lx-master">
            <div class="lx-sh"><span class="lx-sname">Master</span></div>
            <div class="lx-knobs">${knobHTML("mlevel", "Loops")}${knobHTML("feedback", "Feedback")}</div>
            <div class="lx-tgls lx-ur">
              <button class="lx-tg" data-act="undo" title="Undo the last layer of the selected track (U)">UNDO</button>
              <button class="lx-tg" data-act="redo" title="Redo (⇧U)">REDO</button>
            </div>
            <div class="lx-fs">
              <div class="lx-ring all"><svg viewBox="0 0 100 100" aria-hidden="true"><circle class="bg" cx="50" cy="50" r="44"/></svg>
                <button class="footswitch" data-act="all" aria-label="All start / stop"></button></div>
              <div class="lx-fl"><b id="lx-all-lbl">STOP</b><kbd>S</kbd></div>
            </div>
          </div>
        </div>
      </section>
      <div class="lx-hint"><span><kbd>1</kbd><kbd>2</kbd><kbd>3</kbd>track</span><span><kbd>R</kbd>rec · play · dub</span><span><kbd>S</kbd>start / stop</span><span><kbd>U</kbd>undo <kbd>⇧U</kbd>redo</span><span><kbd>⌫</kbd>clear</span><span><kbd>↑</kbd><kbd>↓</kbd>select</span></div>
    </div>`;
    box.addEventListener("click", onClick);
    box.querySelectorAll(".lx-knob").forEach(wireKnob);
  }

  /* =====================================================================
     ACTIONS
     ===================================================================== */
  const sel = () => (S.st && typeof S.st.selected === "number" ? S.st.selected : 0);
  const trk = (t) => (S.st && S.st.tracks ? S.st.tracks[t] : null);
  function press(t) { cmd(`action=press&track=${t}`); }
  function stopTrack(t) { cmd(`action=stop&track=${t}`); }
  function clearArmed(t) { return S.clearArm.t === t && now() - S.clearArm.at < 2200; }
  function clearTrack(t) {
    const tr = trk(t);
    if (t !== "all" && (!tr || tr.state === "empty")) return;
    if (clearArmed(t)) { S.clearArm.t = -1; cmd(`action=clear&track=${t}`); return; }
    S.clearArm = { t, at: now() };
    if (t === "all") say("Press again to clear every track.");
    paint();
    setTimeout(paint, 2300);
  }
  function setTrack(t, k, v) { cmd(`track=${t}&${k}=${v}`); }
  function onClick(e) {
    if (e.detail > 0) { const b = e.target.closest("button"); if (b) b.blur(); }   // a click shouldn't keep Space / keys
    const a = e.target.closest("[data-act]");
    if (a) {
      const t = a.dataset.t != null ? +a.dataset.t : sel();
      switch (a.dataset.act) {
        case "press": press(t); break;
        case "stop": stopTrack(t); break;
        case "clear": clearTrack(t); break;
        case "undo": cmd(`action=undo&track=${sel()}`); break;
        case "redo": cmd(`action=redo&track=${sel()}`); break;
        case "all": cmd("action=all"); break;
      }
      return;
    }
    const tg = e.target.closest("[data-tg]");
    if (tg) { const t = +tg.dataset.t, tr = trk(t); if (tr) setTrack(t, tg.dataset.tg, tr[tg.dataset.tg] ? 0 : 1); return; }
    const sp = e.target.closest("[data-spd]");
    if (sp) { setTrack(+sp.dataset.t, "speed", sp.dataset.spd); return; }
    const ch = e.target.closest("[data-chip]");
    if (ch) { chipAction(ch); return; }
    if (e.target.closest("#lx-export")) { exportMenu(e.target.closest("#lx-export")); return; }
    const lane = e.target.closest(".lx-lane");
    if (lane) { cmd(`select=${lane.dataset.t}`); return; }
    const strip = e.target.closest(".lx-strip[data-t]");
    if (strip && !e.target.closest("button, .lx-knob")) cmd(`select=${strip.dataset.t}`);
  }
  function menuAt(el, items) {
    if (typeof showMenu !== "function") return;
    const r = el.getBoundingClientRect();
    showMenu(r.left, r.bottom + 4, items);
  }
  function chipAction(el) {
    const st = S.st;
    if (!st) return;
    const id = el.dataset.chip;
    if (el.classList.contains("off")) return;
    switch (id) {
      case "source": cmd(`pre_rig=${st.pre_rig ? 0 : 1}`); break;
      case "monitor": cmd(`monitor=${st.monitor ? 0 : 1}`); break;
      case "count_in": cmd(`count_in=${st.count_in ? 0 : 1}`); break;
      case "click": cmd(`click=${st.click ? 0 : 1}`); break;
      case "after_rec": cmd(`after_rec=${st.after_rec === "dub" ? "play" : "dub"}`); break;
      case "sync": menuAt(el, [{ h: "Sync" },
        ...["free", "loop", "tempo"].map((k) => ({ label: SYNC_LBL[k], sub: { free: "independent lengths", loop: "multiples of the first loop", tempo: `bars at ${Math.round(st.bpm)} BPM` }[k], k: st.sync === k ? "✓" : "", onClick: () => cmd(`sync=${k}`) }))]); break;
      case "bars": menuAt(el, [{ h: "Loop length" },
        ...BAR_OPTS.map((b) => ({ label: b ? `${b} ${b === 1 ? "bar" : "bars"}` : "Auto", sub: b ? "closes by itself" : "nearest bar", k: st.bars_setting === b ? "✓" : "", onClick: () => cmd(`bars=${b}`) }))]); break;
      case "stop_mode": menuAt(el, [{ h: "Stop" },
        { label: "Instant", sub: "stops at once", k: st.stop_mode === "instant" ? "✓" : "", onClick: () => cmd("stop_mode=instant") },
        { label: "Loop end", sub: "finishes the loop", k: st.stop_mode === "loop" ? "✓" : "", onClick: () => cmd("stop_mode=loop") },
        ...[2, 4, 8].map((s) => ({ label: `Fade ${s} s`, sub: "fades out", k: st.stop_mode === "fade" && Math.round(st.fade_s) === s ? "✓" : "", onClick: () => cmd(`stop_mode=fade&fade_s=${s}`) }))]); break;
    }
  }
  function exportMenu(el) {
    const st = S.st;
    if (!st || !st.tracks.some((t) => t.length_s > 0)) { say("Record a loop first."); return; }
    const items = [{ h: "Export 24-bit WAV" },
      { label: "Mix", sub: "one full cycle", onClick: () => doExport("what=mix") },
      { label: "Stems", sub: "mix + each track", onClick: () => doExport("what=stems") }];
    st.tracks.forEach((t, i) => { if (t.length_s > 0) items.push({ label: `Track ${i + 1} only`, onClick: () => doExport(`what=track&track=${i}`) }); });
    const bridge = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.headroom;
    if (bridge) items.push({ sep: true }, { label: "Show last export in Finder", disabled: !S.lastExport, onClick: () => reveal(S.lastExport) });
    menuAt(el, items);
  }
  // The native bridge's revealRig selects any existing file in Finder (it is not rig-specific).
  function reveal(path) { try { window.webkit.messageHandlers.headroom.postMessage({ action: "revealRig", path }); } catch (e) { /* not in the app */ } }
  async function doExport(q) {
    const d = await cmd(`action=export&${q}`);
    if (d && d.ok) {
      S.lastExport = d.path || "";
      const n = (d.files || []).length;
      say(n > 1 ? `Exported ${n} files — Headroom/Loops/${String(d.path || "").split("/").slice(-2, -1)[0]}` : `Exported ${String(d.path || "").split("/").pop()} — in Headroom/Loops`);
    } else if (d) say(d.error ? `Couldn’t export: ${d.error}` : "Export failed.");
  }

  /* ---------- knobs: track level / pan, master loop level, feedback ---------- */
  const KN = {
    level: { get: (t) => (trk(t) ? trk(t).level : 1) / 1.5, set: (t, n) => ({ q: `track=${t}&level=${(n * 1.5).toFixed(3)}`, apply: (st) => (st.tracks[t].level = n * 1.5) }), fmt: (n) => fmtDb(n * 1.5), def: 1 / 1.5 },
    pan: { get: (t) => ((trk(t) ? trk(t).pan : 0) + 1) / 2, set: (t, n) => ({ q: `track=${t}&pan=${(n * 2 - 1).toFixed(3)}`, apply: (st) => (st.tracks[t].pan = n * 2 - 1) }), fmt: (n) => fmtPan(n * 2 - 1), def: 0.5 },
    mlevel: { get: () => (S.st ? S.st.level : 1) / 1.5, set: (t, n) => ({ q: `level=${(n * 1.5).toFixed(3)}`, apply: (st) => (st.level = n * 1.5) }), fmt: (n) => fmtDb(n * 1.5), def: 1 / 1.5 },
    feedback: { get: () => (S.st ? S.st.feedback : 1), set: (t, n) => ({ q: `feedback=${n.toFixed(3)}`, apply: (st) => (st.feedback = n) }), fmt: (n) => (n >= 0.995 ? "100%" : `${Math.round(n * 100)}%`), def: 1 },
  };
  function paintKnob(el, n) {
    const kn = el.querySelector(".kn");
    if (kn) kn.style.setProperty("--r", `${(-135 + 270 * n).toFixed(1)}deg`);
    const arc = el.querySelector(".arc-v");
    if (arc) arc.setAttribute("stroke-dasharray", `${(n * 100).toFixed(2)} 200`);
    setText(el.querySelector(".lx-kv"), KN[el.dataset.k].fmt(n));
    el.setAttribute("aria-valuenow", Math.round(n * 100));
  }
  function setKnob(el, n) {
    n = clamp(n, 0, 1);
    const k = el.dataset.k, t = el.dataset.t != null ? +el.dataset.t : -1;
    const { q, apply } = KN[k].set(t, n);
    if (S.st) apply(S.st);
    paintKnob(el, n);
    if (typeof queueSend === "function") queueSend(`looper-${k}-${t}`, `/api/looper?${q}`);
    else api(`/api/looper?${q}`).catch(() => {});
  }
  function wireKnob(el) {
    const k = el.dataset.k, t = el.dataset.t != null ? +el.dataset.t : -1;
    const val = () => clamp(KN[k].get(t), 0, 1);
    el.addEventListener("pointerdown", (e) => {
      if (e.button !== 0) return;
      e.preventDefault();
      el.setPointerCapture(e.pointerId);
      let y = e.clientY, v = val();
      S.dragging = el;
      const move = (me) => { v = clamp(v + (y - me.clientY) * (me.shiftKey ? 0.0015 : 0.006), 0, 1); y = me.clientY; setKnob(el, v); };
      const up = () => { el.removeEventListener("pointermove", move); el.removeEventListener("pointerup", up); el.removeEventListener("pointercancel", up); S.dragging = null; };
      el.addEventListener("pointermove", move); el.addEventListener("pointerup", up); el.addEventListener("pointercancel", up);
    });
    el.addEventListener("wheel", (e) => { e.preventDefault(); setKnob(el, val() + (e.deltaY < 0 ? 0.02 : -0.02)); }, { passive: false });
    el.addEventListener("dblclick", () => setKnob(el, KN[k].def));
    el.addEventListener("keydown", (e) => {
      const step = e.shiftKey ? 0.01 : 0.05;
      if (e.key === "ArrowUp" || e.key === "ArrowRight") { e.preventDefault(); e.stopPropagation(); setKnob(el, val() + step); }
      else if (e.key === "ArrowDown" || e.key === "ArrowLeft") { e.preventDefault(); e.stopPropagation(); setKnob(el, val() - step); }
    });
  }

  /* =====================================================================
     PAINT (DOM, on every status)
     ===================================================================== */
  function nextLabel(tr, st) {
    if (!tr) return "REC";
    switch (tr.state) {
      case "empty": return "REC";
      case "armed": return "CANCEL";
      case "recording": return st.after_rec === "dub" ? "DUB" : "PLAY";
      case "playing": return tr.stopping !== "none" ? "PLAY" : "DUB";
      case "overdubbing": return "PLAY";
      case "stopped": return "PLAY";
    }
    return "REC";
  }
  const beatS = (st) => 60 / Math.max(20, st.bpm || 120);
  function counterFor(st, tr) {
    // [main, sub]
    if (!tr) return ["0:00.0", ""];
    const tempo = st.sync === "tempo";
    if (st.clock && st.clock.count_in > 0) return [`${st.clock.count_in}`, "COUNT-IN"];
    if (tr.state === "armed") return [tempo || st.sync === "loop" ? `${tr.wait_s.toFixed(1)}s` : "—", tr.armed_rec ? "REC ON THE BAR" : "STARTS ON THE GRID"];
    if (tr.state === "recording") {
      if (tempo) { const b = tr.rec_s / (beatS(st) * st.beats); return [`${Math.floor(b) + 1}.${Math.floor((b % 1) * st.beats) + 1}`, tr.close_s > 0 ? `REC · CLOSES AT ${Math.round(tr.close_s / (beatS(st) * st.beats))} BARS` : "REC · BAR.BEAT"]; }
      return [fmtT(tr.rec_s), tr.close_s > 0 ? `REC · CLOSES AT ${fmtT(tr.close_s)}` : "RECORDING"];
    }
    if (tr.length_s > 0) {
      const p = livePos(tr);
      if (tempo && tr.bars > 0.5) {
        const beats = p * tr.length_s / beatS(st);
        const bar = Math.floor(beats / st.beats) + 1, beat = Math.floor(beats % st.beats) + 1;
        return [`${bar}.${beat}`, `OF ${Math.round(tr.bars)} ${Math.round(tr.bars) === 1 ? "BAR" : "BARS"}`];
      }
      return [fmtT(p * tr.length_s), `OF ${fmtT(tr.length_s)}`];
    }
    return ["0:00.0", ""];
  }
  function livePos(tr) {
    let p = tr.pos || 0;
    if ((tr.state === "playing" || tr.state === "overdubbing") && tr.length_s > 0) {
      const dt = (now() - S.stAt) / 1000;
      p += (tr.reverse ? -1 : 1) * dt * (tr.speed || 1) / tr.length_s;
      p -= Math.floor(p);
    }
    return p;
  }
  function paint() {
    if (!S.built) return;
    const box = $("view-looper"), st = S.st;
    box.classList.toggle("unsupported", !S.supported);
    if (!st || !st.tracks) { paintLed(); return; }
    const s = sel(), cur = st.tracks[s];
    const unit = $("lx-unit");
    unit.dataset.state = cur.state;
    unit.dataset.sync = st.sync;
    // display head
    $("lx-sled").style.setProperty("--c", STATE_COL[cur.state] || STATE_COL.empty);
    $("lx-sled").className = `lx-sled ${cur.state}`;
    const stopping = cur.stopping !== "none" && cur.state === "playing";
    setText($("lx-slbl"), (stopping ? (cur.stopping === "fade" ? "Fading" : "Stopping") : STATE_LBL[cur.state] || cur.state).toUpperCase());
    const sub = [`TRACK ${s + 1}`];
    if (cur.length_s > 0) sub.push(`${cur.layers} ${cur.layers === 1 ? "LAYER" : "LAYERS"}`);
    if (cur.full) sub.push("LAYERS FULL");
    setText($("lx-ssub"), sub.join(" · "));
    setText($("lx-bpm"), (st.bpm || 120).toFixed(st.bpm % 1 ? 1 : 0));
    const tsub = st.sync === "tempo" ? `BPM · ${st.beats}/4${st.tempo_locked && Math.abs(st.rig_bpm - st.bpm) > 0.05 ? ` · RIG ${Math.round(st.rig_bpm)}` : ""}` : `BPM · ${SYNC_LBL[st.sync].toUpperCase()} SYNC`;
    setText($("lx-tsub"), tsub);
    $("lx-bpm").parentNode.classList.toggle("locked", !!st.tempo_locked);
    // lanes
    st.tracks.forEach((tr, t) => {
      const lane = box.querySelector(`.lx-lane[data-t="${t}"]`);
      lane.dataset.state = tr.state;
      lane.classList.toggle("sel", t === s);
      lane.classList.toggle("muted", !!tr.mute);
      lane.querySelector(".lx-lled").style.setProperty("--c", STATE_COL[tr.state]);
      const lenTxt = tr.length_s > 0 ? (st.sync === "tempo" && tr.bars > 0.5 ? `${+tr.bars.toFixed(2)} ${Math.abs(tr.bars - 1) < 0.01 ? "bar" : "bars"}` : `${tr.length_s.toFixed(1)} s`) : tr.state === "recording" ? `${tr.rec_s.toFixed(1)} s` : "—";
      setText(lane.querySelector(".lx-lmeta b"), lenTxt);
      const flags = [];
      if (tr.state === "armed") flags.push(tr.armed_rec ? (st.clock && st.clock.count_in > 0 ? "count-in" : "rec on bar") : "waits");
      else if (tr.state === "recording") flags.push("rec");
      else if (tr.length_s > 0) flags.push(`${tr.layers} ${tr.layers === 1 ? "layer" : "layers"}`);
      if (tr.speed !== 1) flags.push(tr.speed < 1 ? "½×" : "2×");
      if (tr.reverse) flags.push("rev");
      if (tr.mute) flags.push("mute");
      setText(lane.querySelector(".lx-lmeta small"), flags.join(" · "));
      const msg = tr.state === "empty" ? (t === s ? `Press ${t + 1} or the footswitch to record` : "") : tr.state === "armed" && tr.armed_rec ? (st.clock && st.clock.count_in > 0 ? `Count-in · ${st.clock.count_in}` : "Recording starts on the next bar") : "";
      setText(lane.querySelector(".lx-lmsg"), msg);
      // strip
      const strip = box.querySelector(`.lx-strip[data-t="${t}"]`);
      strip.classList.toggle("sel", t === s);
      strip.dataset.state = tr.state;
      strip.querySelector('[data-tg="mute"]').classList.toggle("on", !!tr.mute);
      strip.querySelector('[data-tg="reverse"]').classList.toggle("on", !!tr.reverse);
      strip.querySelectorAll("[data-spd]").forEach((b) => b.classList.toggle("on", Math.abs(+b.dataset.spd - (tr.speed || 1)) < 0.01));
      const fl = strip.querySelector(".lx-fl b");
      setText(fl, nextLabel(tr, st));
      strip.querySelector(".lx-ring").style.setProperty("--c", STATE_COL[tr.state]);
      strip.querySelector(".lx-ring").dataset.state = stopping && t === s ? "stopping" : tr.state;
      strip.querySelector('[data-act="stop"]').disabled = !(tr.state === "playing" || tr.state === "overdubbing" || tr.state === "recording" || tr.state === "armed");
      const cl = strip.querySelector('[data-act="clear"]');
      cl.disabled = tr.state === "empty";
      cl.classList.toggle("armed", clearArmed(t));
    });
    // setup chips
    const chipSet = (id, val, on, off, ledOn) => {
      const el = box.querySelector(`[data-chip="${id}"]`);
      setText(el.querySelector("b"), val);
      el.classList.toggle("on", !!on);
      el.classList.toggle("off", !!off);
      el.querySelector(".lx-cled").classList.toggle("lit", !!ledOn);
    };
    const tempo = st.sync === "tempo";
    chipSet("source", st.pre_rig ? "Pre-rig" : "Post-rig", st.pre_rig, false, st.pre_rig);
    chipSet("monitor", st.monitor ? "On" : "Off", st.monitor, false, st.monitor);
    chipSet("sync", SYNC_LBL[st.sync], st.sync !== "free", false, st.sync !== "free");
    chipSet("bars", !tempo ? "Free" : st.bars_setting ? `${st.bars_setting} ${st.bars_setting === 1 ? "bar" : "bars"}` : "Auto", tempo && st.bars_setting > 0, !tempo, tempo && st.bars_setting > 0);
    chipSet("count_in", st.count_in ? "1 bar" : "Off", st.count_in && tempo, !tempo, st.count_in && tempo);
    chipSet("click", st.click ? "On" : "Off", st.click && tempo, !tempo, st.click && tempo);
    chipSet("stop_mode", st.stop_mode === "fade" ? `Fade ${Math.round(st.fade_s)}s` : STOP_LBL[st.stop_mode], st.stop_mode !== "instant", false, st.stop_mode !== "instant");
    chipSet("after_rec", st.after_rec === "dub" ? "Overdub" : "Play", st.after_rec === "dub", false, st.after_rec === "dub");
    // master
    const anyRun = st.tracks.some((t) => t.state === "playing" || t.state === "overdubbing" || t.state === "recording" || t.state === "armed");
    const anyContent = st.tracks.some((t) => t.length_s > 0);
    setText($("lx-all-lbl"), anyRun ? "STOP" : anyContent ? "START" : "STOP");
    const ring = box.querySelector(".lx-ring.all");
    ring.dataset.state = anyRun ? "playing" : anyContent ? "stopped" : "empty";
    box.querySelector(".lx-master").classList.toggle("idle", !anyRun && !anyContent);
    const u = box.querySelector('[data-act="undo"]'), r = box.querySelector('[data-act="redo"]');
    u.disabled = !cur.can_undo && cur.state !== "recording";
    r.disabled = !cur.can_redo;
    setHTML(u, `UNDO${cur.layers > 1 ? `<small>${cur.layers - 1}</small>` : ""}`);
    setHTML(r, `REDO${cur.redo ? `<small>${cur.redo}</small>` : ""}`);
    $("lx-export").disabled = !anyContent;
    if (!S.dragging) box.querySelectorAll(".lx-knob").forEach((k) => paintKnob(k, clamp(KN[k.dataset.k].get(k.dataset.t != null ? +k.dataset.t : -1), 0, 1)));
    paintLed();
  }
  function paintLed() {
    const l = $("vt-looper-led");
    if (!l) return;
    const st = S.st;
    let cls = "";
    if (st && st.tracks) {
      const ss = st.tracks.map((t) => t.state);
      cls = ss.includes("recording") || ss.includes("armed") ? "rec" : ss.includes("overdubbing") ? "dub" : ss.includes("playing") ? "play" : "";
    }
    l.className = `vt-led ${cls}`;
  }

  /* =====================================================================
     DRAW (canvas lanes + rings, every frame while visible)
     ===================================================================== */
  function mix(hex, to, f) {   // blend a #rrggbb toward another
    const a = parseInt(hex.slice(1), 16), b = parseInt(to.slice(1), 16);
    const c = (s) => Math.round(((a >> s) & 255) * (1 - f) + ((b >> s) & 255) * f);
    return `rgb(${c(16)},${c(8)},${c(0)})`;
  }
  function draw() {
    const st = S.st;
    if (!st || !st.tracks) return;
    const dpr = window.devicePixelRatio || 1;
    const meter = $("lx-inmeter");
    if (meter) { const db = 20 * Math.log10(Math.max(1e-5, st.peak || 0)); const w = clamp((db + 54) / 54, 0, 1); meter.style.width = `${(w * 100).toFixed(1)}%`; meter.classList.toggle("hot", (st.peak || 0) > 0.9); }
    const box = $("view-looper");
    // live counter (smooth between polls)
    const cur = st.tracks[sel()];
    const [cm, cs] = counterFor(st, cur);
    setText($("lx-cmain"), cm);
    setText($("lx-csub"), cs);
    const maxLen = Math.max(0, ...st.tracks.map((t) => (t.state !== "recording" ? t.length_s / (t.speed || 1) : 0)));
    st.tracks.forEach((tr, t) => {
      const lane = box.querySelector(`.lx-lane[data-t="${t}"]`);
      const cv = lane.querySelector("canvas");
      const W = Math.round(cv.clientWidth * dpr), H = Math.round(cv.clientHeight * dpr);
      if (!W || !H) return;
      if (cv.width !== W || cv.height !== H) { cv.width = W; cv.height = H; }
      const x = cv.getContext("2d");
      x.clearRect(0, 0, W, H);
      const mid = H / 2;
      const w = S.waves[t].data;
      const hue = HUES[t];
      // span: a recording grows into its target (close point, bar or a provisional window)
      let span = tr.length_s, filled = 1;
      if (tr.state === "recording") {
        const bar = beatS(st) * st.beats;
        span = tr.close_s > 0 ? tr.close_s : st.sync === "tempo" ? Math.max(1, Math.ceil(tr.rec_s / bar + 0.0001)) * bar : Math.max(tr.rec_s * 1.15, 2, st.sync === "loop" && maxLen ? maxLen : 0);
        filled = span > 0 ? clamp(tr.rec_s / span, 0, 1) : 0;
      }
      // grid: beats / bars in Tempo, master cycles in Loop sync
      x.lineWidth = Math.max(1, dpr * 0.75);
      if (span > 0) {
        let step = 0, strong = 0;
        if (st.sync === "tempo") { step = beatS(st); strong = st.beats; }
        else if (st.sync === "loop" && st.master >= 0 && st.tracks[st.master].length_s > 0 && t !== st.master) { step = st.tracks[st.master].length_s; strong = 1; }
        if (step > 0) {
          const n = Math.round(span / step);
          if (n > 1 && n <= 256) {
            for (let i = 1; i < n; i++) {
              const gx = Math.round(W * i / n) + 0.5;
              const isBar = strong <= 1 || i % strong === 0;
              if (!isBar && W / n < 5 * dpr) continue;
              x.strokeStyle = isBar ? "rgba(255,255,255,0.13)" : "rgba(255,255,255,0.05)";
              x.beginPath(); x.moveTo(gx, isBar ? 2 * dpr : H * 0.22); x.lineTo(gx, isBar ? H - 2 * dpr : H * 0.78); x.stroke();
            }
          }
        }
      }
      x.strokeStyle = "rgba(255,255,255,0.06)";
      x.beginPath(); x.moveTo(0, mid + 0.5); x.lineTo(W, mid + 0.5); x.stroke();
      // layers: stacked from the oldest (core) out, each a shade of the track colour; undone layers as a ghost
      if (w && w.layers && w.layers.length && (tr.length_s > 0 || tr.state === "recording")) {
        const bins = w.bins, L = w.layers, gains = tr.gains && tr.gains.length === L.length ? tr.gains : L.map((l) => l.gain);
        const audible = L.map((l, k) => k < tr.layers || (tr.state === "recording" && k === 0));
        const drawW = tr.state === "recording" ? W * filled : W;
        const bw = drawW / bins;
        const amp = (v) => Math.pow(clamp(v, 0, 1.5) / 1.5, 0.62) * (H * 0.47);
        const n = L.length;
        const tops = new Float32Array(bins);
        for (let k = 0; k < n; k++) {
          if (!audible[k]) continue;
          const newest = k === tr.layers - 1;
          const shade = tr.layers > 1 ? k / (tr.layers - 1) : 1;   // 0 = first recording … 1 = newest
          let col = newest && k > 0 ? mix(hue, "#ffffff", 0.3) : mix(hue, "#0b0d10", 0.62 - 0.5 * shade);
          if (tr.state === "recording") col = mix(STATE_COL.recording, "#0b0d10", 0.1);
          else if (tr.state === "overdubbing" && newest && k > 0) col = mix(STATE_COL.overdubbing, "#ffffff", 0.55);
          x.fillStyle = col;
          x.globalAlpha = tr.mute ? 0.35 : tr.state === "stopped" ? 0.55 : 1;
          for (let i = 0; i < bins; i++) {
            const v = L[k].peaks[i] * gains[k];
            const prev = tops[i];
            const sum = prev + v;
            tops[i] = sum;
            const a0 = amp(prev) + (k > 0 && prev > 0.004 ? 0.75 * dpr : 0), a1 = amp(sum);   // a hairline between layers
            if (a1 - a0 < 0.35 * dpr) continue;
            const bx = i * bw, ww = Math.max(dpr, bw - Math.min(dpr, bw * 0.3));
            x.fillRect(bx, mid - a1, ww, a1 - a0);
            x.fillRect(bx, mid + a0, ww, a1 - a0);
          }
        }
        x.globalAlpha = 1;
        // ghosts of undone layers (redo)
        const ghost = L.map((l, k) => k).filter((k) => !audible[k]);
        if (ghost.length && tr.state !== "recording") {
          x.strokeStyle = "rgba(255,255,255,0.32)";
          x.setLineDash([2 * dpr, 2 * dpr]);
          x.beginPath();
          const sum2 = Float32Array.from(tops);
          ghost.forEach((k) => { for (let i = 0; i < bins; i++) sum2[i] += L[k].peaks[i] * (L[k].gain || 1); });
          for (let i = 0; i < bins; i++) { const y = mid - amp(sum2[i]); if (i) x.lineTo(i * bw + bw / 2, y); else x.moveTo(bw / 2, y); }
          x.stroke();
          x.setLineDash([]);
        }
      }
      // playhead / write head
      if (tr.state === "recording") {
        const hx = W * filled;
        x.fillStyle = STATE_COL.recording;
        x.shadowColor = STATE_COL.recording; x.shadowBlur = 8 * dpr;
        x.fillRect(Math.round(hx - dpr), 0, 2 * dpr, H);
        x.shadowBlur = 0;
      } else if (tr.length_s > 0 && tr.state !== "empty") {
        const p = tr.state === "stopped" ? 0 : livePos(tr);
        const hx = W * p;
        const col = tr.state === "overdubbing" ? STATE_COL.overdubbing : "#ffffff";
        x.globalAlpha = tr.state === "stopped" ? 0.35 : 1;
        x.fillStyle = col;
        x.shadowColor = tr.state === "overdubbing" ? col : hue; x.shadowBlur = 9 * dpr;
        x.fillRect(Math.round(hx - dpr), 0, 2 * dpr, H);
        x.shadowBlur = 0;
        x.globalAlpha = 1;
      }
      // footswitch ring: loop position
      const arc = box.querySelector(`.lx-strip[data-t="${t}"] .lx-ring .arc`);
      if (arc) {
        let p = 0;
        if (tr.state === "recording") p = 1;
        else if (tr.state === "playing" || tr.state === "overdubbing") p = livePos(tr);
        else if (tr.state === "armed") p = 1;
        const v = `${(p * 100).toFixed(2)} 100`;
        if (arc._v !== v) { arc._v = v; arc.setAttribute("stroke-dasharray", v); }
      }
    });
  }

  /* =====================================================================
     KEYS — the footswitches
     ===================================================================== */
  function onKey(e) {
    if (!S.visible || e.metaKey || e.ctrlKey || e.altKey) return;
    if ((typeof ui !== "undefined" && ui.modalOpen) || typingIn(document.activeElement)) return;
    const menu = $("menu");
    if (menu && !menu.hidden) return;
    if (document.activeElement && document.activeElement.classList.contains("lx-knob") && /^Arrow/.test(e.key)) return;
    const st = S.st;
    if (!st || !st.tracks) return;
    const k = e.key.toLowerCase(), s = sel();
    let done = true;
    if (e.repeat && k !== "arrowup" && k !== "arrowdown") return;
    const digit = /^Digit([1-3])$/.exec(e.code);
    if (digit) { const t = +digit[1] - 1; if (e.shiftKey) stopTrack(t); else press(t); }
    else if (k === "r") press(s);
    else if (k === "s") cmd(e.shiftKey ? "action=stop&mode=instant" : "action=all");
    else if (k === "u") cmd(`action=${e.shiftKey ? "redo" : "undo"}&track=${s}`);
    else if (e.key === "Backspace" || e.key === "Delete") clearTrack(e.shiftKey ? "all" : s);
    else if (k === "m") setTrack(s, "mute", st.tracks[s].mute ? 0 : 1);
    else if (k === "v") setTrack(s, "reverse", st.tracks[s].reverse ? 0 : 1);
    else if (e.key === "-" || e.key === "_") setTrack(s, "speed", st.tracks[s].speed > 1 ? 1 : 0.5);
    else if (e.key === "=" || e.key === "+") setTrack(s, "speed", st.tracks[s].speed < 1 ? 1 : 2);
    else if (k === "k") { if (st.sync === "tempo") cmd(`click=${st.click ? 0 : 1}`); else say("The click needs Tempo sync."); }
    else if (k === "arrowup") cmd(`select=${(s + NT - 1) % NT}`);
    else if (k === "arrowdown") cmd(`select=${(s + 1) % NT}`);
    else done = false;
    if (done) { e.preventDefault(); e.stopPropagation(); }
  }

  /* =====================================================================
     VIEW LIFECYCLE
     ===================================================================== */
  function enter() {
    S.visible = true;
    if (!S.built) build();
    paint();
    poll();
    cancelAnimationFrame(S.raf); S.raf = 0;
    drawSoon();
  }
  // Canvas lanes: every frame while a track records/plays (moving playheads), otherwise once per poll
  // (drawSoon from poll()); never while hidden or asleep.
  function loop() {
    S.raf = 0;
    if (!S.visible || (window.HRActivity && !HRActivity.awake())) return;
    try { draw(); } catch (err) { console.error(err); }
    if (busyState(S.st)) S.raf = requestAnimationFrame(loop);
  }
  function drawSoon() { if (!S.raf && S.visible) S.raf = requestAnimationFrame(loop); }
  function leave() {
    S.visible = false;
    cancelAnimationFrame(S.raf); S.raf = 0;
    clearTimeout(S.pollT);
    S.pollT = S.telLed ? 0 : setTimeout(poll, 1000);
  }
  document.addEventListener("keydown", onKey);
  function reg() {
    if (!(window.HRViews && HRViews.register)) return false;
    HRViews.register("looper", { enter, leave, onTelemetry });
    return true;
  }
  function init() {
    if (!reg()) { setTimeout(init, 50); return; }
    if (!S.visible) poll();   // background poll keeps the tab LED in step with the engine
  }
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", init); else init();
  window.HRLooper = { state: () => S.st };
})();
