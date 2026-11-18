/* Headroom — Settings  (assets/settings.js + assets/settings.css)
 *
 * One window for every app setting, opened by the top-right gear and ⌘, (Comma). Sections:
 *   Audio            index.html's audio panel (devices, sample rate, buffer, input source, input gain + the
 *                    auto-calibration). Rendered by index.html's renderAudio() into #audio-body here.
 *   AI · Guitar Tech provider, key (macOS Keychain via the native bridge), model, base URL, Test connection.
 *                    Logic stays in guitar-tech.js and is driven through window.HRTech.settings.
 *   Appearance       theme (dark / light / match macOS), bypassed units in the chain, rig list order, pane layout,
 *                    the right pane's cards: show / hide, order, reset (window.ListenPane, listen-pane.js).
 *   Library & Content your Rigs folder + author name, IRs, NAM captures, backing tracks.
 *   Plugins          Audio Units in Components (HRPlugins in plugins.js): import dialog, rescan, remove.
 *   MIDI             the Headroom MIDI Out port and the engine's controller (CC) map (GET /api/midi-learn).
 *   Live             Live view preferences (HRLive.prefs in live-view.js) and the tuner: instrument, tuning and
 *                    A4 reference (window.ListenPane; the right-pane tuner and Live's tuner share them).
 *   About            engine, data folder, keyboard shortcuts.
 * In a DAW (RIG_HOSTED) Audio, Plugins and MIDI are left out: the host owns devices and MIDI, and the plugin
 * can't host other plugins.
 *
 * API: HRSettings.open(section?, { msg }?) — section ids above ("audio", "ai", "appearance", "library",
 * "plugins", "midi", "live", "about"); HRSettings.close(); HRSettings.isOpen().
 * Uses index.html globals at call time only: API_BASE, ui, openModal, closeModal, toast, loadAudioPanel,
 * setTheme, toggleShowBypassedUnits, setRigSort, rigsUI, userRigs, loadPanes, revealRigsFolder, namBridge,
 * fetchJSON. A dialog opened from here (Import plugins…) takes the modal's place;
 * when it closes, Settings comes back on the same section.
 */
