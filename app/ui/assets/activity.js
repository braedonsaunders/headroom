/* Headroom — HRActivity: the one place that knows whether the UI is being looked at (activity.js)
 *
 * Idle-CPU rule: when Headroom isn't displayed it costs nothing. In a DAW with eight instances, a closed
 * or hidden editor must not poll, animate or draw. Every loop, poller and animated layer asks this module.
 *
 *   awake    = the page is visible (document.visibilityState) AND the host says the view is shown
 *              (plugin editors call HRActivity.setHostVisible(false) when the DAW hides or closes the window)
 *   view     = the active centre tab (studio-views.js reports it with setView)
 *
 *   HRActivity.awake()            → boolean
 *   HRActivity.view()             → "chain" | "room" | "live" | …
 *   HRActivity.isActive(v)        → awake && view === v
 *   HRActivity.on(fn)             → fn({ awake, view, prev }) on every change; returns an unsubscribe function
 *   HRActivity.setView(v)         → studio-views.js
 *   HRActivity.setHostVisible(b)  → native editor (plugin/editor/HeadroomEditor.mm) / app shell
 *
 * While asleep <html> carries .hr-asleep, which pauses every CSS animation (see the style block below).
 * Loaded first in <head>, before any other script.
 */
(function () {
  "use strict";
  const A = { host: true, view: "chain", awake: true, subs: new Set() };
  const pageVisible = () => document.visibilityState !== "hidden";
  function update() {
    const awake = pageVisible() && A.host;
    if (awake === A.awake) return;
    A.awake = awake;
    document.documentElement.classList.toggle("hr-asleep", !awake);
    emit(A.view);
  }
  function emit(prev) {
    const ev = { awake: A.awake, view: A.view, prev };
    A.subs.forEach((fn) => { try { fn(ev); } catch (e) { console.error(e); } });
  }
  document.addEventListener("visibilitychange", update);
  // Safety net for setHostVisible(false): a window the user is pointing at or typing into is being looked at,
  // whatever the host reported (a wrong "hidden" must never freeze a visible editor).
  const seen = () => { if (!A.host && pageVisible()) { A.host = true; update(); } };
  ["pointerdown", "pointermove", "wheel", "keydown"].forEach((t) => window.addEventListener(t, seen, { capture: true, passive: true }));
  window.addEventListener("focus", seen);
  // No animation runs while nobody can see it. Hidden views are display:none already, so this only covers
  // the window/editor being hidden (the top-bar logo, tempo LED, Listen pane, splash …).
  const st = document.createElement("style");
  st.textContent = "html.hr-asleep *, html.hr-asleep *::before, html.hr-asleep *::after { animation-play-state: paused !important; }";
  document.head.appendChild(st);
  A.awake = pageVisible();
  document.documentElement.classList.toggle("hr-asleep", !A.awake);

  window.HRActivity = {
    awake: () => A.awake,
    view: () => A.view,
    isActive: (v) => A.awake && A.view === v,
    on(fn) { A.subs.add(fn); return () => A.subs.delete(fn); },
    setView(v) { if (v === A.view) return; const prev = A.view; A.view = v; emit(prev); },
    setHostVisible(b) { A.host = !!b; update(); },
    // setInterval that only ticks while awake (no wakeups at all while hidden). Returns a stop function.
    every(ms, fn) {
      let t = 0, dead = false;
      const tick = () => { t = 0; if (dead || !A.awake) return; try { fn(); } catch (e) { console.error(e); } arm(); };
      const arm = () => { if (!t && !dead && A.awake) t = setTimeout(tick, ms); };
      const off = this.on(() => { if (A.awake) arm(); else { clearTimeout(t); t = 0; } });
      arm();
      return () => { dead = true; clearTimeout(t); off(); };
    },
  };
})();
