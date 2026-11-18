/* Headroom — Live view: the classic full-screen performance / stage view  (assets/live-view.js → #view-live)
 *
 *   Built to be read from 2–3 m away and played mid-song with a mouse, keyboard or a keyboard-style footswitch:
 *     - Rig: the current rig name, huge, with big Prev / Next pads and the neighbours' names. Steps through the
 *       current list (exactly what [ / ] step: your Rigs while one is loaded, otherwise the Library as filtered in
 *       the sidebar) or through a setlist kept here (localStorage "hr.live.setlist"). "/" opens a quick jump.
 *     - Scenes A–D: GET /api/scenes?action=select&idx=N (rack JSON). Right-click a pad to store the current sound
 *       in it (action=save). Labels and the active scene come from rack JSON `scenes`.
 *     - Macros 1–8: the same send path as the footer macro bar (queueSend "macro:<col>" → /api/macro?col=&norm=).
 *     - Tuner: the engine has no tuner route, so this view runs a YIN pitch detector on the engine's own input
 *       stream — the pre-rig input samples index.html already pulls from /api/scope into its `scope` ring
 *       (12 kHz, peak-decimated). Nothing is synthesised: no signal, no reading. Mute-while-tuning switches the
 *       output off with /api/audio/set?monitor=0 and restores it when the tuner closes.
 *     - Meters: telemetry in_rms / out_rms_l / out_rms_r; IN clip = telemetry input_clip, OUT peak = output samples
 *       reaching the limiter ceiling (−1 dBFS) in the scope stream.
 *     - Tempo: HRTempo (assets/cabinet.js) — tap and nudge go through the same path as the top-bar control.
 *     - Looper: GET /api/looper (the selected track's status) and action=toggle (its Rec/Play/Dub) | stop (all).
 *     - Bypass = the top bar's Rig/Dry switch (setDIMode → /api/transport?action=bypass). Mute = monitor off.
 *   Keys while Live is open (never while typing): ← → ↑ ↓ PageUp PageDown Space [ ] step rigs, 1–4 scenes,
 *   T tap, U tuner, B bypass, M mute, R / S looper, / jump, F full width, Esc closes an overlay or returns to the
 *   Signal Chain.
 *   No master output level exists in the engine (only input gain), so there's no master volume here.
 *
 * Registered with HRViews (assets/studio-views.js); telemetry arrives via HRViews → handlers.onTelemetry.
 * Uses index.html globals at call time only: rackState, lib, ui, scope, audioCfg, rigContext, rigSorted,
 * filteredPresets, isUserPreset, isDirty, selectPresetByIndex, queueSend, applyRackState, rackCall, setDIMode,
 * setAudio, fetchJSON, showMenu, promptDialog, toast, esc.
 */
