/* Headroom — top-bar IN / OUT level knobs (io-gain.css). A 16 px knob in front of each meter:
 *   IN  = level into the rig (DI player and live input), −24…+12 dB   → /api/audio/set?rig_input_db=
 *   OUT = master output level, −60 (off)…+12 dB                         → /api/audio/set?output_gain_db=
 * Drag (vertical or horizontal), scroll, arrow keys; ⇧ for fine; double-click resets to 0 dB. Both persist in the
 * engine's audio settings. Shown only when the engine reports them, and never in a DAW (the host has its own trims). */
(function () {
  "use strict";
  const K = {
    in: { key: "rig_input_db", min: -24, max: 12, label: "Input level", short: "IN", anchor: "#m-in" },
    out: { key: "output_gain_db", min: -60, max: 12, label: "Output level", short: "OUT", anchor: ".io .mbar-pair" },
  };
  const val = { in: 0, out: 0 };
  let ready = false, bubble = null, hoverId = null, dragId = null;
  const base = () => (typeof API_BASE !== "undefined" ? API_BASE : "http://127.0.0.1:17770");

  const norm = (id, db) => (db - K[id].min) / (K[id].max - K[id].min);
  const fmt = (id, db) => (id === "out" && db <= K.out.min + 0.05 ? "off" : `${db > 0.05 ? "+" : db < -0.05 ? "−" : ""}${Math.abs(db).toFixed(1)} dB`);

  function knobSVG(id) {
    const v = val[id], n = norm(id, v), z = norm(id, 0);
    const a0 = -135, ang = (t) => a0 + t * 270;
    const pt = (deg, r) => { const t = (deg - 90) * Math.PI / 180; return [8 + r * Math.cos(t), 8 + r * Math.sin(t)]; };
    const arc = (from, to) => {
      if (Math.abs(to - from) < 0.002) return "";
      const [x0, y0] = pt(ang(Math.min(from, to)), 6.4), [x1, y1] = pt(ang(Math.max(from, to)), 6.4);
      return `<path class="v" d="M${x0.toFixed(2)} ${y0.toFixed(2)} A6.4 6.4 0 ${Math.abs(to - from) * 270 > 180 ? 1 : 0} 1 ${x1.toFixed(2)} ${y1.toFixed(2)}"/>`;
    };
    const [tx0, ty0] = pt(ang(0), 6.4), [tx1, ty1] = pt(ang(1), 6.4);
    const [px, py] = pt(ang(n), 4.2), [zx, zy] = pt(ang(z), 7.9);
    return `<svg viewBox="0 0 16 16" aria-hidden="true"><path class="t" d="M${tx0.toFixed(2)} ${ty0.toFixed(2)} A6.4 6.4 0 1 1 ${tx1.toFixed(2)} ${ty1.toFixed(2)}"/>${arc(z, n)}<circle class="z" cx="${zx.toFixed(2)}" cy="${zy.toFixed(2)}" r=".8"/><circle class="c" cx="8" cy="8" r="4.6"/><line class="p" x1="8" y1="8" x2="${px.toFixed(2)}" y2="${py.toFixed(2)}"/></svg>`;
  }
  function paint(id) {
    document.querySelectorAll(`.io-knob[data-g="${id}"]`).forEach((b) => {
      b.innerHTML = knobSVG(id);
      b.classList.toggle("set", Math.abs(val[id]) > 0.05);
      b.setAttribute("aria-valuenow", val[id].toFixed(1));
      b.setAttribute("aria-valuetext", fmt(id, val[id]));
      b.title = `${K[id].label}: ${fmt(id, val[id])} · drag or scroll, ⇧ fine, double-click for 0 dB`;
    });
    if (bubble && (hoverId === id || dragId === id)) showBubble(id);
  }
  function set(id, db, send = true) {
    const k = K[id];
    db = Math.max(k.min, Math.min(k.max, Math.round(db * 10) / 10));
    if (db === val[id]) return;
    val[id] = db;
    paint(id);
    if (send) {
      const url = `/api/audio/set?${k.key}=${db}`;
      const done = (d) => {   // gain-only requests get a small reply ({gains}), the full one lists every device
        if (typeof audioCfg === "undefined") return;
        if (d && d.current) audioCfg = d;
        else if (d && d.gains && audioCfg && audioCfg.current) Object.assign(audioCfg.current, d.gains);
      };
      if (typeof queueSend === "function") queueSend("io-gain-" + id, url, done);
      else fetch(base() + url, { cache: "no-store" }).then((r) => r.json()).then(done).catch(() => {});
    }
  }

  function showBubble(id) {
    const b = document.querySelector(`.io-knob[data-g="${id}"]`);
    if (!b || !b.offsetParent) return hideBubble();
    if (!bubble) { bubble = document.createElement("div"); bubble.className = "io-bubble"; document.body.append(bubble); }
    bubble.innerHTML = `<span>${K[id].short}</span><b>${fmt(id, val[id])}</b>`;
    // to the left of the IN / OUT label, so it never covers the other row
    const r = b.getBoundingClientRect(), lbl = b.previousElementSibling, lr = lbl ? lbl.getBoundingClientRect() : r;
    bubble.style.left = `${Math.min(r.left, lr.left) - 8}px`;
    bubble.style.top = `${r.top + r.height / 2}px`;
    bubble.classList.add("on");
  }
  function hideBubble() { if (bubble) bubble.classList.remove("on"); }

  function install() {
    if (window.RIG_HOSTED || document.documentElement.classList.contains("hosted")) return false;
    const io = document.querySelector(".io.tb-c3");
    if (!io) return false;
    for (const id of ["in", "out"]) {
      if (io.querySelector(`.io-knob[data-g="${id}"]`)) continue;
      const anchor = document.querySelector(K[id].anchor);
      if (!anchor) return false;
      const b = document.createElement("button");
      b.className = "io-knob"; b.dataset.g = id; b.type = "button";
      b.setAttribute("role", "slider"); b.setAttribute("aria-label", K[id].label);
      b.setAttribute("aria-valuemin", K[id].min); b.setAttribute("aria-valuemax", K[id].max);
      anchor.before(b);
    }
    io.classList.add("has-gain");
    paint("in"); paint("out");
    return true;
  }

  function wire() {
    document.addEventListener("pointerdown", (e) => {
      const b = e.target.closest(".io-knob");
      if (!b || e.button !== 0) return;
      e.preventDefault();
      const id = b.dataset.g;
      dragId = id;
      try { b.setPointerCapture(e.pointerId); } catch (err) { /* synthetic or already released */ }
      let lx = e.clientX, ly = e.clientY, acc = val[id];
      document.body.classList.add("io-dragging");
      showBubble(id);
      const move = (me) => {
        const d = (ly - me.clientY) + (me.clientX - lx);
        lx = me.clientX; ly = me.clientY;
        acc += d * (me.shiftKey ? 0.05 : 0.25);
        // gentle detent at 0 dB
        set(id, Math.abs(acc) < 0.6 ? 0 : acc);
      };
      const up = () => {
        window.removeEventListener("pointermove", move);
        window.removeEventListener("pointerup", up);
        window.removeEventListener("pointercancel", up);
        dragId = null;
        document.body.classList.remove("io-dragging");
        if (hoverId !== id) hideBubble();
      };
      window.addEventListener("pointermove", move);
      window.addEventListener("pointerup", up);
      window.addEventListener("pointercancel", up);
    });
    document.addEventListener("dblclick", (e) => { const b = e.target.closest(".io-knob"); if (b) { e.preventDefault(); set(b.dataset.g, 0); } });
    document.addEventListener("wheel", (e) => {
      const b = e.target.closest(".io-knob");
      if (!b) return;
      e.preventDefault();
      const d = (Math.abs(e.deltaY) > Math.abs(e.deltaX) ? -e.deltaY : e.deltaX) > 0 ? 1 : -1;
      set(b.dataset.g, val[b.dataset.g] + d * (e.shiftKey ? 0.1 : 0.5));
    }, { passive: false });
    document.addEventListener("keydown", (e) => {
      const b = e.target.closest && e.target.closest(".io-knob");
      if (!b) return;
      const step = e.shiftKey ? 0.1 : 0.5, id = b.dataset.g;
      if (e.key === "ArrowUp" || e.key === "ArrowRight") { e.preventDefault(); e.stopPropagation(); set(id, val[id] + step); showBubble(id); }
      else if (e.key === "ArrowDown" || e.key === "ArrowLeft") { e.preventDefault(); e.stopPropagation(); set(id, val[id] - step); showBubble(id); }
      else if (e.key === "0" || e.key === "Delete" || e.key === "Backspace") { e.preventDefault(); set(id, 0); showBubble(id); }
    }, true);
    document.addEventListener("pointerover", (e) => { const b = e.target.closest(".io-knob"); if (b) { hoverId = b.dataset.g; showBubble(hoverId); } });
    document.addEventListener("pointerout", (e) => { const b = e.target.closest(".io-knob"); if (b && !b.contains(e.relatedTarget)) { hoverId = null; if (!dragId) hideBubble(); } });
    document.addEventListener("focusout", (e) => { if (e.target.closest && e.target.closest(".io-knob") && !dragId) hideBubble(); });
  }

  // Read the engine's values (and whether it supports them); refresh when the window regains focus.
  async function sync() {
    try {
      const r = await fetch(base() + "/api/audio", { cache: "no-store" });
      const d = await r.json();
      const c = d && d.current;
      if (!c || typeof c.output_gain_db !== "number") return;   // older engine: no knobs
      if (!ready) ready = install();
      if (!ready) return;
      if (dragId !== "in") set("in", +c.rig_input_db || 0, false);
      if (dragId !== "out") set("out", +c.output_gain_db || 0, false);
    } catch (e) { /* engine not up yet */ }
  }
  function init() {
    wire();
    sync();
    let tries = 0;
    const t = setInterval(() => { if (ready || ++tries > 20) clearInterval(t); sync(); }, 1500);
    window.addEventListener("focus", sync);
    window.HRIoGain = { set: (id, db) => set(id, db), get: (id) => val[id], sync };
  }
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", init); else init();
})();
