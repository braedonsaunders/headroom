/* Headroom — Practice view: play along with a backing track or a YouTube video  (assets/practice-view.js → #view-practice)
 *
 *   - Backing track: your own songs, copied into ~/Library/Application Support/Headroom/Backing Tracks and played by
 *     the engine, mixed in AFTER the rig (the guitar goes through the amp, the track doesn't). The waveform is the
 *     hero: click to seek, drag across it to loop a section, drag the A / B flags to adjust. Speed 50–150 % keeps
 *     the pitch, transpose keeps the tempo (engine: AUNewTimePitch), count-in clicks before playback, optional duck
 *     while you play. Engine: GET /api/backing?action=load|play|pause|toggle|stop|seek|unload & settings
 *     (volume, speed, semitones, loop/a/b, countin/bpm, duck, normalize), /api/backing/list, /import (POST
 *     {"paths":[…]}), /rename, /delete, /peaks; telemetry `backing` = {id, playing, pos, dur, speed, count, loading}.
 *     Adding tracks: the native picker (bridge action "importBackingTracks") or audio files dropped on the window
 *     (StudioApp.mm calls window.headroomBackingDrag(on) / window.headroomBackingImport(paths)).
 *   - YouTube: the official IFrame player, hosted by the engine at http://127.0.0.1:<port>/practice/youtube.html
 *     (a file:// page has no origin, and YouTube refuses embeds without a referrer — errors 152/153). This file
 *     talks to that page with postMessage ({hr:"yt", cmd:"load"|"call"|"loop"} ⇄ {hr:"yt", type:"state"|…}).
 *     Audio stays in the web view (it doesn't go through the engine). Recent links live in localStorage.
 *   - Test DI tracks (⋯ menu): the built-in DI player's trainer (GET /api/trainer, /api/transport,
 *     /api/tracks/envelope) — speed, A–B loop and guitar cut on the DI recordings the rig uses as its input.
 *   Keys while Practice is open (never while typing): Space play / pause · ← → 5 s · A / B loop points · L loop.
 *
 * Registered with HRViews (assets/studio-views.js), which forwards telemetry here (also while hidden, for the tab
 * LED). Uses index.html globals at call time only: API_BASE, queueSend, engineCmd, toggleDIPlayback, prettyTrack,
 * showMenu, closeMenu, promptDialog, confirmDialog, toast, esc, ui, HRTempo.
 */
