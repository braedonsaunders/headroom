/* Headroom — centre-pane views: Signal Chain | Room | Live | Looper | Practice ‖ Component | Guitar Tech  (studio-views.js)
 *
 *   - Other files plug views in with HRViews.register(name, { enter(), leave() }) and render into
 *     #view-<name> (Room: assets/stage-room.js → #view-room; Live: assets/live-view.js → #view-live; Guitar Tech: assets/guitar-tech.js → #view-tech).
 *
 *   - Tab controller for the centre pane. "Signal Chain" is index.html's #rack-bay; "Component" hosts the
 *     component detail page (component-detail.js calls HRViews.onDetailOpen / onDetailClose). The Looper is
 *     assets/looper-view.js and Practice is assets/practice-view.js (both registered like the other external views).
 *
 * Loaded in <head> before index.html's script; it only touches the page's globals (API_BASE, rackState, ui,
 * queueSend, setLeftTab, toast, esc …) at call time. index.html forwards telemetry via HRViews.onTelemetry().
 */
(function () {
  "use strict";
  const $ = (id) => document.getElementById(id);
  const api = async (path) => { const r = await fetch(API_BASE + path, { cache: "no-store" }); if (!r.ok) throw new Error(r.status); return r.json(); };
  const clamp = (v, a, b) => Math.max(a, Math.min(b, v));
  const typingIn = (a) => a && (a.tagName === "INPUT" && a.type !== "range" || a.tagName === "SELECT" || a.tagName === "TEXTAREA" || a.isContentEditable);
  const fmtT = (s, dec = 1) => { s = Math.max(0, s || 0); const m = Math.floor(s / 60), r = s - m * 60; return `${m}:${r.toFixed(dec).padStart(dec ? dec + 3 : 2, "0")}`; };
  const cssVar = (n) => getComputedStyle(document.body).getPropertyValue(n).trim();
  const VIEWS = ["chain", "room", "live", "looper", "practice", "component", "fretboard", "tech"];
  const EXT = new Map();   // views registered by other files: name -> { enter, leave }
  const V = { view: "chain", compName: "" };

  /* =====================================================================
     TABS
     ===================================================================== */
  function show(view) {
    if (!VIEWS.includes(view)) view = "chain";
    const prev = V.view;
    V.view = view;
    if (window.HRActivity) HRActivity.setView(view);   // loops and pollers follow the active tab (assets/activity.js)
    const stage = $("stage");
    if (!stage) return;
    stage.dataset.view = view;
    if (prev !== view) applyRightPane(view);
    document.querySelectorAll("#view-tabs [data-view]").forEach((b) => {
      const on = b.dataset.view === view;
      b.classList.toggle("on", on);
      b.setAttribute("aria-selected", on);
    });
    const detailOpen = !!(window.ComponentDetail && ComponentDetail.isOpen());
    $("view-component-empty").hidden = !(view === "component" && !detailOpen);
    $("view-looper").hidden = view !== "looper";
    for (const [name, h] of EXT) {
      const el = $("view-" + name);
      if (el) el.hidden = view !== name;
      try { if (view === name) { if (prev !== name && h.enter) h.enter(); } else if (prev === name && h.leave) h.leave(); } catch (err) { console.error(err); }
    }
    if (view === "component" && !detailOpen) renderComponentEmpty();
    // Looper: rebuilt in assets/looper-view.js (registers "looper" via HRViews.register, enter/leave run in the EXT loop above).
    // Practice: rebuilt in assets/practice-view.js (registers "practice"; enter/leave run in the EXT loop above).
    if (typeof closeMenu === "function") closeMenu();
    // Views measured while hidden (lane zoom, detail hero) re-fit when they come back.
    if (prev !== view && (view === "chain" || view === "component")) requestAnimationFrame(() => window.dispatchEvent(new Event("resize")));
  }
  function onDetailOpen(name) {
    V.compName = name || "";
    const sub = $("vt-comp-name");
    if (sub) sub.textContent = V.compName;
    show("component");
  }
  function onDetailClose() {
    V.compName = "";
    const sub = $("vt-comp-name");
    if (sub) sub.textContent = "";
    if (V.view === "component") show("chain");
  }

  /* ---------- Component tab with nothing open ---------- */
  function renderComponentEmpty() {
    const box = $("view-component-empty");
    if (!box) return;
    const slots = (typeof rackState !== "undefined" && rackState.slots) || [];
    const known = slots.filter((s) => typeof comps !== "undefined" && comps.byTid && comps.byTid.has(s.table_id));
    const chips = known.slice(0, 12).map((s) => `<button class="ce-chip" data-ce-tid="${s.table_id}" data-ce-slot="${s.slot_idx}"><i style="background:${typeof swatchFor === "function" ? swatchFor(s) : "var(--line-3)"}"></i>${esc(s.reference_name || s.display_name)}</button>`).join("");
    box.innerHTML = `<div class="ce-wrap">
      <div class="ce-art" aria-hidden="true">
        <svg viewBox="0 0 120 64" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round">
          <rect x="6" y="18" width="40" height="30" rx="4" opacity=".5"/><circle cx="16" cy="28" r="3" opacity=".5"/><circle cx="26" cy="28" r="3" opacity=".5"/><circle cx="36" cy="28" r="3" opacity=".5"/><path d="M12 40h28" opacity=".35"/>
          <path d="M52 33h10" opacity=".5"/><path d="M59 30l3 3-3 3" opacity=".5"/>
          <g class="acc"><rect x="68" y="8" width="46" height="48" rx="6"/><path d="M75 44c5-14 9-22 12-22s5 10 8 10 5-8 7-8 3 4 5 6"/><path d="M75 50h32" opacity=".5"/><circle cx="87" cy="22" r="1.6" fill="currentColor"/></g>
        </svg>
      </div>
      <h3>Look inside any unit</h3>
      <p>Click a card in <a href="#" data-ce-comps>Components</a>, or right-click a unit in the chain and choose <b>Component details</b>. You’ll see how it’s modelled — the circuit, its curves and every value the engine uses.</p>
      ${chips ? `<div class="ce-in">In this rig</div><div class="ce-chips">${chips}</div>` : ""}
      <button class="btn" data-ce-comps>Browse Components</button>
    </div>`;
  }
  function wireComponentEmpty() {
    const box = $("view-component-empty");
    box.addEventListener("click", (e) => {
      if (e.target.closest("[data-ce-comps]")) { e.preventDefault(); setLeftTab("components"); const q = $("comp-search"); if (q) q.focus(); return; }
      const c = e.target.closest("[data-ce-tid]");
      if (c && window.ComponentDetail) { if (typeof selectUnit === "function") selectUnit(+c.dataset.ceSlot, false); ComponentDetail.open(+c.dataset.ceTid); }
    });
  }

  /* LOOPER — moved to assets/looper-view.js (+ looper-view.css), which registers the "looper" view itself. */

  /* PRACTICE — moved to assets/practice-view.js (+ practice-view.css): backing tracks, YouTube, and the DI-player
     trainer under "Test DI tracks". It registers the "practice" view itself. */

  /* =====================================================================
     WIRING
     ===================================================================== */
  function onTelemetry(d) {
    // Practice (assets/practice-view.js) gets telemetry even while hidden: its tab LED shows a playing backing track.
    const pr = EXT.get("practice");
    if (pr && pr.onTelemetry) try { pr.onTelemetry(d); } catch (err) { console.error(err); }
    // Looper (assets/looper-view.js): its tab LED follows telemetry's "looper" summary (no background poll).
    const lx = EXT.get("looper");
    if (lx && lx.onTelemetry) try { lx.onTelemetry(d); } catch (err) { console.error(err); }
    // Live view (assets/live-view.js): meters / tuner status, only while it's open
    if (V.view === "live") { const h = EXT.get("live"); if (h && h.onTelemetry) try { h.onTelemetry(d); } catch (err) { console.error(err); } }
  }
  function init() {
    const tabs = $("view-tabs");
    if (!tabs) return;
    wrapPaneToggle();
    applyRightPane(V.view);
    tabs.addEventListener("click", (e) => { const b = e.target.closest("[data-view]"); if (b) show(b.dataset.view); });
    tabs.addEventListener("keydown", (e) => {
      if (e.key !== "ArrowRight" && e.key !== "ArrowLeft") return;
      e.preventDefault();
      const i = VIEWS.indexOf(V.view), n = VIEWS[(i + (e.key === "ArrowRight" ? 1 : VIEWS.length - 1)) % VIEWS.length];
      show(n);
      const b = tabs.querySelector(`[data-view="${n}"]`);
      if (b) b.focus();
    });
    wireComponentEmpty();
    // Picking a unit in the right-hand chain list shows it in the chain.
    const tf = $("topology-flow");
    if (tf) tf.addEventListener("click", (e) => { if (V.view !== "chain" && e.target.closest("[data-sel]") && !e.target.closest("[data-byp]")) show("chain"); }, true);
    document.addEventListener("keydown", (e) => {
      if (e.metaKey || e.ctrlKey || e.altKey || e.repeat) return;
      if ((typeof ui !== "undefined" && ui.modalOpen) || typingIn(document.activeElement)) return;
      const menu = $("menu");
      if (menu && !menu.hidden) return;
      // Looper keys are handled by assets/looper-view.js, Practice keys by assets/practice-view.js.
      if (e.key === "Escape" && (V.view === "looper" || V.view === "practice" || V.view === "tech" || (V.view === "component" && !(window.ComponentDetail && ComponentDetail.isOpen())))) show("chain");
    });
    // (Looper tab LED: assets/looper-view.js polls the engine in the background.)
  }
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", init); else init();

  /* Right pane follows the tab: open where it helps (Signal Chain, Looper, Backing), closed where the view wants the
     room (Room, Live, Component, Guitar Tech, Fretboard). The toggle button still works on any tab; a manual
     toggle is remembered for that tab for the rest of the session. */
  const RP_OPEN = { chain: true, room: false, live: false, looper: true, practice: true, component: false, tech: false, fretboard: false };
  const rpOverride = new Map();
  function applyRightPane(view) {
    if (typeof ui === "undefined" || typeof applyPanes !== "function") return;
    const open = rpOverride.has(view) ? rpOverride.get(view) : (RP_OPEN[view] !== undefined ? RP_OPEN[view] : true);
    if (!!ui.rc === !open) return;
    ui.rc = !open;
    applyPanes(true);
  }
  function wrapPaneToggle() {
    if (typeof window.togglePane !== "function" || window.togglePane._hrWrapped) return;
    const orig = window.togglePane;
    window.togglePane = function (side) { orig(side); if (side === "r" && typeof ui !== "undefined") rpOverride.set(V.view, !ui.rc); };
    window.togglePane._hrWrapped = true;
  }
  function register(name, handlers) { EXT.set(name, handlers || {}); if (V.view === name && handlers && handlers.enter) handlers.enter(); }
  window.HRViews = { show, onDetailOpen, onDetailClose, onTelemetry, register, current: () => V.view };
})();