(function () {
  "use strict";
  const $ = (id) => document.getElementById(id);
  const clamp = (v, a, b) => Math.max(a, Math.min(b, v));
  const now = () => performance.now();
  const typingIn = (a) => a && (a.tagName === "INPUT" && a.type !== "range" || a.tagName === "SELECT" || a.tagName === "TEXTAREA" || a.isContentEditable);
  const escH = (s) => (typeof esc === "function" ? esc(s) : String(s ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c])));
  const say = (m) => { if (typeof toast === "function") toast(m); };
  const setText = (el, v) => { if (el && el._v !== v) { el._v = v; el.textContent = v; } };
  const setHTML = (el, v) => { if (el && el._h !== v) { el._h = v; el.innerHTML = v; } };
  const RS = () => (typeof rackState !== "undefined" ? rackState : null);
  const LIB = () => (typeof lib !== "undefined" ? lib : { presets: [], byIdx: new Map() });

  const KEY = { setlist: "hr.live.setlist", mode: "hr.live.mode", full: "hr.live.full", lights: "hr.live.lights", tmute: "hr.live.tunerMute" };
  const load = (k, d) => { try { const v = localStorage.getItem(k); return v == null ? d : JSON.parse(v); } catch (e) { return d; } };
  const save = (k, v) => { try { localStorage.setItem(k, JSON.stringify(v)); } catch (e) { /* private mode */ } };

  const NOTE = ["C", "C♯", "D", "D♯", "E", "F", "F♯", "G", "G♯", "A", "A♯", "B"];
  const SCENE_L = ["A", "B", "C", "D"];

  const L = {
    built: false, visible: false, raf: 0,
    rs: null, libSig: "", listKey: "", nameKey: "", target: null, targetT: 0, dir: 0,
    mode: load(KEY.mode, "list") === "setlist" ? "setlist" : "list",
    setlist: (Array.isArray(load(KEY.setlist, [])) ? load(KEY.setlist, []) : []).filter((e) => e && typeof e.name === "string"),
    full: load(KEY.full, true) !== false,
    lights: load(KEY.lights, "dim") === "theme" ? "theme" : "dim",
    tunerMute: load(KEY.tmute, true) !== false,
    tel: null, sceneOpt: null, sceneOptT: 0,
    drag: null,
    looper: null, loopSupported: true, loopT: 0, loopAt: 0,
    monitor: null, audioOk: true, audioT: 0,
    meters: { in: 0, l: 0, r: 0, pin: 0, pl: 0, pr: 0, inClip: 0, outClip: 0, lastW: -1 },
    tuner: { open: false, mutedByTuner: false, lastRun: 0, lastW: -1, freshT: 0, hz: 0, midi: 0, cents: 0, shown: 0, est: [], sigT: 0, note: -1 },
    search: { open: false, sel: 0, results: [] },
    sl: { open: false },
    bpmShown: null,
  };

  /* =====================================================================
     RIG LIST — the current list (as [ / ] step it) or the setlist
     ===================================================================== */
  function libName(p) { return p ? (p.name || p.preset_name || "") : ""; }
  function currentList() {
    if (L.mode === "setlist") {
      return { kind: "setlist", label: "Setlist", items: resolveSetlist().filter((e) => e.idx != null) };
    }
    let items = [];
    let label = "Library";
    try {
      const c = rigContext();
      const rigs = c.isUser || (c.isNew && ui.leftTab === "rigs");
      items = (rigs ? rigSorted() : filteredPresets()).map((p) => ({ idx: p.idx, name: libName(p), category: p.category || "", user: isUserPreset(p) }));
      if (rigs) label = "Your rigs";
      else {
        label = ui.tag === "all" ? "Library" : ui.tag === "Featured" ? "Featured" : `Library · ${ui.tag}`;
        if (ui.search && ui.search.trim()) label += ` · “${ui.search.trim()}”`;
      }
    } catch (e) {
      const rs = RS();
      items = ((rs && rs.presets) || []).map((p) => ({ idx: p.idx, name: p.preset_name, category: p.category || "", user: false }));
    }
    return { kind: "list", label, items };
  }
  function resolveSetlist() {
    const lb = LIB();
    return L.setlist.map((e) => {
      let p = lb.byIdx && lb.byIdx.get(e.idx);
      if (!p || libName(p) !== e.name) p = (lb.presets || []).find((q) => libName(q) === e.name && !!e.user === (typeof isUserPreset === "function" && isUserPreset(q))) || (lb.presets || []).find((q) => libName(q) === e.name) || null;
      return { name: e.name, user: !!e.user, idx: p ? p.idx : null, category: p ? (p.category || "") : "" };
    });
  }
  function curIdx() {
    if (L.target != null && now() - L.targetT < 5000) return L.target;
    const rs = RS();
    try { const c = rigContext(); if (c.cur) return c.cur.idx; } catch (e) { /* fall through */ }
    return rs ? rs.preset_idx : -1;
  }
  function step(dir) {
    const { items, kind } = currentList();
    if (!items.length) { say(kind === "setlist" ? "The setlist is empty — add rigs from Jump ( / ) or Setlist." : "No rigs in this list."); return; }
    const cur = curIdx();
    let pos = items.findIndex((p) => p.idx === cur);
    pos = pos < 0 ? (dir > 0 ? 0 : items.length - 1) : (pos + dir + items.length) % items.length;
    flash(dir > 0 ? "lv-next" : "lv-prev");
    loadRig(items[pos].idx, dir);
  }
  function loadRig(idx, dir = 0) {
    if (idx == null || typeof selectPresetByIndex !== "function") return;
    L.target = idx; L.targetT = now(); L.dir = dir;
    selectPresetByIndex(idx);
    renderRig(true);
  }
  function flash(id) { const b = $(id); if (!b) return; b.classList.add("hit"); setTimeout(() => b.classList.remove("hit"), 140); }

  /* =====================================================================
     BUILD
     ===================================================================== */
  const ICON = {
    prev: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M15 4.5 7.5 12l7.5 7.5"/></svg>`,
    next: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M9 4.5 16.5 12 9 19.5"/></svg>`,
    search: `<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"><circle cx="7" cy="7" r="4.6"/><path d="m10.5 10.5 3 3"/></svg>`,
    list: `<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"><path d="M5.5 4h8M5.5 8h8M5.5 12h8"/><circle cx="2.6" cy="4" r=".9" fill="currentColor" stroke="none"/><circle cx="2.6" cy="8" r=".9" fill="currentColor" stroke="none"/><circle cx="2.6" cy="12" r=".9" fill="currentColor" stroke="none"/></svg>`,
    bulb: `<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"><path d="M5.6 10.4C4.4 9.5 3.6 8.2 3.6 6.6a4.4 4.4 0 0 1 8.8 0c0 1.6-.8 2.9-2 3.8v1.4H5.6z"/><path d="M6.2 14h3.6"/></svg>`,
    expand: `<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"><path d="M2.5 6V2.5H6M10 2.5h3.5V6M13.5 10v3.5H10M6 13.5H2.5V10"/></svg>`,
    shrink: `<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"><path d="M6 2.5V6H2.5M13.5 6H10V2.5M10 13.5V10h3.5M2.5 10H6v3.5"/></svg>`,
    close: `<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round"><path d="m4 4 8 8M12 4l-8 8"/></svg>`,
    up: `<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"><path d="M4 10l4-4 4 4"/></svg>`,
    down: `<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"><path d="M4 6l4 4 4-4"/></svg>`,
    fork: `<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"><path d="M6 2v6.5a2 2 0 0 0 4 0V2M8 10.5V14"/></svg>`,
  };

  function build() {
    const box = $("view-live");
    if (!box) return false;
    box.innerHTML = `<div class="lv" id="lv" data-lights="${L.lights}">
      <div class="lv-bar">
        <div class="lv-bar-l">
          <span class="lv-eyebrow"><i></i>Live</span>
          <div class="lv-seg" role="group" aria-label="Step through">
            <button data-mode="list" title="Step through the current list — what [ and ] step">List</button><button data-mode="setlist" title="Step through your setlist">Setlist<b id="lv-sl-n"></b></button>
          </div>
          <span class="lv-listlbl" id="lv-listlbl"></span>
        </div>
        <div class="lv-bar-r">
          <button class="lv-ib" data-act="search" title="Jump to any rig ( / )">${ICON.search}<span>Jump</span><kbd>/</kbd></button>
          <button class="lv-ib" data-act="setlist" title="Build and reorder the setlist">${ICON.list}<span>Setlist</span></button>
          <button class="lv-ib" data-act="lights" id="lv-lights" title="Stage lights: dim stage, or follow the app theme">${ICON.bulb}<span id="lv-lights-lbl"></span></button>
          <button class="lv-ib" data-act="full" id="lv-full" title="Hide or show the side panels ( F )"></button>
        </div>
      </div>

      <section class="lv-hero">
        <button class="lv-step prev" id="lv-prev" data-act="prev" aria-label="Previous rig">${ICON.prev}<span class="k">Prev</span><span class="nb" id="lv-prev-name"></span></button>
        <div class="lv-now">
          <div class="lv-meta"><span class="lv-cat" id="lv-cat"></span><span class="lv-pos num" id="lv-pos"></span></div>
          <div class="lv-name-wrap"><h1 class="lv-name" id="lv-name">—</h1></div>
          <div class="lv-sub"><span class="lv-src" id="lv-src"></span><span class="lv-edited" id="lv-edited">Edited</span><span class="lv-load" id="lv-load"><i></i>Loading…</span><span id="lv-author"></span></div>
        </div>
        <button class="lv-step next" id="lv-next" data-act="next" aria-label="Next rig">${ICON.next}<span class="k">Next</span><span class="nb" id="lv-next-name"></span></button>
      </section>

      <section class="lv-scenes" id="lv-scenes" aria-label="Scenes"></section>

      <section class="lv-macros" id="lv-macros" aria-label="Macros"></section>

      <section class="lv-dock">
        <div class="lv-tile lv-tuner" id="lv-tuner" data-act="tuner" data-st="none" role="button" tabindex="0" aria-label="Open the tuner">
          <div class="lv-tile-h"><span class="lv-eyebrow">Tuner</span><button class="lv-btn" data-act="tuner" tabindex="-1">Tune <kbd>U</kbd></button></div>
          <div class="lv-tn-row">
            <div class="lv-tn-note num" id="lv-tn-note">–</div>
            <div class="lv-tn-meter"><div class="lv-tn-strip"><i id="lv-tn-dot"></i></div><div class="lv-tn-read"><span id="lv-tn-cents">no signal</span><span id="lv-tn-hz"></span></div></div>
          </div>
        </div>
        <div class="lv-tile lv-tempo" id="lv-tempo">
          <div class="lv-tile-h"><span class="lv-eyebrow">Tempo</span><small id="lv-tempo-src"></small></div>
          <div class="lv-bpm-row"><span class="lv-bpm num" id="lv-bpm">120.0<small>BPM</small></span><i class="lv-beat" id="lv-beat"></i></div>
          <div class="lv-tempo-ctl"><button class="lv-btn" data-act="bpm-" aria-label="Slower">−</button><button class="lv-btn lv-tap" id="lv-tap" data-act="tap">TAP <kbd>T</kbd></button><button class="lv-btn" data-act="bpm+" aria-label="Faster">+</button></div>
        </div>
        <div class="lv-tile lv-loop" id="lv-loop" data-st="empty">
          <div class="lv-tile-h"><span class="lv-eyebrow">Looper</span><small class="num" id="lv-loop-t"></small></div>
          <div class="lv-loop-st" id="lv-loop-st"><i></i><span>EMPTY</span></div>
          <div class="lv-loop-bar"><i id="lv-loop-pos"></i></div>
          <div class="lv-loop-ctl"><button class="lv-btn lv-loop-main" id="lv-loop-main" data-act="loop">REC <kbd>R</kbd></button><button class="lv-btn" data-act="loop-stop">STOP <kbd>S</kbd></button></div>
        </div>
        <div class="lv-tile lv-meters">
          <div class="lv-tile-h"><span class="lv-eyebrow">Levels</span><small id="lv-src-in"></small></div>
          <div class="lv-meters-body">
            <div class="lv-mg"><div class="lab"><i class="lv-clip" id="lv-clip-in" title="Input clip"></i>IN</div><div class="bars"><div class="lv-vm"><div class="f" id="lv-m-in"></div><div class="p" id="lv-p-in"></div></div></div><span class="db num" id="lv-db-in">−∞</span></div>
            <div class="lv-mg"><div class="lab"><i class="lv-clip" id="lv-clip-out" title="Output at the limiter ceiling (−1 dBFS)"></i>OUT</div><div class="bars"><div class="lv-vm"><div class="f" id="lv-m-l"></div><div class="p" id="lv-p-l"></div></div><div class="lv-vm"><div class="f" id="lv-m-r"></div><div class="p" id="lv-p-r"></div></div></div><span class="db num" id="lv-db-out">−∞</span></div>
          </div>
        </div>
        <div class="lv-tile lv-kill">
          <button class="lv-btn lv-big lv-byp" id="lv-byp" data-act="bypass" title="Bypass the rig — hear the dry input ( B )">BYPASS<small>rig off · dry input</small></button>
          <button class="lv-btn lv-big lv-mute" id="lv-mute" data-act="mute" title="Mute the output ( M )">MUTE<small>output on</small></button>
        </div>
      </section>

      <div class="lv-hint"><span><kbd>←</kbd><kbd>→</kbd><kbd>Space</kbd>rig</span><span><kbd>1</kbd>–<kbd>4</kbd>scene</span><span><kbd>T</kbd>tap</span><span><kbd>U</kbd>tuner</span><span><kbd>B</kbd>bypass</span><span><kbd>M</kbd>mute</span><span><kbd>R</kbd><kbd>S</kbd>looper</span><span><kbd>/</kbd>jump</span><span><kbd>Esc</kbd>back</span></div>

      <div class="lv-ov lv-tov" id="lv-tov" data-st="none" hidden role="dialog" aria-label="Tuner">
        <button class="lv-ib lv-ov-close" data-act="tuner-close" title="Close the tuner (Esc / U)">${ICON.close}<span>Close</span></button>
        <div class="tv-muted">Output muted while tuning</div>
        <div class="tv-note num" id="lv-tv-note">–</div>
        <div class="tv-dir"><span id="lv-tv-flat">♭ Flat</span><span id="lv-tv-sharp">Sharp ♯</span></div>
        <div class="tv-dial"><svg viewBox="0 0 640 300" id="lv-tv-dial"></svg></div>
        <div class="tv-read"><span><b class="num" id="lv-tv-cents">–</b> cents</span><span><b class="num" id="lv-tv-hz">–</b> Hz</span></div>
        <div class="tv-foot"><button class="lv-switch" id="lv-tv-mute" data-act="tuner-mute"><i></i>Mute output while tuning</button></div>
        <div class="tv-src" id="lv-tv-src"></div>
      </div>

      <div class="lv-ov" id="lv-sov" hidden role="dialog" aria-label="Jump to a rig">
        <div class="lv-panel">
          <div class="lv-panel-h"><label class="lv-search">${ICON.search}<input id="lv-q" type="text" placeholder="Jump to a rig…" autocomplete="off" spellcheck="false"></label><button class="lv-ib" data-act="search-close" title="Close (Esc)">${ICON.close}</button></div>
          <div class="lv-list" id="lv-results"></div>
          <div class="lv-panel-f"><span><kbd>↑</kbd><kbd>↓</kbd>choose <kbd>↵</kbd>load <kbd>⌘↵</kbd>add to setlist</span><span id="lv-q-n"></span></div>
        </div>
      </div>

      <div class="lv-ov" id="lv-lov" hidden role="dialog" aria-label="Setlist">
        <div class="lv-panel">
          <div class="lv-panel-h"><h3>Setlist</h3><button class="lv-btn" data-act="sl-add">+ Add current rig</button><button class="lv-ib" data-act="sl-close" title="Close (Esc)">${ICON.close}</button></div>
          <div class="lv-list" id="lv-sl-list"></div>
          <div class="lv-panel-f"><span>Click a song to load it. Saved on this Mac.</span><button class="lv-btn" data-act="sl-clear">Clear setlist</button></div>
        </div>
      </div>
    </div>`;
    buildDial();
    wire(box);
    L.built = true;
    return true;
  }

  function buildDial() {
    const svg = $("lv-tv-dial");
    if (!svg) return;
    const cx = 320, cy = 290, r = 250, span = 62;   // ±50 cents across ±62°
    const pt = (deg, rr) => { const a = (deg - 90) * Math.PI / 180; return [cx + rr * Math.cos(a), cy + rr * Math.sin(a)]; };
    let s = "";
    const z0 = pt(-span * 3 / 50, r + 18), z1 = pt(span * 3 / 50, r + 18), z2 = pt(span * 3 / 50, r - 34), z3 = pt(-span * 3 / 50, r - 34);
    s += `<path class="tv-zone" d="M${z0} A${r + 18} ${r + 18} 0 0 1 ${z1} L${z2} A${r - 34} ${r - 34} 0 0 0 ${z3}Z"/>`;
    for (let c = -50; c <= 50; c += 5) {
      const deg = c / 50 * span, mj = c % 25 === 0, len = c === 0 ? 46 : mj ? 34 : c % 10 === 0 ? 22 : 14;
      const [x0, y0] = pt(deg, r), [x1, y1] = pt(deg, r - len);
      s += `<line class="tv-tick${c === 0 ? " z" : mj ? " mj" : ""}" x1="${x0.toFixed(1)}" y1="${y0.toFixed(1)}" x2="${x1.toFixed(1)}" y2="${y1.toFixed(1)}"/>`;
      if (mj) { const [lx, ly] = pt(deg, r + 24); s += `<text class="tv-lbl" x="${lx.toFixed(1)}" y="${(ly + 5).toFixed(1)}">${c > 0 ? "+" : c < 0 ? "−" : ""}${Math.abs(c)}</text>`; }
    }
    s += `<g id="lv-tv-needle" style="transform-origin:${cx}px ${cy}px"><line class="tv-needle" x1="${cx}" y1="${cy}" x2="${cx}" y2="${cy - r + 6}"/></g><circle class="tv-hub" cx="${cx}" cy="${cy}" r="13"/>`;
    svg.innerHTML = s;
  }

  /* =====================================================================
     WIRING — pointer, context menu, keys
     ===================================================================== */
  function wire(box) {
    box.addEventListener("click", (e) => {
      const m = e.target.closest("[data-mode]");
      if (m) { setMode(m.dataset.mode); return; }
      const sc = e.target.closest("[data-scene]");
      if (sc) { selectScene(+sc.dataset.scene); return; }
      const r = e.target.closest("[data-load]");
      if (r && !e.target.closest("[data-sl]") && !e.target.closest("[data-slmv]") && !e.target.closest("[data-slrm]")) {
        const idx = r.dataset.load === "" ? null : +r.dataset.load;
        if (idx == null) { say("That rig isn’t in your library any more."); return; }
        closeSearch(); closeSetlist(); loadRig(idx, 0); return;
      }
      const slb = e.target.closest("[data-sl]");
      if (slb) { toggleSetlistEntry(+slb.dataset.sl); renderResults(); return; }
      const mv = e.target.closest("[data-slmv]");
      if (mv) { moveSetlist(+mv.dataset.slmv, +mv.dataset.d); return; }
      const rm = e.target.closest("[data-slrm]");
      if (rm) { L.setlist.splice(+rm.dataset.slrm, 1); saveSetlist(); renderSetlist(); return; }
      const a = e.target.closest("[data-act]");
      if (!a) return;
      act(a.dataset.act, a);
    });
    box.addEventListener("contextmenu", (e) => {
      const sc = e.target.closest("[data-scene]");
      if (!sc) return;
      e.preventDefault();
      sceneMenu(e.clientX, e.clientY, +sc.dataset.scene);
    });
    // Overlays close when you click the backdrop
    ["lv-tov", "lv-sov", "lv-lov"].forEach((id) => $(id).addEventListener("pointerdown", (e) => {
      if (e.target.id !== id) return;
      if (id === "lv-tov") closeTuner(); else if (id === "lv-sov") closeSearch(); else closeSetlist();
    }));
    // Macros: vertical drag (Shift = fine), wheel, arrow keys when focused, double-click = 50 %
    const mac = $("lv-macros");
    mac.addEventListener("pointerdown", (e) => {
      const t = e.target.closest(".lv-mac[data-col]");
      if (!t || e.button !== 0) return;
      e.preventDefault();
      t.focus({ preventScroll: true });
      const col = +t.dataset.col, m = macroAt(col);
      if (!m) return;
      t.setPointerCapture(e.pointerId);
      t.classList.add("drag");
      L.drag = { col, el: t, y: e.clientY, x: e.clientX, v: m.val };
    });
    mac.addEventListener("pointermove", (e) => {
      const d = L.drag;
      if (!d) return;
      const k = e.shiftKey ? 0.0012 : 0.0055;
      const dv = ((d.y - e.clientY) + (e.clientX - d.x) * 0.5) * k;
      d.y = e.clientY; d.x = e.clientX;
      setMacro(d.col, d.v + dv);
    });
    const end = () => { if (L.drag) { L.drag.el.classList.remove("drag"); L.drag = null; } };
    mac.addEventListener("pointerup", end);
    mac.addEventListener("pointercancel", end);
    mac.addEventListener("wheel", (e) => {
      const t = e.target.closest(".lv-mac[data-col]");
      if (!t) return;
      e.preventDefault();
      const m = macroAt(+t.dataset.col);
      if (m) setMacro(+t.dataset.col, m.val + (e.deltaY < 0 ? 1 : -1) * (e.shiftKey ? 0.005 : 0.02));
    }, { passive: false });
    mac.addEventListener("dblclick", (e) => { const t = e.target.closest(".lv-mac[data-col]"); if (t) setMacro(+t.dataset.col, 0.5); });
    mac.addEventListener("keydown", (e) => {
      const t = e.target.closest(".lv-mac[data-col]");
      if (!t || e.metaKey || e.ctrlKey || e.altKey) return;
      const m = macroAt(+t.dataset.col);
      if (!m) return;
      const st = e.shiftKey ? 0.005 : 0.02;
      let v = null;
      if (e.key === "ArrowUp" || e.key === "ArrowRight") v = m.val + st;
      else if (e.key === "ArrowDown" || e.key === "ArrowLeft") v = m.val - st;
      else if (e.key === "Home") v = 0;
      else if (e.key === "End") v = 1;
      if (v == null) return;
      e.preventDefault(); e.stopPropagation();
      setMacro(+t.dataset.col, v);
    });
    $("lv-tuner").addEventListener("keydown", (e) => { if (e.key === "Enter") { e.preventDefault(); openTuner(); } });
    // Jump field
    const q = $("lv-q");
    q.addEventListener("input", () => { L.search.sel = 0; renderResults(); });
    q.addEventListener("keydown", (e) => {
      const n = L.search.results.length;
      if (e.key === "ArrowDown") { e.preventDefault(); L.search.sel = n ? (L.search.sel + 1) % n : 0; renderResults(true); }
      else if (e.key === "ArrowUp") { e.preventDefault(); L.search.sel = n ? (L.search.sel - 1 + n) % n : 0; renderResults(true); }
      else if (e.key === "Enter") {
        e.preventDefault();
        const r = L.search.results[L.search.sel];
        if (!r) return;
        if (e.metaKey || e.ctrlKey) { toggleSetlistEntry(r.idx); renderResults(); return; }
        closeSearch(); loadRig(r.idx, 0);
      } else if (e.key === "Escape") { e.preventDefault(); e.stopPropagation(); closeSearch(); }
      e.stopPropagation();   // keep the rig shortcuts out of the field
    });
  }

  function act(a, el) {
    switch (a) {
      case "prev": step(-1); break;
      case "next": step(1); break;
      case "search": openSearch(); break;
      case "search-close": closeSearch(); break;
      case "setlist": openSetlist(); break;
      case "sl-close": closeSetlist(); break;
      case "sl-add": addCurrentToSetlist(); break;
      case "sl-clear": if (L.setlist.length && confirmClear(el)) { L.setlist = []; saveSetlist(); renderSetlist(); } break;
      case "lights": L.lights = L.lights === "dim" ? "theme" : "dim"; save(KEY.lights, L.lights); paintChrome(); break;
      case "full": L.full = !L.full; save(KEY.full, L.full); applyFull(); paintChrome(); break;
      case "tuner": openTuner(); break;
      case "tuner-close": closeTuner(); break;
      case "tuner-mute": toggleTunerMute(); break;
      case "tap": tap(); break;
      case "bpm-": nudge(-1); break;
      case "bpm+": nudge(1); break;
      case "loop": looperCmd("action=toggle", "lv-loop-main"); break;
      case "loop-stop": looperCmd("action=stop"); break;
      case "bypass": toggleBypass(); break;
      case "mute": toggleMute(); break;
    }
  }
  function confirmClear(el) {
    if (el._armed && now() - el._armed < 2500) { el._armed = 0; el.textContent = "Clear setlist"; return true; }
    el._armed = now(); el.textContent = "Click again to clear";
    setTimeout(() => { if (el._armed) { el._armed = 0; el.textContent = "Clear setlist"; } }, 2600);
    return false;
  }

  // Window capture phase: runs before index.html's global keys (Space = DI play, [ ] = step), so Live owns them.
  function onKey(e) {
    if (!L.visible || e.metaKey || e.ctrlKey || e.altKey) return;
    if (typeof ui !== "undefined" && ui.modalOpen) return;
    const menu = $("menu");
    if (menu && !menu.hidden) return;
    const a = document.activeElement;
    if (typingIn(a)) return;
    if (L.search.open || L.sl.open) {
      if (e.key === "Escape") { stop(e); closeSearch(); closeSetlist(); }
      return;
    }
    const k = e.key;
    const onMacro = a && a.classList && a.classList.contains("lv-mac");
    if (L.tuner.open && (k === "Escape" || k.toLowerCase() === "u")) { stop(e); closeTuner(); return; }
    if (k === "Escape") { stop(e); if (window.HRViews) HRViews.show("chain"); return; }
    if (e.repeat && !/^Arrow|^Page/.test(k)) { if (k === " " || k === "[" || k === "]" || k === "/" || /^[1-4]$/.test(k) || /^[tubmrsf]$/i.test(k)) stop(e); return; }   // held keys don't retrigger (or leak to index.html)
    if ((k === "ArrowRight" || k === "ArrowDown" || k === "ArrowLeft" || k === "ArrowUp") && onMacro) return;   // the knob takes them
    if (k === "ArrowRight" || k === "ArrowDown" || k === "PageDown" || k === " " || k === "]") { stop(e); step(1); return; }
    if (k === "ArrowLeft" || k === "ArrowUp" || k === "PageUp" || k === "[") { stop(e); step(-1); return; }
    if (/^[1-4]$/.test(k)) { stop(e); selectScene(+k - 1); return; }
    const lk = k.toLowerCase();
    if (lk === "t") { stop(e); tap(); return; }
    if (lk === "u") { stop(e); openTuner(); return; }
    if (lk === "b") { stop(e); toggleBypass(); return; }
    if (lk === "m") { stop(e); toggleMute(); return; }
    if (lk === "r") { stop(e); looperCmd("action=toggle", "lv-loop-main"); return; }
    if (lk === "s") { stop(e); looperCmd("action=stop"); return; }
    if (lk === "f") { stop(e); act("full"); return; }
    if (k === "/") { stop(e); openSearch(); return; }
  }
  function stop(e) { e.preventDefault(); e.stopPropagation(); }

  /* =====================================================================
     CHROME — modes, lights, full width
     ===================================================================== */
  function setMode(m) {
    if (m === "setlist" && !L.setlist.length) { openSetlist(); say("Add songs to the setlist first."); }
    L.mode = m === "setlist" ? "setlist" : "list";
    save(KEY.mode, L.mode);
    renderRig(false);
  }
  function applyFull() {
    document.body.classList.toggle("hr-live-full", L.visible && L.full);
    requestAnimationFrame(() => window.dispatchEvent(new Event("resize")));
  }
  function paintChrome() {
    const root = $("lv");
    if (!root) return;
    root.dataset.lights = L.lights;
    setText($("lv-lights-lbl"), L.lights === "dim" ? "Dim stage" : "Theme");
    $("lv-lights").classList.toggle("on", L.lights === "dim");
    setHTML($("lv-full"), L.full ? `${ICON.shrink}<span>Panels</span><kbd>F</kbd>` : `${ICON.expand}<span>Full width</span><kbd>F</kbd>`);
    document.querySelectorAll("#lv [data-mode]").forEach((b) => b.classList.toggle("on", b.dataset.mode === L.mode));
    setText($("lv-sl-n"), L.setlist.length ? String(L.setlist.length) : "");
  }

  /* =====================================================================
     RIG HERO
     ===================================================================== */
  function renderRig(force) {
    if (!L.built) return;
    const rs = RS() || {};
    const lb = LIB();
    // A pending load clears once the engine reports it (or after 5 s if it never does).
    if (L.target != null && (rs.preset_idx === L.target || now() - L.targetT > 5000)) L.target = null;
    const pending = L.target != null && rs.preset_idx !== L.target;
    const tp = pending ? lb.byIdx && lb.byIdx.get(L.target) : null;
    let cur = null;
    try { cur = rigContext().cur; } catch (e) { /* engine not ready */ }
    const name = pending ? libName(tp) || "…" : rs.preset_name || libName(cur) || (rs.preset_idx < 0 ? "New Rig" : "—");
    const nameKey = `${pending ? "p" : ""}${name}`;
    const el = $("lv-name");
    if (nameKey !== L.nameKey || force) {
      const changedName = name !== el.textContent;
      L.nameKey = nameKey;
      el.textContent = name;
      el.style.setProperty("--len", String(Math.max(6, name.length)));
      el.classList.toggle("pending", pending);
      if (changedName) {
        el.classList.remove("in-r", "in-l", "in-0");
        void el.offsetWidth;
        el.classList.add(L.dir > 0 ? "in-r" : L.dir < 0 ? "in-l" : "in-0");
      }
    }
    const info = pending ? tp : cur;
    let isUser = false;
    try { isUser = typeof isUserPreset === "function" && isUserPreset(info); } catch (e) { /* */ }
    const isNew = !pending && (rs.preset_idx < 0 || rs.preset_source === "new");
    setText($("lv-cat"), (info && info.category) || rs.category || (isNew ? "New rig" : ""));
    setText($("lv-src"), isNew ? "New" : isUser ? "Your rig" : "Library");
    setText($("lv-author"), (info && info.author) || rs.author || "");
    $("lv-edited").classList.toggle("on", !pending && typeof isDirty === "function" && isDirty());
    $("lv-load").classList.toggle("on", pending);

    const { items, label, kind } = currentList();
    const ci = pending ? L.target : curIdx();
    const pos = items.findIndex((p) => p.idx === ci);
    const n = items.length;
    setHTML($("lv-pos"), n ? (pos >= 0 ? `<b>${pos + 1}</b> / ${n}` : `— / ${n}`) : "");
    setText($("lv-listlbl"), kind === "setlist" ? `${n} song${n === 1 ? "" : "s"}` : label);
    const prev = n ? items[pos < 0 ? n - 1 : (pos - 1 + n) % n] : null;
    const next = n ? items[pos < 0 ? 0 : (pos + 1) % n] : null;
    setText($("lv-prev-name"), prev && n > 1 ? prev.name : "");
    setText($("lv-next-name"), next && n > 1 ? next.name : "");
    paintChrome();
  }

  /* =====================================================================
     SCENES
     ===================================================================== */
  function sceneParts(s, i) {
    const raw = String((s && s.label) || "").trim();
    const m = raw.match(/^([A-D])\s*[·:\-–]\s*(.+)$/i);
    if (m) return m[2];
    if (/^scene\s+[a-d]$/i.test(raw) || !raw) return "";
    return raw.replace(new RegExp(`^${SCENE_L[i]}\\s+`), "");
  }
  function renderScenes() {
    const box = $("lv-scenes");
    const sc = (RS() || {}).scenes;
    if (!sc || !Array.isArray(sc.scenes) || !sc.scenes.length) {
      setHTML(box, `<div class="lv-scenes-none">This engine build doesn’t report scenes.</div>`);
      return;
    }
    const active = L.sceneOpt != null && now() - L.sceneOptT < 1500 ? L.sceneOpt : sc.active;
    const html = sc.scenes.slice(0, 4).map((s, i) => {
      const nm = sceneParts(s, i), pop = s.populated !== false;
      return `<button class="lv-scene${i === active ? " on" : ""}${pop ? "" : " empty"}" data-scene="${i}" aria-pressed="${i === active}" title="Scene ${SCENE_L[i]} (${i + 1}) — right-click to store the current sound here">
        <span class="sl">${SCENE_L[i]}</span><span class="sn"><b>${escH(nm || (pop ? `Scene ${SCENE_L[i]}` : "Empty"))}</b><small>${pop ? "" : "Right-click to store"}</small></span><i class="lv-led"></i><span class="sk">${i + 1}</span></button>`;
    }).join("");
    setHTML(box, html);
  }
  async function selectScene(i) {
    const sc = (RS() || {}).scenes;
    if (!sc || !Array.isArray(sc.scenes) || !sc.scenes[i]) { say("This rig has no scenes."); return; }
    L.sceneOpt = i; L.sceneOptT = now();
    renderScenes();
    if (typeof rackCall === "function") await rackCall(`/api/scenes?action=select&idx=${i}`, "Scenes need the updated engine.");
    L.sceneOpt = null;
    renderScenes();
  }
  function sceneMenu(x, y, i) {
    if (typeof showMenu !== "function") return;
    const sc = (RS() || {}).scenes, s = sc && sc.scenes && sc.scenes[i];
    if (!s) return;
    showMenu(x, y, [
      { h: `Scene ${SCENE_L[i]}` },
      { label: "Store the current sound here", onClick: () => saveScene(i, null) },
      { label: "Store and rename…", onClick: async () => {
        const v = typeof promptDialog === "function" ? await promptDialog(`Store Scene ${SCENE_L[i]}`, "Name", sceneParts(s, i) || "", "Store") : null;
        if (v != null && v.trim()) saveScene(i, `${SCENE_L[i]} · ${v.trim()}`);
      } },
    ]);
  }
  async function saveScene(i, label) {
    const q = `/api/scenes?action=save&idx=${i}${label ? `&label=${encodeURIComponent(label)}` : ""}`;
    const d = typeof rackCall === "function" ? await rackCall(q, "Scenes need the updated engine.") : null;
    if (d) say(`Stored in Scene ${SCENE_L[i]}`);
    renderScenes();
  }

  /* =====================================================================
     MACROS — same send path as the footer macro bar
     ===================================================================== */
  const macroAt = (col) => { const m = ((RS() || {}).macros || [])[col]; return m && !isEmptyMacro(m) ? m : null; };
  const isEmptyMacro = (m) => !m || m.source === "empty" || !((m.targets && m.targets.length) || m.target);
  function setMacro(col, v) {
    const m = macroAt(col);
    if (!m || typeof queueSend !== "function") return;
    const n = clamp(v, 0, 1);
    m.val = n;
    if (L.drag && L.drag.col === col) L.drag.v = n;
    paintMacro(col);
    if (typeof sendMacro === "function") sendMacro(col, n, m);   // small reply; targets painted locally (index.html)
    else queueSend(`macro:${col}`, `/api/macro?col=${col}&norm=${n.toFixed(4)}&full=1`, (d) => { if (d && d.slots && typeof applyRackState === "function") applyRackState(d); });
  }
  const arcD = (v) => {
    const a0 = -135, a1 = -135 + 270 * clamp(v, 0, 1), r = 52, c = 60;
    const p = (deg) => { const a = (deg - 90) * Math.PI / 180; return `${(c + r * Math.cos(a)).toFixed(2)} ${(c + r * Math.sin(a)).toFixed(2)}`; };
    return `M${p(a0)} A${r} ${r} 0 ${a1 - a0 > 180 ? 1 : 0} 1 ${p(a1 + 0.01)}`;
  };
  const TRACK = arcD(1);
  function targetText(m) {
    const rs = RS() || {};
    const t = (m.targets && m.targets[0]) || null;
    if (!t) return escH(m.target || "");
    if (t.split != null && (t.slot == null || t.slot < 0)) return "Split A↔B mix";
    const s = (rs.slots || []).find((x) => x.slot_idx === t.slot);
    const p = s && (s.params || []).find((x) => x.id === t.param);
    const more = m.targets.length > 1 ? ` +${m.targets.length - 1}` : "";
    const unit = String(t.unit || "").replace(/^(Ibanez|Marshall|Fender|Vox|Mesa\/Boogie|Mesa|EMT|Electro-Harmonix|Boss|MXR|Roland)\s+/i, "");
    return `${escH(unit)}${p && p.fmt ? ` · <b>${escH(p.fmt)}</b>` : ""}${more}`;
  }
  function renderMacros() {
    const box = $("lv-macros");
    const ms = ((RS() || {}).macros || []).slice(0, 8);
    while (ms.length < 8) ms.push(null);
    const key = ms.map((m) => (m ? `${m.name}|${m.source}|${(m.targets || []).map((t) => `${t.slot}:${t.param}`).join("+")}` : "-")).join(",");
    if (key !== box._key) {
      box._key = key;
      box.innerHTML = ms.map((m, k) => {
        const empty = isEmptyMacro(m);
        return `<div class="lv-mac${empty ? " empty" : ""}" ${empty ? "" : `data-col="${k}" tabindex="0" role="slider" aria-valuemin="0" aria-valuemax="100"`} aria-label="Macro ${k + 1}${empty ? " (not assigned)" : `: ${escH(m.name)}`}">
          <span class="mno">M${k + 1}</span>
          <div class="mk"><svg viewBox="0 0 120 120"><path class="tr" d="${TRACK}" fill="none" stroke-width="7" stroke-linecap="round"/><path class="va" d="" fill="none" stroke-width="7" stroke-linecap="round"/><circle class="face" cx="60" cy="60" r="38" stroke-width="1.5"/><line class="ptr" x1="60" y1="24" x2="60" y2="31.5" stroke-width="4" stroke-linecap="round" style="transform-origin:60px 60px"/></svg><span class="mv num"></span></div>
          <div class="mtxt"><span class="mn">${empty ? `Macro ${k + 1}` : escH(m.name || `Macro ${k + 1}`)}</span><span class="mt"></span></div>
        </div>`;
      }).join("");
    }
    ms.forEach((m, k) => paintMacro(k));
  }
  function paintMacro(col) {
    const box = $("lv-macros");
    const el = box && box.children[col];
    if (!el) return;
    const m = ((RS() || {}).macros || [])[col];
    const empty = isEmptyMacro(m);
    const v = empty ? 0 : (L.drag && L.drag.col === col ? L.drag.v : m.val);
    const pk = empty ? "e" : v.toFixed(4);
    if (el._pk !== pk) {
      el._pk = pk;
      el.querySelector(".va").setAttribute("d", empty || v < 0.002 ? "" : arcD(v));
      const ptr = el.querySelector(".ptr");
      ptr.style.transform = `rotate(${(-135 + 270 * v).toFixed(1)}deg)`;
      ptr.style.display = empty ? "none" : "";
      setText(el.querySelector(".mv"), empty ? "" : String(Math.round(v * 100)));
      if (!empty) el.setAttribute("aria-valuenow", String(Math.round(v * 100)));
    }
    setHTML(el.querySelector(".mt"), empty ? "Not assigned" : targetText(m));
  }

  /* =====================================================================
     TEMPO — HRTempo (assets/cabinet.js)
     ===================================================================== */
  function tempoInfo() {
    const t = (RS() || {}).tempo || {};
    const bpm = window.HRTempo && typeof HRTempo.get === "function" ? HRTempo.get() : (t.bpm || 120);
    return { bpm: +bpm || 120, host: t.source === "host" };
  }
  function tap() {
    if (tempoInfo().host) { say("The host sets the tempo."); return; }
    const b = $("lv-tap");
    if (b) { b.classList.add("hit"); setTimeout(() => b.classList.remove("hit"), 110); }
    if (window.HRTempo && HRTempo.tap) { try { HRTempo.tap(); } catch (e) { fetchJSON("/api/tempo/tap").catch(() => {}); } }
    else if (typeof fetchJSON === "function") fetchJSON("/api/tempo/tap").catch(() => {});
    setTimeout(paintTempo, 30);
  }
  function nudge(d) {
    const { bpm, host } = tempoInfo();
    if (host) return;
    if (window.HRTempo && HRTempo.set) HRTempo.set(bpm + d);
    setTimeout(paintTempo, 30);
  }
  function paintTempo() {
    const { bpm, host } = tempoInfo();
    const s = (Math.round(bpm * 10) / 10).toFixed(1);
    if (L.bpmShown !== s) {
      L.bpmShown = s;
      setHTML($("lv-bpm"), `${s}<small>BPM</small>`);
      const beat = $("lv-beat");
      beat.style.setProperty("--beat", `${(60 / Math.max(20, bpm)).toFixed(4)}s`);
      beat.style.animation = "none"; void beat.offsetWidth; beat.style.animation = "";
    }
    $("lv-tempo").classList.toggle("host", host);
    setText($("lv-tempo-src"), host ? "from the host" : "rig tempo");
  }

  /* =====================================================================
     LOOPER — /api/looper
     ===================================================================== */
  async function pollLooper() {
    if (!L.visible || typeof fetchJSON !== "function") return;
    if (now() - L.loopT < 180) return;
    L.loopT = now();
    try {
      const d = await fetchJSON("/api/looper");
      if (d && typeof d.state === "string") { L.looper = d; L.loopAt = now(); L.loopSupported = true; }
      else L.loopSupported = false;
    } catch (e) { /* engine offline */ }
    paintLooper();
  }
  async function looperCmd(q, flashId) {
    if (flashId) flash(flashId);
    try {
      const d = await fetchJSON(`/api/looper?${q}`);
      if (d && typeof d.state === "string") { L.looper = d; L.loopAt = now(); paintLooper(); }
      else say("This engine build has no looper.");
    } catch (e) { say("Couldn’t reach the engine."); }
  }
  const LOOP_LBL = { empty: "EMPTY", armed: "ARMED", recording: "RECORDING", playing: "PLAYING", overdubbing: "OVERDUB", stopped: "STOPPED" };
  const LOOP_NEXT = { empty: "REC", armed: "CANCEL", recording: "PLAY", playing: "DUB", overdubbing: "PLAY", stopped: "PLAY" };
  const fmtT = (s) => { s = Math.max(0, s || 0); const m = Math.floor(s / 60); return `${m}:${(s - m * 60).toFixed(1).padStart(4, "0")}`; };
  function paintLooper() {
    const box = $("lv-loop");
    if (!box) return;
    const st = L.looper, s = st ? st.state : "empty";
    box.dataset.st = s;
    box.classList.toggle("unsupported", !L.loopSupported);
    setHTML($("lv-loop-st"), `<i></i><span>${L.loopSupported ? LOOP_LBL[s] || s.toUpperCase() : "NO LOOPER"}</span>`);
    setHTML($("lv-loop-main"), `${LOOP_NEXT[s] || "REC"} <kbd>R</kbd>`);
    const len = st && st.length_s > 0 ? st.length_s : 0;
    // pos is 0..1 through the loop; between polls it's carried forward on the wall clock
    let frac = st ? clamp(+st.pos || 0, 0, 1) : 0;
    if (len && (s === "playing" || s === "overdubbing")) { frac += (now() - L.loopAt) / 1000 * (st.speed || (st.half_speed ? 0.5 : 1)) * (st.reverse ? -1 : 1) / len; frac -= Math.floor(frac); }
    $("lv-loop-pos").style.width = `${(s === "recording" ? 100 : frac * 100).toFixed(1)}%`;
    setText($("lv-loop-t"), len ? `${fmtT(frac * len)} / ${fmtT(len)}${st.layers ? ` · ${1 + st.layers} layers` : ""}` : "");
  }

  /* =====================================================================
     BYPASS · MUTE
     ===================================================================== */
  function toggleBypass() {
    if (typeof setDIMode !== "function") return;
    const dry = !(typeof ui !== "undefined" && ui.masterBypass);
    setDIMode(dry);
    paintKill();
  }
  async function refreshAudio(force) {
    if (!force && now() - L.audioT < 4000) return;
    L.audioT = now();
    try {
      const d = await fetchJSON("/api/audio");
      if (d && d.current) { L.audioOk = true; L.monitor = d.current.monitor !== false; if (typeof audioCfg !== "undefined") audioCfg = d; }
      else L.audioOk = false;
    } catch (e) { /* offline */ }
    paintKill();
  }
  async function setMonitor(on) {
    if (!L.audioOk) { say("This engine build has no output mute."); return; }
    L.monitor = on; paintKill();
    if (typeof setAudio === "function") await setAudio({ monitor: on ? 1 : 0 });
    else await fetchJSON(`/api/audio/set?monitor=${on ? 1 : 0}`).catch(() => {});
    await refreshAudio(true);
  }
  function toggleMute() {
    if (L.tuner.mutedByTuner) { L.tuner.mutedByTuner = false; setMonitor(true); return; }
    setMonitor(!(L.monitor !== false));
  }
  function paintKill() {
    const byp = typeof ui !== "undefined" && !!ui.masterBypass;
    const b = $("lv-byp"), m = $("lv-mute");
    if (!b) return;
    b.classList.toggle("on", byp);
    b.setAttribute("aria-pressed", byp);
    setHTML(b, `BYPASS<small>${byp ? "rig is off · dry input" : "rig off · dry input"}</small>`);
    const muted = L.monitor === false;
    m.classList.toggle("on", muted);
    m.setAttribute("aria-pressed", muted);
    m.disabled = !L.audioOk;
    setHTML(m, `${muted ? "MUTED" : "MUTE"}<small>${!L.audioOk ? "not available" : muted ? (L.tuner.mutedByTuner ? "while tuning" : "output is off") : "output on"}</small>`);
    const tov = $("lv-tov");
    if (tov) tov.classList.toggle("muted", muted && L.tuner.open);
    const sw = $("lv-tv-mute");
    if (sw) sw.classList.toggle("on", L.tunerMute);
  }

  /* =====================================================================
     TUNER — YIN on the engine's input stream (index.html's `scope` ring from /api/scope)
     ===================================================================== */
  function scopeRing() { return typeof scope !== "undefined" && scope && scope.inb && typeof scope.w === "number" ? scope : null; }
  const WIN = 3072, FMIN = 60, FMAX = 1100;
  const buf = new Float32Array(WIN);
  let dif = null, cmn = null;
  function detect(x, sr) {
    const W = x.length >> 1, tmax = Math.floor(sr / FMIN), tmin = Math.floor(sr / FMAX);
    if (!dif || dif.length < tmax + 2) { dif = new Float64Array(tmax + 2); cmn = new Float64Array(tmax + 2); }
    for (let t = 1; t < tmax + 2; t++) { let s = 0; for (let i = 0; i < W; i++) { const e = x[i] - x[i + t]; s += e * e; } dif[t] = s; }
    let run = 0;
    cmn[0] = 1;
    for (let t = 1; t < tmax + 2; t++) { run += dif[t]; cmn[t] = run > 0 ? dif[t] * t / run : 1; }
    let tau = -1;
    for (let t = tmin; t < tmax; t++) { if (cmn[t] < 0.15) { while (t + 1 < tmax && cmn[t + 1] < cmn[t]) t++; tau = t; break; } }
    if (tau < 0) return null;
    const q = cmn[tau];
    const a = dif[tau - 1], b = dif[tau], c = dif[tau + 1], den = a - 2 * b + c;
    let per = tau + (den ? 0.5 * (a - c) / den : 0);
    // refine on the m-th period: m× the resolution for higher notes
    const m = Math.floor((tmax - 2) / per);
    if (m >= 2) {
      let p = Math.round(m * per);
      for (let j = 0; j < 3; j++) { if (p > 1 && dif[p - 1] < dif[p]) p--; else if (p < tmax && dif[p + 1] < dif[p]) p++; }
      const a2 = dif[p - 1], b2 = dif[p], c2 = dif[p + 1], d2 = a2 - 2 * b2 + c2;
      per = (p + (d2 ? 0.5 * (a2 - c2) / d2 : 0)) / m;
    }
    return { hz: sr / per, q };
  }
  function runTuner(t) {
    const T = L.tuner;
    if (t - T.lastRun < 55) return;
    T.lastRun = t;
    // Engine tuner (/api/listen via the Listen pane): full-rate, fundamental-locked, reference-aware. The YIN
    // below on the decimated scope stream is the fallback for engines without the route.
    const lp = window.ListenPane;
    if (lp && lp.want) {
      lp.want();
      if (lp.fresh()) {
        const m = lp.tuner();
        T.src = "engine";
        if (m && m.state !== "none") {
          if (m.midi !== T.note) { T.note = m.midi; T.shown = m.cents; }
          T.hz = m.hz; T.cents = m.cents; T.sigT = t;
        }
        return;
      }
    }
    const sc = scopeRing();
    if (!sc) { T.src = "none"; return; }
    T.src = "scope";
    if (sc.w !== T.lastW) { T.lastW = sc.w; T.freshT = t; }
    const fresh = t - T.freshT < 400 && sc.w >= WIN;
    let est = null;
    if (fresh) {
      const N = sc.N || sc.inb.length, start = sc.w - WIN;
      let e2 = 0;
      for (let i = 0; i < WIN; i++) { const v = sc.inb[(start + i) & (N - 1)]; buf[i] = v; e2 += v * v; }
      const rms = Math.sqrt(e2 / WIN);
      if (rms > 0.0025) est = detect(buf, sc.rate || 12000);
    }
    if (est && est.hz >= FMIN && est.hz <= FMAX) {
      T.est.push(est.hz); if (T.est.length > 3) T.est.shift();
      const sorted = [...T.est].sort((a, b) => a - b), hz = sorted[sorted.length >> 1];
      const midi = 69 + 12 * Math.log2(hz / 440), note = Math.round(midi), cents = (midi - note) * 100;
      if (note !== T.note) { T.note = note; T.shown = cents; }
      T.hz = hz; T.cents = cents; T.sigT = t;
    } else if (!est) {
      if (t - T.sigT > 600) { T.est = []; }
    }
  }
  function paintTuner(t) {
    const T = L.tuner;
    const sig = t - T.sigT < 450 && T.note >= 0;
    if (sig) T.shown += (T.cents - T.shown) * 0.35;
    const c = T.shown, intune = sig && Math.abs(c) < 3;
    const note = T.note >= 0 ? `${NOTE[((T.note % 12) + 12) % 12]}<sub>${Math.floor(T.note / 12) - 1}</sub>` : "–";
    const tile = $("lv-tuner");
    tile.dataset.st = sig ? "sig" : "none";
    tile.classList.toggle("intune", intune);
    setHTML($("lv-tn-note"), T.note >= 0 ? note : `<small class="lv-ph">—</small>`);
    $("lv-tn-dot").style.left = `${(50 + clamp(c, -50, 50)).toFixed(2)}%`;
    const sc = scopeRing();
    setText($("lv-tn-cents"), !sc ? "no input stream" : sig ? `${c >= 0 ? "+" : "−"}${Math.abs(c).toFixed(1)} ¢` : "no signal");
    setText($("lv-tn-hz"), sig ? `${T.hz.toFixed(1)} Hz` : "");
    if (!T.open) return;
    const ov = $("lv-tov");
    ov.dataset.st = sig ? "sig" : "none";
    ov.classList.toggle("intune", intune);
    setHTML($("lv-tv-note"), T.note >= 0 ? note : `<small class="lv-ph">Play a string</small>`);
    const needle = $("lv-tv-needle");
    if (needle) needle.style.transform = `rotate(${(sig ? clamp(c, -50, 50) / 50 * 62 : 0).toFixed(2)}deg)`;
    setText($("lv-tv-cents"), sig ? `${c >= 0 ? "+" : "−"}${Math.abs(c).toFixed(1)}` : "–");
    setText($("lv-tv-hz"), sig ? T.hz.toFixed(2) : "–");
    $("lv-tv-flat").classList.toggle("on", sig && c <= -3);
    $("lv-tv-sharp").classList.toggle("on", sig && c >= 3);
    const tel = L.tel || {};
    const src = typeof ui !== "undefined" && ui.source === "live" ? "your audio interface input" : "the built-in DI player";
    setText($("lv-tv-src"), !sc ? "No input stream from the engine — the tuner needs /api/scope."
      : `Pitch is detected from the engine’s input signal (${src}, before the rig). ${tel.source !== "live" && !tel.playing ? "The DI player is stopped — switch to Live input in Audio settings to tune your guitar." : sig ? "" : "Play a single string."}`);
  }
  async function openTuner() {
    const T = L.tuner;
    if (T.open) return;
    T.open = true;
    $("lv-tov").hidden = false;
    lockScroll(true);
    if (L.tunerMute && L.monitor !== false && L.audioOk) { T.mutedByTuner = true; await setMonitor(false); }
    paintKill();
  }
  async function closeTuner() {
    const T = L.tuner;
    if (!T.open) return;
    T.open = false;
    $("lv-tov").hidden = true;
    lockScroll(false);
    if (T.mutedByTuner) { T.mutedByTuner = false; await setMonitor(true); }
    paintKill();
  }
  async function toggleTunerMute() {
    L.tunerMute = !L.tunerMute;
    save(KEY.tmute, L.tunerMute);
    const T = L.tuner;
    if (T.open) {
      if (L.tunerMute && L.monitor !== false) { T.mutedByTuner = true; await setMonitor(false); }
      else if (!L.tunerMute && T.mutedByTuner) { T.mutedByTuner = false; await setMonitor(true); }
    }
    paintKill();
  }
  function lockScroll(on) {
    const v = $("view-live");
    if (!v) return;
    if (on) { v.scrollTop = 0; const r = $("lv"); if (r) r.style.setProperty("--lv-ovh", `${v.clientHeight}px`); }
    v.classList.toggle("lv-locked", on || L.tuner.open || L.search.open || L.sl.open);
  }

  /* =====================================================================
     METERS — telemetry RMS + clip / peak lights
     ===================================================================== */
  const toDbV = (r) => 20 * Math.log10(Math.max(1e-6, r || 0));
  const pct = (db) => clamp((db + 60) / 60, 0, 1);
  const fmtDbV = (db) => (db <= -59.5 ? "−∞" : `${db < 0 ? "−" : "+"}${Math.abs(db).toFixed(1)}`);
  function paintMeters(t) {
    const M = L.meters, d = L.tel || {};
    const ti = pct(toDbV(d.in_rms)), tl = pct(toDbV(d.out_rms_l)), tr = pct(toDbV(d.out_rms_r));
    const ease = (cur, tgt) => (tgt > cur ? cur + (tgt - cur) * 0.6 : cur + (tgt - cur) * 0.18);
    M.in = ease(M.in, ti); M.l = ease(M.l, tl); M.r = ease(M.r, tr);
    M.pin = Math.max(M.in, M.pin - 0.006); M.pl = Math.max(M.l, M.pl - 0.006); M.pr = Math.max(M.r, M.pr - 0.006);
    const bar = (id, v, pid, pv) => { const f = $(id), p = $(pid); if (f) { f.style.height = `${(v * 100).toFixed(1)}%`; f.style.setProperty("--mh", `${f.parentNode.clientHeight}px`); } if (p) p.style.bottom = `calc(${(pv * 100).toFixed(1)}% - 2px)`; };
    bar("lv-m-in", M.in, "lv-p-in", M.pin);
    bar("lv-m-l", M.l, "lv-p-l", M.pl);
    bar("lv-m-r", M.r, "lv-p-r", M.pr);
    setText($("lv-db-in"), fmtDbV(toDbV(d.in_rms)));
    setText($("lv-db-out"), fmtDbV(toDbV(Math.sqrt(((d.out_rms_l || 0) ** 2 + (d.out_rms_r || 0) ** 2) / 2))));
    if (d.input_clip) M.inClip = t + 1500;
    const sc = scopeRing();
    if (sc && sc.outb) {
      const N = sc.N || sc.outb.length;
      if (M.lastW < 0 || sc.w - M.lastW > N) M.lastW = sc.w;
      let pk = 0;
      for (let k = M.lastW; k < sc.w; k++) { const v = Math.abs(sc.outb[k & (N - 1)]); if (v > pk) pk = v; }
      M.lastW = sc.w;
      if (pk >= 0.891) M.outClip = t + 1200;   // −1 dBFS: the rack limiter's ceiling
    }
    $("lv-clip-in").classList.toggle("on", t < M.inClip);
    $("lv-clip-out").classList.toggle("on", t < M.outClip);
    setText($("lv-src-in"), d.source === "live" ? "live input" : d.playing ? "DI player" : "DI player · stopped");
  }

  /* =====================================================================
     JUMP + SETLIST
     ===================================================================== */
  function openSearch() {
    closeSetlist();
    L.search.open = true;
    $("lv-sov").hidden = false;
    lockScroll(true);
    const q = $("lv-q");
    q.value = ""; L.search.sel = 0;
    renderResults();
    setTimeout(() => q.focus(), 0);
  }
  function closeSearch() {
    if (!L.search.open) return;
    L.search.open = false;
    $("lv-sov").hidden = true;
    const q = $("lv-q"); if (q) q.blur();
    lockScroll(false);
  }
  function inSetlist(idx) { const p = LIB().byIdx && LIB().byIdx.get(idx); return p ? L.setlist.findIndex((e) => e.name === libName(p)) : -1; }
  function toggleSetlistEntry(idx) {
    const p = LIB().byIdx && LIB().byIdx.get(idx);
    if (!p) return;
    const i = inSetlist(idx);
    if (i >= 0) { L.setlist.splice(i, 1); say(`Removed “${libName(p)}” from the setlist`); }
    else { L.setlist.push({ idx, name: libName(p), user: typeof isUserPreset === "function" && isUserPreset(p) }); say(`Added “${libName(p)}” to the setlist`); }
    saveSetlist();
  }
  function saveSetlist() { save(KEY.setlist, L.setlist); paintChrome(); renderRig(false); }
  function renderResults(scrollSel) {
    const q = ($("lv-q").value || "").trim().toLowerCase(), toks = q.split(/\s+/).filter(Boolean);
    const lb = LIB(), cur = curIdx();
    let res = (lb.presets || []).filter((p) => {
      if (!toks.length) return true;
      const t = `${libName(p)} ${p.category || ""} ${p.amp || ""} ${p.author || ""} ${(p.tags || []).join(" ")}`.toLowerCase();
      return toks.every((k) => t.includes(k));
    });
    if (q) res.sort((a, b) => (libName(b).toLowerCase().startsWith(q) - libName(a).toLowerCase().startsWith(q)) || libName(a).localeCompare(libName(b)));
    else {
      // no query: your setlist first, then the current list
      const ids = new Set(currentList().items.map((p) => p.idx));
      res = res.filter((p) => ids.has(p.idx));
    }
    const total = res.length;
    res = res.slice(0, 80);
    L.search.results = res;
    if (L.search.sel >= res.length) L.search.sel = Math.max(0, res.length - 1);
    const list = $("lv-results");
    list.innerHTML = res.length ? res.map((p, i) => {
      const inS = inSetlist(p.idx) >= 0, user = typeof isUserPreset === "function" && isUserPreset(p);
      return `<div class="lv-row${i === L.search.sel ? " is-sel" : ""}${p.idx === cur ? " cur" : ""}" data-load="${p.idx}" role="button">
        <span class="rt"><span class="rn">${escH(libName(p))}</span><span class="rc">${user ? "Your rig" : "Library"}${p.category ? ` · ${escH(p.category)}` : ""}${p.amp ? ` · ${escH(p.amp)}` : ""}</span></span>
        <button class="rb${inS ? " on" : ""}" data-sl="${p.idx}" title="${inS ? "Remove from" : "Add to"} the setlist">${inS ? "✓" : "+"}</button></div>`;
    }).join("") : `<div class="lv-empty">${lb.presets && lb.presets.length ? "No rigs match." : "The library is still loading…"}</div>`;
    setText($("lv-q-n"), q ? `${total} match${total === 1 ? "" : "es"}` : `${total} in the current list · type to search everything`);
    if (scrollSel) { const s = list.querySelector(".is-sel"); if (s) s.scrollIntoView({ block: "nearest" }); }
  }
  function openSetlist() {
    closeSearch();
    L.sl.open = true;
    $("lv-lov").hidden = false;
    lockScroll(true);
    renderSetlist();
  }
  function closeSetlist() {
    if (!L.sl.open) return;
    L.sl.open = false;
    $("lv-lov").hidden = true;
    lockScroll(false);
  }
  function addCurrentToSetlist() {
    const rs = RS() || {};
    let p = null;
    try { p = rigContext().cur; } catch (e) { /* */ }
    if (!p) { say(rs.preset_idx < 0 ? "Save this rig first — new rigs can’t go in a setlist." : "Couldn’t find this rig in the library."); return; }
    if (inSetlist(p.idx) >= 0) { say("Already in the setlist."); return; }
    L.setlist.push({ idx: p.idx, name: libName(p), user: typeof isUserPreset === "function" && isUserPreset(p) });
    saveSetlist(); renderSetlist();
  }
  function moveSetlist(i, d) {
    const j = i + d;
    if (j < 0 || j >= L.setlist.length) return;
    const [e] = L.setlist.splice(i, 1);
    L.setlist.splice(j, 0, e);
    saveSetlist(); renderSetlist();
  }
  function renderSetlist() {
    const list = $("lv-sl-list");
    if (!list) return;
    const items = resolveSetlist(), cur = curIdx();
    list.innerHTML = items.length ? items.map((e, i) => `<div class="lv-row${e.idx == null ? " missing" : ""}${e.idx === cur ? " cur" : ""}" data-load="${e.idx == null ? "" : e.idx}" role="button">
        <span class="ri num">${i + 1}</span>
        <span class="rt"><span class="rn">${escH(e.name)}</span><span class="rc">${e.idx == null ? "Not found in your library" : `${e.user ? "Your rig" : "Library"}${e.category ? ` · ${escH(e.category)}` : ""}`}</span></span>
        <button class="rb" data-slmv="${i}" data-d="-1" title="Move up"${i === 0 ? " disabled" : ""}>${ICON.up}</button>
        <button class="rb" data-slmv="${i}" data-d="1" title="Move down"${i === items.length - 1 ? " disabled" : ""}>${ICON.down}</button>
        <button class="rb" data-slrm="${i}" title="Remove">${ICON.close}</button></div>`).join("")
      : `<div class="lv-empty">Your setlist is empty.<br>Load a rig and press <b>+ Add current rig</b>, or press <b>/</b> and use <b>+</b> next to any rig.<br>Then choose <b>Setlist</b> at the top to step through it in order.</div>`;
  }

  /* =====================================================================
     LOOP
     ===================================================================== */
  function frame(t) {
    if (!L.visible) return;
    if (window.HRActivity && !HRActivity.awake()) { L.raf = 0; return; }   // window/editor hidden: no loop, no polls (resumed below)
    const rs = RS(), lb = LIB();
    const lk = typeof ui !== "undefined" ? `${ui.tag}|${ui.search}|${ui.leftTab}|${L.mode}|${L.setlist.length}` : L.mode;
    if (rs !== L.rs || lb.sig !== L.libSig || lk !== L.listKey) {
      const presetMoved = !L.rs || !rs || L.rs.preset_idx !== rs.preset_idx || L.rs.preset_name !== rs.preset_name;
      if (presetMoved && L.target == null) L.dir = 0;
      L.rs = rs; L.libSig = lb.sig; L.listKey = lk;
      renderRig(false);
      renderScenes();
      renderMacros();
      if (L.sl.open) renderSetlist();
    } else if (L.target != null) renderRig(false);
    for (let k = 0; k < 8; k++) if (!(L.drag && L.drag.col === k)) paintMacro(k);
    runTuner(t);
    paintTuner(t);
    paintMeters(t);
    paintTempo();
    if (L.looper && (L.looper.state === "playing" || L.looper.state === "overdubbing")) paintLooper();
    pollLooper();
    refreshAudio(false);
    if (typeof ui !== "undefined" && !!ui.masterBypass !== L._byp) { L._byp = !!ui.masterBypass; paintKill(); }
    L.raf = requestAnimationFrame(frame);
  }

  /* =====================================================================
     VIEW LIFECYCLE
     ===================================================================== */
  function enter() {
    if (!L.built && !build()) return;
    L.visible = true;
    L.rs = null; L.nameKey = ""; L.dir = 0;
    applyFull();
    paintChrome();
    renderRig(true);
    renderScenes();
    renderMacros();
    paintKill();
    refreshAudio(true);
    L.loopT = 0; pollLooper();
    cancelAnimationFrame(L.raf);
    L.raf = requestAnimationFrame(frame);
    // keys go to Live, not the tab button that was just clicked
    const a = document.activeElement;
    if (a && a.closest && a.closest("#view-tabs")) a.blur();
  }
  function leave() {
    L.visible = false;
    cancelAnimationFrame(L.raf);
    if (L.drag) { L.drag.el.classList.remove("drag"); L.drag = null; }
    closeSearch(); closeSetlist();
    if (L.tuner.open) closeTuner();
    applyFull();
    if (L.monitor === false) say("The output is still muted — unmute it in Live or Audio settings.");
  }
  function onTelemetry(d) { if (d) L.tel = d; }
  // Idle CPU (assets/activity.js): frame() ends while asleep; restart it when the window comes back.
  if (window.HRActivity) HRActivity.on((e) => { if (e.awake && L.visible && !L.raf) L.raf = requestAnimationFrame(frame); });

  window.addEventListener("keydown", onKey, true);
  window.addEventListener("resize", () => { if (L.visible && (L.tuner.open || L.search.open || L.sl.open)) lockScroll(true); });
  function reg() { if (window.HRViews && HRViews.register) HRViews.register("live", { enter, leave, onTelemetry }); }
  if (window.HRViews) reg(); else document.addEventListener("DOMContentLoaded", reg);
  // Settings → Live (assets/settings.js) reads and writes these through the same paths as the Live chrome.
  const prefs = {
    get: () => ({ lights: L.lights, full: L.full, tunerMute: L.tunerMute, mode: L.mode, setlist: L.setlist.length }),
    set(k, v) {
      if (k === "lights" && (v === "theme" ? "theme" : "dim") !== L.lights) act("lights");
      else if (k === "full" && !!v !== L.full) act("full");
      else if (k === "tunerMute" && !!v !== L.tunerMute) toggleTunerMute();
      else if (k === "mode" && (v === "list" || L.setlist.length)) setMode(v);
    },
    openSetlist,
  };
  window.HRLive = { step, selectScene, openTuner, closeTuner, state: () => L, prefs };
})();