(function () {
  "use strict";
  const $ = (id) => document.getElementById(id);
  const clamp = (v, a, b) => Math.max(a, Math.min(b, v));
  const now = () => performance.now();
  const escH = (s) => (typeof esc === "function" ? esc(s) : String(s ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c])));
  const say = (m) => { if (typeof toast === "function") toast(m); };
  const setText = (el, v) => { if (el && el._t !== v) { el._t = v; el.textContent = v; } };
  const setHTML = (el, v) => { if (el && el._h !== v) { el._h = v; el.innerHTML = v; } };
  const typingIn = (a) => a && ((a.tagName === "INPUT" && a.type !== "range") || a.tagName === "SELECT" || a.tagName === "TEXTAREA" || a.isContentEditable);
  const base = () => (typeof API_BASE !== "undefined" ? API_BASE : "http://127.0.0.1:17770");
  const api = async (path, opts) => { const r = await fetch(base() + path, Object.assign({ cache: "no-store" }, opts)); if (!r.ok) throw new Error(String(r.status)); return r.json(); };
  const lazy = (key, path, cb) => { if (typeof queueSend === "function") queueSend(key, path, cb); else api(path).then((d) => cb && cb(d)).catch(() => {}); };
  const hasBridge = () => !!(window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.headroom);
  const bridge = (msg) => { try { window.webkit.messageHandlers.headroom.postMessage(msg); return true; } catch (e) { return false; } };
  const q = (o) => Object.entries(o).map(([k, v]) => `${k}=${encodeURIComponent(v)}`).join("&");
  const fmtT = (s, dec = 0) => {
    s = Math.max(0, +s || 0);
    if (dec) s = Math.floor(s * 10) / 10; else s = Math.floor(s);
    const m = Math.floor(s / 60), r = s - m * 60;
    return `${m}:${(dec ? r.toFixed(1) : String(r)).padStart(dec ? 4 : 2, "0")}`;
  };
  const dbOf = (g) => (g <= 0.0005 ? "−∞" : `${g >= 1 ? "+" : "−"}${Math.abs(20 * Math.log10(g)).toFixed(1)}`.replace("+0.0", "0.0").replace("−0.0", "0.0"));

  /* ---------- persistence ---------- */
  const LS = { prefs: "hr.practice.prefs", tracks: "hr.practice.tracks", yt: "hr.practice.yt" };
  const load = (k, d) => { try { const v = localStorage.getItem(k); return v == null ? d : (JSON.parse(v) ?? d); } catch (e) { return d; } };
  const save = (k, v) => { try { localStorage.setItem(k, JSON.stringify(v)); } catch (e) { /* private mode */ } };
  const prefs = Object.assign({ mode: "track", volume: 1, duck: false, countin: 0, normalize: true, ytVol: 100, ytRate: 1, ytLast: "" }, load(LS.prefs, {}));
  const trackPrefs = load(LS.tracks, {});         // id → { speed, semis, loop, a, b, bpm }
  let ytRecent = load(LS.yt, []);                  // [{ id, title, author, t, pos }]
  const savePrefs = () => save(LS.prefs, prefs);

  /* ---------- icons ---------- */
  const I = {
    play: `<svg viewBox="0 0 16 16"><path d="M4.6 2.5v11l8.8-5.5z" fill="currentColor"/></svg>`,
    pause: `<svg viewBox="0 0 16 16"><rect x="3.6" y="2.8" width="3.1" height="10.4" rx="1" fill="currentColor"/><rect x="9.3" y="2.8" width="3.1" height="10.4" rx="1" fill="currentColor"/></svg>`,
    loop: `<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"><path d="M3 7.5V7a3 3 0 0 1 3-3h6.5"/><path d="M10.5 2l2 2-2 2"/><path d="M13 8.5V9a3 3 0 0 1-3 3H3.5"/><path d="M5.5 14l-2-2 2-2"/></svg>`,
    plus: `<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"><path d="M8 3.5v9M3.5 8h9"/></svg>`,
    minus: `<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"><path d="M3.5 8h9"/></svg>`,
    more: `<svg viewBox="0 0 16 16" fill="currentColor"><circle cx="3.5" cy="8" r="1.4"/><circle cx="8" cy="8" r="1.4"/><circle cx="12.5" cy="8" r="1.4"/></svg>`,
    close: `<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"><path d="M4 4l8 8M12 4l-8 8"/></svg>`,
    vol: `<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"><path d="M2.5 6h2.2L8 3.2v9.6L4.7 10H2.5z" fill="currentColor" stroke="none"/><path d="M10.5 5.6a3.4 3.4 0 0 1 0 4.8"/><path d="M12.4 3.8a6 6 0 0 1 0 8.4"/></svg>`,
    lib: `<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"><path d="M2.5 4h7M2.5 8h7M2.5 12h4.5"/><circle cx="12" cy="11.3" r="1.6"/><path d="M13.6 11.3V3.6l1.4.6"/></svg>`,
    chev: `<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"><path d="M4.5 6.5L8 10l3.5-3.5"/></svg>`,
    yt: `<svg viewBox="0 0 24 24"><rect x="1.5" y="4.5" width="21" height="15" rx="4.5" fill="currentColor"/><path d="M10 9v6l5.2-3z" fill="var(--pv-yt-ink, #fff)"/></svg>`,
    wave: `<svg viewBox="0 0 64 40" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round"><path d="M6 20v0M12 15v10M18 9v22M24 13v14M30 4v32M36 11v18M42 16v8M48 8v24M54 14v12M60 18v4"/></svg>`,
    flask: `<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"><path d="M6 2h4M6.6 2v4.2L3 12.6A1 1 0 0 0 3.9 14h8.2a1 1 0 0 0 .9-1.4L9.4 6.2V2"/><path d="M4.6 10h6.8"/></svg>`,
  };

  /* =====================================================================
     STATE
     ===================================================================== */
  const P = {
    built: false, visible: false, raf: 0,
    lib: [], libReady: false, autoLoaded: false,
    st: null,                 // full /api/backing state
    tel: null, telT: 0,       // telemetry `backing` + when it arrived
    peaks: null,              // { id, peak:Float32Array, rms:Float32Array }
    lastId: "", lastLoading: false, refreshing: false,
    libOpen: false,
  };
  const YT = { frame: null, hostReady: false, ready: false, pending: null, st: null, stT: 0, id: "", loop: { on: false, a: 0, b: 0 }, err: "", holdPause: 0 };
  const DI = { open: false, tr: null, tracks: [], env: new Map(), tel: null, built: false };

  /* =====================================================================
     TIMELINE (waveform / YouTube bar / DI) — click seeks, drag loops, flags adjust
     ===================================================================== */
  let theme = null;
  function themeColors() {
    const light = document.body.classList.contains("light");
    if (theme && theme.light === light) return theme;
    const cs = getComputedStyle(document.body);
    const v = (n, d) => (cs.getPropertyValue(n).trim() || d);
    theme = { light, acc: v("--accent", "#e3a04b"), idle: light ? "rgba(20,25,40,0.34)" : "rgba(220,224,232,0.30)", text: v("--text", "#ececef") };
    return theme;
  }
  function makeTimeline(host, o) {
    host.classList.add("pv-tl");
    if (o.flat) host.classList.add("flat");
    host.innerHTML = `<canvas aria-hidden="true"></canvas>
      <div class="pv-reg"><button class="pv-reg-x" title="Remove the loop" aria-label="Remove loop">${I.close}</button></div>
      <div class="pv-mk a" data-mk="a" title="Drag to move A"><span>A</span></div><div class="pv-mk b" data-mk="b" title="Drag to move B"><span>B</span></div>
      <div class="pv-ph"></div><div class="pv-hov"><span></span></div><div class="pv-tl-msg"></div>`;
    const c = host.querySelector("canvas"), reg = host.querySelector(".pv-reg"), mA = host.querySelector(".pv-mk.a"), mB = host.querySelector(".pv-mk.b");
    const ph = host.querySelector(".pv-ph"), hov = host.querySelector(".pv-hov"), hovT = hov.querySelector("span"), msg = host.querySelector(".pv-tl-msg");
    const T = { drag: null, draft: null, hx: -1 };
    const norm = (cx) => { const r = host.getBoundingClientRect(); return clamp((cx - r.left) / Math.max(1, r.width), 0, 1); };
    const minLen = () => Math.min(0.5, (o.dur() || 1) / 8);
    host.addEventListener("pointermove", (e) => { if (!T.drag) T.hx = e.clientX; });
    host.addEventListener("pointerleave", () => { if (!T.drag) T.hx = -1; });
    reg.querySelector(".pv-reg-x").addEventListener("click", (e) => { e.stopPropagation(); o.onClear(); });
    reg.querySelector(".pv-reg-x").addEventListener("pointerdown", (e) => e.stopPropagation());
    host.addEventListener("pointerdown", (e) => {
      const dur = o.dur();
      if (e.button !== 0 || !(dur > 0) || (o.enabled && !o.enabled())) return;
      e.preventDefault();
      host.setPointerCapture(e.pointerId);
      const mk = e.target.closest("[data-mk]");
      const lp = o.loop();
      T.drag = { mk: mk ? mk.dataset.mk : null, x0: e.clientX, n0: norm(e.clientX), moved: false, a: lp.a, b: lp.b, on: lp.on };
      const move = (me) => {
        const d = T.drag;
        if (!d) return;
        T.hx = me.clientX;
        if (Math.abs(me.clientX - d.x0) > 4) d.moved = true;
        if (!d.moved) return;
        const t = norm(me.clientX) * dur, m = minLen();
        if (d.mk === "a") T.draft = { a: clamp(t, 0, d.b - m), b: d.b, on: d.on };
        else if (d.mk === "b") T.draft = { a: d.a, b: clamp(t, d.a + m, dur), on: d.on };
        else { const s = d.n0 * dur; T.draft = { a: Math.min(s, t), b: Math.max(s, t), on: true }; }
      };
      const up = (ue) => {
        host.removeEventListener("pointermove", move); host.removeEventListener("pointerup", up); host.removeEventListener("pointercancel", up);
        const d = T.drag, draft = T.draft;
        T.drag = null; T.draft = null;
        if (!d) return;
        if (ue.type === "pointerup" && !(ue.clientX >= 0)) T.hx = -1;
        if (!d.moved) { if (!d.mk) o.onSeek(norm(ue.clientX) * dur); return; }
        if (!draft || draft.b - draft.a < minLen() * 0.99) return;
        o.onLoop(draft.a, draft.b, draft.on, !d.mk);
      };
      host.addEventListener("pointermove", move); host.addEventListener("pointerup", up); host.addEventListener("pointercancel", up);
    });
    const pct = (v) => `${(clamp(v, 0, 1) * 100).toFixed(3)}%`;
    function draw(pos) {
      const dur = o.dur() || 0;
      const lp = T.draft || o.loop();
      const has = dur > 0 && lp.b - lp.a > 0.02;
      host.classList.toggle("has-ab", has);
      host.classList.toggle("looping", has && !!lp.on);
      host.classList.toggle("empty", !(dur > 0));
      host.classList.toggle("dragging", !!T.drag);
      if (has) {
        reg.style.left = pct(lp.a / dur); reg.style.width = pct((lp.b - lp.a) / dur);
        mA.style.left = pct(lp.a / dur); mB.style.left = pct(lp.b / dur);
      }
      ph.style.left = dur ? pct(pos / dur) : "0%";
      if (T.hx >= 0 && dur > 0) {
        const r = host.getBoundingClientRect(), x = clamp(T.hx - r.left, 0, r.width);
        hov.style.display = "block";
        hov.style.left = `${x}px`;
        setText(hovT, fmtT(x / Math.max(1, r.width) * dur, dur < 600 ? 1 : 0));
        hov.classList.toggle("flip", x > r.width - 60);
      } else hov.style.display = "none";
      // canvas
      const dpr = window.devicePixelRatio || 1;
      const W = Math.round(c.clientWidth * dpr), H = Math.round(c.clientHeight * dpr);
      if (!W || !H) return;
      if (c.width !== W || c.height !== H) { c.width = W; c.height = H; }
      const x = c.getContext("2d");
      x.clearRect(0, 0, W, H);
      const th = themeColors();
      const playedN = dur ? pos / dur : 0;
      if (o.flat) {
        const bh = Math.round(6 * dpr), y = Math.round(H / 2 - bh / 2);
        const buf = o.buffered ? clamp(o.buffered(), 0, 1) : 0;
        x.fillStyle = th.idle; x.globalAlpha = 0.55; x.fillRect(0, y, W, bh);
        if (buf > 0) { x.globalAlpha = 0.5; x.fillRect(0, y, W * buf, bh); }
        x.globalAlpha = 1; x.fillStyle = th.acc; x.fillRect(0, y, W * clamp(playedN, 0, 1), bh);
        return;
      }
      const pk = o.peaks ? o.peaks() : null;
      const bw = Math.max(2, Math.round(2 * dpr)), gap = Math.max(1, Math.round(1.2 * dpr));
      const cols = Math.max(1, Math.floor(W / (bw + gap)));
      const mid = H / 2, amp = H / 2 - 3 * dpr;
      const loopOn = has && lp.on;
      for (let i = 0; i < cols; i++) {
        const n0 = i / cols, n1 = (i + 1) / cols, nc = (n0 + n1) / 2;
        let v = 0, r = 0;
        if (pk && pk.peak.length) {
          const L = pk.peak.length, a = Math.floor(n0 * L), b = Math.max(a + 1, Math.floor(n1 * L));
          for (let k = a; k < b && k < L; k++) { if (pk.peak[k] > v) v = pk.peak[k]; if (pk.rms[k] > r) r = pk.rms[k]; }
        }
        const played = dur > 0 && nc <= playedN;
        const inLoop = !loopOn || (nc * dur >= lp.a && nc * dur <= lp.b);
        x.fillStyle = played ? th.acc : th.idle;
        const al = inLoop ? 1 : 0.32;
        const xx = Math.round(i * (bw + gap));
        if (!pk) { x.globalAlpha = al * 0.6; x.fillRect(xx, mid - dpr * 0.5, bw, dpr); continue; }
        const hp = Math.max(dpr, v * amp), hr = Math.max(dpr, Math.min(hp, r * amp * 1.25));
        x.globalAlpha = al * (played ? 0.42 : 0.55);
        x.fillRect(xx, mid - hp, bw, hp * 2);
        x.globalAlpha = al;
        x.fillRect(xx, mid - hr, bw, hr * 2);
      }
      x.globalAlpha = 1;
    }
    return { draw, host, msg: (t) => setText(msg, t || ""), busy: (on) => host.classList.toggle("busy", !!on) };
  }
  function paintRuler(el, dur) {
    const w = el.clientWidth;
    if (el._d === dur && el._w === w) return;
    el._d = dur; el._w = w;
    if (!(dur > 0) || !w) { el.innerHTML = ""; return; }
    const steps = [1, 2, 5, 10, 15, 30, 60, 120, 300, 600];
    const step = steps.find((s) => dur / s <= Math.max(3, w / 64)) || 1200;
    let h = "";
    for (let s = 0; s <= dur + 1e-6; s += step) { const n = s / dur; if (n > 0.985 && s > 0) break; h += `<span style="left:${(n * 100).toFixed(3)}%">${fmtT(s)}</span>`; }
    el.innerHTML = h;
  }

  /* =====================================================================
     BUILD
     ===================================================================== */
  function build() {
    P.built = true;
    const box = $("view-practice");
    box.innerHTML = `<div class="pv" id="pv">
      <header class="pv-head">
        <div class="seg pv-mode" role="tablist" aria-label="Play along with">
          <button role="tab" data-mode="track">${I.wave}Backing track</button>
          <button role="tab" data-mode="yt">${I.yt}YouTube</button>
        </div>
        <div class="pv-head-r">
          <button class="text-btn pv-only-track pv-lib-btn" data-act="lib" title="Your backing tracks">${I.lib}<span>Library</span><b class="pv-n" id="pv-lib-n"></b>${I.chev}</button>
          <button class="text-btn pv-only-track" data-act="add" title="Add audio files (or drop them on the window)">${I.plus}Add tracks</button>
          <button class="icon-btn" data-act="more" title="More" aria-label="More">${I.more}</button>
        </div>
      </header>

      <section class="pv-body pv-track" id="pv-track">
        <div class="pv-main" id="pv-main">
          <div class="pv-deck">
            <div class="pv-meta">
              <div class="pv-titles"><h2 class="pv-title" id="pv-title">—</h2><div class="pv-sub" id="pv-sub"></div></div>
              <div class="pv-count" id="pv-count" aria-live="polite"></div>
            </div>
            <div class="pv-wave" id="pv-wave"></div>
            <div class="pv-ruler num" id="pv-ruler"></div>
            <div class="pv-trans">
              <button class="pv-play" id="pv-play" title="Play / pause (Space)" aria-label="Play">${I.play}</button>
              <div class="pv-clock num"><span id="pv-pos">0:00.0</span><small id="pv-dur"> / 0:00</small></div>
              <button class="pv-chip" id="pv-loop" data-act="loop" title="Loop A–B (L) — or drag across the waveform">${I.loop}<span>Loop</span><i class="pv-ab num" id="pv-ab"></i></button>
              <span class="pv-sp"></span>
              <label class="pv-vol" title="Track volume (double-click: 0 dB)">${I.vol}<input type="range" class="pv-range" id="pv-vol" min="0" max="1000" step="1" aria-label="Track volume"><span class="num" id="pv-vol-v">0.0</span><small>dB</small></label>
            </div>
          </div>
          <div class="pv-ctrls">
            <div class="pv-cell" id="pv-c-speed">
              <div class="pv-cell-h"><span>Speed</span><button class="pv-reset" data-act="speed1" title="Back to 100%">Reset</button></div>
              <div class="pv-val num" id="pv-speed-v">100%</div>
              <input type="range" class="pv-range" id="pv-speed" min="50" max="150" step="1" value="100" aria-label="Speed" title="Pitch stays the same">
            </div>
            <div class="pv-cell" id="pv-c-key">
              <div class="pv-cell-h"><span>Transpose</span><button class="pv-reset" data-act="semi0" title="Original key">Reset</button></div>
              <div class="pv-stepper"><button data-act="semi-" aria-label="Down a semitone">${I.minus}</button><div class="pv-val num" id="pv-semi-v">0</div><button data-act="semi+" aria-label="Up a semitone">${I.plus}</button></div>
            </div>
            <div class="pv-cell" id="pv-c-count">
              <div class="pv-cell-h"><span>Count-in</span><button class="pv-reset pv-tap" data-act="tap" title="Tap the tempo">Tap</button></div>
              <div class="seg pv-seg-s" id="pv-countin"><button data-ci="0">Off</button><button data-ci="4" title="One bar of clicks">1<span> bar</span></button><button data-ci="8" title="Two bars of clicks">2<span> bars</span></button></div>
              <div class="pv-bpm"><button data-act="bpm-" aria-label="Slower">${I.minus}</button><span class="num" id="pv-bpm-v">120</span><small>BPM</small><button data-act="bpm+" aria-label="Faster">${I.plus}</button></div>
            </div>
            <div class="pv-cell" id="pv-c-duck">
              <div class="pv-cell-h"><span>Duck</span><button class="switch" id="pv-duck" role="switch" aria-checked="false" aria-label="Duck the track while I play"></button></div>
              <div class="pv-note">Dips the track while you play</div>
              <div class="pv-duckmeter"><i id="pv-duck-m"></i></div>
            </div>
          </div>
        </div>
        <aside class="pv-lib" id="pv-lib" aria-label="Backing tracks">
          <div class="pv-lib-h"><span>Library</span><span class="pv-lib-c num" id="pv-lib-c"></span></div>
          <div class="pv-lib-list" id="pv-lib-list" role="listbox"></div>
          <button class="pv-lib-add" data-act="add">${I.plus}Add tracks</button>
        </aside>
        <div class="pv-empty" id="pv-empty" hidden>
          <div class="pv-drop" id="pv-drop">
            <div class="pv-drop-art">${I.wave}</div>
            <h3>Add a backing track</h3>
            <p>Drop audio files here or <button class="pv-link" data-act="add">choose files</button></p>
            <div class="pv-fmts">MP3 · WAV · AIFF · M4A · FLAC</div>
          </div>
          <form class="pv-empty-yt" data-form="yt"><span>${I.yt}</span><input type="text" placeholder="…or paste a YouTube link" spellcheck="false" autocomplete="off" aria-label="YouTube link"><button class="text-btn" type="submit">Play along</button></form>
        </div>
      </section>

      <section class="pv-body pv-yt" id="pv-yt" hidden>
        <form class="pv-yt-bar" data-form="yt"><span class="pv-yt-glyph">${I.yt}</span><input type="text" id="pv-yt-url" placeholder="Paste a YouTube link" spellcheck="false" autocomplete="off" aria-label="YouTube link"><button class="btn primary" type="submit">Load</button></form>
        <div class="pv-yt-main">
          <div class="pv-yt-video" id="pv-yt-video"><div class="pv-yt-ph" id="pv-yt-ph">${I.yt}<span id="pv-yt-msg">Paste a link to play along</span></div></div>
          <div class="pv-yt-side">
            <div class="pv-yt-now"><b id="pv-yt-title"></b><small id="pv-yt-author"></small></div>
            <div class="pv-yt-rh">Recent</div>
            <div class="pv-yt-recent" id="pv-yt-recent"></div>
          </div>
        </div>
        <div class="pv-deck pv-yt-deck" id="pv-yt-deck">
          <div class="pv-ytl" id="pv-ytl"></div>
          <div class="pv-ruler num" id="pv-yt-ruler"></div>
          <div class="pv-trans">
            <button class="pv-play" id="pv-yt-play" title="Play / pause (Space)" aria-label="Play">${I.play}</button>
            <div class="pv-clock num"><span id="pv-yt-pos">0:00.0</span><small id="pv-yt-dur"> / 0:00</small></div>
            <button class="pv-chip" id="pv-yt-loop" data-act="yt-loop" title="Loop A–B (L) — or drag across the timeline">${I.loop}<span>Loop</span><i class="pv-ab num" id="pv-yt-ab"></i></button>
            <span class="pv-sp"></span>
            <div class="seg pv-seg-s pv-rates" id="pv-yt-rates" title="Playback speed"></div>
            <label class="pv-vol pv-vol-s" title="Video volume">${I.vol}<input type="range" class="pv-range" id="pv-yt-vol" min="0" max="100" step="1" aria-label="Video volume"></label>
          </div>
        </div>
      </section>

      <div class="pv-sheet" id="pv-di" hidden>
        <div class="pv-sheet-card" role="dialog" aria-label="Test DI tracks">
          <div class="pv-sheet-h"><div><b>${I.flask}Test DI tracks</b><small>Built-in guitar recordings played into the rig as its input</small></div><button class="icon-btn" data-act="di-close" title="Close (Esc)" aria-label="Close">${I.close}</button></div>
          <div class="pv-di-top">
            <button class="pv-play sm" id="pv-di-play" title="Play / stop the DI track" aria-label="Play DI">${I.play}</button>
            <select id="pv-di-track" aria-label="DI track"></select>
            <div class="pv-clock sm num"><span id="pv-di-pos">0:00</span><small id="pv-di-dur"> / 0:00</small></div>
          </div>
          <div class="pv-ditl" id="pv-ditl"></div>
          <div class="pv-di-ctl">
            <button class="pv-chip" id="pv-di-loop" data-act="di-loop" title="Loop A–B on the DI track">${I.loop}<span>Loop</span></button>
            <label class="pv-di-s"><span>Speed</span><input type="range" class="pv-range" id="pv-di-speed" min="50" max="150" step="1"><b class="num" id="pv-di-speed-v">100%</b></label>
            <label class="pv-di-s"><span>Guitar cut</span><input type="range" class="pv-range" id="pv-di-cut" min="0" max="100" step="1"><b class="num" id="pv-di-cut-v">Off</b></label>
          </div>
        </div>
      </div>
    </div>`;

    P.wave = makeTimeline($("pv-wave"), {
      dur: () => (P.st && P.st.loaded ? P.st.duration : 0),
      loop: () => loopOf(),
      peaks: () => (P.peaks && P.st && P.peaks.id === P.st.id ? P.peaks : null),
      onSeek: (t) => seek(t),
      onLoop: (a, b, on, fresh) => setLoop(a, b, on, fresh),
      onClear: () => setLoop(0, 0, false),
    });
    P.ytl = makeTimeline($("pv-ytl"), {
      flat: true,
      enabled: () => !!(YT.st && YT.st.duration > 0),
      dur: () => (YT.st ? YT.st.duration : 0),
      buffered: () => (YT.st ? YT.st.buffered : 0),
      loop: () => YT.loop,
      onSeek: (t) => ytSeek(t),
      onLoop: (a, b, on, fresh) => ytSetLoop(a, b, on, fresh),
      onClear: () => ytSetLoop(0, 0, false),
    });

    const root = $("pv");
    root.addEventListener("click", onClick);
    root.addEventListener("submit", (e) => {
      const f = e.target.closest("[data-form='yt']");
      if (!f) return;
      e.preventDefault();
      const inp = f.querySelector("input");
      if (ytOpen(inp.value)) { inp.value = ""; inp.blur(); }
    });
    root.addEventListener("dblclick", (e) => {
      if (e.target.closest("#pv-vol")) setVolume(1, true);
      else if (e.target.closest("#pv-speed")) setSpeed(1, true);
    });
    $("pv-vol").addEventListener("input", (e) => setVolume(volFromSlider(+e.target.value / 1000)));
    $("pv-speed").addEventListener("input", (e) => setSpeed(+e.target.value / 100));
    $("pv-yt-vol").addEventListener("input", (e) => { prefs.ytVol = +e.target.value; savePrefs(); ytCall("setVolume", [prefs.ytVol]); if (prefs.ytVol > 0) ytCall("unMute"); });
    $("pv-di-speed").addEventListener("input", (e) => diSet(`speed=${(+e.target.value / 100).toFixed(2)}`, true));
    $("pv-di-cut").addEventListener("input", (e) => diSet(`stem_cut=${(+e.target.value / 100).toFixed(2)}`, true));
    $("pv-di-track").addEventListener("change", (e) => { if (typeof engineCmd === "function") engineCmd(`/api/transport?action=track&idx=${e.target.value}&play=1`); });
    // Files dragged onto the page itself (only happens outside the app, where there's no native drop): explain.
    root.addEventListener("dragover", (e) => { if (e.dataTransfer && [...e.dataTransfer.types].includes("Files")) { e.preventDefault(); root.classList.add("drop-hover"); } });
    root.addEventListener("dragleave", (e) => { if (!root.contains(e.relatedTarget)) root.classList.remove("drop-hover"); });
    root.addEventListener("drop", (e) => { if (e.dataTransfer && [...e.dataTransfer.types].includes("Files")) { e.preventDefault(); root.classList.remove("drop-hover"); say("Drop audio files on the Headroom app window to add them."); } });
    document.addEventListener("pointerdown", (e) => {
      if (P.libOpen && !e.target.closest("#pv-lib") && !e.target.closest("[data-act='lib']")) toggleLib(false);
    }, true);

    DI.timeline = makeTimeline($("pv-ditl"), {
      dur: () => (DI.tel && DI.tel.duration_sec > 0 ? DI.tel.duration_sec : 0),
      loop: () => { const d = DI.tel && DI.tel.duration_sec > 0 ? DI.tel.duration_sec : 0, tr = DI.tr; return tr && d && (tr.ab_loop || tr.loop_b - tr.loop_a < 0.985) ? { on: !!tr.ab_loop, a: tr.loop_a * d, b: tr.loop_b * d } : { on: false, a: 0, b: 0 }; },
      peaks: () => DI.env.get(diKey()) || null,
      onSeek: (t) => { const d = DI.tel && DI.tel.duration_sec; if (d) lazy("seek", `/api/transport?action=seek&pos=${(t / d).toFixed(4)}`); },
      onLoop: (a, b) => { const d = DI.tel.duration_sec; diSet(`loop_a=${(a / d).toFixed(4)}&loop_b=${(b / d).toFixed(4)}&ab=1`).then(() => lazy("seek", `/api/transport?action=seek&pos=${(a / d).toFixed(4)}`)); },
      onClear: () => diSet("action=clear_ab"),
    });
    paintMode();
    paintControls();
    paintYtRecent();
  }

  /* =====================================================================
     BACKING TRACK — engine calls
     ===================================================================== */
  const loopOf = () => (P.st && P.st.loop ? P.st.loop : { on: false, a: 0, b: 0 });
  function applyState(d) {
    if (!d || d.ok === false || typeof d.volume !== "number") return d;
    const prevId = P.st && P.st.id;
    P.st = d;
    if (d.loaded && d.id !== prevId) onTrackChanged();
    if (P.built) paintControls();
    return d;
  }
  async function cmd(params) {
    try { return applyState(await api(`/api/backing?${typeof params === "string" ? params : q(params)}`)); }
    catch (e) { say("Couldn’t reach the audio engine."); return null; }
  }
  const cmdLater = (key, params) => lazy(`bt-${key}`, `/api/backing?${q(params)}`, (d) => applyState(d));
  async function refresh() {
    if (P.refreshing) return;
    P.refreshing = true;
    try {
      const [list, st] = await Promise.all([api("/api/backing/list"), api("/api/backing")]);
      if (list && Array.isArray(list.tracks)) { P.lib = list.tracks; P.libReady = true; }
      applyState(st);
      if (P.st && P.st.ok === false) P.st = null;
      // First visit this session: re-send your saved mix settings (the engine forgets them on restart).
      if (!P.primed && P.st) {
        P.primed = true;
        if (Math.abs(P.st.volume - prefs.volume) > 1e-3 || P.st.duck.on !== !!prefs.duck || P.st.count_in.beats !== prefs.countin || P.st.normalize !== !!prefs.normalize)
          await cmd({ volume: prefs.volume.toFixed(4), duck: prefs.duck ? 1 : 0, countin: prefs.countin, normalize: prefs.normalize ? 1 : 0 });
      }
      if (P.st && !P.st.loaded && !P.st.loading && P.lib.length && !P.autoLoaded) { P.autoLoaded = true; loadTrack(P.lib[0].id, false); }
      if (P.st && P.st.loaded && (!P.peaks || P.peaks.id !== P.st.id)) fetchPeaks();
    } catch (e) { /* engine offline: keep what we have */ }
    P.refreshing = false;
    paintAll();
  }
  async function fetchPeaks() {
    const id = P.st && P.st.id;
    if (!id) return;
    try {
      const r = await api("/api/backing/peaks?bins=1600");
      if (r && r.ok && r.id === id) P.peaks = { id, peak: Float32Array.from(r.peak), rms: Float32Array.from(r.rms) };
    } catch (e) { /* keep */ }
  }
  function onTrackChanged() {
    P.peaks = null;
    fetchPeaks();
    api("/api/backing/list").then((l) => { if (l && Array.isArray(l.tracks)) { P.lib = l.tracks; paintLib(); } }).catch(() => {});
  }
  // Loading a track applies what you last used with it: loop, speed, key and count-in tempo.
  function loadTrack(id, play) {
    const tp = trackPrefs[id] || {};
    const bpm = tp.bpm || (window.HRTempo && HRTempo.get ? Math.round(HRTempo.get()) : 0) || 120;
    const loopOn = !!tp.loop && tp.b > tp.a;
    if (P.st && P.st.playing && P.st.id !== id) play = true; // switching while playing keeps playing
    P.peaks = null;
    if (P.st) { P.st.loading = true; P.st.loading_id = id; }
    paintAll();
    cmd({ action: "load", id, play: play ? 1 : 0, at: loopOn ? tp.a : 0, speed: tp.speed || 1, semitones: tp.semis || 0,
      loop: loopOn ? 1 : 0, a: tp.a || 0, b: tp.b || 0, bpm }).then(() => paintAll());
    if (play) ytCall("pauseVideo");
  }
  function rememberTrack(patch) {
    const id = P.st && P.st.id;
    if (!id) return;
    trackPrefs[id] = Object.assign(trackPrefs[id] || {}, patch);
    save(LS.tracks, trackPrefs);
  }
  function playPause() {
    if (!P.st || !P.st.loaded) { if (P.lib.length) loadTrack(P.lib[0].id, true); return; }
    const playing = livePlaying();
    if (!playing) ytCall("pauseVideo");
    if (P.tel) { P.tel.playing = !playing; P.telT = now(); }
    paintLib(); paintLed();
    cmd({ action: playing ? "pause" : "play" });
  }
  function seek(t) {
    if (!P.st || !P.st.loaded) return;
    t = clamp(t, 0, P.st.duration);
    if (P.tel) { P.tel.pos = t; P.telT = now(); }
    P.st.position = t;
    cmdLater("seek", { action: "seek", sec: t.toFixed(3) });
  }
  function setLoop(a, b, on, fresh) {
    if (!P.st || !P.st.loaded) return;
    P.st.loop = { on: !!on && b - a > 0.05, a, b };
    rememberTrack({ loop: P.st.loop.on, a, b });
    const params = { loop: P.st.loop.on ? 1 : 0, a: a.toFixed(3), b: b.toFixed(3) };
    if (fresh && P.st.loop.on) { params.action = "seek"; params.sec = a.toFixed(3); if (P.tel) { P.tel.pos = a; P.telT = now(); } }
    cmd(params);
    paintControls();
  }
  function toggleLoop() {
    if (!P.st || !P.st.loaded) return;
    const lp = loopOf();
    if (lp.b - lp.a > 0.05) { setLoop(lp.a, lp.b, !lp.on); return; }
    // no section yet: loop the next 8 seconds from the playhead
    const t = curPos(), d = P.st.duration;
    const a = clamp(t, 0, Math.max(0, d - 1)), b = Math.min(d, a + 8);
    setLoop(a, b, true, false);
  }
  function setLoopPoint(which) {
    if (!P.st || !P.st.loaded) return;
    const lp = loopOf(), t = curPos(), d = P.st.duration;
    let a = lp.b - lp.a > 0.05 ? lp.a : 0, b = lp.b - lp.a > 0.05 ? lp.b : d;
    if (which === "a") a = Math.min(t, b - 0.25); else b = Math.max(t, a + 0.25);
    setLoop(clamp(a, 0, d), clamp(b, 0, d), which === "b" ? true : lp.on, false);
  }
  // Volume slider: 0 → silent, ~86 % → 0 dB, 100 % → +6 dB (a dB taper, like a fader)
  const volFromSlider = (p) => (p <= 0.001 ? 0 : Math.pow(10, (p * 42 - 36) / 20));
  const sliderFromVol = (g) => (g <= 0.0005 ? 0 : clamp((20 * Math.log10(g) + 36) / 42, 0, 1));
  function setVolume(g, now_) {
    g = clamp(g, 0, 2);
    prefs.volume = g; savePrefs();
    if (P.st) P.st.volume = g;
    paintControls();
    if (now_) cmd({ volume: g.toFixed(4) }); else cmdLater("vol", { volume: g.toFixed(4) });
  }
  function setSpeed(s, now_) {
    s = clamp(Math.round(s * 100) / 100, 0.5, 1.5);
    if (P.st) P.st.speed = s;
    rememberTrack({ speed: s });
    paintControls();
    if (now_) cmd({ speed: s }); else cmdLater("speed", { speed: s });
  }
  function setSemis(v) {
    v = clamp(Math.round(v), -12, 12);
    if (P.st) P.st.semitones = v;
    rememberTrack({ semis: v });
    paintControls();
    cmdLater("semis", { semitones: v });
  }
  function setBpm(v) {
    v = clamp(Math.round(v), 40, 260);
    if (P.st) P.st.count_in.bpm = v;
    rememberTrack({ bpm: v });
    paintControls();
    cmdLater("bpm", { bpm: v });
  }
  const taps = [];
  function tap() {
    const t = now();
    if (taps.length && t - taps[taps.length - 1] > 2000) taps.length = 0;
    taps.push(t);
    if (taps.length > 6) taps.shift();
    if (taps.length >= 2) setBpm(60000 / ((taps[taps.length - 1] - taps[0]) / (taps.length - 1)));
    const b = document.querySelector(".pv-tap");
    if (b) { b.classList.remove("flash"); void b.offsetWidth; b.classList.add("flash"); }
  }
  function setCountIn(beats) {
    prefs.countin = beats; savePrefs();
    if (P.st) P.st.count_in.beats = beats;
    paintControls();
    cmd({ countin: beats, bpm: P.st ? P.st.count_in.bpm : 120 });
  }
  function setDuck(on) {
    prefs.duck = on; savePrefs();
    if (P.st) P.st.duck.on = on;
    paintControls();
    cmd({ duck: on ? 1 : 0 });
  }
  function livePlaying() { return !!(P.tel ? P.tel.playing : P.st && P.st.playing); }
  function curPos() {
    const t = P.tel, st = P.st;
    if (!st || !st.loaded) return 0;
    if (!t || t.id !== st.id) return st.position || 0;
    let p = t.pos;
    if (t.playing && !t.count && !t.loading) {
      p += (now() - P.telT) / 1000 * (t.speed || st.speed || 1);
      const lp = st.loop;
      if (lp && lp.on && lp.b > lp.a && p > lp.b) p = lp.a + ((p - lp.b) % (lp.b - lp.a));
    }
    return clamp(p, 0, st.duration || 0);
  }

  /* ---------- library ---------- */
  async function importPaths(paths) {
    paths = (paths || []).filter((p) => typeof p === "string" && p);
    if (!paths.length) return;
    if (window.HRViews && HRViews.current() !== "practice") HRViews.show("practice");
    if (prefs.mode !== "track") setMode("track");
    say(paths.length > 1 ? `Adding ${paths.length} tracks…` : "Adding the track…");
    try {
      const r = await api("/api/backing/import", { method: "POST", headers: { "Content-Type": "text/plain" }, body: JSON.stringify({ paths }) });
      const ok = (r && r.imported) || [], bad = (r && r.failed) || [];
      if (ok.length) {
        say(bad.length ? `Added ${ok.length} · ${bad.length} couldn’t be read` : ok.length === 1 ? `Added “${ok[0].title}”` : `Added ${ok.length} tracks`);
        await refresh();
        if (!livePlaying()) loadTrack(ok[0].id, false); // never swap out a track you're playing along with
      } else say(bad.length ? (bad.length === 1 ? `Couldn’t add “${bad[0].path.split("/").pop()}”: ${bad[0].error}` : `None of those files could be read as audio.`) : "Nothing to add.");
    } catch (e) { say("Couldn’t reach the audio engine."); }
  }
  function chooseFiles() {
    if (hasBridge() && bridge({ action: "importBackingTracks" })) return;
    say("Adding tracks works in the Headroom app — pick them there, or drop audio files on its window.");
  }
  async function renameTrack(t) {
    if (typeof promptDialog !== "function") return;
    const v = await promptDialog("Rename track", "Title", t.title, "Rename");
    if (v == null || !v.trim() || v.trim() === t.title) return;
    const r = await api(`/api/backing/rename?${q({ id: t.id, title: v.trim() })}`).catch(() => null);
    if (!r || !r.ok) { say("Couldn’t rename that track."); return; }
    refresh();
  }
  async function deleteTrack(t) {
    const ok = typeof confirmDialog === "function"
      ? await confirmDialog(`Delete “${t.title}”?`, "Its copy in your Backing Tracks folder is removed. The original file isn’t touched.", "Delete", true)
      : true;
    if (!ok) return;
    const r = await api(`/api/backing/delete?${q({ id: t.id })}`).catch(() => null);
    if (!r || !r.ok) { say("Couldn’t delete that track."); return; }
    delete trackPrefs[t.id]; save(LS.tracks, trackPrefs);
    if (P.st && P.st.id === t.id) { P.st.loaded = false; P.st.id = ""; P.peaks = null; P.autoLoaded = false; }
    refresh();
  }
  function trackMenu(t, x, y) {
    if (typeof showMenu !== "function") return;
    showMenu(x, y, [
      { label: "Play", onClick: () => loadTrack(t.id, true) },
      { label: "Rename…", onClick: () => renameTrack(t) },
      { sep: true },
      { label: "Delete…", danger: true, onClick: () => deleteTrack(t) },
    ]);
  }
  function toggleLib(on) {
    P.libOpen = on == null ? !P.libOpen : !!on;
    const l = $("pv-lib");
    if (l) l.classList.toggle("open", P.libOpen);
    const b = document.querySelector(".pv-lib-btn");
    if (b) b.classList.toggle("on", P.libOpen);
  }
  function moreMenu(x, y) {
    if (typeof showMenu !== "function") return;
    const items = [{ h: "Backing tracks" }];
    if (hasBridge()) items.push({ label: "Show tracks folder", onClick: () => bridge({ action: "revealBackingFolder" }) });
    items.push({ label: "Match track loudness", sub: prefs.normalize ? "On" : "Off", onClick: () => { prefs.normalize = !prefs.normalize; savePrefs(); cmd({ normalize: prefs.normalize ? 1 : 0 }); say(prefs.normalize ? "Tracks are levelled to sit with the rig" : "Tracks play at their own level"); } });
    if (P.st && P.st.loaded) items.push({ label: "Unload track", onClick: () => { P.autoLoaded = true; cmd({ action: "unload" }).then(() => { P.peaks = null; paintAll(); }); } });
    items.push({ sep: true }, { label: "Test DI tracks…", sub: "Developer", onClick: () => openDI(true) });
    showMenu(x, y, items);
  }

  /* =====================================================================
     YOUTUBE
     ===================================================================== */
  function parseYouTube(s) {
    s = String(s || "").trim();
    if (!s) return null;
    if (/^[\w-]{11}$/.test(s)) return { id: s, start: 0 };
    let u;
    try { u = new URL(/^https?:\/\//i.test(s) ? s : `https://${s}`); } catch (e) { return null; }
    const host = u.hostname.replace(/^www\.|^m\.|^music\./, "");
    let id = "";
    if (host === "youtu.be") id = u.pathname.slice(1, 12);
    else if (/(^|\.)youtube(-nocookie)?\.com$/.test(host)) {
      if (u.pathname === "/watch") id = u.searchParams.get("v") || "";
      else { const m = u.pathname.match(/^\/(?:embed|shorts|live|v)\/([\w-]{11})/); if (m) id = m[1]; }
    }
    if (!/^[\w-]{11}$/.test(id)) return null;
    const t = u.searchParams.get("t") || u.searchParams.get("start") || "";
    let start = 0;
    const m = String(t).match(/^(?:(\d+)h)?(?:(\d+)m)?(?:(\d+)s?)?$/);
    if (m) start = (+m[1] || 0) * 3600 + (+m[2] || 0) * 60 + (+m[3] || 0);
    return { id, start };
  }
  function ytOpen(text) {
    const v = parseYouTube(text);
    if (!v) { say("That doesn’t look like a YouTube link."); return false; }
    if (prefs.mode !== "yt") setMode("yt");
    const rec = ytRecent.find((r) => r.id === v.id);
    ytLoad(v.id, v.start || (rec && rec.pos > 5 && rec.dur && rec.pos < rec.dur - 10 ? rec.pos : 0));
    return true;
  }
  function ensureFrame() {
    if (YT.frame) return;
    const box = $("pv-yt-video");
    const f = document.createElement("iframe");
    f.id = "pv-yt-frame";
    f.title = "YouTube player";
    f.allow = "autoplay; encrypted-media; picture-in-picture";
    f.referrerPolicy = "strict-origin-when-cross-origin";
    f.src = `${base()}/practice/youtube.html`;
    box.appendChild(f);
    YT.frame = f;
  }
  function ytPost(m) {
    if (!YT.frame || !YT.frame.contentWindow) return;
    m.hr = "yt";
    try { YT.frame.contentWindow.postMessage(m, "*"); } catch (e) { /* not loaded yet */ }
  }
  function ytCall(fn, args) { if (YT.ready) ytPost({ cmd: "call", fn, args: args || [] }); }
  function ytLoad(id, start) {
    ensureFrame();
    YT.id = id; YT.err = ""; YT.st = null;
    const rec = ytRecent.find((r) => r.id === id);
    const loop = rec && rec.loop && rec.b > rec.a ? { on: true, a: rec.a, b: rec.b } : { on: false, a: 0, b: 0 };
    YT.loop = loop;
    const msg = { cmd: "load", videoId: id, start: start || 0, autoplay: false };
    if (YT.hostReady) { ytPost(msg); ytPost({ cmd: "loop", ...loop }); } else YT.pending = msg;
    prefs.ytLast = id; savePrefs();
    ytRemember({ id, t: Date.now() });
    paintYt();
  }
  function ytRemember(patch) {
    const i = ytRecent.findIndex((r) => r.id === patch.id);
    const e = Object.assign(i >= 0 ? ytRecent[i] : { id: patch.id, title: "", author: "" }, patch);
    if (i >= 0) ytRecent.splice(i, 1);
    if (patch.t) ytRecent.unshift(e); else ytRecent.splice(Math.max(0, i), 0, e);
    ytRecent = ytRecent.slice(0, 12);
    save(LS.yt, ytRecent);
  }
  window.addEventListener("message", (e) => {
    if (!YT.frame || e.source !== YT.frame.contentWindow) return;
    const m = e.data;
    if (!m || m.hr !== "yt") return;
    if (m.type === "host") {
      YT.hostReady = true;
      if (YT.pending) { const p = YT.pending; YT.pending = null; ytPost(p); ytPost({ cmd: "loop", ...YT.loop }); }
    } else if (m.type === "ready") {
      YT.ready = true;
      ytCall("setVolume", [prefs.ytVol]);
      if (prefs.ytRate && prefs.ytRate !== 1) ytCall("setPlaybackRate", [prefs.ytRate]);
    } else if (m.type === "state") {
      const was = YT.st;
      YT.st = m; YT.stT = now();
      if (YT.holdPause && (m.state === 1 || m.state === 3)) {
        if (now() - YT.holdPause < 4000) ytCall("pauseVideo"); else YT.holdPause = 0;
      } else if (m.state === 2) YT.holdPause = 0;
      if (m.state === 1 && (!was || was.state !== 1) && !YT.holdPause) { YT.err = ""; if (P.st && livePlaying()) cmd({ action: "pause" }); }
      if (m.videoId && m.videoId === YT.id) {
        const rec = ytRecent.find((r) => r.id === m.videoId);
        if (rec && ((m.title && rec.title !== m.title) || (m.duration && Math.abs((rec.dur || 0) - m.duration) > 1) || Math.abs((rec.pos || 0) - m.time) > 3)) {
          rec.title = m.title || rec.title; rec.author = m.author || rec.author; rec.dur = m.duration || rec.dur; rec.pos = m.time;
          save(LS.yt, ytRecent);
          paintYtRecent();
        }
      }
      paintYt();
    } else if (m.type === "error") {
      const msgs = { 2: "That link doesn’t point to a video.", 5: "This video can’t play in an embedded player.", 100: "Video not found — it may be private or removed.",
        101: "The owner doesn’t allow this video to be played outside YouTube.", 150: "The owner doesn’t allow this video to be played outside YouTube.",
        152: "YouTube refused the embedded player (error 152).", 153: "YouTube refused the embedded player (error 153).", offline: "Couldn’t reach YouTube — check your connection." };
      YT.err = msgs[m.code] || `YouTube error ${m.code}`;
      paintYt();
    }
  });
  const ytPlaying = () => !!(YT.st && (YT.st.state === 1 || YT.st.state === 3));
  function ytPos() {
    const s = YT.st;
    if (!s) return 0;
    let t = s.time || 0;
    if (s.state === 1) t += (now() - YT.stT) / 1000 * (s.rate || 1);
    return clamp(t, 0, s.duration || t);
  }
  function ytPlayPause() {
    if (!YT.ready || !YT.st) return;
    if (ytPlaying()) ytCall("pauseVideo");
    else { YT.holdPause = 0; if (P.st && livePlaying()) cmd({ action: "pause" }); ytCall("playVideo"); }
  }
  function ytSeek(t) {
    // YouTube starts a cued / unstarted video when you seek it: keep it paused unless it was already playing.
    if (YT.st && !ytPlaying()) YT.holdPause = now();
    if (YT.st) { YT.st.time = t; YT.stT = now(); }
    ytCall("seekTo", [t, true]);
  }
  function ytSetLoop(a, b, on, fresh) {
    YT.loop = { on: !!on && b - a > 0.2, a, b };
    ytPost({ cmd: "loop", ...YT.loop });
    if (YT.id) ytRemember({ id: YT.id, loop: YT.loop.on, a, b });
    if (fresh && YT.loop.on) ytSeek(a);
    paintYt();
  }
  function ytToggleLoop() {
    if (!YT.st || !(YT.st.duration > 0)) return;
    const lp = YT.loop;
    if (lp.b - lp.a > 0.2) { ytSetLoop(lp.a, lp.b, !lp.on); return; }
    const a = ytPos();
    ytSetLoop(a, Math.min(YT.st.duration, a + 8), true);
  }
  function ytLoopPoint(which) {
    if (!YT.st || !(YT.st.duration > 0)) return;
    const lp = YT.loop, t = ytPos(), d = YT.st.duration;
    let a = lp.b - lp.a > 0.2 ? lp.a : 0, b = lp.b - lp.a > 0.2 ? lp.b : d;
    if (which === "a") a = Math.min(t, b - 0.5); else b = Math.max(t, a + 0.5);
    ytSetLoop(a, b, which === "b" ? true : lp.on);
  }

  /* =====================================================================
     TEST DI TRACKS (trainer on the built-in DI player)
     ===================================================================== */
  const diKey = () => (DI.tel ? `${DI.tel.track_idx}:${(+DI.tel.duration_sec || 0).toFixed(2)}` : "");
  async function openDI(on) {
    DI.open = !!on;
    const s = $("pv-di");
    if (!s) return;
    s.hidden = !DI.open;
    if (!DI.open) return;
    try {
      const [tr, tl] = await Promise.all([api("/api/trainer"), api("/api/tracks")]);
      if (tr && typeof tr.speed === "number") DI.tr = tr;
      if (tl && Array.isArray(tl.tracks)) DI.tracks = tl.tracks;
    } catch (e) { /* offline */ }
    paintDITracks();
    paintDI();
  }
  async function diSet(qs, lazy_) {
    if (lazy_) {
      // optimistic while dragging a slider
      const m = qs.match(/^(speed|stem_cut)=([\d.]+)/);
      if (m && DI.tr) DI.tr[m[1]] = +m[2];
      paintDI();
      lazy("trainer", `/api/trainer?${qs}`, (d) => { if (d && typeof d.speed === "number") { DI.tr = d; paintDI(); } });
      return null;
    }
    try { const d = await api(`/api/trainer?${qs}`); if (d && typeof d.speed === "number") { DI.tr = d; paintDI(); } return d; } catch (e) { return null; }
  }
  function diEnvelope() {
    const t = DI.tel;
    if (!t || !(t.duration_sec > 0)) return;
    const k = diKey();
    if (DI.env.has(k)) return;
    DI.env.set(k, null);
    api(`/api/tracks/envelope?idx=${t.track_idx}&bins=800`).then((r) => {
      if (!r || !r.ok || !Array.isArray(r.bins)) return;
      let top = 1e-6;
      r.bins.forEach((v) => { if (v > top) top = v; });
      const pk = Float32Array.from(r.bins, (v) => v / top);
      DI.env.set(k, { peak: pk, rms: Float32Array.from(pk, (v) => v * 0.55) });
    }).catch(() => DI.env.delete(k));
  }
  function paintDITracks() {
    const sel = $("pv-di-track");
    if (!sel || !DI.tracks.length) return;
    const pretty = (t) => (typeof prettyTrack === "function" ? prettyTrack(t) : t);
    const groups = [];
    DI.tracks.forEach((t) => { const g = t.group || "Built-in"; let e = groups.find((x) => x.g === g); if (!e) groups.push((e = { g, list: [] })); e.list.push(t); });
    const opt = (t) => `<option value="${t.idx}">${escH(pretty(t.title))}${t.duration ? ` · ${fmtT(t.duration)}` : ""}</option>`;
    sel.innerHTML = groups.length > 1 ? groups.map((e) => `<optgroup label="${escH(e.g)}">${e.list.map(opt).join("")}</optgroup>`).join("") : DI.tracks.map(opt).join("");
    if (DI.tel) sel.value = String(DI.tel.track_idx);
  }
  function paintDI() {
    if (!DI.open) return;
    const tr = DI.tr || { speed: 1, stem_cut: 0, ab_loop: false, loop_a: 0, loop_b: 1 };
    const sp = Math.round(tr.speed * 100), cut = Math.round((tr.stem_cut || 0) * 100);
    const s = $("pv-di-speed"), c = $("pv-di-cut");
    if (document.activeElement !== s) s.value = String(sp);
    if (document.activeElement !== c) c.value = String(cut);
    s.style.setProperty("--p", `${sp - 50}%`);
    c.style.setProperty("--p", `${cut}%`);
    setText($("pv-di-speed-v"), `${sp}%`);
    setText($("pv-di-cut-v"), cut ? `${cut}%` : "Off");
    $("pv-di-loop").classList.toggle("on", !!tr.ab_loop);
  }
  function drawDI() {
    const t = DI.tel;
    const dur = t && t.duration_sec > 0 ? t.duration_sec : 0;
    const pos = t ? +t.position_sec || 0 : 0;
    const pb = $("pv-di-play"), playing = !!(t && t.playing);
    if (pb._p !== playing) { pb._p = playing; pb.innerHTML = playing ? I.pause : I.play; pb.classList.toggle("playing", playing); pb.setAttribute("aria-label", playing ? "Stop DI" : "Play DI"); }
    setText($("pv-di-pos"), fmtT(pos));
    setText($("pv-di-dur"), ` / ${fmtT(dur)}`);
    const sel = $("pv-di-track");
    if (t && document.activeElement !== sel && sel.options.length && sel.value !== String(t.track_idx)) sel.value = String(t.track_idx);
    diEnvelope();
    DI.timeline.msg(t && t.source === "live" ? "The DI player is off — Audio settings has the input on your interface" : "");
    DI.timeline.draw(pos);
  }

  /* =====================================================================
     PAINT
     ===================================================================== */
  function setMode(m) {
    const prev = prefs.mode;
    prefs.mode = m === "yt" ? "yt" : "track";
    savePrefs();
    if (prev !== prefs.mode) {
      if (prefs.mode === "yt" && livePlaying()) cmd({ action: "pause" });
      if (prefs.mode === "track" && ytPlaying()) ytCall("pauseVideo");
      toggleLib(false);
    }
    paintMode();
    if (prefs.mode === "yt") {
      if (!YT.id && prefs.ytLast) ytLoad(prefs.ytLast, (ytRecent.find((r) => r.id === prefs.ytLast) || {}).pos || 0);
      paintYt();
    }
    paintLed();
  }
  function paintMode() {
    if (!P.built) return;
    const yt = prefs.mode === "yt";
    document.querySelectorAll("#pv .pv-mode [data-mode]").forEach((b) => { const on = (b.dataset.mode === "yt") === yt; b.classList.toggle("on", on); b.setAttribute("aria-selected", on); });
    $("pv-track").hidden = yt;
    $("pv-yt").hidden = !yt;
    $("pv").classList.toggle("mode-yt", yt);
  }
  function paintAll() { paintLib(); paintControls(); paintEmpty(); }
  function paintEmpty() {
    if (!P.built) return;
    const empty = P.libReady && !P.lib.length && !(P.st && (P.st.loaded || P.st.loading));
    $("pv-empty").hidden = !empty;
    $("pv-main").hidden = empty;
    $("pv-lib").hidden = empty;
    $("pv").classList.toggle("lib-empty", empty);
  }
  function paintLib() {
    if (!P.built) return;
    setText($("pv-lib-c"), P.lib.length ? String(P.lib.length) : "");
    setText($("pv-lib-n"), P.lib.length ? String(P.lib.length) : "");
    const cur = P.st && (P.st.loading ? P.st.loading_id : P.st.loaded ? P.st.id : "");
    const playing = livePlaying();
    const html = P.lib.map((t) => {
      const on = t.id === cur;
      const sub = [t.artist, fmtT(t.duration), t.format].filter(Boolean).join(" · ");
      return `<div class="pv-li${on ? " on" : ""}${on && playing ? " playing" : ""}" data-id="${escH(t.id)}" role="option" aria-selected="${on}" tabindex="0" title="${escH(t.title)}">
        <span class="pv-li-ico">${on && playing ? `<i></i><i></i><i></i>` : I.play}</span>
        <span class="pv-li-t"><b>${escH(t.title)}</b><small>${escH(sub)}</small></span>
        <button class="pv-li-more" data-more="${escH(t.id)}" title="Rename, delete…" aria-label="Track options">${I.more}</button>
      </div>`;
    }).join("");
    setHTML($("pv-lib-list"), html || `<div class="pv-lib-none">No tracks yet</div>`);
  }
  function paintControls() {
    if (!P.built) return;
    const st = P.st, loaded = !!(st && st.loaded);
    $("pv").classList.toggle("no-track", !loaded);
    // volume
    const g = st ? st.volume : prefs.volume;
    const vr = $("pv-vol"), vp = sliderFromVol(g);
    if (document.activeElement !== vr) vr.value = String(Math.round(vp * 1000));
    vr.style.setProperty("--p", `${(vp * 100).toFixed(1)}%`);
    setText($("pv-vol-v"), dbOf(g));
    // speed
    const sp = Math.round((st ? st.speed : 1) * 100);
    const sr = $("pv-speed");
    if (document.activeElement !== sr) sr.value = String(sp);
    sr.style.setProperty("--p", `${sp - 50}%`);
    setText($("pv-speed-v"), `${sp}%`);
    $("pv-c-speed").classList.toggle("changed", sp !== 100);
    // transpose
    const se = st ? Math.round(st.semitones) : 0;
    setText($("pv-semi-v"), se ? `${se > 0 ? "+" : "−"}${Math.abs(se)}` : "0");
    $("pv-c-key").classList.toggle("changed", se !== 0);
    // count-in
    const ci = st ? st.count_in.beats : prefs.countin;
    document.querySelectorAll("#pv-countin [data-ci]").forEach((b) => b.classList.toggle("on", +b.dataset.ci === ci));
    setText($("pv-bpm-v"), String(Math.round(st ? st.count_in.bpm : 120)));
    $("pv-c-count").classList.toggle("off", !ci);
    // duck
    const dk = st ? st.duck.on : prefs.duck;
    const sw = $("pv-duck");
    sw.classList.toggle("on", dk); sw.setAttribute("aria-checked", dk);
    $("pv-c-duck").classList.toggle("off", !dk);
    // loop chip
    const lp = loopOf(), has = loaded && lp.b - lp.a > 0.05;
    $("pv-loop").classList.toggle("on", has && lp.on);
    setText($("pv-ab"), has ? `${fmtT(lp.a)}–${fmtT(lp.b)}` : "");
    paintLed();
  }
  function draw() {
    if (prefs.mode === "yt") drawYt(); else drawTrack();
    if (DI.open) drawDI();
  }
  function drawTrack() {
    const st = P.st, loaded = !!(st && st.loaded);
    const loading = !!(st && st.loading) || !!(P.tel && P.tel.loading);
    const pos = curPos();
    const dur = loaded ? st.duration : 0;
    const t = loaded ? P.lib.find((x) => x.id === st.id) : null;
    const lt = loading ? P.lib.find((x) => x.id === (st && st.loading_id)) : null;
    setText($("pv-title"), loaded ? st.title : lt ? lt.title : loading ? "Loading…" : "No track loaded");
    setText($("pv-sub"), loaded ? [st.artist, st.format, fmtT(dur), Math.round(st.speed * 100) !== 100 ? `at ${Math.round(st.speed * 100)}%` : "", st.semitones ? `${st.semitones > 0 ? "+" : "−"}${Math.abs(st.semitones)} st` : ""].filter(Boolean).join(" · ") : lt ? "Loading…" : P.lib.length ? "Pick one from the library" : "");
    const playing = loaded && livePlaying();
    const pb = $("pv-play");
    if (pb._p !== playing) { pb._p = playing; pb.innerHTML = playing ? I.pause : I.play; pb.classList.toggle("playing", playing); pb.setAttribute("aria-label", playing ? "Pause" : "Play"); }
    setText($("pv-pos"), fmtT(pos, 1));
    setText($("pv-dur"), ` / ${fmtT(dur)}`);
    const cnt = P.tel && P.tel.playing ? P.tel.count || 0 : 0;
    const ce = $("pv-count");
    setText(ce, cnt ? String(cnt) : "");
    ce.classList.toggle("on", !!cnt);
    P.wave.busy(loading && !loaded);
    P.wave.msg(loading ? "Loading…" : st && st.error && !loaded ? st.error : "");
    P.wave.draw(pos);
    paintRuler($("pv-ruler"), dur);
    // duck activity
    const dm = $("pv-duck-m");
    const da = P.tel && P.tel.playing && st && st.duck.on ? P.tel.duck || 0 : 0;
    dm.style.transform = `scaleX(${da.toFixed(3)})`;
  }
  function paintYt() {
    if (!P.built) return;
    const s = YT.st, has = !!YT.id;
    $("pv-yt").classList.toggle("has-video", has);
    $("pv-yt-ph").hidden = has && !YT.err;
    setText($("pv-yt-msg"), YT.err || "Paste a link to play along");
    $("pv-yt-ph").classList.toggle("err", !!YT.err);
    if (YT.frame) YT.frame.style.visibility = YT.err ? "hidden" : "";
    const rec = ytRecent.find((r) => r.id === YT.id);
    setText($("pv-yt-title"), (s && s.title) || (rec && rec.title) || (has ? "Loading…" : ""));
    setText($("pv-yt-author"), (s && s.author) || (rec && rec.author) || "");
    $("pv-yt-deck").classList.toggle("off", !(s && s.duration > 0));
    const lp = YT.loop, hasLp = s && s.duration > 0 && lp.b - lp.a > 0.2;
    $("pv-yt-loop").classList.toggle("on", !!(hasLp && lp.on));
    setText($("pv-yt-ab"), hasLp ? `${fmtT(lp.a)}–${fmtT(lp.b)}` : "");
    const rates = (s && Array.isArray(s.rates) && s.rates.length ? s.rates : [0.5, 0.75, 1, 1.25, 1.5]).filter((r) => r >= 0.5 && r <= 1.5);
    const cur = s ? s.rate : 1;
    setHTML($("pv-yt-rates"), rates.map((r) => `<button data-rate="${r}" class="${Math.abs(r - cur) < 0.01 ? "on" : ""}">${r === 1 ? "1×" : `${r}×`}</button>`).join(""));
    const vr = $("pv-yt-vol");
    if (document.activeElement !== vr) vr.value = String(prefs.ytVol);
    vr.style.setProperty("--p", `${prefs.ytVol}%`);
    paintLed();
  }
  function paintYtRecent() {
    if (!P.built) return;
    const html = ytRecent.map((r) => `<div class="pv-yr${r.id === YT.id ? " on" : ""}" data-yt="${escH(r.id)}" tabindex="0" title="${escH(r.title || r.id)}">
        <img src="https://i.ytimg.com/vi/${escH(r.id)}/mqdefault.jpg" alt="" loading="lazy" referrerpolicy="no-referrer">
        <span><b>${escH(r.title || "YouTube video")}</b><small>${escH([r.author, r.dur ? fmtT(r.dur) : ""].filter(Boolean).join(" · "))}</small></span>
        <button class="pv-yr-x" data-yt-x="${escH(r.id)}" title="Remove from recent" aria-label="Remove">${I.close}</button>
      </div>`).join("");
    setHTML($("pv-yt-recent"), html || `<div class="pv-yr-none">Links you play show up here</div>`);
  }
  function drawYt() {
    const s = YT.st, dur = s && s.duration > 0 ? s.duration : 0;
    const pos = ytPos();
    const playing = ytPlaying();
    const pb = $("pv-yt-play");
    if (pb._p !== playing) { pb._p = playing; pb.innerHTML = playing ? I.pause : I.play; pb.classList.toggle("playing", playing); pb.setAttribute("aria-label", playing ? "Pause" : "Play"); }
    setText($("pv-yt-pos"), fmtT(pos, 1));
    setText($("pv-yt-dur"), ` / ${fmtT(dur)}`);
    P.ytl.draw(pos);
    paintRuler($("pv-yt-ruler"), dur);
  }
  function paintLed() {
    const l = $("vt-practice-led");
    const playing = livePlaying() || ytPlaying();
    if (l) l.className = `vt-led${playing ? " play" : ""}`;
    const tab = document.querySelector('#view-tabs [data-view="practice"]');
    if (tab) {
      const title = playing ? (livePlaying() && P.st && P.st.title ? `Backing — playing “${P.st.title}” (⌘5)` : "Backing — YouTube playing (⌘5)") : "Backing — play along with your own tracks (⌘5)";
      if (tab.title !== title) tab.title = title;
    }
  }

  /* =====================================================================
     EVENTS
     ===================================================================== */
  function onClick(e) {
    if (e.detail > 0) { const bt = e.target.closest("button"); if (bt && !bt.closest("form")) bt.blur(); } // keep Space = play / pause after a click
    const mode = e.target.closest("[data-mode]");
    if (mode) { setMode(mode.dataset.mode); return; }
    const more = e.target.closest("[data-more]");
    if (more) { e.stopPropagation(); const t = P.lib.find((x) => x.id === more.dataset.more); const r = more.getBoundingClientRect(); if (t) trackMenu(t, r.left, r.bottom + 4); return; }
    const li = e.target.closest(".pv-li");
    if (li) { loadTrack(li.dataset.id, livePlaying()); if (P.libOpen) toggleLib(false); return; }
    const yx = e.target.closest("[data-yt-x]");
    if (yx) { e.stopPropagation(); ytRecent = ytRecent.filter((r) => r.id !== yx.dataset.ytX); save(LS.yt, ytRecent); paintYtRecent(); return; }
    const yr = e.target.closest("[data-yt]");
    if (yr) { const r = ytRecent.find((x) => x.id === yr.dataset.yt); ytLoad(yr.dataset.yt, r && r.pos > 5 && r.dur && r.pos < r.dur - 10 ? r.pos : 0); paintYtRecent(); return; }
    const rate = e.target.closest("[data-rate]");
    if (rate) { prefs.ytRate = +rate.dataset.rate; savePrefs(); ytCall("setPlaybackRate", [prefs.ytRate]); return; }
    const ci = e.target.closest("[data-ci]");
    if (ci) { setCountIn(+ci.dataset.ci); return; }
    if (e.target.closest("#pv-play")) { playPause(); return; }
    if (e.target.closest("#pv-yt-play")) { ytPlayPause(); return; }
    if (e.target.closest("#pv-di-play")) { if (typeof toggleDIPlayback === "function") toggleDIPlayback(); return; }
    if (e.target.closest("#pv-duck")) { setDuck(!(P.st ? P.st.duck.on : prefs.duck)); return; }
    const a = e.target.closest("[data-act]");
    if (!a) return;
    const act = a.dataset.act;
    const st = P.st;
    if (act === "add") chooseFiles();
    else if (act === "lib") toggleLib();
    else if (act === "more") { const r = a.getBoundingClientRect(); moreMenu(r.right - 220, r.bottom + 4); }
    else if (act === "loop") toggleLoop();
    else if (act === "speed1") setSpeed(1, true);
    else if (act === "semi0") setSemis(0);
    else if (act === "semi-") setSemis((st ? st.semitones : 0) - 1);
    else if (act === "semi+") setSemis((st ? st.semitones : 0) + 1);
    else if (act === "bpm-") setBpm((st ? st.count_in.bpm : 120) - 1);
    else if (act === "bpm+") setBpm((st ? st.count_in.bpm : 120) + 1);
    else if (act === "tap") tap();
    else if (act === "yt-loop") ytToggleLoop();
    else if (act === "di-close") openDI(false);
    else if (act === "di-loop") {
      const tr = DI.tr || {};
      if (!tr.ab_loop && tr.loop_b - tr.loop_a > 0.985) { say("Drag across the DI timeline to pick a section."); return; }
      diSet(`ab=${tr.ab_loop ? 0 : 1}`);
    }
  }
  function onKey(e) {
    if (!P.visible || e.metaKey || e.ctrlKey || e.altKey) return;
    if ((typeof ui !== "undefined" && ui.modalOpen) || typingIn(document.activeElement)) return;
    const menu = $("menu");
    if (menu && !menu.hidden) {
      // Esc closes the ⋯ / track menu but stays in Practice (otherwise the tab bar's Esc would leave the view)
      if (e.key === "Escape") { if (typeof closeMenu === "function") closeMenu(); e.preventDefault(); e.stopPropagation(); }
      return;
    }
    if (e.key === "Escape") {
      if (DI.open) { openDI(false); e.preventDefault(); e.stopPropagation(); }
      else if (P.libOpen) { toggleLib(false); e.preventDefault(); e.stopPropagation(); }
      return;
    }
    if (DI.open) return; // the sheet is about the DI player: leave Space etc. to the app
    const a = document.activeElement;
    const k = e.key.length === 1 ? e.key.toLowerCase() : e.key;
    const yt = prefs.mode === "yt";
    let done = true;
    if (k === " ") {
      if (a && a.tagName === "BUTTON" && !a.closest(".pv-tl")) return;
      if (yt) ytPlayPause(); else playPause();
    } else if (k === "ArrowLeft" || k === "ArrowRight") {
      if (a && a.tagName === "INPUT" && a.type === "range") return;
      const d = (k === "ArrowLeft" ? -1 : 1) * (e.shiftKey ? 1 : 5);
      if (yt) ytSeek(Math.max(0, ytPos() + d)); else seek(curPos() + d);
    } else if (k === "l") { if (yt) ytToggleLoop(); else toggleLoop(); }
    else if (k === "a" || k === "b") { if (yt) ytLoopPoint(k); else setLoopPoint(k); }
    else done = false;
    if (done) { e.preventDefault(); e.stopPropagation(); if (e.repeat && k === " ") return; }
  }

  /* =====================================================================
     VIEW LIFECYCLE + TELEMETRY + NATIVE HOOKS
     ===================================================================== */
  function loop() {
    if (!P.visible) return;
    if (window.HRActivity && !HRActivity.awake()) { P.raf = 0; return; }   // window/editor hidden: stop (resumed below)
    try { draw(); } catch (err) { console.error(err); }
    P.raf = requestAnimationFrame(loop);
  }
  // Idle CPU (assets/activity.js): restart the draw loop when the window comes back.
  if (window.HRActivity) HRActivity.on((e) => { if (e.awake && P.visible && !P.raf) P.raf = requestAnimationFrame(loop); });
  function enter() {
    P.visible = true;
    if (!P.built) build();
    paintMode();
    refresh();
    if (prefs.mode === "yt" && !YT.id && prefs.ytLast) setMode("yt");
    if (DI.open) openDI(true);
    cancelAnimationFrame(P.raf);
    P.raf = requestAnimationFrame(loop);
  }
  function leave() {
    P.visible = false;
    cancelAnimationFrame(P.raf);
    toggleLib(false);
    if (typeof closeMenu === "function") closeMenu();
  }
  function onTelemetry(d) {
    if (!d) return;
    DI.tel = d;
    const b = d.backing;
    if (!b) return;
    const prev = P.telSig;   // compare with what we last saw from the engine (P.tel is also nudged optimistically)
    P.tel = b; P.telT = now();
    P.telSig = { id: b.id, loading: b.loading, playing: b.playing, ended: b.ended };
    const changed = !prev || prev.id !== b.id || prev.loading !== b.loading || prev.playing !== b.playing || prev.ended !== b.ended;
    if (P.st) { P.st.playing = b.playing; P.st.position = b.pos; }
    if (changed) {
      if (prev && (prev.id !== b.id || (prev.loading && !b.loading))) { if (P.visible) refresh(); else api("/api/backing").then(applyState).catch(() => {}); }
      if (P.built) paintLib();
      paintLed();
    }
  }

  // Native bridge (StudioApp.mm): audio files dropped on the window, or picked with "Add tracks".
  let dropEl = null;
  window.headroomBackingDrag = function (on) {
    if (!dropEl) {
      dropEl = document.createElement("div");
      dropEl.className = "pv-dropall";
      dropEl.innerHTML = `<div>${I.wave}<b>Drop to add backing tracks</b><span>They’re copied into your Backing Tracks library</span></div>`;
      document.body.appendChild(dropEl);
    }
    dropEl.classList.toggle("on", !!on);
  };
  window.headroomBackingImport = function (paths) { importPaths(Array.isArray(paths) ? paths : []); };

  window.addEventListener("keydown", onKey, true);
  window.addEventListener("resize", () => { if (P.visible) { const r = $("pv-ruler"), y = $("pv-yt-ruler"); if (r) r._w = -1; if (y) y._w = -1; } });
  function reg() { if (window.HRViews && HRViews.register) HRViews.register("practice", { enter, leave, onTelemetry }); }
  if (window.HRViews) reg(); else document.addEventListener("DOMContentLoaded", reg);
  window.HRPractice = { importPaths, openYouTube: ytOpen, openDI, state: () => ({ P, YT, DI, prefs }) };
})();