(function () {
  "use strict";

  const $ = (id) => document.getElementById(id);
  const h = (s) => String(s ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
  const say = (m) => { if (typeof toast === "function") toast(m); };
  const hosted = () => !!window.RIG_HOSTED;
  const bridge = () => !!(window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.headroom);
  const inApp = () => bridge() && !hosted();
  const post = (action, extra) => {
    try { window.webkit.messageHandlers.headroom.postMessage(Object.assign({ action }, extra || {})); }
    catch (e) { say("Couldn’t reach the app."); }
  };
  const base = () => (typeof API_BASE !== "undefined" ? API_BASE : "http://127.0.0.1:17770");
  async function api(path) {
    const r = await fetch(base() + path, { cache: "no-store" });
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    return r.json();
  }
  const tilde = (p) => String(p || "").replace(/^(\/private)?\/Users\/[^/]+/, "~");
  const plural = (n, w, ws) => `${n} ${n === 1 ? w : ws || w + "s"}`;
  const lsGet = (k) => { try { return localStorage.getItem(k); } catch (e) { return null; } };
  const lsSet = (k, v) => { try { localStorage.setItem(k, v); } catch (e) { /* private mode */ } };
  const IN_APP_ONLY = "Works in the Headroom app";

  const I = (d) => `<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.3" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${d}</svg>`;
  const ICON = {
    audio: I(`<path d="M2.2 6.3v3.4h2.5L8 12.4V3.6L4.7 6.3z"/><path d="M10.4 5.8a3.1 3.1 0 0 1 0 4.4M12.2 4a5.6 5.6 0 0 1 0 8"/>`),
    sound: I(`<path d="M2 8h2l2-4 4 8 2-4h2"/>`),
    ai: I(`<path d="M2.6 3.4h10.8v7.1H7.2l-3 2.4v-2.4H2.6z"/><path d="M5.6 7h.01M8 7h.01M10.4 7h.01" stroke-width="1.8"/>`),
    appearance: I(`<circle cx="8" cy="8" r="5.6"/><path d="M8 2.4a5.6 5.6 0 0 1 0 11.2z" fill="currentColor" stroke="none"/>`),
    library: I(`<path d="M2.2 4.4c0-.55.45-1 1-1h3.1l1.5 1.6h5c.55 0 1 .45 1 1v6.1c0 .55-.45 1-1 1H3.2c-.55 0-1-.45-1-1z"/>`),
    plugins: I(`<rect x="2.3" y="3" width="11.4" height="10" rx="1.6"/><path d="M2.3 6h11.4M5 8.6h2.4"/>`),
    midi: I(`<circle cx="8" cy="8" r="5.7"/><path d="M5 8.4h.01M11 8.4h.01M8 5.3h.01M5.9 6.2h.01M10.1 6.2h.01" stroke-width="1.7"/><path d="M7 12h2"/>`),
    live: I(`<path d="M8 1.8v1.7M3.3 3.9l1.2 1.2M12.7 3.9l-1.2 1.2"/><path d="M2.5 13.4h11M5.2 13.4 6.6 7.6h2.8l1.4 5.8"/>`),
    about: I(`<circle cx="8" cy="8" r="5.7"/><path d="M8 7.3v3.9"/><path d="M8 5h.01" stroke-width="1.8"/>`),
    search: I(`<circle cx="7" cy="7" r="4.6"/><path d="m10.5 10.5 3 3"/>`),
    close: `<svg viewBox="0 0 12 12" aria-hidden="true"><path d="M3 3l6 6M9 3l-6 6" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"/></svg>`,
    folder: I(`<path d="M2.2 4.4c0-.55.45-1 1-1h3.1l1.5 1.6h5c.55 0 1 .45 1 1v6.1c0 .55-.45 1-1 1H3.2c-.55 0-1-.45-1-1z"/>`),
    import: I(`<path d="M8 2.4v7.2M5.1 6.9 8 9.8l2.9-2.9M2.8 12.8h10.4"/>`),
    up: I(`<path d="M4.5 9.8 8 6.3l3.5 3.5"/>`),
    down: I(`<path d="M4.5 6.2 8 9.7l3.5-3.5"/>`),
  };

  // id, nav label, extra search words, shown?
  const SECTIONS = [
    { id: "audio", label: "Audio", k: "interface device input output sample rate buffer latency monitor gain calibrate microphone", when: () => !hosted() },
    { id: "sound", label: "Sound", k: "noise hiss hum tube circuit realism" },
    { id: "ai", label: "AI · Guitar Tech", k: "provider api key openrouter anthropic openai model keychain augie llm" },
    { id: "appearance", label: "Appearance", k: "theme dark light system mode bypassed panes sort right pane cards listen key scale tone input chain" },
    { id: "library", label: "Library & Content", k: "rigs folder finder author import ir impulse wav nam captures backing tracks" },
    { id: "plugins", label: "Plugins", k: "audio units au vst3 import rescan components", when: () => !hosted() },
    { id: "midi", label: "MIDI", k: "midi out cc controller footswitch expression mapping", when: () => !hosted() },
    { id: "live", label: "Live", k: "stage lights dim full width tuner mute setlist instrument tuning reference a4 hz pitch bass drop" },
    { id: "about", label: "About", k: "version engine shortcuts keyboard data folder disclaimer trademarks affiliation" },
  ];
  const sections = () => SECTIONS.filter((s) => !s.when || s.when());

  const St = { open: false, sec: null, q: "", rendered: new Set(), displaced: null };
  const modal = () => $("modal");
  const isOpen = () => { const m = modal(), b = $("modal-back"); return !!(m && b && !b.hidden && m.classList.contains("hr-settings")); };
  const secEl = (id) => modal().querySelector(`.st-sec[data-sec="${id}"]`);
  const bodyEl = (id) => { const s = secEl(id); return s && s.querySelector(".st-body"); };

  /* =====================================================================
     SHELL
     ===================================================================== */
  function shellHTML() {
    const list = sections();
    return `<div class="st-nav">
        <div class="st-title">Settings</div>
        <label class="st-search">${ICON.search}<input type="search" id="st-q" placeholder="Search settings" autocomplete="off" spellcheck="false" aria-label="Search settings"></label>
        <div class="st-navlist" role="tablist" aria-label="Settings sections" aria-orientation="vertical">
          ${list.map((s) => `<button type="button" class="st-nav-i" role="tab" data-st-nav="${s.id}" aria-selected="false">${ICON[s.id]}<span>${h(s.label)}</span></button>`).join("")}
        </div>
        <div class="st-nothing" id="st-nothing" hidden>No settings match.</div>
      </div>
      <div class="st-main">
        <button type="button" class="icon-btn st-close" data-st-close title="Close (Esc)" aria-label="Close settings">${ICON.close}</button>
        <div class="st-scroll" id="st-scroll">
          ${list.map((s) => `<section class="st-sec" data-sec="${s.id}" role="tabpanel" aria-label="${h(s.label)}" hidden>
            <header class="st-sec-h"><h2>${h(s.label)}</h2></header>
            <div class="st-body"${s.id === "audio" ? ' id="audio-body"' : ""}></div></section>`).join("")}
        </div>
      </div>`;
  }

  function open(sec, opts) {
    if (typeof openModal !== "function") return;
    const list = sections();
    const want = list.some((s) => s.id === sec) ? sec : list.some((s) => s.id === St.sec) ? St.sec : list[0].id;
    if (isOpen()) { if (St.q) setQuery(""); show(want, opts); return; }
    if (typeof ui === "object" && ui && ui.modalOpen) closeModal();   // resolve whatever dialog was up first
    St.displaced = null;
    St.rendered = new Set();
    St.q = "";
    AI.msg = "";
    openModal(shellHTML(), "hr-settings");
    St.open = true;
    const m = modal();
    m.setAttribute("aria-label", "Settings");
    m.onclick = onClick;
    m.oninput = onInput;
    m.onchange = onChange;
    m.onkeydown = onKeyDown;
    watch();
    show(want, opts);
  }
  function close() { if (isOpen()) closeModal(); }

  function show(id, opts) {
    St.sec = id;
    modal().querySelectorAll(".st-nav-i").forEach((b) => {
      const on = b.dataset.stNav === id;
      b.classList.toggle("on", on);
      b.setAttribute("aria-selected", String(on));
      b.tabIndex = on ? 0 : -1;
    });
    modal().querySelectorAll(".st-sec").forEach((s) => { s.hidden = s.dataset.sec !== id; });
    ensure(id, opts);
    const sc = $("st-scroll");
    if (sc) sc.scrollTop = 0;
  }
  function ensure(id, opts) {
    // Library re-reads its folders each time it's shown (imports may have landed); the rest render once per open.
    if (St.rendered.has(id) && !opts && id !== "library") return;
    St.rendered.add(id);
    const b = bodyEl(id);
    if (!b) return;
    const fn = RENDER[id];
    if (fn) Promise.resolve(fn(b, opts || {})).catch((e) => { b.innerHTML = statusLine("err", `Couldn’t load this section (${h(e && e.message || e)}).`); }).then(() => { if (St.q) applySearch(); });
  }

  // Settings closes through closeModal (Esc, backdrop, ×). If another dialog replaced it instead (Import
  // plugins…), bring Settings back when that one closes.
  let mo = null;
  function watch() {
    if (mo) return;
    mo = new MutationObserver(() => {
      const back = $("modal-back"), m = modal();
      const showing = !back.hidden && m.classList.contains("hr-settings");
      if (St.open && !showing) {
        St.open = false;
        if (!back.hidden) {
          St.displaced = { sec: St.sec, t: Date.now() };
          if (typeof ui === "object" && ui) ui.audioOpen = false;   // the audio panel is gone; stop its meter / calibration
        } else St.displaced = null;
      } else if (!St.open && St.displaced && back.hidden) {
        const d = St.displaced;
        St.displaced = null;
        if (Date.now() - d.t < 30 * 60e3) setTimeout(() => { if ($("modal-back").hidden) open(d.sec); }, 0);
      }
    });
    mo.observe($("modal-back"), { attributes: true, attributeFilter: ["hidden"] });
    mo.observe(modal(), { attributes: true, attributeFilter: ["class"] });
  }

  /* =====================================================================
     SEARCH — filters rows across every section
     ===================================================================== */
  function setQuery(q) {
    St.q = q;
    const inp = $("st-q");
    if (inp && inp.value !== q) inp.value = q;
    if (q.trim()) sections().forEach((s) => ensure(s.id));
    applySearch();
  }
  // Word-prefix match ("cc" finds "CC 64", not "access"); q is lower-case.
  let reQ = "", re = null;
  const words = (q) => { if (q !== reQ) { reQ = q; re = new RegExp("(^|[^\\p{L}\\p{N}])" + q.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"), "iu"); } return re; };
  const hit = (el, q) => words(q).test(el.textContent + " " + (el.dataset.k || ""));
  function applySearch() {
    const m = modal();
    if (!m || !isOpen()) return;
    const q = St.q.trim().toLowerCase();
    m.classList.toggle("st-searching", !!q);
    m.querySelectorAll(".st-miss").forEach((x) => x.classList.remove("st-miss"));
    let any = false;
    for (const s of sections()) {
      const el = secEl(s.id), nav = m.querySelector(`[data-st-nav="${s.id}"]`);
      if (!el) continue;
      if (!q) { el.hidden = s.id !== St.sec; if (nav) nav.hidden = false; continue; }
      let found = words(q).test(`${s.label} ${s.k}`);
      if (!found || !words(q).test(s.label)) {
        // Rows inside groups, then loose blocks; a matching group title keeps its whole group.
        const body = el.querySelector(".st-body");
        let rows = 0;
        for (const c of [...body.children]) {
          if (c.classList.contains("fgroup-t")) continue;
          if (c.classList.contains("fgroup")) {
            const title = c.previousElementSibling && c.previousElementSibling.classList.contains("fgroup-t") ? c.previousElementSibling : null;
            if (title && hit(title, q)) { rows++; continue; }
            let n = 0;
            c.querySelectorAll(":scope > .frow, :scope > .st-item").forEach((r) => { const ok = hit(r, q); r.classList.toggle("st-miss", !ok); if (ok) n++; });
            c.classList.toggle("st-miss", !n);
            if (title) title.classList.toggle("st-miss", !n);
            rows += n;
          } else {
            const ok = hit(c, q);
            c.classList.toggle("st-miss", !ok);
            if (ok) rows++;
          }
        }
        if (rows) found = true;
        else if (found) body.querySelectorAll(".st-miss").forEach((x) => x.classList.remove("st-miss"));   // matched by keyword only: show it all
      }
      el.hidden = !found;
      if (nav) nav.hidden = !found;
      any = any || found;
    }
    const none = $("st-nothing");
    if (none) none.hidden = !q || any;
  }

  /* =====================================================================
     EVENTS (delegated on the modal)
     ===================================================================== */
  function onClick(e) {
    const t = e.target;
    if (t.closest("[data-st-close]")) { close(); return; }
    const nav = t.closest("[data-st-nav]");
    if (nav) { if (St.q) setQuery(""); show(nav.dataset.stNav); return; }
    const a = t.closest("[data-act]");
    if (a && !a.disabled) { const fn = ACT[a.dataset.act]; if (fn) fn(a, e); }
  }
  function onInput(e) {
    const t = e.target;
    if (t.id === "st-q") { setQuery(t.value); return; }
    const fn = t.dataset && INPUT[t.dataset.in];
    if (fn) fn(t, e);
  }
  function onChange(e) {
    const t = e.target;
    const fn = t.dataset && CHANGE[t.dataset.ch];
    if (fn) fn(t, e);
  }
  function onKeyDown(e) {
    const t = e.target;
    if (t.id === "st-q" && e.key === "Escape" && t.value) { e.preventDefault(); e.stopPropagation(); setQuery(""); return; }
    if (t.id === "st-q" && e.key === "ArrowDown") { const f = modal().querySelector(".st-nav-i:not([hidden])"); if (f) { e.preventDefault(); f.focus(); } return; }
    if (t.classList && t.classList.contains("st-nav-i") && (e.key === "ArrowDown" || e.key === "ArrowUp")) {
      e.preventDefault();
      const items = [...modal().querySelectorAll(".st-nav-i:not([hidden])")];
      const i = items.indexOf(t) + (e.key === "ArrowDown" ? 1 : -1);
      if (items[i]) { items[i].focus(); if (St.q) setQuery(""); show(items[i].dataset.stNav); items[i].focus(); }
      return;
    }
    if (t.id === "st-ai-key" && e.key === "Enter") { e.preventDefault(); ACT["ai-savekey"](); return; }
    if (t.id === "st-ai-skey" && e.key === "Enter") { e.preventDefault(); ACT["ai-web-savekey"](); return; }
    if (t.id === "st-author" && e.key === "Enter") { e.preventDefault(); t.blur(); }
  }

  /* =====================================================================
     BUILDING BLOCKS
     ===================================================================== */
  const statusLine = (kind, html) => `<div class="status-line ${kind || ""}"><i></i><div>${html}</div></div>`;
  const row = (label, ctrl, opt = {}) => `<div class="frow${opt.top ? " st-top" : ""}"${opt.k ? ` data-k="${h(opt.k)}"` : ""}><label${opt.for ? ` for="${opt.for}"` : ""}>${label}</label>${ctrl}</div>`;
  const seg = (act, cur, opts) => `<div class="seg st-seg" role="group">${opts.map(([v, l, dis]) => `<button type="button" data-act="${act}" data-v="${h(v)}" class="${String(v) === String(cur) ? "on" : ""}"${dis ? ` disabled title="${h(dis)}"` : ""} aria-pressed="${String(v) === String(cur)}">${l}</button>`).join("")}</div>`;
  const sw = (act, on, label) => `<div class="ctrl"><button type="button" class="switch${on ? " on" : ""}" data-act="${act}" role="switch" aria-checked="${!!on}" aria-label="${h(label)}"></button><span class="ro">${label}</span></div>`;
  const btn = (act, label, o = {}) => `<button type="button" class="btn${o.primary ? " primary" : ""} st-btn" data-act="${act}"${o.disabled ? ` disabled title="${h(o.disabled)}"` : ""}${o.data ? ` ${o.data}` : ""}>${o.icon || ""}${label}</button>`;
  const fileRow = (label, path, meta, buttons, k) => row(label, `<div class="st-file"><code title="${h(path)}">${h(tilde(path))}</code>${meta ? `<span class="st-meta">${meta}</span>` : ""}<span class="st-acts">${buttons}</span></div>`, { k });

  /* =====================================================================
     SECTIONS
     ===================================================================== */
  const RENDER = {};
  const ACT = {};
  const INPUT = {};
  const CHANGE = {};

  /* ---------- Audio: index.html renders the panel into #audio-body ---------- */
  RENDER.audio = (b) => {
    b.innerHTML = `<div class="comp-empty">Loading…</div>`;
    if (typeof loadAudioPanel !== "function") { b.innerHTML = statusLine("warn", "The audio panel isn’t available in this build."); return; }
    if (!b._mo) { b._mo = new MutationObserver(() => { if (St.q) applySearch(); }); b._mo.observe(b, { childList: true }); }
    return loadAudioPanel();
  };

  RENDER.sound = async (b) => {
    const s = await api("/api/settings/sound");
    b.innerHTML = `<div class="fgroup">${row("Amp noise & hum", sw("sound-noise", s.circuit_noise,
      "Real tube hiss and mains hum from the circuit models. Off by default."), { k: "noise hiss hum tube circuit realism" })}</div>`;
  };
  ACT["sound-noise"] = async (el) => {
    el.disabled = true;
    try {
      const s = await api("/api/settings/sound/set?circuit_noise=" + (el.getAttribute("aria-checked") === "true" ? "0" : "1"));
      el.classList.toggle("on", s.circuit_noise);
      el.setAttribute("aria-checked", String(s.circuit_noise));
    } catch (e) { say("Couldn’t change amp noise & hum."); }
    finally { el.disabled = false; }
  };

  /* ---------- AI · Guitar Tech (guitar-tech.js → HRTech.settings) ---------- */
  const T = () => window.HRTech && window.HRTech.settings;
  const AI = { orQ: "" };
  RENDER.ai = async (b, opts) => {
    const S = T();
    if (!S) { b.innerHTML = statusLine("warn", "The Guitar Tech isn’t loaded."); return; }
    const c = S.get(), k = await S.key(), p = c.provider;
    const web = S.web ? await aiWebHTML(S) : "";
    if (opts.msg) AI.msg = opts.msg;
    const provs = S.providers();
    const store = k.native ? `Stored in your macOS Keychain as “${h(k.account)}”.`
      : hosted() ? `<b>Session only</b> in the plugin: kept in memory until it closes.`
      : `<b>Session only:</b> this page isn’t running inside Headroom, so the key is forgotten when it reloads.`;
    let model = "";
    if (p === "openrouter") model = row("Model", `<div class="st-stack"><input type="search" class="st-in" id="st-ai-orq" data-in="ai-orq" placeholder="Filter tool-capable models" spellcheck="false" value="${h(AI.orQ)}"><select class="st-in st-listbox" id="st-ai-model" data-ch="ai-model" size="6" aria-label="Model"></select><div class="st-hint" id="st-ai-ornote">Loading OpenRouter’s model list…</div></div>`, { top: true, for: "st-ai-orq", k: "model" });
    else if (p === "anthropic" || p === "compatible") {
      const dl = p === "anthropic" ? S.anthropicModels.map((m) => `<option value="${h(m)}">`).join("") : "";
      model = row("Model", `<div class="st-stack"><input class="st-in" id="st-ai-model-in" data-in="ai-model" list="st-ai-models" spellcheck="false" value="${h(c.model)}" placeholder="${p === "compatible" ? "e.g. llama3.1 or gpt-…" : ""}"><datalist id="st-ai-models">${dl}</datalist>
        <div class="st-hint"><button type="button" class="pf-link" data-act="ai-loadmodels">${p === "anthropic" ? "Load my available models" : "Load models from the endpoint"}</button>${p === "compatible" ? " · it must support tool calling" : ""}</div></div>`, { top: true, for: "st-ai-model-in", k: "model" });
    }
    b.innerHTML = `
      ${AI.msg ? statusLine("warn", h(AI.msg)) : ""}
      <div class="fgroup-t">Provider</div>
      <div class="st-cards" role="radiogroup" aria-label="Provider" data-k="provider">${provs.map((v) => `<button type="button" class="st-card${v.id === p ? " on" : ""}" role="radio" aria-checked="${v.id === p}" data-act="ai-prov" data-v="${v.id}"><b>${h(v.label)}${v.id === "openrouter" ? '<i class="st-tag">Default</i>' : ""}</b><span>${h(v.blurb)}</span></button>`).join("")}</div>
      <div class="fgroup-t">Connection</div>
      <div class="fgroup">
        ${p === "compatible" ? row("Base URL", `<div class="st-stack"><input class="st-in" id="st-ai-base" data-ch="ai-base" type="url" spellcheck="false" value="${h(c.baseUrl)}" placeholder="https://api.openai.com/v1"><div class="st-pills">${S.compatPresets.map((x) => `<button type="button" class="st-pill${x.url === c.baseUrl ? " on" : ""}" data-act="ai-base" data-v="${h(x.url)}">${h(x.name)}</button>`).join("")}</div></div>`, { top: true, for: "st-ai-base", k: "endpoint url openai groq lm studio ollama" }) : ""}
        ${k.applies ? row(`API key${k.needsKey ? "" : ` <em>optional</em>`}`, `<div class="st-stack"><div class="st-inrow"><input class="st-in st-mono" id="st-ai-key" type="password" autocomplete="off" spellcheck="false" placeholder="${k.has ? "•••••••••••• saved — paste to replace" : h(k.hint)}"><button type="button" class="btn primary" data-act="ai-savekey">Save</button>${k.has ? `<button type="button" class="btn" data-act="ai-forget">Forget</button>` : ""}</div>
          <div class="st-hint${k.native ? "" : " warn"}">${k.has ? '<b class="st-ok">Key on file.</b> ' : ""}${store}</div></div>`, { top: true, for: "st-ai-key", k: "api key keychain secret" }) : ""}
        ${model}
        ${row("Check", `<div class="st-inrow"><button type="button" class="btn" data-act="ai-test">Test connection</button><span class="st-out" id="st-ai-out" aria-live="polite"></span></div>`, { k: "test connection" })}
      </div>
      ${web}
      <p class="st-note">Your questions and a summary of your rig go to the provider you pick. Keys go only to that provider${k.native ? " and otherwise stay in the Keychain" : ""}. Nothing is sent until you ask something.</p>`;
    if (p === "openrouter") loadOr();
    if (opts.msg) setTimeout(() => { const i = $("st-ai-key"); if (i) i.focus(); }, 40);
  };
  const aiOut = (cls, html) => { const o = $("st-ai-out"); if (o) o.innerHTML = cls ? `<span class="${cls}">${html}</span>` : html; };
  async function loadOr(force) {
    const S = T();
    try { AI.or = await S.openRouterModels(force); }
    catch (e) { const n = $("st-ai-ornote"); if (n) n.textContent = `Couldn’t load the model list (${e.message}). Type a model id in the filter box and press Return to use it.`; AI.or = null; }
    paintOr();
  }
  function paintOr() {
    const S = T(), sel = $("st-ai-model"), note = $("st-ai-ornote");
    if (!sel || !S) return;
    const cur = S.get().model, f = AI.orQ.trim().toLowerCase();
    const all = AI.or || [];
    const list = all.filter((m) => !f || m.id.toLowerCase().includes(f) || m.name.toLowerCase().includes(f));
    if (!list.some((m) => m.id === cur) && (!f || cur.toLowerCase().includes(f))) list.unshift({ id: cur, name: cur, inP: NaN, outP: NaN });
    const money = (v) => `$${v < 1 ? v.toFixed(2) : v.toFixed(v < 10 ? 1 : 0)}`;
    const price = (m) => (Number.isFinite(m.inP) ? ` — ${money(m.inP)} / ${money(m.outP)} per M tokens` : "");
    sel.innerHTML = list.slice(0, 300).map((m) => `<option value="${h(m.id)}"${m.id === cur ? " selected" : ""}>${h(m.name)}${h(price(m))}</option>`).join("");
    if (note && AI.or) note.innerHTML = `${AI.or.length} tool-capable models · selected <code>${h(cur)}</code>`;
  }
  ACT["ai-prov"] = (el) => { T().set({ provider: el.dataset.v }); AI.msg = ""; RENDER.ai(bodyEl("ai"), {}); };
  ACT["ai-base"] = (el) => { T().set({ baseUrl: el.dataset.v }); RENDER.ai(bodyEl("ai"), {}); };
  CHANGE["ai-base"] = (el) => { T().set({ baseUrl: el.value }); RENDER.ai(bodyEl("ai"), {}); };
  ACT["ai-savekey"] = async () => {
    const i = $("st-ai-key");
    if (!i) return;
    if (!i.value.trim()) { i.focus(); return; }
    try { await T().saveKey(i.value); i.value = ""; AI.msg = ""; await RENDER.ai(bodyEl("ai"), {}); aiOut("ok", "Key saved."); }
    catch (e) { aiOut("bad", h(e.message)); }
  };
  ACT["ai-forget"] = async () => { try { await T().forgetKey(); } catch (e) { /* already gone */ } RENDER.ai(bodyEl("ai"), {}); };
  ACT["ai-test"] = async (el) => {
    el.disabled = true;
    aiOut("", "Ringing…");
    try { const txt = await T().test(); aiOut("ok", `Connected — “${h(String(txt || "(no text)").replace(/\s+/g, " ").slice(0, 40))}”`); }
    catch (e) { aiOut("bad", h(e && e.message || "Failed")); }
    el.disabled = false;
  };
  ACT["ai-loadmodels"] = async () => {
    aiOut("", "Asking…");
    try {
      const ids = await T().endpointModels();
      const dl = $("st-ai-models");
      if (dl) dl.innerHTML = ids.map((id) => `<option value="${h(id)}">`).join("");
      aiOut("ok", `${plural(ids.length, "model")} found — pick one in the Model box.`);
    } catch (e) { aiOut("bad", h(e.message)); }
  };
  INPUT["ai-orq"] = (el) => { AI.orQ = el.value; paintOr(); };
  // Web search group: on/off, provider's built-in search or Headroom's own tools, and the fallback's search engine + key.
  async function aiWebHTML(S) {
    const w = S.web(), E = S.searchEngines.find((e) => e.id === w.engine) || S.searchEngines[0];
    const rows = [row("Web search", sw("ai-web-on", w.on, w.on ? "On — Augie can look things up online" : "Off — answers from what he knows"), { k: "web search internet online browse look up" })];
    let privacy = "";
    if (w.on) {
      const native = w.mode === "native";
      rows.push(row("Method", `<div class="st-stack">${seg("ai-web-method", native ? "native" : "tools", [["native", "Provider’s built-in", w.nativeAvailable ? "" : `${w.providerLabel} has no built-in search, so Headroom’s tools are used`], ["tools", "Headroom’s tools"]])}
        <div class="st-hint">${native ? `${h(w.providerLabel)} runs the searches and page reads on its side and bills them with the reply.` : `Augie runs <code>web_search</code> and <code>read_webpage</code> himself, through the app — works with any model that can call tools.`}</div></div>`, { top: true, k: "web search method built-in native tools fallback" }));
      if (!native) {
        rows.push(row("Search with", `<div class="st-stack">${seg("ai-web-engine", w.engine, S.searchEngines.map((e) => [e.id, h(e.label)]))}<div class="st-hint">${h(E.blurb)}</div></div>`, { top: true, k: "search engine duckduckgo brave tavily" }));
        const sk = await S.searchKey(w.engine);
        if (sk.applies) rows.push(row(`${h(E.label)} key`, `<div class="st-stack"><div class="st-inrow"><input class="st-in st-mono" id="st-ai-skey" type="password" autocomplete="off" spellcheck="false" placeholder="${sk.has ? "•••••••••••• saved — paste to replace" : h(sk.hint)}"><button type="button" class="btn primary" data-act="ai-web-savekey">Save</button>${sk.has ? `<button type="button" class="btn" data-act="ai-web-forget">Forget</button>` : ""}</div>
          <div class="st-hint${sk.native ? "" : " warn"}">${sk.has ? '<b class="st-ok">Key on file.</b> ' : ""}${sk.native ? `Stored in your macOS Keychain as “${h(sk.account)}”.` : "<b>Session only</b> outside the app: forgotten when the page reloads."}</div></div>`, { top: true, for: "st-ai-skey", k: "search api key brave tavily keychain" }));
        rows.push(row("Check", `<div class="st-inrow"><button type="button" class="btn" data-act="ai-web-test">Test search</button><span class="st-out" id="st-ai-wout" aria-live="polite"></span></div>`, { k: "test search" }));
      }
      privacy = native ? `Search words and the pages Augie opens go through ${h(w.providerLabel)} (and the search service it uses).`
        : `Search words go to ${h(E.label)}; pages Augie reads are fetched from your Mac. Local and private-network addresses are never opened.`;
    }
    return `<div class="fgroup-t">Web search</div><div class="fgroup">${rows.join("")}</div>
      ${privacy ? `<p class="st-note">${privacy} Web text is treated as reference material only — Augie won’t follow instructions found on a page, and rig changes still need your OK.</p>` : ""}`;
  }
  const aiReRender = () => RENDER.ai(bodyEl("ai"), {});
  ACT["ai-web-on"] = () => { T().setWeb({ on: !T().web().on }); aiReRender(); };
  ACT["ai-web-method"] = (el) => { T().setWeb({ method: el.dataset.v }); aiReRender(); };
  ACT["ai-web-engine"] = (el) => { T().setWeb({ engine: el.dataset.v }); aiReRender(); };
  const webOut = (cls, html) => { const o = $("st-ai-wout"); if (o) o.innerHTML = cls ? `<span class="${cls}">${html}</span>` : html; };
  ACT["ai-web-savekey"] = async () => {
    const i = $("st-ai-skey");
    if (!i) return;
    if (!i.value.trim()) { i.focus(); return; }
    try { await T().saveSearchKey(i.value); i.value = ""; await aiReRender(); webOut("ok", "Key saved."); }
    catch (e) { webOut("bad", h(e.message)); }
  };
  ACT["ai-web-forget"] = async () => { try { await T().forgetSearchKey(); } catch (e) { /* already gone */ } aiReRender(); };
  ACT["ai-web-test"] = async (el) => {
    el.disabled = true;
    webOut("", "Searching…");
    try { const r = await T().testSearch(); webOut(r.n ? "ok" : "bad", r.n ? `Works — ${plural(r.n, "result")}${r.first ? `, first from ${h(r.first)}` : ""}.` : "No results came back."); }
    catch (e) { webOut("bad", h(e && e.message || "Failed")); }
    el.disabled = false;
  };
  INPUT["ai-model"] = (el) => { T().set({ model: el.value }); };
  CHANGE["ai-model"] = (el) => { T().set({ model: el.value }); paintOr(); };
  // Return in the filter box uses what was typed as the model id (when the list can't load).
  document.addEventListener("keydown", (e) => {
    if (e.key === "Enter" && e.target && e.target.id === "st-ai-orq" && e.target.value.trim() && !AI.or) { e.preventDefault(); T().set({ model: e.target.value }); paintOr(); }
  });

  /* ---------- Appearance ---------- */
  const themeNow = () => (typeof ui === "object" && ui && ui.theme) || lsGet("rig.theme") || "dark";
  const PANE_KEYS = ["rig.lw", "rig.rw", "rig.lc", "rig.rc"];
  RENDER.appearance = (b) => {
    const t = themeNow();
    const tile = (v, label) => `<button type="button" class="st-theme${t === v ? " on" : ""}" data-act="theme" data-v="${v}" aria-pressed="${t === v}">
        <span class="st-sw st-sw-t-${v}" aria-hidden="true"><span class="st-sw-in st-sw-${v === "light" ? "light" : "dark"}"><i></i><i></i><i></i></span>${v === "system" ? '<span class="st-sw-in st-sw-light"><i></i><i></i><i></i></span>' : ""}</span>${label}</button>`;
    const showB = typeof ui === "object" && ui ? ui.showBypassed !== false : lsGet("rig.showBypassed") !== "0";
    const sort = typeof rigsUI === "object" && rigsUI ? rigsUI.sort : lsGet("rig.rigSort") === "name" ? "name" : "recent";
    const custom = PANE_KEYS.some((k) => lsGet(k) != null && lsGet(k) !== "0");
    b.innerHTML = `
      <div class="fgroup-t">Theme</div>
      <div class="st-themes" data-k="theme dark light system">${tile("dark", "Dark")}${tile("light", "Light")}${tile("system", "Match macOS")}</div>
      <div class="fgroup-t">Workspace</div>
      <div class="fgroup">
        ${row("Bypassed units", sw("bypassed", showB, "Show bypassed units in the signal chain"), { k: "hide bypassed" })}
        ${row("Your rigs", seg("rigsort", sort, [["recent", "Recently saved first"], ["name", "By name"]]), { k: "sort order rigs list" })}
        ${row("Side panes", `<div class="ctrl">${btn("panes", "Reset widths", { disabled: custom ? "" : "Already at the default layout" })}<span class="ro">Both panes shown at their default widths</span></div>`, { k: "layout resize pane width collapse" })}
      </div>
      ${LP() ? `<div class="fgroup-t">Right pane</div><div class="fgroup st-items" id="st-lp-cards">${lpCardsHTML()}</div>` : ""}`;
  };
  /* Right-pane cards (listen-pane.js → ListenPane): show / hide, order (top to bottom), reset. */
  const LP = () => window.ListenPane && typeof ListenPane.cards === "function" ? ListenPane : null;
  function lpCardsHTML() {
    const cards = LP().cards(), n = cards.length;
    return cards.map((c, i) => `<div class="st-item st-lp-card${c.visible ? "" : " off"}" data-k="right pane card ${h(c.id)}">
        <span class="st-ord"><button type="button" class="icon-btn" data-act="lp-move" data-v="${h(c.id)}" data-d="-1" title="Move up" aria-label="Move ${h(c.label)} up"${i ? "" : " disabled"}>${ICON.up}</button><button type="button" class="icon-btn" data-act="lp-move" data-v="${h(c.id)}" data-d="1" title="Move down" aria-label="Move ${h(c.label)} down"${i < n - 1 ? "" : " disabled"}>${ICON.down}</button></span>
        <div class="st-nm"><b>${h(c.label)}</b></div>
        <button type="button" class="switch${c.visible ? " on" : ""}" data-act="lp-show" data-v="${h(c.id)}" role="switch" aria-checked="${c.visible}" aria-label="Show the ${h(c.label)} card"></button></div>`).join("")
      + `<div class="st-item" data-k="right pane reset default"><div class="st-nm"><span>Cards show top to bottom in this order. You can also drag them in the pane.</span></div>${btn("lp-reset", "Reset")}</div>`;
  }
  function paintLpCards() { const g = $("st-lp-cards"); if (g && LP()) g.innerHTML = lpCardsHTML(); }
  ACT["lp-show"] = (el) => { LP().setVisible(el.dataset.v, !el.classList.contains("on")); paintLpCards(); };
  ACT["lp-move"] = (el) => {
    const ids = LP().cards().map((c) => c.id), i = ids.indexOf(el.dataset.v), j = i + Number(el.dataset.d);
    if (i < 0 || j < 0 || j >= ids.length) return;
    [ids[i], ids[j]] = [ids[j], ids[i]];
    LP().setOrder(ids);
    paintLpCards();
    const f = modal().querySelector(`[data-act="lp-move"][data-v="${el.dataset.v}"][data-d="${el.dataset.d}"]:not(:disabled)`);
    if (f) f.focus();
  };
  ACT["lp-reset"] = () => { LP().reset(); paintLpCards(); };
  ACT.theme = (el) => {
    if (typeof setTheme === "function") setTheme(el.dataset.v); else lsSet("rig.theme", el.dataset.v);
    modal().querySelectorAll(".st-theme").forEach((x) => { const on = x === el; x.classList.toggle("on", on); x.setAttribute("aria-pressed", String(on)); });
  };
  ACT.bypassed = (el) => {
    if (typeof toggleShowBypassedUnits === "function") toggleShowBypassedUnits();
    const on = typeof ui === "object" ? ui.showBypassed !== false : true;
    el.classList.toggle("on", on); el.setAttribute("aria-checked", String(on));
  };
  ACT.rigsort = (el) => { if (typeof setRigSort === "function") setRigSort(el.dataset.v); paintSeg(el); };
  ACT.panes = (el) => {
    PANE_KEYS.forEach((k) => { try { localStorage.removeItem(k); } catch (e) { /* ignore */ } });
    if (typeof loadPanes === "function") loadPanes();
    el.disabled = true; el.title = "Already at the default layout";
  };
  function paintSeg(el) {
    el.parentElement.querySelectorAll("button").forEach((x) => { const on = x === el; x.classList.toggle("on", on); x.setAttribute("aria-pressed", String(on)); });
  }

  /* ---------- Library & Content ---------- */
  const DEFAULT_RIGS_DIR = "~/Library/Application Support/Headroom/User Presets";
  RENDER.library = async (b) => {
    if (!b.childElementCount) b.innerHTML = `<div class="comp-empty">Loading…</div>`;
    const [irs, nam, backing] = await Promise.all([
      api("/api/irs").catch(() => null),
      api("/api/nam").catch(() => null),
      hosted() ? null : api("/api/backing/list").catch(() => null),
    ]);
    const rigs = typeof userRigs === "function" ? userRigs() : [];
    const withPath = rigs.find((r) => r.path);
    const rigDir = withPath ? withPath.path.replace(/\/[^/]+$/, "") : DEFAULT_RIGS_DIR;
    const author = lsGet("rig.author") || "";
    const no = inApp() ? "" : IN_APP_ONLY;

    const irN = irs && typeof irs.count === "number" ? plural(irs.count, "IR") : "";
    const bk = backing && backing.ok && backing.dir ? backing : null;
    b.innerHTML = `
      <div class="fgroup-t">Your rigs</div>
      <div class="fgroup">
        ${fileRow("Folder", rigDir, rigs.length ? plural(rigs.length, "rig") : "", inApp() ? btn("rigs-reveal", "Show in Finder", { icon: ICON.folder }) : "", "rigs folder finder")}
        ${row("Author name", `<div class="st-stack"><input class="st-in" id="st-author" data-in="author" type="text" maxlength="60" spellcheck="false" placeholder="Your name" value="${h(author)}"><div class="st-hint">Filled in when you save a rig.</div></div>`, { top: true, for: "st-author", k: "author name save" })}
      </div>
      <div class="fgroup-t">Impulse responses</div>
      <div class="fgroup">
        ${irs ? fileRow("Your IRs", irs.folder, irN, inApp() ? btn("ir-reveal", "Show in Finder", { icon: ICON.folder }) : "", "ir impulse response wav folder") : row("Your IRs", `<span class="ro">The engine didn’t report an IR folder.</span>`)}
        ${hosted() ? "" : row("Import", `<div class="ctrl">${btn("ir-import", "Import .wav files…", { icon: ICON.import, disabled: no })}<span class="ro">Files or folders · they appear in the IR Loader’s list</span></div>`, { k: "ir impulse wav import cabinet loader" })}
      </div>
      <div class="fgroup-t">NAM captures</div>
      <div class="fgroup">
        ${nam && nam.folder ? fileRow("Your captures", nam.folder, "", bridge() ? btn("nam-reveal", "Show in Finder", { icon: ICON.folder }) : "", "nam neural amp modeler captures folder") : row("Your captures", `<span class="ro">The engine didn’t report a captures folder.</span>`)}
        ${row("Import", `<div class="ctrl">${btn("nam-import", "Import .nam captures…", { icon: ICON.import, disabled: bridge() ? "" : IN_APP_ONLY })}<span class="ro">Files or folders · they appear in the NAM Capture unit</span></div>`, { k: "nam neural amp modeler import" })}
      </div>
      ${bk ? `<div class="fgroup-t">Backing tracks</div>
      <div class="fgroup">
        ${fileRow("Folder", bk.dir, Array.isArray(bk.tracks) ? plural(bk.tracks.length, "track") : "", (inApp() && typeof window.headroomBackingImport === "function" ? btn("bk-import", "Add tracks…", { icon: ICON.import }) : "") + (inApp() ? btn("bk-reveal", "Show in Finder", { icon: ICON.folder }) : ""), "backing tracks practice audio folder")}
      </div>` : ""}`;
  };
  INPUT.author = (el) => lsSet("rig.author", el.value.trim());
  ACT["rigs-reveal"] = () => { if (typeof revealRigsFolder === "function") revealRigsFolder(); };
  ACT["ir-import"] = () => { if (window.HRCabinet && HRCabinet.importIr) HRCabinet.importIr(); else post("importIr"); };
  ACT["ir-reveal"] = () => { if (window.HRCabinet && HRCabinet.revealIrFolder) HRCabinet.revealIrFolder(); else post("revealIrFolder"); };
  ACT["nam-import"] = () => { if (typeof namBridge === "function") namBridge("importNam"); else post("importNam"); };
  ACT["nam-reveal"] = () => { if (typeof namBridge === "function") namBridge("revealNamFolder"); else post("revealNamFolder"); };
  ACT["bk-import"] = () => post("importBackingTracks");
  ACT["bk-reveal"] = () => post("revealBackingFolder");
  // Imports finish in a native callback: refresh the counts if Library is on screen.
  function refreshLibrarySoon() { setTimeout(() => { if (isOpen() && St.sec === "library") RENDER.library(bodyEl("library")); }, 400); }
  function hookImports() {
    ["headroomIrImported", "headroomNamImported"].forEach((name) => {
      const orig = window[name];
      if (typeof orig !== "function" || orig._hrSettings) return;
      const wrapped = function () { const r = orig.apply(this, arguments); refreshLibrarySoon(); return r; };
      wrapped._hrSettings = true;
      window[name] = wrapped;
    });
  }

  /* ---------- Plugins (plugins.js → HRPlugins) ---------- */
  const KIND = { effect: "Effect", music_effect: "Music effect", instrument: "Instrument" };
  RENDER.plugins = async (b) => {
    if (!window.HRPlugins || !HRPlugins.list) { b.innerHTML = statusLine("warn", "Plugin hosting isn’t loaded."); return; }
    if (!b.childElementCount) b.innerHTML = `<div class="comp-empty">Looking for Audio Units…</div>`;
    let d;
    try { d = await HRPlugins.list(); } catch (e) { b.innerHTML = statusLine("err", "Couldn’t reach the engine."); return; }
    const avail = d.available || [], imp = (d.imported || []).slice().sort((x, y) => String(x.name).localeCompare(String(y.name)));
    const au = avail.filter((p) => p.format === "AU"), vst = avail.filter((p) => p.format !== "AU");
    const auFree = au.filter((p) => p.supported && p.table_id == null).length;
    const meta = (p) => [p.vendor, KIND[p.kind] || p.kind, p.format === "AU" ? (p.v3 ? "AUv3" : "Audio Unit") : p.format, p.version && p.version !== "0.0.0" ? "v" + p.version : ""].filter(Boolean).join(" · ");
    b.innerHTML = `
      <div class="fgroup-t">Audio Units</div>
      <div class="fgroup">
        ${row("On this Mac", `<div class="ctrl st-wrapctl"><span class="ro">${plural(au.length, "Audio Unit")} installed · ${auFree} not imported</span><span class="st-acts">${btn("pl-rescan", "Rescan")}${btn("pl-import", "Import plugins…", { primary: true })}</span></div>`, { k: "audio units au installed import rescan" })}
        ${vst.length ? row("VST3", `<span class="ro">${plural(vst.length, "VST3 plugin")} found · ${h(d.vst3 && d.vst3.supported ? "supported" : "can’t be hosted yet")}</span>`, { k: "vst3" }) : ""}
      </div>
      <div class="fgroup-t">In Components · ${imp.length}</div>
      ${imp.length ? `<div class="fgroup st-items">${imp.map((p) => `<div class="st-item"><div class="st-nm"><b>${h(p.name)}</b><span>${h(meta(p))}</span></div>${p.installed === false ? '<span class="st-tag warn">Not installed</span>' : ""}<button type="button" class="pf-link" data-act="pl-remove" data-v="${h(p.table_id)}" title="Remove from Components (rigs that use it keep working)">Remove</button></div>`).join("")}</div>`
      : `<div class="fgroup st-items"><div class="st-item st-empty">None yet. Imported plugins appear in Components → Plugins and drag into any rig.</div></div>`}`;
  };
  ACT["pl-rescan"] = (el) => { el.disabled = true; el.textContent = "Scanning…"; RENDER.plugins(bodyEl("plugins")); };
  ACT["pl-import"] = () => { if (window.HRPlugins) HRPlugins.openImport(); };   // takes the modal; Settings comes back after
  ACT["pl-remove"] = async (el) => {
    el.disabled = true;
    try { await HRPlugins.remove(el.dataset.v); } catch (e) { say("Couldn’t reach the engine."); }
    RENDER.plugins(bodyEl("plugins"));
  };

  /* ---------- MIDI ---------- */
  const SCENE_L = ["A", "B", "C", "D"];
  function targetLabel(t) {
    let m;
    if ((m = /^macro:(\d+)$/.exec(t))) return `Macro ${+m[1] + 1}`;
    if ((m = /^scene:(\d+)$/.exec(t))) return `Scene ${SCENE_L[+m[1]] || +m[1] + 1}`;
    if ((m = /^looper:track:(\d+)$/.exec(t))) return `Looper track ${m[1]}`;
    return ({ "looper:toggle": "Looper record / play / overdub", "looper:stop": "Looper start / stop all", "looper:clear": "Looper clear",
      "looper:undo": "Looper undo", "looper:redo": "Looper redo", tap_tempo: "Tap tempo", "transpose:down": "Transpose down a semitone", "transpose:up": "Transpose up a semitone",
      "bloom:engage": "Bloom engage (hold / latch)", "bloom:toggle": "Bloom engage (toggle)", "bloom:motion": "Bloom motion" })[t] || t;
  }
  RENDER.midi = async (b) => {
    let map = null;
    try { map = await api("/api/midi-learn"); } catch (e) { map = null; }
    const binds = map && Array.isArray(map.bindings) ? map.bindings.slice().sort((x, y) => x.cc - y.cc) : null;
    b.innerHTML = `
      <div class="fgroup-t">MIDI out</div>
      <div class="fgroup">
        ${row("Port", `<div class="ctrl st-wrapctl"><span class="st-chip">Headroom MIDI Out</span><span class="ro">Notes from Pitch → MIDI and the MIDI units. Pick it as a MIDI input in your DAW or synth.</span></div>`, { k: "midi out port virtual source daw" })}
      </div>
      <div class="fgroup-t">Controller map${binds ? ` · ${binds.length}` : ""}</div>
      ${binds ? `<div class="fgroup st-items">${binds.map((x) => `<div class="st-item" data-k="cc ${x.cc}"><span class="st-cc">CC ${x.cc}</span><div class="st-nm"><b>${h(targetLabel(x.target))}</b></div><span class="st-meta">${x.channel ? `Ch ${x.channel}` : "Any channel"}</span></div>`).join("") || `<div class="st-item st-empty">No mappings.</div>`}</div>`
      : statusLine("warn", "This engine build doesn’t report a controller map.")}`;
  };

  /* ---------- Live (live-view.js → HRLive.prefs) ---------- */
  RENDER.live = (b) => {
    const P = window.HRLive && HRLive.prefs;
    if (!P) { b.innerHTML = statusLine("warn", "The Live view isn’t loaded."); return; }
    const v = P.get();
    b.innerHTML = `
      <div class="fgroup-t">Stage</div>
      <div class="fgroup">
        ${row("Lights", seg("live-lights", v.lights, [["dim", "Dim stage"], ["theme", "Follow the app theme"]]), { k: "dim stage lights dark" })}
        ${row("Layout", seg("live-full", v.full ? 1 : 0, [[1, "Full width"], [0, "Keep the side panes"]]), { k: "full width panels" })}
      </div>
      <div class="fgroup-t">Tuner</div>
      <div class="fgroup" id="st-tuner">
        ${LP() && LP().instruments ? tunerRowsHTML() : ""}
        ${row("Mute in Live", sw("live-tmute", v.tunerMute, "Mute the output while Live’s tuner is open"), { k: "tuner mute" })}
      </div>
      <div class="fgroup-t">Rig stepping</div>
      <div class="fgroup">
        ${row("Step through", seg("live-mode", v.mode, [["list", "The current list"], ["setlist", "The setlist", v.setlist ? "" : "Add songs to the setlist first"]]), { k: "step next previous list setlist" })}
        ${row("Setlist", `<div class="ctrl"><span class="ro">${v.setlist ? plural(v.setlist, "song") : "Empty"}</span>${btn("live-setlist", "Edit in Live…")}</div>`, { k: "setlist songs edit" })}
      </div>
      <div class="fgroup-t">Keys in Live</div>
      <div class="st-keys" data-k="shortcuts keyboard footswitch">${[["← →", "Previous / next rig"], ["1–4", "Scenes"], ["T", "Tap tempo"], ["U", "Tuner"], ["B", "Bypass"], ["M", "Mute"], ["R / S", "Looper rec · stop"], ["F", "Full width"], ["/", "Jump to a rig"]].map(([k, l]) => `<span><kbd>${k}</kbd>${l}</span>`).join("")}</div>`;
  };
  // Shared by the right-pane tuner and Live's tuner (listen-pane.js owns them; the engine gets them from there).
  function tunerRowsHTML() {
    const st = LP().state(), insts = LP().instruments();
    const inst = insts.find((x) => x.id === st.instrument) || insts[0];
    const tun = inst.tunings.find((t) => t.id === st.tuning) || inst.tunings[0];
    const a4 = Number(st.a4) || 440;
    return `${row("Instrument", `<select class="st-in" id="st-tn-inst" data-ch="tn-inst">${insts.map((x) => `<option value="${h(x.id)}"${x.id === inst.id ? " selected" : ""}>${h(x.label)}</option>`).join("")}</select>`, { for: "st-tn-inst", k: "instrument guitar bass ukulele violin cello banjo mandolin chromatic 7-string 8-string" })}
      ${inst.tunings.length > 1 || tun.notes ? row("Tuning", `<div class="st-inrow">${inst.tunings.length > 1 ? `<select class="st-in" id="st-tn-tuning" data-ch="tn-tuning">${inst.tunings.map((t) => `<option value="${h(t.id)}"${t.id === tun.id ? " selected" : ""}>${h(t.label)}</option>`).join("")}</select>` : `<span class="ro">${h(tun.label)}</span>`}${tun.notes ? `<span class="st-meta st-notes">${h(tun.notes.replace(/b(?=\d)/g, "♭").replace(/#/g, "♯"))}</span>` : ""}</div>`, { for: "st-tn-tuning", k: "tuning drop standard open dadgad" }) : ""}
      ${row("Reference A4", `<div class="st-inrow st-ref"><button type="button" class="btn" data-act="tn-ref" data-d="-0.5" title="Lower 0.5 Hz" aria-label="Lower the reference 0.5 Hz">−</button><input class="st-in st-mono" id="st-tn-a4" data-ch="tn-a4" type="number" min="415" max="466" step="0.1" value="${a4}" aria-label="A4 reference in hertz"><span class="ro">Hz</span><button type="button" class="btn" data-act="tn-ref" data-d="0.5" title="Raise 0.5 Hz" aria-label="Raise the reference 0.5 Hz">+</button>
        <span class="st-pills">${[432, 440, 442].map((f) => `<button type="button" class="st-pill${a4 === f ? " on" : ""}" data-act="tn-refset" data-v="${f}">${f}</button>`).join("")}</span></div>`, { for: "st-tn-a4", k: "reference a4 hz pitch 440 432 442 concert" })}`;
  }
  function paintTuner() {
    const g = $("st-tuner");
    if (!g || !LP()) return;
    const mute = g.querySelector('[data-act="live-tmute"]');
    const keep = mute ? mute.closest(".frow").outerHTML : "";
    g.innerHTML = tunerRowsHTML() + keep;
  }
  CHANGE["tn-inst"] = (el) => { const inst = LP().instruments().find((x) => x.id === el.value); if (inst) { LP().setTuning(inst.id, inst.tunings[0].id); paintTuner(); } };
  CHANGE["tn-tuning"] = (el) => { LP().setTuning(LP().state().instrument, el.value); paintTuner(); };
  CHANGE["tn-a4"] = (el) => { const f = parseFloat(el.value); if (Number.isFinite(f)) LP().setReference(f); paintTuner(); };
  ACT["tn-ref"] = (el) => { LP().setReference((Number(LP().state().a4) || 440) + Number(el.dataset.d)); paintTuner(); };
  ACT["tn-refset"] = (el) => { LP().setReference(Number(el.dataset.v)); paintTuner(); };
  ACT["live-lights"] = (el) => { HRLive.prefs.set("lights", el.dataset.v); paintSeg(el); };
  ACT["live-full"] = (el) => { HRLive.prefs.set("full", el.dataset.v === "1"); paintSeg(el); };
  ACT["live-tmute"] = (el) => {
    HRLive.prefs.set("tunerMute", !HRLive.prefs.get().tunerMute);
    const on = HRLive.prefs.get().tunerMute;
    el.classList.toggle("on", on); el.setAttribute("aria-checked", String(on));
  };
  ACT["live-mode"] = (el) => { HRLive.prefs.set("mode", el.dataset.v); paintSeg(el); };
  ACT["live-setlist"] = () => {
    close();
    if (window.HRViews) HRViews.show("live");
    setTimeout(() => { if (window.HRLive && HRLive.prefs) HRLive.prefs.openSetlist(); }, 60);
  };

  /* ---------- About ---------- */
  RENDER.about = (b) => {
    const mark = document.querySelector(".topbar .brand-mark");
    const where = hosted() ? "Plugin, running in your DAW" : bridge() ? "Standalone app" : "Browser preview";
    let host = base();
    try { host = new URL(base()).host; } catch (e) { /* keep */ }
    const K = [["⌘ ,", "Settings"], ["⌘ S", "Save the rig"], ["⇧ ⌘ S", "Save as a new rig"], ["⌘ 1–7", "Switch view, in tab order"],
      ["[ ]", "Previous / next rig"], ["/", "Search"], ["Space", "Play / stop the DI player"], ["Esc", "Close a dialog or menu"]];
    b.innerHTML = `
      <div class="st-about" data-k="headroom version">
        ${mark ? `<span class="st-mark">${mark.innerHTML}</span>` : ""}
        <div><b>Headroom</b><span>${h(where)}</span></div>
      </div>
      <div class="fgroup">
        ${row("Engine", `<span class="ro st-mono">${h(host)}</span>`, { k: "engine port http" })}
        ${hosted() ? "" : row("Data folder", `<span class="ro st-mono">~/Library/Application Support/Headroom</span>`, { k: "support folder data" })}
      </div>
      <div class="fgroup-t">Keyboard shortcuts</div>
      <div class="st-keys st-keys-wide" data-k="shortcuts keyboard">${K.map(([k, l]) => `<span><kbd>${k}</kbd>${h(l)}</span>`).join("")}</div>
      <div class="fgroup-t">Names and trademarks</div>
      ${window.HRNative?.asio ? '<p class="st-legal"><img src="assets/asio-compatible.png" width="76" alt="ASIO® Compatible"><br>ASIO is a registered trademark of Steinberg Media Technologies GmbH.</p>' : ""}
      <p class="st-legal" data-k="disclaimer trademarks affiliation artist inspired library" style="margin:0 2px 10px;font-size:11.5px;line-height:1.5;color:var(--text-3)">Artist, song, amplifier, pedal and other product names in Headroom, its library and its unit descriptions are used only to describe a sound or the equipment a model was studied from. They belong to their owners. Headroom is not affiliated with, sponsored or endorsed by any of them. Library rigs “inspired by” a recording are original settings built from public interviews and rig rundowns, not captures of the original gear.</p>`;
  };

  /* =====================================================================
     ⌘, and boot
     ===================================================================== */
  window.addEventListener("keydown", (e) => {
    if ((e.metaKey || e.ctrlKey) && !e.altKey && !e.shiftKey && (e.key === "," || e.code === "Comma")) {
      e.preventDefault();
      e.stopPropagation();
      if (isOpen()) { const q = $("st-q"); if (q) q.focus(); }
      else open();
    }
  }, true);
  const boot = () => hookImports();
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", boot); else boot();

  window.HRSettings = { open: (sec, opts) => { hookImports(); open(sec, opts); }, close, isOpen, sections: () => sections().map((s) => s.id) };
})();
