/* =====================================================================
   CIRCUIT PARTS: parametric, photo-referenced SVG illustrations of the real electronic parts in a unit's netlist,
   shown large in the circuit view's part drawer (circuit-view.js). Everything is computed from the part itself:
     * resistors: colour bands from value + tolerance (4-band carbon film, 5-band metal film, carbon comp in tube gear)
     * capacitors: package from value/type/circuit (radial or axial electrolytic, red film box, orange drop, yellow
       axial film, ceramic disc, silver mica) with the printed value, code, tolerance letter and voltage rating
     * pots: 16 mm (pedals) or 24 mm (amps), value code (A500K, B20K) and the shaft turned to the live wiper position
     * semiconductors with their real markings and pinouts (DO-35/DO-7/DO-41 diodes, LEDs, TO-92, TO-1 cans, DIP-8)
     * tubes (12AX7/12AT7/12AU7, EL84, EL34, 6L6GC, 6V6GT, KT88, GZ34/5AR4 …): envelope, plates, micas, getter flash,
       pins, and a heater/cathode glow while the unit is on
     * transformers, chokes, toroids, switches, batteries/supplies, LDRs, speakers and a generic module fallback
   Every lead/pin is tagged with the node it connects to and its DC operating-point voltage.
   Rendering rules (WKWebView rasterises SVG filters at low resolution): filters only on soft shapes (shadows,
   glows); every detail is plain vector so it stays sharp at any zoom. Every drawing gets unique gradient ids.
   API: window.CircuitParts.render(part, {circuit, dc, on}) -> {svg, name, pkg, chips[], bands?}
        window.CircuitParts.figure(part, {circuit, dc, on}) -> the same drawing as a compact figure for the circuit
          canvas's Illustrated view: no pin tags, axial leads straight out along the body, soft shadows as gradients
          (no filters: hundreds of these are on screen at once) -> {defs, shadow, body, anchors[{node, x, y, dim}], kind}.
          anchors are the lead / pin ends (where the canvas connects its wires), in the art's own coordinates.
   Loaded on demand by circuit-view.js; no globals besides window.CircuitParts.
   ===================================================================== */
(function () {
  "use strict";
  const CSS = `
  .cp-art { display: block; width: 100%; height: auto; overflow: visible; }
  .cp-art text { font-family: -apple-system, BlinkMacSystemFont, "SF Pro Text", "Helvetica Neue", Arial, sans-serif; }
  .cp-pill { fill: color-mix(in srgb, var(--card, #1c1d21) 88%, transparent); stroke: var(--line-3, rgba(255,255,255,.17)); stroke-width: 1.2; }
  .cp-tag.dim { opacity: .55; }
  .cp-pin-n { font: 650 17px ui-monospace, "SF Mono", Menlo, monospace; fill: var(--text, #ececef); }
  .cp-pin-v { font: 600 14px ui-monospace, "SF Mono", Menlo, monospace; fill: var(--accent, #e3a04b); }
  .cp-pin-k { font: 700 11px -apple-system, "SF Pro Text", sans-serif; fill: var(--text-3, #6c6f78); letter-spacing: .08em; }
  .cp-lead { stroke: var(--text-3, #6c6f78); stroke-width: 1.3; stroke-dasharray: 3 4; fill: none; opacity: .9; }
  .cp-dot { fill: var(--text-2, #a4a6ae); }
  .cp-dial { stroke: var(--accent, #e3a04b); fill: none; stroke-linecap: round; }
  .cp-dialbg { stroke: rgba(20, 22, 26, .35); fill: none; stroke-linecap: round; }
  .cp-ann { font: 700 13px -apple-system, "SF Pro Text", sans-serif; letter-spacing: .1em; fill: var(--text-3, #6c6f78); }
  .cp-ann.on { fill: var(--accent, #e3a04b); }
  .cp-glow { animation: cp-flick 3.6s ease-in-out infinite alternate; }
  .cp-glow.b { animation-duration: 4.7s; animation-delay: -1.3s; }
  @keyframes cp-flick { 0% { opacity: .82; } 35% { opacity: 1; } 60% { opacity: .9; } 100% { opacity: .97; } }
  @media (prefers-reduced-motion: reduce) { .cp-glow { animation: none; } }
  `;
  function injectCss() {
    if (typeof document === "undefined" || document.getElementById("cp-css")) return;
    const s = document.createElement("style");
    s.id = "cp-css";
    s.textContent = CSS;
    document.head.appendChild(s);
  }

  /* ---------------- small utilities ---------------- */
  const clamp = (v, a, b) => Math.max(a, Math.min(b, v));
  const r2 = (v) => Math.round(v * 100) / 100;
  const r3 = (v) => Math.round(v * 1000) / 1000;
  const esc = (s) => String(s == null ? "" : s).replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
  const isGnd = (n) => ["0", "gnd", "ground"].includes(String(n).toLowerCase());
  const DEG = Math.PI / 180;
  function rgb(h) { h = String(h).replace("#", ""); if (h.length === 3) h = h.split("").map((c) => c + c).join(""); const n = parseInt(h, 16); return [(n >> 16) & 255, (n >> 8) & 255, n & 255]; }
  function hex(a) { return "#" + a.map((v) => clamp(Math.round(v), 0, 255).toString(16).padStart(2, "0")).join(""); }
  function mix(a, b, t) { const A = rgb(a), B = rgb(b); return hex(A.map((v, i) => v + (B[i] - v) * t)); }
  function shade(c, t) { return t >= 0 ? mix(c, "#ffffff", t) : mix(c, "#000000", -t); }
  function scaleC(c, k) { return hex(rgb(c).map((v) => v * k)); }
  function fmtV(v) {
    if (v == null || !isFinite(v)) return "";
    const a = Math.abs(v);
    let s;
    if (a < 5e-4) s = "0 V";
    else if (a >= 100) s = `${v.toFixed(0)} V`;
    else if (a >= 10) s = `${v.toFixed(1)} V`;
    else if (a >= 1) s = `${v.toFixed(2)} V`;
    else s = `${(v * 1e3).toFixed(0)} mV`;
    return s.replace("-", "−");
  }
  function fmtEng(v, unit, sep = " ") {
    const a = Math.abs(v);
    const P = [[1e6, "M"], [1e3, "k"], [1, ""], [1e-3, "m"], [1e-6, "µ"], [1e-9, "n"], [1e-12, "p"]];
    for (const [m, s] of P) if (a >= m * 0.9999) return `${+(v / m).toPrecision(3)}${sep}${s}${unit}`;
    return `${+v.toPrecision(3)}${sep}${unit}`;
  }
  const nodeName = (n) => (isGnd(n) ? "GND" : String(n));

  /* ---------------- SVG builder ---------------- */
  let SEQ = 0;
  // set while a compact canvas figure is being drawn (figure()): collects the pin anchors instead of drawing tags
  let FIG = null;
  // a filter-free soft shadow: an ellipse with a radial falloff
  function softShadow(A, cx, cy, rx, ry, op) {
    const id = A.gid();
    A.defs.push(`<radialGradient id="${id}"><stop offset="0" stop-color="#000" stop-opacity="${r3(op)}"/><stop offset=".55" stop-color="#000" stop-opacity="${r3(op * 0.55)}"/><stop offset="1" stop-color="#000" stop-opacity="0"/></radialGradient>`);
    A.sh.push(`<ellipse cx="${r2(cx)}" cy="${r2(cy)}" rx="${r2(Math.max(1, rx))}" ry="${r2(Math.max(1, ry))}" fill="url(#${id})"/>`);
  }
  function pathBox(d) {
    const v = (String(d).match(/-?\d*\.?\d+(?:e[-+]?\d+)?/gi) || []).map(Number);
    let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
    for (let i = 0; i + 1 < v.length; i += 2) { x0 = Math.min(x0, v[i]); x1 = Math.max(x1, v[i]); y0 = Math.min(y0, v[i + 1]); y1 = Math.max(y1, v[i + 1]); }
    return isFinite(x0) ? [x0, y0, x1, y1] : null;
  }
  class Art {
    constructor(w, h) { this.w = w; this.h = h; this.id = `cp${(++SEQ).toString(36)}`; this.n = 0; this.defs = []; this.out = []; this.fl = {}; this.sh = []; }
    gid() { return `${this.id}-${++this.n}`; }
    stops(st) { return st.map(([o, c, a]) => `<stop offset="${r3(clamp(o, 0, 1))}" stop-color="${c}"${a != null && a !== 1 ? ` stop-opacity="${r3(a)}"` : ""}/>`).join(""); }
    lin(st, x1, y1, x2, y2) { const id = this.gid(); this.defs.push(`<linearGradient id="${id}" gradientUnits="userSpaceOnUse" x1="${r2(x1)}" y1="${r2(y1)}" x2="${r2(x2)}" y2="${r2(y2)}">${this.stops(st)}</linearGradient>`); return `url(#${id})`; }
    rad(st, cx, cy, r, fx, fy, tf) {
      const id = this.gid();
      this.defs.push(`<radialGradient id="${id}" gradientUnits="userSpaceOnUse" cx="${r2(cx)}" cy="${r2(cy)}" r="${r2(r)}"${fx != null ? ` fx="${r2(fx)}" fy="${r2(fy)}"` : ""}${tf ? ` gradientTransform="${tf}"` : ""}>${this.stops(st)}</radialGradient>`);
      return `url(#${id})`;
    }
    blur(sd) { if (!this.fl[sd]) { const id = this.gid(); this.defs.push(`<filter id="${id}" x="-80%" y="-80%" width="260%" height="260%" color-interpolation-filters="sRGB"><feGaussianBlur stdDeviation="${sd}"/></filter>`); this.fl[sd] = `url(#${id})`; } return this.fl[sd]; }
    clip(d) { const id = this.gid(); this.defs.push(`<clipPath id="${id}"><path d="${d}"/></clipPath>`); return `url(#${id})`; }
    add(...s) { this.out.push(...s); return this; }
    shadow(cx, cy, rx, ry, op = 0.5, sd = 8) { if (FIG) { softShadow(this, cx, cy, rx + sd * 0.5, ry + sd * 0.3, op * 0.7); return; } this.out.push(`<ellipse cx="${r2(cx)}" cy="${r2(cy)}" rx="${r2(rx)}" ry="${r2(ry)}" fill="#000" opacity="${op}" filter="${this.blur(sd)}"/>`); }
    shadowPath(d, op = 0.5, sd = 8) { if (FIG) { const b = pathBox(d); if (b) softShadow(this, (b[0] + b[2]) / 2, (b[1] + b[3]) / 2, (b[2] - b[0]) / 2 + sd * 0.5, (b[3] - b[1]) / 2 + sd * 0.3, op * 0.7); return; } this.out.push(`<path d="${d}" fill="#000" opacity="${op}" filter="${this.blur(sd)}"/>`); }
    svg(label) { return `<svg class="cp-art" viewBox="0 0 ${this.w} ${this.h}" role="img" aria-label="${esc(label || "")}" xmlns="http://www.w3.org/2000/svg"><defs>${this.defs.join("")}</defs>${this.out.join("")}</svg>`; }
  }
  const poly = (pts) => pts.length ? `M${pts.map((p) => `${r2(p[0])} ${r2(p[1])}`).join(" L")} Z` : "";
  const pline = (pts) => pts.length ? `M${pts.map((p) => `${r2(p[0])} ${r2(p[1])}`).join(" L")}` : "";

  // A metal lead/wire: dark edge, body, specular line.
  function wire(d, w = 6, col = "#c5c9ce", hi = 0.75) {
    return `<path d="${d}" fill="none" stroke="${shade(col, -0.6)}" stroke-width="${r2(w + 1.8)}" stroke-linecap="round" stroke-linejoin="round"/>`
      + `<path d="${d}" fill="none" stroke="${col}" stroke-width="${r2(w)}" stroke-linecap="round" stroke-linejoin="round"/>`
      + `<path d="${d}" fill="none" stroke="#fff" stroke-opacity="${hi}" stroke-width="${r2(Math.max(1, w * 0.26))}" stroke-linecap="round" stroke-linejoin="round" transform="translate(${r2(-w * 0.2)} ${r2(-w * 0.2)})"/>`;
  }
  // Glossy specular streak along a straight segment (for bodies drawn in 2D).
  const streak = (A, x1, y1, x2, y2, w, op = 0.6) => `<path d="M${r2(x1)} ${r2(y1)} L${r2(x2)} ${r2(y2)}" stroke="${A.lin([[0, "#fff", 0], [0.18, "#fff", op], [0.82, "#fff", op], [1, "#fff", 0]], x1, y1, x2, y2)}" stroke-width="${r2(w)}" stroke-linecap="round" fill="none"/>`;

  /* ---------------- pin tags ---------------- */
  function tagSize(t) {
    const nw = t.node.length * 10.5, vw = t.v ? t.v.length * 8.6 + 10 : 0, kw = t.k ? t.k.length * 8.1 : 0;
    return { w: Math.max(nw + vw, kw) + 22, h: t.k ? 48 : 33 };
  }
  function tagAt(t, x, y) {
    const { w, h } = tagSize(t), nw = t.node.length * 10.5;
    const ny = t.k ? y + 37 : y + 22.5;
    return `<g class="cp-tag${t.dim ? " dim" : ""}"><rect class="cp-pill" x="${r2(x)}" y="${r2(y)}" width="${r2(w)}" height="${h}" rx="10"/>`
      + (t.k ? `<text class="cp-pin-k" x="${r2(x + 11)}" y="${r2(y + 17)}">${esc(t.k)}</text>` : "")
      + `<text class="cp-pin-n" x="${r2(x + 11)}" y="${r2(ny)}">${esc(t.node)}</text>`
      + (t.v ? `<text class="cp-pin-v" x="${r2(x + 11 + nw + 10)}" y="${r2(ny)}">${esc(t.v)}</text>` : "") + `</g>`;
  }
  const mkTag = (o, node, k, extra) => Object.assign({ node: nodeName(node), v: fmtV(o.V(node)), k }, extra || {});
  // Tags in a row below (dir 1) or above (dir -1) the given lead ends; spread apart so they never overlap.
  function tagsRow(A, items, yRow, dir = 1) {
    if (FIG) { items.forEach((t) => FIG.anchors.push({ node: t.node, x: r2(t.x), y: r2(t.y), dim: !!t.dim })); return; }
    const it = items.map((t) => Object.assign({}, t, tagSize(t))).sort((a, b) => a.x - b.x);
    it.forEach((t) => { t.cx = t.tx != null ? t.tx : t.x; });
    const need = (i) => (it[i - 1].w + it[i].w) / 2 + 10;
    for (let k = 0; k < 6; k++) for (let i = 1; i < it.length; i++) { const gap = it[i].cx - it[i - 1].cx; if (gap < need(i)) { const d = (need(i) - gap) / 2; it[i - 1].cx -= d; it[i].cx += d; } }
    if (it.length) it[0].cx = Math.max(it[0].cx, it[0].w / 2 + 6);
    for (let i = 1; i < it.length; i++) it[i].cx = Math.max(it[i].cx, it[i - 1].cx + need(i));
    if (it.length) { const L = it[it.length - 1]; L.cx = Math.min(L.cx, A.w - L.w / 2 - 6); }
    for (let i = it.length - 2; i >= 0; i--) it[i].cx = Math.min(it[i].cx, it[i + 1].cx - need(i + 1));
    let s = "";
    it.forEach((t) => {
      const ty = dir > 0 ? yRow : yRow - t.h, ey = dir > 0 ? ty : ty + t.h, my = (t.y + ey) / 2;
      s += `<path class="cp-lead" d="M${r2(t.x)} ${r2(t.y)} C${r2(t.x)} ${r2(my)} ${r2(t.cx)} ${r2(my)} ${r2(t.cx)} ${r2(ey)}"/><circle class="cp-dot" cx="${r2(t.x)}" cy="${r2(t.y)}" r="3.2"/>`;
      s += tagAt(t, t.cx - t.w / 2, ty);
    });
    A.add(s);
  }
  // Tags in a column at the left (side -1, right-aligned at x) or right (side 1) of the drawing.
  function tagsCol(A, items, side, x, yTop, gap = 10) {
    if (FIG) { items.forEach((t) => FIG.anchors.push({ node: t.node, x: r2(t.x), y: r2(t.y), dim: !!t.dim })); return; }
    const it = items.map((t) => Object.assign({}, t, tagSize(t))).sort((a, b) => a.y - b.y);
    let y = yTop;
    const pos = it.map((t) => { const want = Math.max(y, t.y - t.h / 2); y = want + t.h + gap; return want; });
    const over = y - gap - (A.h - 6);
    if (over > 0) for (let i = 0; i < pos.length; i++) pos[i] -= over;
    let s = "";
    it.forEach((t, i) => {
      const tx = side < 0 ? x - t.w : x, ty = pos[i], ex = side < 0 ? x : x, ey = ty + t.h / 2;
      s += `<path class="cp-lead" d="M${r2(t.x)} ${r2(t.y)} C${r2((t.x + ex) / 2)} ${r2(t.y)} ${r2((t.x + ex) / 2)} ${r2(ey)} ${r2(ex)} ${r2(ey)}"/><circle class="cp-dot" cx="${r2(t.x)}" cy="${r2(t.y)}" r="3.2"/>`;
      s += tagAt(t, tx, ty);
    });
    A.add(s);
  }

  /* ---------------- tiny 3D: orthographic camera, lit cylinders, prisms, text on planes ---------------- */
  const add3 = (a, b) => [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
  const sub3 = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
  const mul3 = (a, k) => [a[0] * k, a[1] * k, a[2] * k];
  const dot3 = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  const cross3 = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
  const norm3 = (a) => { const l = Math.hypot(a[0], a[1], a[2]) || 1; return [a[0] / l, a[1] / l, a[2] / l]; };
  function basis(ax) { const a = norm3(ax); const t = Math.abs(a[1]) < 0.9 ? [0, 1, 0] : [1, 0, 0]; const u = norm3(cross3(t, a)); return [u, cross3(a, u)]; }
  function Cam(yaw, pitch, s, ox, oy) {
    const cy = Math.cos(yaw), sy = Math.sin(yaw), cp = Math.cos(pitch), sp = Math.sin(pitch);
    const rot = (P) => { const x1 = P[0] * cy + P[2] * sy, z1 = -P[0] * sy + P[2] * cy; return [x1, P[1] * cp - z1 * sp, P[1] * sp + z1 * cp]; };
    return { s, rot, p: (P) => { const r = rot(P); return [ox + s * r[0], oy - s * r[1]]; }, z: (P) => rot(P)[2], vis: (n) => rot(n)[2] > 0 };
  }
  // camera that fits the given 3D points into the screen box [x0, y0, x1, y1] (largest scale up to sMax)
  function fitCam(yaw, pitch, pts, box, sMax = 1e9) {
    const C0 = Cam(yaw, pitch, 1, 0, 0), P = pts.map(C0.p);
    const xs = P.map((q) => q[0]), ys = P.map((q) => q[1]);
    const mnx = Math.min(...xs), mxx = Math.max(...xs), mny = Math.min(...ys), mxy = Math.max(...ys);
    const s = Math.min((box[2] - box[0]) / Math.max(1e-6, mxx - mnx), (box[3] - box[1]) / Math.max(1e-6, mxy - mny), sMax);
    return Cam(yaw, pitch, s, (box[0] + box[2]) / 2 - s * (mnx + mxx) / 2, (box[1] + box[3]) / 2 - s * (mny + mxy) / 2);
  }
  const boxPts = (x0, x1, y0, y1, z0, z1) => [[x0, y0, z0], [x1, y0, z0], [x0, y1, z0], [x1, y1, z0], [x0, y0, z1], [x1, y0, z1], [x0, y1, z1], [x1, y1, z1]];
  const cylPts = (r, y0, y1) => { const o = []; for (let i = 0; i < 16; i++) { const a = i / 16 * 2 * Math.PI; o.push([r * Math.cos(a), y0, r * Math.sin(a)], [r * Math.cos(a), y1, r * Math.sin(a)]); } return o; };
  const LIGHT = norm3([-0.52, 0.72, 0.6]), HALF = norm3(add3(LIGHT, [0, 0, 1]));
  // material {c, amb, kd, ks, pw, env, sc}; nc = normal in camera space
  // canvas figures sit on a near-black canvas: black epoxy / bakelite is lifted a little so the part still reads
  const liftDark = (c) => { const [r, g, b] = rgb(c), l = (0.3 * r + 0.59 * g + 0.11 * b) / 255; return l < 0.2 ? mix(c, "#6a6e76", 0.42 * (1 - l / 0.2)) : c; };
  function shadeN(m, nc) {
    if (FIG && !m.lifted) m = Object.assign({}, m, { c: liftDark(m.c), lifted: true });
    const d = Math.max(0, dot3(nc, LIGHT));
    const amb = m.amb != null ? m.amb : 0.34, kd = m.kd != null ? m.kd : 0.72;
    const b = amb + kd * d + (m.env || 0) * nc[1];
    const sp = (m.ks || 0) * Math.pow(Math.max(0, dot3(nc, HALF)), m.pw || 24);
    return mix(scaleC(m.c, Math.max(0.05, b)), m.sc || "#ffffff", clamp(sp, 0, 1));
  }
  function hull(P) {
    const p = P.slice().sort((a, b) => a[0] - b[0] || a[1] - b[1]);
    const cr = (o, a, b) => (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
    const lo = [], up = [];
    for (const q of p) { while (lo.length >= 2 && cr(lo[lo.length - 2], lo[lo.length - 1], q) <= 0) lo.pop(); lo.push(q); }
    for (let i = p.length - 1; i >= 0; i--) { const q = p[i]; while (up.length >= 2 && cr(up[up.length - 2], up[up.length - 1], q) <= 0) up.pop(); up.push(q); }
    return lo.slice(0, -1).concat(up.slice(0, -1));
  }
  const ring = (c, ax, r, N = 72, t0 = 0, t1 = 2 * Math.PI) => { const [u, v] = basis(ax); const out = []; for (let i = 0; i <= N; i++) { const t = t0 + (t1 - t0) * i / N; out.push(add3(c, add3(mul3(u, r * Math.cos(t)), mul3(v, r * Math.sin(t))))); } return out; };
  // gradient across a cylinder's silhouette, lit per surface normal
  function cylGrad(A, C, c0, ax, len, r, m) {
    const [u, v] = basis(ax), c1 = add3(c0, mul3(norm3(ax), len));
    const P0 = C.p(c0), P1 = C.p(c1);
    let q = [-(P1[1] - P0[1]), P1[0] - P0[0]]; const ql = Math.hypot(q[0], q[1]);
    q = ql < 1e-6 ? [1, 0] : [q[0] / ql, q[1] / ql];
    const st = []; let smax = 1e-6;
    for (let i = 0; i < 160; i++) {
      const t = i / 160 * 2 * Math.PI, n = add3(mul3(u, Math.cos(t)), mul3(v, Math.sin(t))), nc = C.rot(n);
      if (nc[2] < -0.05) continue;
      const P = C.p(add3(c0, mul3(n, r))), s = (P[0] - P0[0]) * q[0] + (P[1] - P0[1]) * q[1];
      st.push([s, shadeN(m, nc)]); smax = Math.max(smax, Math.abs(s));
    }
    st.sort((a, b) => a[0] - b[0]);
    const thin = st.filter((_, i) => i % 2 === 0 || i === st.length - 1);
    return A.lin(thin.map(([s, c]) => [(s + smax) / (2 * smax), c]), P0[0] - q[0] * smax, P0[1] - q[1] * smax, P0[0] + q[0] * smax, P0[1] + q[1] * smax);
  }
  function cylSide(A, C, c0, ax, len, r, m, extra = "") {
    const a = norm3(ax), c1 = add3(c0, mul3(a, len));
    const pts = ring(c0, a, r, 64).concat(ring(c1, a, r, 64)).map(C.p);
    return `<path d="${poly(hull(pts))}" fill="${cylGrad(A, C, c0, a, len, r, m)}" ${extra}/>`;
  }
  // a frustum (two radii)
  function coneSide(A, C, c0, ax, len, r0, r1, m) {
    const a = norm3(ax), c1 = add3(c0, mul3(a, len));
    const pts = ring(c0, a, r0, 64).concat(ring(c1, a, r1, 64)).map(C.p);
    return `<path d="${poly(hull(pts))}" fill="${cylGrad(A, C, c0, a, len, (r0 + r1) / 2, m)}"/>`;
  }
  const discD = (C, c, ax, r, N = 72) => poly(ring(c, ax, r, N).map(C.p));
  const disc = (C, c, ax, r, fill, extra = "") => `<path d="${discD(C, c, ax, r)}" fill="${fill}" ${extra}/>`;
  // the camera-facing half of a rim (for grooves/edges)
  function rimFront(C, c, ax, r, N = 96) {
    const [u, v] = basis(ax), out = [];
    for (let i = 0; i <= N; i++) {
      const t = i / N * 2 * Math.PI, n = add3(mul3(u, Math.cos(t)), mul3(v, Math.sin(t)));
      out.push({ P: C.p(add3(c, mul3(n, r))), vis: C.rot(n)[2] > 0 });
    }
    // rotate so the run of visible points is contiguous
    const k = out.findIndex((o, i) => !o.vis && out[(i + 1) % out.length].vis);
    const seq = k < 0 ? out : out.slice(k + 1).concat(out.slice(0, k + 1));
    return pline(seq.filter((o) => o.vis).map((o) => o.P));
  }
  // prism: polygon in the (u, v) plane at origin o, extruded along ax by depth. Returns lit faces.
  function prism(C, o, ax, u, v, pg, depth, m, opt = {}) {
    const a = norm3(ax);
    const P0 = pg.map(([x, y]) => add3(o, add3(mul3(u, x), mul3(v, y)))), P1 = P0.map((P) => add3(P, mul3(a, depth)));
    const cen = add3(o, mul3(a, depth / 2));
    const faces = [];
    for (let i = 0; i < P0.length; i++) {
      const j = (i + 1) % P0.length;
      let n = norm3(cross3(sub3(P0[j], P0[i]), a));
      const mid = mul3(add3(P0[i], P0[j]), 0.5);
      if (dot3(n, sub3(mid, add3(o, [0, 0, 0]))) < 0 && dot3(n, sub3(mid, cen)) < 0) n = mul3(n, -1);
      else if (dot3(n, sub3(add3(mid, mul3(a, depth / 2)), cen)) < 0) n = mul3(n, -1);
      const nc = C.rot(n);
      if (nc[2] <= 0.001) continue;
      faces.push({ d: poly([P0[i], P0[j], P1[j], P1[i]].map(C.p)), c: shadeN(m, nc), z: C.z(mid) });
    }
    let s = `<path d="${poly(hull(P0.concat(P1).map(C.p)))}" fill="${shadeN(m, C.rot(norm3(add3(mul3(a, 0.5), [0, 0, 1]))))}"/>`;
    faces.sort((x, y) => x.z - y.z).forEach((f) => { s += `<path d="${f.d}" fill="${f.c}" stroke="${f.c}" stroke-width=".6" stroke-linejoin="round"/>`; });
    const capN = C.rot(a), cap = capN[2] > 0 ? P1 : C.rot(mul3(a, -1))[2] > 0 ? P0 : null;
    const capNormal = cap === P1 ? a : mul3(a, -1);
    if (cap) { const cc = opt.cap || shadeN(opt.capM || m, C.rot(capNormal)); s += `<path d="${poly(cap.map(C.p))}" fill="${cc}" stroke="${opt.cap ? "none" : cc}" stroke-width=".6"/>`; }
    return { svg: s, top: P1, bot: P0, capPts: cap };
  }
  const rrect = (w, h, r, seg = 5) => {
    const pts = [], cs = [[w / 2 - r, h / 2 - r, 0], [-w / 2 + r, h / 2 - r, 90], [-w / 2 + r, -h / 2 + r, 180], [w / 2 - r, -h / 2 + r, 270]];
    cs.forEach(([cx, cy, a0]) => { for (let i = 0; i <= seg; i++) { const a = (a0 + 90 * i / seg) * DEG; pts.push([cx + r * Math.cos(a), cy + r * Math.sin(a)]); } });
    return pts;
  };
  // affine transform mapping local 2D (x along u, y DOWN = -v) onto a 3D plane; u, v already scaled to local units
  function planeM(C, o, u, v) {
    const P0 = C.p(o), Pu = C.p(add3(o, u)), Pv = C.p(add3(o, v));
    return `matrix(${r3(Pu[0] - P0[0])} ${r3(Pu[1] - P0[1])} ${r3(-(Pv[0] - P0[0]))} ${r3(-(Pv[1] - P0[1]))} ${r2(P0[0])} ${r2(P0[1])})`;
  }
  // text/graphics on a plane, never mirrored, reading left to right on screen
  function onPlane(C, o, u, v, inner, k = 0.1) {
    let U = u, Vv = v;
    const P0 = C.p(o), pu = C.p(add3(o, U)), pv = C.p(add3(o, Vv));
    const ux = pu[0] - P0[0], uy = pu[1] - P0[1], vx = pv[0] - P0[0], vy = pv[1] - P0[1];
    if (ux < 0) { U = mul3(U, -1); }
    const det = (ux < 0 ? -ux : ux) * vy - (ux < 0 ? -uy : uy) * vx;
    if (det > 0) Vv = mul3(Vv, -1);
    return `<g transform="${planeM(C, o, mul3(norm3(U), k), mul3(norm3(Vv), k))}">${inner}</g>`;
  }

  /* =====================================================================
     RESISTORS
     ===================================================================== */
  const BAND = [["black", "#1b1b1d"], ["brown", "#6c3a1e"], ["red", "#c4231c"], ["orange", "#e4701c"], ["yellow", "#f0c21c"], ["green", "#1d8a4b"],
    ["blue", "#2450b4"], ["violet", "#7b3fa8"], ["gray", "#8c8d91"], ["white", "#f2f1ec"]];
  const GOLD = ["gold", "#c39232"], SILVER = ["silver", "#b8bdc3"];
  function tolBand(t) {
    if (t == null) return GOLD;
    if (t <= 0.0011) return BAND[7]; if (t <= 0.0026) return BAND[6]; if (t <= 0.0051) return BAND[5];
    if (t <= 0.011) return BAND[1]; if (t <= 0.021) return BAND[2]; if (t <= 0.051) return GOLD; if (t <= 0.11) return SILVER;
    return null;
  }
  function colorBands(v, tol, digits) {
    v = Math.max(+v || 0, 0.01);
    let e = Math.floor(Math.log10(v) + 1e-9) - (digits - 1), m = Math.round(v / Math.pow(10, e));
    if (m >= Math.pow(10, digits)) { m = Math.round(m / 10); e++; }
    if (e < -2) { e = -2; m = Math.round(v / 0.01); }
    if (e > 9) { e = 9; m = Math.min(Math.pow(10, digits) - 1, Math.round(v / 1e9)); }
    const ds = String(m).padStart(digits, "0").slice(-digits).split("").map(Number);
    const mult = e >= 0 ? BAND[e] : e === -1 ? GOLD : SILVER;
    const tb = tolBand(tol);
    return ds.map((d) => BAND[d]).concat([mult]).concat(tb ? [tb] : []);
  }
  const needs3 = (v) => { const e = Math.floor(Math.log10(v) + 1e-9) - 1, m = v / Math.pow(10, e); return Math.abs(m - Math.round(m)) > 0.02; };
  // dog-bone / straight body outline along x from x0 to x1, centred on y
  function bodyPath(x0, x1, y, R, waist, capL) {
    const L = x1 - x0, rc = R * 0.62, c = capL * L, s = R * 0.55, Rw = R * waist;
    if (waist >= 0.999) return `M${x0} ${y} C${x0} ${y - R * 0.8} ${x0 + rc * 0.25} ${y - R} ${x0 + rc} ${y - R} H${x1 - rc} C${x1 - rc * 0.25} ${y - R} ${x1} ${y - R * 0.8} ${x1} ${y} C${x1} ${y + R * 0.8} ${x1 - rc * 0.25} ${y + R} ${x1 - rc} ${y + R} H${x0 + rc} C${x0 + rc * 0.25} ${y + R} ${x0} ${y + R * 0.8} ${x0} ${y} Z`;
    return `M${x0} ${y} C${x0} ${y - R * 0.82} ${x0 + rc * 0.22} ${y - R} ${x0 + rc} ${y - R} L${x0 + c - s} ${y - R} C${x0 + c} ${y - R} ${x0 + c} ${y - Rw} ${x0 + c + s} ${y - Rw} L${x1 - c - s} ${y - Rw} C${x1 - c} ${y - Rw} ${x1 - c} ${y - R} ${x1 - c + s} ${y - R} L${x1 - rc} ${y - R} C${x1 - rc * 0.22} ${y - R} ${x1} ${y - R * 0.82} ${x1} ${y}`
      + ` C${x1} ${y + R * 0.82} ${x1 - rc * 0.22} ${y + R} ${x1 - rc} ${y + R} L${x1 - c + s} ${y + R} C${x1 - c} ${y + R} ${x1 - c} ${y + Rw} ${x1 - c - s} ${y + Rw} L${x0 + c + s} ${y + Rw} C${x0 + c} ${y + Rw} ${x0 + c} ${y + R} ${x0 + c - s} ${y + R} L${x0 + rc} ${y + R} C${x0 + rc * 0.22} ${y + R} ${x0} ${y + R * 0.82} ${x0} ${y} Z`;
  }
  // cylinder shading overlay (vertical), for 2D axial bodies
  function axialShade(A, y, R, gloss) {
    return A.lin([[0, "#000", 0.62], [0.08, "#000", 0.32], [0.2, "#fff", 0.02], [0.27, "#fff", gloss], [0.35, "#fff", gloss * 0.25], [0.52, "#000", 0], [0.76, "#000", 0.2], [0.92, "#000", 0.45], [1, "#000", 0.68]], 0, y - R, 0, y + R);
  }
  const endShade = (A, x0, x1) => A.lin([[0, "#000", 0.42], [0.05, "#000", 0.08], [0.1, "#000", 0], [0.9, "#000", 0], [0.95, "#000", 0.08], [1, "#000", 0.42]], x0, 0, x1, 0);
  // axial leads leaving both ends, bent down to the floor (like parts formed for a board)
  function axialLeads(A, x0, x1, y, yFloor, w = 6.5, reach = 62, col) {
    if (FIG) { // the canvas figure: leads straight out along the axis, so the part sits on its wire
      const k = FIG.reach;
      A.add(wire(`M${x0 + 4} ${y} H${x0 - k}`, w, col), wire(`M${x1 - 4} ${y} H${x1 + k}`, w, col));
      return [[x0 - k, y], [x1 + k, y]];
    }
    const r = 20;
    const dl = `M${x0 + 4} ${y} H${x0 - reach + r} Q${x0 - reach} ${y} ${x0 - reach} ${y + r} V${yFloor}`;
    const dr = `M${x1 - 4} ${y} H${x1 + reach - r} Q${x1 + reach} ${y} ${x1 + reach} ${y + r} V${yFloor}`;
    A.add(wire(dl, w, col), wire(dr, w, col));
    return [[x0 - reach, yFloor], [x1 + reach, yFloor]];
  }

  function artResistor(p, o) {
    const A = new Art(640, 340);
    const v = +p.value || 1000, tol = p.tolerance != null ? +p.tolerance : 0.05;
    const style = /metal/.test(o.pkg) ? "metal" : /comp/.test(o.pkg) ? "comp" : /carbon|film/.test(o.pkg) ? "carbon" : (tol <= 0.021 || needs3(v)) ? "metal" : o.tubes ? "comp" : "carbon";
    const five = style === "metal";
    const bands = colorBands(v, tol, five ? 3 : 2);
    const y = 132, L = style === "comp" ? 316 : 350, R = style === "metal" ? 46 : style === "comp" ? 58 : 52;
    const x0 = 320 - L / 2, x1 = 320 + L / 2, yF = 262;
    A.shadow(320, yF + 4, L * 0.62, 12, 0.42, 10);
    A.shadow(320, yF - 2, L * 0.4, 5, 0.3, 4);
    const ends = axialLeads(A, x0, x1, y, yF, 7, 54);
    const d = style === "comp" ? bodyPath(x0, x1, y, R, 1, 0) : bodyPath(x0, x1, y, R, style === "metal" ? 0.88 : 0.84, 0.2);
    const base = style === "metal" ? "#5fa6dc" : style === "comp" ? "#80502f" : "#d8be96";
    const cl = A.clip(d);
    let s = `<path d="${d}" fill="${base}"/>`;
    // subtle lacquer variation along the body
    s += `<path d="${d}" fill="${A.lin([[0, shade(base, 0.06)], [0.5, shade(base, -0.04)], [1, shade(base, 0.03)]], x0, 0, x1, 0)}" opacity=".7"/>`;
    const pos = five ? [0.115, 0.235, 0.345, 0.455, 0.865] : style === "comp" ? [0.13, 0.26, 0.39, 0.52, 0.85] : [0.12, 0.29, 0.41, 0.53, 0.865];
    const bw = five ? 17 : 20;
    s += `<g clip-path="${cl}">`;
    bands.forEach((b, i) => {
      const isTol = i === bands.length - 1 && bands.length === (five ? 5 : 4);
      const xc = x0 + L * (isTol ? pos[4] : pos[i]);
      const metal = b === GOLD || b === SILVER;
      const fill = metal ? A.lin([[0, shade(b[1], -0.35)], [0.3, shade(b[1], 0.45)], [0.45, b[1]], [0.7, shade(b[1], -0.25)], [1, shade(b[1], -0.5)]], 0, y - R, 0, y + R) : b[1];
      s += `<rect x="${r2(xc - bw / 2)}" y="${y - R - 2}" width="${bw}" height="${2 * R + 4}" fill="${fill}"/>`;
      s += `<rect x="${r2(xc - bw / 2)}" y="${y - R - 2}" width="1.4" height="${2 * R + 4}" fill="#000" opacity=".18"/><rect x="${r2(xc + bw / 2 - 1.4)}" y="${y - R - 2}" width="1.4" height="${2 * R + 4}" fill="#000" opacity=".12"/>`;
    });
    s += `</g>`;
    const gloss = style === "metal" ? 0.6 : style === "comp" ? 0.55 : 0.34;
    s += `<path d="${d}" fill="${axialShade(A, y, R, gloss)}"/><path d="${d}" fill="${endShade(A, x0, x1)}"/>`;
    s += `<g clip-path="${cl}">${streak(A, x0 + 16, y - R * 0.5, x1 - 16, y - R * 0.5, style === "carbon" ? 3 : 4, style === "carbon" ? 0.3 : 0.55)}${streak(A, x0 + 30, y + R * 0.62, x1 - 30, y + R * 0.62, 2, 0.12)}</g>`;
    s += `<path d="${d}" fill="none" stroke="#000" stroke-opacity=".35" stroke-width="1"/>`;
    A.add(s);
    tagsRow(A, [mkTag(o, p.nodes[0], "PIN 1", { x: ends[0][0], y: ends[0][1] }), mkTag(o, p.nodes[1], "PIN 2", { x: ends[1][0], y: ends[1][1] })], yF + 18);
    const name = style === "metal" ? "metal-film resistor" : style === "comp" ? "carbon-composition resistor" : "carbon-film resistor";
    const watt = style === "comp" ? "½ W" : "¼ W";
    return {
      A, name: `${p.text || fmtEng(v, "Ω")} ${name}`,
      pkg: `${watt} axial · ${five ? "5-band" : "4-band"} code`,
      chips: [style === "metal" ? "Metal film" : style === "comp" ? "Carbon comp" : "Carbon film", watt, tol ? `±${+(tol * 100).toFixed(2)} %` : ""],
      bands: bands.map((b) => ({ name: b[0], color: b[1] })),
    };
  }

  /* =====================================================================
     CAPACITORS
     ===================================================================== */
  const STD_V = [6.3, 10, 16, 25, 35, 50, 63, 100, 160, 200, 250, 350, 400, 450, 500, 600, 630];
  const ratingFor = (vd, min) => STD_V.find((x) => x >= Math.max(min, Math.abs(vd || 0) * 1.6)) || 630;
  function capCode(F) {
    const pF = F * 1e12;
    if (pF < 99.5) return String(+pF.toPrecision(2)).replace(".", "R");
    const e = Math.floor(Math.log10(pF) + 1e-9) - 1, m = Math.round(pF / Math.pow(10, e));
    return m >= 100 ? `${Math.round(m / 10)}${e + 1}` : `${m}${e}`;
  }
  const tolLetter = (t) => (t == null ? "K" : t <= 0.011 ? "F" : t <= 0.021 ? "G" : t <= 0.051 ? "J" : t <= 0.11 ? "K" : "M");
  function capText(F, style) {
    const uF = F * 1e6;
    if (style === "wima") {
      if (uF >= 1) return `${+uF.toPrecision(2)}µ`.replace(".", ",");
      if (uF >= 0.1) return `${+uF.toPrecision(2)}µ`.replace(".", ",");
      if (F >= 1e-9) return `${+(F * 1e9).toPrecision(2)}n`.replace(".", ",");
      return `${Math.round(F * 1e12)}p`;
    }
    if (uF >= 1) return `${+uF.toPrecision(3)}µF`;
    if (uF >= 0.001) return `${String(+uF.toPrecision(2)).replace(/^0/, "")}µF`;
    return `${Math.round(F * 1e12)}pF`;
  }
  function capStyle(p, o) {
    const F = +p.value || 1e-7, pk = o.pkg, fam = o.fam, pack = o.pack;
    // engine-provided family/package first (headroom.unit-model: part.family / part.package)
    if (fam === "electrolytic" || pack === "radial-can") return pack === "axial" ? "elec-ax" : pack === "radial-can" ? "elec-rad" : o.tubes ? "elec-ax" : "elec-rad";
    if (fam === "ceramic" || pack === "disc") return "disc";
    if (/mica/.test(fam)) return "mica";
    if (fam === "film" || pack === "box-film") {
      if (pack === "box-film") return "box";
      if (/orange|drop|715|716/.test(pk)) return "drop";
      if (pack === "axial") return o.tubes ? "mallory" : "mallory";
      if (o.tubes) return F < 1e-9 ? "mica" : "drop";
      return "box";
    }
    if (/elec|alum|ecap/.test(pk)) return (o.tubes && !/radial/.test(pk)) || /axial/.test(pk) ? "elec-ax" : "elec-rad";
    if (/orange|715|716|drop/.test(pk)) return "drop";
    if (/mallory|150|yellow|axial/.test(pk)) return "mallory";
    if (/wima|box|mks|mkp|film/.test(pk)) return "box";
    if (/cer|disc|disk/.test(pk)) return "disc";
    if (/mica|sm\b/.test(pk)) return "mica";
    if (o.tubes) {
      if (F >= 4.7e-6) return "elec-ax";
      if (F >= 1e-9) return "drop";
      return "mica";
    }
    if (F >= 1e-6) return "elec-rad";
    if (F >= 1e-9) return "box";
    return "disc";
  }
  function capPolarity(p, o) {
    const a = o.V(p.nodes[0]), b = o.V(p.nodes[1]);
    if (a == null || b == null) return 0;
    return b > a + 1e-6 ? 1 : 0; // index of the + terminal
  }

  function artCap(p, o) {
    const st = capStyle(p, o), F = +p.value || 1e-7, tol = p.tolerance;
    const vd = (o.V(p.nodes[0]) != null && o.V(p.nodes[1]) != null) ? Math.abs(o.V(p.nodes[0]) - o.V(p.nodes[1])) : 0;
    const fn = { "elec-rad": capElecRadial, "elec-ax": capElecAxial, drop: capDrop, mallory: capMallory, box: capBox, disc: capDisc, mica: capMica }[st];
    return fn(p, o, F, tol, vd);
  }

  function capElecRadial(p, o, F, tol, vd) {
    const A = new Art(640, 380);
    const uF = F * 1e6, Vr = ratingFor(vd, o.tubes ? 25 : 16), cv = uF * Vr;
    const D = cv < 60 ? 5 : cv < 300 ? 6.3 : cv < 1200 ? 8 : cv < 4000 ? 10 : 12.5, H = D <= 6.3 ? 11 : D <= 8 ? 11.5 : D <= 10 ? 16 : 20, R = D / 2;
    const legL = FIG ? 2.2 : 4.5;
    const C = fitCam(-0.38, 0.36, cylPts(R, -legL, H + 0.3), [180, 14, 460, 300], 26);
    const s = C.s;
    const blue = /bipolar|np|nonpolar/.test(o.pkg);
    const sleeve = blue ? "#1d56a6" : "#1b1c20", ink = blue ? "#f2f4f7" : "#d9b45c", stripe = blue ? "#e9edf2" : "#b7bcc2";
    const pos = capPolarity(p, o), pitch = D <= 5 ? 2 : D <= 6.3 ? 2.5 : D <= 8 ? 3.5 : 5;
    // shadow + leads
    A.shadowPath(discD(C, [0.6, -legL, -0.4], [0, 1, 0], R * 1.25), 0.45, 9);
    const leg = (x) => { const a = C.p([x, 0.2, 0]), b = C.p([x, -legL, 0]); return { d: `M${r2(a[0])} ${r2(a[1])} L${r2(b[0])} ${r2(b[1])}`, end: b }; };
    const lg = [leg(-pitch / 2), leg(pitch / 2)];
    lg.forEach((l) => A.add(wire(l.d, 5.5)));
    const sm = { c: sleeve, amb: 0.42, kd: 0.6, ks: blue ? 0.45 : 0.5, pw: 26, env: 0.12 };
    let sv = cylSide(A, C, [0, 0, 0], [0, 1, 0], H, R, sm);
    // polarity stripe on the side (the minus lead's side)
    const [u, v] = basis([0, 1, 0]);
    let tf = 0, best = -9;
    for (let i = 0; i < 360; i++) { const t = i * DEG, n = add3(mul3(u, Math.cos(t)), mul3(v, Math.sin(t))); const z = C.rot(n)[2]; if (z > best) { best = z; tf = t; } }
    const minusSide = pos === 0 ? 1 : -1, sc = tf + minusSide * 0.55, sw = 0.36;
    const sp = [];
    for (let i = 0; i <= 12; i++) { const t = sc - sw + 2 * sw * i / 12; sp.push(add3(add3(mul3(u, R * Math.cos(t)), mul3(v, R * Math.sin(t))), [0, 0.35, 0])); }
    for (let i = 12; i >= 0; i--) { const t = sc - sw + 2 * sw * i / 12; sp.push(add3(add3(mul3(u, R * Math.cos(t)), mul3(v, R * Math.sin(t))), [0, H - 0.25, 0])); }
    sv += `<path d="${poly(sp.map(C.p))}" fill="${cylGrad(A, C, [0, 0, 0], [0, 1, 0], H, R, { c: stripe, amb: 0.42, kd: 0.6, ks: 0.4, pw: 26, env: 0.1 })}"/>`;
    const nS = add3(mul3(u, Math.cos(sc)), mul3(v, Math.sin(sc))), tS = norm3(cross3([0, 1, 0], nS));
    let minus = "";
    for (let k = 0; k < 4; k++) minus += `<rect x="${-7}" y="${-1.3 - (k - 1.5) * 22}" width="14" height="2.6" rx="1" fill="${sleeve}" opacity=".85"/>`;
    sv += onPlane(C, add3(mul3(nS, R * 1.001), [0, H / 2, 0]), tS, [0, 1, 0], minus);
    // printed text (vertical, reading upwards like the real sleeves)
    const nF = add3(mul3(u, Math.cos(tf - minusSide * 0.18)), mul3(v, Math.sin(tf - minusSide * 0.18))), tF = norm3(cross3([0, 1, 0], nF));
    const txt = `<g fill="${ink}" text-anchor="middle" font-weight="700"><text x="0" y="-8" font-size="${D >= 8 ? 16 : 13}" letter-spacing=".5">${esc(capText(F))}</text><text x="0" y="${D >= 8 ? 10 : 8}" font-size="${D >= 8 ? 13 : 11}">${Vr}V</text>${D >= 6.3 ? `<text x="0" y="${D >= 8 ? 26 : 22}" font-size="10" opacity=".85">105°C</text>` : ""}</g>`;
    sv += onPlane(C, add3(mul3(nF, R * 1.001), [0, H * 0.5, 0]), [0, 1, 0], mul3(tF, -1), txt);
    // seal groove near the bottom
    sv += `<path d="${rimFront(C, [0, 1.3, 0], [0, 1, 0], R * 1.001)}" fill="none" stroke="#000" stroke-opacity=".55" stroke-width="2.4"/><path d="${rimFront(C, [0, 1.05, 0], [0, 1, 0], R * 1.001)}" fill="none" stroke="#fff" stroke-opacity=".16" stroke-width="1.2"/>`;
    // top: sleeve lip + aluminium with the vent score
    sv += disc(C, [0, H, 0], [0, 1, 0], R, shadeN(sm, C.rot([0, 1, 0])));
    sv += disc(C, [0, H + 0.01, 0], [0, 1, 0], R * 0.84, A.lin([[0, "#e9ecef"], [0.45, "#aeb4bb"], [0.55, "#c9ced3"], [1, "#7f868e"]], ...C.p([-R, H, -R]), ...C.p([R, H, R])));
    const vent = D >= 8 ? [[0, -0.6, 0, 0.6], [-0.55, 0, 0.55, 0]].map(([a, b, c, d]) => [a * R, b * R, c * R, d * R]) : [[-0.5 * R, 0, 0.5 * R, 0], [0, -0.5 * R, 0, 0.5 * R]];
    sv += vent.map(([a, b, c, d]) => { const P = C.p([a, H + 0.02, b]), Q = C.p([c, H + 0.02, d]); return `<path d="M${r2(P[0])} ${r2(P[1])} L${r2(Q[0])} ${r2(Q[1])}" stroke="#5b6168" stroke-width="2.2" stroke-linecap="round"/><path d="M${r2(P[0])} ${r2(P[1] + 1.4)} L${r2(Q[0])} ${r2(Q[1] + 1.4)}" stroke="#fff" stroke-opacity=".5" stroke-width="1"/>`; }).join("");
    A.add(sv);
    const tags = [0, 1].map((i) => mkTag(o, p.nodes[i], i === pos ? "+" : "−", { x: lg[i === pos ? (minusSide > 0 ? 0 : 1) : (minusSide > 0 ? 1 : 0)].end[0], y: lg[i === pos ? (minusSide > 0 ? 0 : 1) : (minusSide > 0 ? 1 : 0)].end[1] }));
    tagsRow(A, tags, lg[0].end[1] + 22);
    return { A, name: `${capText(F)} ${Vr} V electrolytic capacitor`, pkg: `Radial aluminium electrolytic · ${D}×${H} mm`, chips: ["Electrolytic", `${Vr} V`, "Polarised", tol ? `±${Math.round(tol * 100)} %` : ""] };
  }

  function capElecAxial(p, o, F, tol, vd) {
    const A = new Art(640, 340);
    const uF = F * 1e6, Vr = ratingFor(vd, o.tubes ? 25 : 16);
    const big = Vr >= 250;
    const y = 132, L = big ? 380 : 350, R = big ? 74 : 64, x0 = 320 - L / 2, x1 = 320 + L / 2, yF = 268;
    const pos = capPolarity(p, o); // + at index pos; node 0 is the left lead
    A.shadow(320, yF + 4, L * 0.62, 12, 0.42, 10);
    const ends = axialLeads(A, x0, x1, y, yF, 7.5, big ? 44 : 54);
    const plusRight = pos === 1;
    // aluminium can end visible at the minus end
    const ax = plusRight ? x0 : x1, dir = plusRight ? 1 : -1;
    const can = `M${ax + dir * 14} ${y - R * 0.92} C${ax - dir * 4} ${y - R * 0.9} ${ax - dir * 6} ${y + R * 0.9} ${ax + dir * 14} ${y + R * 0.92} Z`;
    let s = `<path d="${can}" fill="${A.lin([[0, "#5e646b"], [0.28, "#eef1f4"], [0.42, "#b3b9c0"], [0.8, "#6c737a"], [1, "#3e4348"]], 0, y - R, 0, y + R)}"/>`;
    const d = bodyPath(x0 + (plusRight ? 10 : 0), x1 - (plusRight ? 0 : 10), y, R, 1, 0);
    s += `<path d="${d}" fill="${FIG ? "#3b3d43" : "#18191c"}"/>`;
    // crimp groove at the + end
    const gx = plusRight ? x1 - 34 : x0 + 34;
    s += `<rect x="${gx - 5}" y="${y - R}" width="10" height="${2 * R}" fill="#000" opacity=".55"/><rect x="${gx + 3 * dir}" y="${y - R}" width="2.5" height="${2 * R}" fill="#fff" opacity=".1"/>`;
    const ink = "#3dbb73";
    const tx = plusRight ? x0 + L * 0.44 : x0 + L * 0.56;
    s += `<g fill="${ink}" font-weight="700" text-anchor="middle"><text x="${tx}" y="${y - 12}" font-size="27" letter-spacing="1">${esc(capText(F).replace("µF", " µF"))}</text><text x="${tx}" y="${y + 20}" font-size="22" letter-spacing="1">${Vr} VDC</text><text x="${tx}" y="${y + 42}" font-size="13" letter-spacing="2" opacity=".85">85°C · TVA</text></g>`;
    const px = plusRight ? x1 - 64 : x0 + 64;
    s += `<g fill="${ink}" font-weight="800" text-anchor="middle" font-size="30"><text x="${px}" y="${y - 6}">+</text><text x="${px}" y="${y + 30}">+</text></g>`;
    s += `<path d="${d}" fill="${axialShade(A, y, R, 0.42)}"/><path d="${d}" fill="${endShade(A, x0, x1)}"/>`;
    s += `<g clip-path="${A.clip(d)}">${streak(A, x0 + 30, y - R * 0.5, x1 - 30, y - R * 0.5, 5, 0.42)}</g>`;
    A.add(s);
    tagsRow(A, [0, 1].map((i) => mkTag(o, p.nodes[i], i === pos ? "+" : "−", { x: ends[i][0], y: ends[i][1] })), yF + 18);
    return { A, name: `${capText(F)} ${Vr} V electrolytic capacitor`, pkg: "Axial aluminium electrolytic", chips: ["Electrolytic", `${Vr} V`, "Polarised", tol ? `±${Math.round(tol * 100)} %` : ""] };
  }

  function capDrop(p, o, F, tol) {
    const A = new Art(640, 360);
    const L = F >= 2e-7 ? 300 : F >= 4e-8 ? 268 : 236, H = F >= 2e-7 ? 150 : F >= 4e-8 ? 132 : 116;
    const cx = 320, cy = 130, x0 = cx - L / 2, x1 = cx + L / 2, y0 = cy - H / 2, y1 = cy + H / 2, yF = 286;
    A.shadow(cx, yF + 2, L * 0.55, 12, 0.42, 10);
    A.shadow(cx, y1 + 24, L * 0.48, 10, 0.25, 12);
    // leads: from the bottom near both ends, kinked, then down
    const lx0 = x0 + 34, lx1 = x1 - 34;
    const ld = (x, s) => `M${x} ${y1 - 6} L${x} ${y1 + 26} Q${x} ${y1 + 34} ${x + s * 10} ${y1 + 42} Q${x + s * 18} ${y1 + 50} ${x + s * 18} ${y1 + 60} V${yF}`;
    A.add(wire(ld(lx0, -1), 6.5), wire(ld(lx1, 1), 6.5));
    const d = `M${x0 + 30} ${y0 + 6} C${x0 + 6} ${y0 + 10} ${x0} ${cy - H * 0.2} ${x0} ${cy} C${x0} ${cy + H * 0.3} ${x0 + 10} ${y1} ${x0 + 40} ${y1 + 4}`
      + ` Q${lx0} ${y1 + 22} ${lx0 + 16} ${y1 + 4} L${lx1 - 16} ${y1 + 4} Q${lx1} ${y1 + 22} ${x1 - 40} ${y1 + 4} C${x1 - 10} ${y1} ${x1} ${cy + H * 0.3} ${x1} ${cy} C${x1} ${cy - H * 0.2} ${x1 - 6} ${y0 + 10} ${x1 - 30} ${y0 + 6} C${cx + L * 0.2} ${y0 - 2} ${cx - L * 0.2} ${y0 - 2} ${x0 + 30} ${y0 + 6} Z`;
    const base = "#e2531c";
    let s = `<path d="${d}" fill="${A.rad([[0, shade(base, 0.22)], [0.45, base], [0.85, shade(base, -0.28)], [1, shade(base, -0.5)]], cx - L * 0.12, cy - H * 0.35, L * 0.75, null, null, `translate(${cx - L * 0.12} ${cy - H * 0.35}) scale(1 .62) translate(${-(cx - L * 0.12)} ${-(cy - H * 0.35)})`)}"/>`;
    s += `<path d="${d}" fill="${A.lin([[0, "#000", 0], [0.72, "#000", 0], [1, "#000", 0.38]], 0, y0, 0, y1 + 20)}"/>`;
    // glossy dip highlights
    s += `<path d="M${x0 + 34} ${y0 + 22} C${cx - L * 0.15} ${y0 + 8} ${cx + L * 0.15} ${y0 + 8} ${x1 - 40} ${y0 + 20}" stroke="#fff" stroke-opacity=".5" stroke-width="7" stroke-linecap="round" fill="none"/>`;
    s += `<ellipse cx="${x0 + 30}" cy="${cy - 10}" rx="10" ry="${H * 0.22}" fill="#fff" opacity=".2"/>`;
    const code = capCode(F) + tolLetter(tol);
    s += `<g fill="#1d1410" font-weight="700" text-anchor="middle" opacity=".88"><text x="${cx}" y="${cy - 4}" font-size="${H * 0.2}" letter-spacing="1">715P600V</text><text x="${cx}" y="${cy + H * 0.22}" font-size="${H * 0.2}" letter-spacing="1">${esc(code)}  0565</text></g>`;
    A.add(s);
    tagsRow(A, [mkTag(o, p.nodes[0], "PIN 1", { x: lx0 - 18, y: yF }), mkTag(o, p.nodes[1], "PIN 2", { x: lx1 + 18, y: yF })], yF + 16);
    return { A, name: `${capText(F)} film capacitor (orange drop)`, pkg: `Dipped polypropylene film · 600 V · code ${code}`, chips: ["Polypropylene", "600 V", tol ? `±${Math.round(tol * 100)} %` : ""] };
  }

  function capMallory(p, o, F, tol) {
    const A = new Art(640, 340);
    const y = 132, L = 350, R = F >= 1e-7 ? 60 : 54, x0 = 320 - L / 2, x1 = 320 + L / 2, yF = 268;
    A.shadow(320, yF + 4, L * 0.62, 12, 0.42, 10);
    const ends = axialLeads(A, x0, x1, y, yF, 7, 54);
    const d = bodyPath(x0, x1, y, R, 1, 0), base = "#e9c52a";
    let s = `<path d="${d}" fill="${base}"/>`;
    s += `<g fill="#2b2410" font-weight="800" text-anchor="middle"><text x="320" y="${y - 6}" font-size="26" letter-spacing="1">${esc(capText(F))}</text><text x="320" y="${y + 22}" font-size="18" letter-spacing="2">600 VDC · 150P</text></g>`;
    s += `<rect x="${x0 + 22}" y="${y - R}" width="8" height="${2 * R}" fill="#2b2410" opacity=".85"/>`;
    s += `<path d="${d}" fill="${axialShade(A, y, R, 0.5)}"/><path d="${d}" fill="${endShade(A, x0, x1)}"/><g clip-path="${A.clip(d)}">${streak(A, x0 + 24, y - R * 0.5, x1 - 24, y - R * 0.5, 5, 0.5)}</g>`;
    A.add(s);
    tagsRow(A, [0, 1].map((i) => mkTag(o, p.nodes[i], i ? "OUTER FOIL ←" : "PIN 1", { x: ends[i][0], y: ends[i][1] })).map((t, i) => (i ? Object.assign(t, { k: "PIN 2" }) : t)), yF + 18);
    return { A, name: `${capText(F)} axial film capacitor`, pkg: "Axial polyester film, yellow wrap · 600 V", chips: ["Polyester film", "600 V", tol ? `±${Math.round(tol * 100)} %` : ""] };
  }

  function capBox(p, o, F, tol) {
    const A = new Art(640, 360);
    const big = F >= 2.2e-7;
    const W = big ? 13 : 7.2, Dp = big ? 6 : 3.5, H = big ? 12 : 8.5, pitch = big ? 10 : 5;
    const legB = FIG ? -2.5 : -5.5;
    const C = fitCam(-0.5, 0.34, boxPts(-W / 2, W / 2, legB, H, -Dp / 2, Dp / 2), [200, 16, 440, 300], 30), s = C.s;
    const m = { c: "#c51f2a", amb: 0.46, kd: 0.6, ks: 0.32, pw: 18, env: 0.1 };
    A.shadowPath(poly(hull(rrect(W * 1.08, Dp * 1.25, 1).map(([x, z]) => C.p([x + 0.6, -5.5, z - 0.4])))), 0.45, 9);
    const legs = [-pitch / 2, pitch / 2].map((x) => { const a = C.p([x, 0.1, 0]), b = C.p([x, legB, 0]); return { d: `M${r2(a[0])} ${r2(a[1])} L${r2(b[0])} ${r2(b[1])}`, end: b }; });
    legs.forEach((l) => A.add(wire(l.d, 5.5)));
    const pr = prism(C, [0, 0, 0], [0, 1, 0], [1, 0, 0], [0, 0, 1], rrect(W, Dp, Math.min(1, Dp * 0.2), 4), H, m);
    let sv = pr.svg;
    // top edge highlight
    sv += `<path d="${poly(pr.top.map(C.p))}" fill="none" stroke="#fff" stroke-opacity=".18" stroke-width="1.2"/>`;
    const txt = `<g fill="#0f0f10" text-anchor="middle" font-weight="800" opacity=".85"><text x="0" y="-${big ? 22 : 16}" font-size="${big ? 20 : 14}" letter-spacing="1">MKS 2</text><text x="0" y="${big ? 6 : 4}" font-size="${big ? 26 : 18}">${esc(capText(F, "wima"))}</text><text x="0" y="${big ? 30 : 22}" font-size="${big ? 18 : 13}">63-</text></g>`;
    sv += onPlane(C, [0, H * 0.55, Dp / 2 + 0.01], [1, 0, 0], [0, 1, 0], txt);
    A.add(sv);
    tagsRow(A, [mkTag(o, p.nodes[0], "PIN 1", { x: legs[0].end[0], y: legs[0].end[1] }), mkTag(o, p.nodes[1], "PIN 2", { x: legs[1].end[0], y: legs[1].end[1] })], legs[0].end[1] + 22);
    return { A, name: `${capText(F)} film capacitor`, pkg: `Boxed polyester film (MKS) · ${pitch} mm pitch · 63 V`, chips: ["Polyester film", "63 V", tol ? `±${Math.round(tol * 100)} %` : ""] };
  }

  function capDisc(p, o, F, tol) {
    const A = new Art(640, 360);
    const Rr = F >= 1e-8 ? 92 : F >= 1e-9 ? 80 : 70, cx = 320, cy = 120, yF = 290;
    const base = F >= 1e-8 ? "#c9894a" : "#d16a2c";
    A.shadow(cx + 8, yF + 2, Rr * 0.9, 10, 0.4, 9);
    A.shadow(cx + 14, cy + Rr + 30, Rr * 0.8, 12, 0.22, 14);
    const lx = 28;
    const leg = (s) => `M${cx + s * lx} ${cy + Rr * 0.7} V${cy + Rr + 26} q${s * 0} 8 ${s * 9} 13 q${s * 9} 5 ${s * 9} 14 V${yF}`;
    A.add(wire(leg(-1), 6), wire(leg(1), 6));
    // thickness (edge), then face
    let s = `<ellipse cx="${cx + 6}" cy="${cy + 7}" rx="${Rr}" ry="${Rr * 0.96}" fill="${shade(base, -0.45)}"/>`;
    s += `<path d="M${cx - lx - 12} ${cy + Rr * 0.86} Q${cx - lx} ${cy + Rr + 22} ${cx - lx + 10} ${cy + Rr * 0.9} Z M${cx + lx - 10} ${cy + Rr * 0.9} Q${cx + lx} ${cy + Rr + 22} ${cx + lx + 12} ${cy + Rr * 0.86} Z" fill="${shade(base, -0.3)}"/>`;
    s += `<ellipse cx="${cx}" cy="${cy}" rx="${Rr}" ry="${Rr * 0.96}" fill="${A.rad([[0, shade(base, 0.25)], [0.5, base], [0.9, shade(base, -0.25)], [1, shade(base, -0.4)]], cx - Rr * 0.3, cy - Rr * 0.35, Rr * 1.3)}"/>`;
    s += `<ellipse cx="${cx - Rr * 0.32}" cy="${cy - Rr * 0.4}" rx="${Rr * 0.38}" ry="${Rr * 0.2}" fill="#fff" opacity=".22" transform="rotate(-28 ${cx - Rr * 0.32} ${cy - Rr * 0.4})"/>`;
    const code = capCode(F), cls = F < 1e-9 ? "NP0" : "Z5U";
    s += `<g fill="#20140c" text-anchor="middle" font-weight="800" opacity=".85"><text x="${cx}" y="${cy + 6}" font-size="${Rr * 0.48}" letter-spacing="1">${esc(code)}</text><text x="${cx}" y="${cy + Rr * 0.48}" font-size="${Rr * 0.2}" letter-spacing="1">${cls}${o.tubes ? " 1KV" : ""}</text></g>`;
    A.add(s);
    tagsRow(A, [mkTag(o, p.nodes[0], "PIN 1", { x: cx - lx - 18, y: yF }), mkTag(o, p.nodes[1], "PIN 2", { x: cx + lx + 18, y: yF })], yF + 14);
    return { A, name: `${capText(F)} ceramic disc capacitor`, pkg: `Ceramic disc · code ${code} (${cls})`, chips: ["Ceramic", cls, o.tubes ? "1 kV" : "50 V"] };
  }

  function capMica(p, o, F, tol) {
    const A = new Art(640, 360);
    const cx = 320, cy = 128, W = 250, H = 150, yF = 290;
    const base = "#843a33";
    A.shadow(cx, yF + 2, W * 0.5, 10, 0.4, 9);
    A.shadow(cx + 8, cy + H / 2 + 26, W * 0.45, 12, 0.22, 14);
    const lx0 = cx - W * 0.3, lx1 = cx + W * 0.3;
    A.add(wire(`M${lx0} ${cy + H * 0.3} V${cy + H / 2 + 30} Q${lx0} ${cy + H / 2 + 40} ${lx0 - 8} ${cy + H / 2 + 48} V${yF}`, 6), wire(`M${lx1} ${cy + H * 0.3} V${cy + H / 2 + 30} Q${lx1} ${cy + H / 2 + 40} ${lx1 + 8} ${cy + H / 2 + 48} V${yF}`, 6));
    const x0 = cx - W / 2, x1 = cx + W / 2, y0 = cy - H / 2, y1 = cy + H / 2;
    const d = `M${x0 + 26} ${y0 + 4} C${cx - 40} ${y0 - 6} ${cx + 40} ${y0 + 8} ${x1 - 24} ${y0 + 2} C${x1 + 4} ${y0} ${x1 + 6} ${cy - 20} ${x1 - 2} ${cy} C${x1 + 8} ${cy + 26} ${x1 + 2} ${y1 - 4} ${x1 - 30} ${y1} Q${lx1 + 8} ${y1 + 18} ${lx1 - 10} ${y1 + 4} C${cx + 20} ${y1 + 10} ${cx - 30} ${y1 - 2} ${lx0 + 10} ${y1 + 4} Q${lx0 - 8} ${y1 + 18} ${x0 + 28} ${y1} C${x0 - 2} ${y1 - 2} ${x0 - 6} ${cy + 24} ${x0 + 2} ${cy} C${x0 - 6} ${cy - 24} ${x0 - 2} ${y0 + 6} ${x0 + 26} ${y0 + 4} Z`;
    let s = `<path d="${d}" fill="${A.rad([[0, shade(base, 0.25)], [0.55, base], [1, shade(base, -0.45)]], cx - W * 0.15, cy - H * 0.3, W * 0.7)}"/>`;
    s += `<path d="M${x0 + 30} ${y0 + 20} C${cx - 30} ${y0 + 8} ${cx + 30} ${y0 + 18} ${x1 - 34} ${y0 + 16}" stroke="#fff" stroke-opacity=".32" stroke-width="6" stroke-linecap="round" fill="none"/>`;
    const pF = Math.round(F * 1e12), tp = tol ? Math.round(tol * 100) : 5;
    s += `<g fill="#f1e7dc" text-anchor="middle" font-weight="700" opacity=".88"><text x="${cx}" y="${cy + 2}" font-size="30" letter-spacing="1">${pF} ±${tp}%</text><text x="${cx}" y="${cy + 36}" font-size="22" letter-spacing="2">500V SM</text></g>`;
    A.add(s);
    tagsRow(A, [mkTag(o, p.nodes[0], "PIN 1", { x: lx0 - 8, y: yF }), mkTag(o, p.nodes[1], "PIN 2", { x: lx1 + 8, y: yF })], yF + 14);
    return { A, name: `${pF} pF silver-mica capacitor`, pkg: "Dipped silver mica · 500 V", chips: ["Silver mica", "500 V", `±${tp} %`] };
  }

  /* =====================================================================
     POTENTIOMETERS
     ===================================================================== */
  function potCode(p) {
    const tp = typeof p.taper === "string" ? p.taper : p.taper && p.taper.type ? p.taper.type : "B";
    const v = +p.value || 1e5;
    let c;
    if (v >= 1e6) c = `${+(v / 1e6).toPrecision(2)}M`; else if (v >= 1e3) c = `${+(v / 1e3).toPrecision(3)}K`; else c = `${Math.round(v)}R`;
    c = c.replace(/^(\d)\.(\d)K$/, "$1K$2");
    return `${String(tp).toUpperCase().slice(0, 1)}${c}`;
  }
  function artPot(p, o) {
    const A = new Art(640, 400);
    const big = /24/.test(o.pkg) || (o.tubes && !/16|9mm/.test(o.pkg));
    const R = big ? 12 : 8, dep = big ? 8.5 : 6.5, rb = big ? 4.75 : 3.5, Lb = big ? 9.5 : 7, rs = big ? 3.17 : 3, Ls = big ? 7 : 7.5, nutF = big ? 14 : 10;
    const fp = []; for (let i = 0; i < 16; i++) { const a = i / 16 * 2 * Math.PI, c = Math.cos(a), sn = Math.sin(a); fp.push([R * c, R * sn, -dep], [R * c, R * sn, 0], [rs * c, rs * sn, Lb + Ls]); }
    fp.push([-6.5, -R - 7.2, -dep + 1.2], [6.5, -R - 7.2, -dep + 1.2]);
    const C = fitCam(-0.52, 0.3, fp, [150, 12, 490, 318], 16), s = C.s;
    const pos = clamp(p.position != null ? +p.position : 0.5, 0, 1), th = (-150 + 300 * pos) * DEG;
    const code = potCode(p);
    const zinc = { c: "#c4c9cf", amb: 0.4, kd: 0.62, ks: 0.85, pw: 34, env: 0.22 };
    const brass = { c: "#cfc7b5", amb: 0.38, kd: 0.6, ks: 0.9, pw: 40, env: 0.2 };
    const steel = { c: "#b9bec5", amb: 0.36, kd: 0.6, ks: 0.95, pw: 50, env: 0.25 };
    // floor shadow
    A.shadowPath(discD(C, [1.4, -R - 7.6, -dep / 2 - 1], [0, 1, 0], R * 1.15), 0.42, 10);
    // lugs (behind the can bottom), with holes
    const lugX = big ? [-6.5, 0, 6.5] : [-5, 0, 5], zl = -dep + 1.2, lugEnd = [];
    lugX.forEach((x) => {
      const pts = [[x - 1.4, -R + 1.6], [x + 1.4, -R + 1.6], [x + 1.4, -R - 3], [x + 0.75, -R - 4.2], [x + 0.75, -R - 7.2], [x - 0.75, -R - 7.2], [x - 0.75, -R - 4.2], [x - 1.4, -R - 3]].map(([a, b]) => C.p([a, b, zl]));
      const hole = C.p([x, -R - 1.4, zl]);
      A.add(`<path d="${poly(pts)}" fill="${A.lin([[0, "#8d9399"], [0.4, "#e4e7ea"], [1, "#9aa0a6"]], pts[0][0], 0, pts[1][0], 0)}" stroke="#5c6167" stroke-width=".8"/><ellipse cx="${r2(hole[0])}" cy="${r2(hole[1])}" rx="${r2(0.55 * s)}" ry="${r2(0.6 * s)}" fill="#2a2c30"/>`);
      lugEnd.push(C.p([x, -R - 7.2, zl]));
    });
    // can
    let sv = cylSide(A, C, [0, 0, -dep], [0, 0, 1], dep, R, zinc);
    // crimp tabs at the back edge + back board visible lip
    sv += `<path d="${rimFront(C, [0, 0, -dep + 0.15], [0, 0, 1], R * 1.002)}" fill="none" stroke="#3a3d42" stroke-width="2" opacity=".7"/>`;
    // marking on the side band, facing the camera
    let tm = 0, best = -9;
    for (let i = 40; i <= 140; i++) { const t = i * DEG, n = [Math.cos(t), Math.sin(t), 0]; const z = C.rot(n)[2] + 0.6 * C.rot(n)[1]; if (z > best) { best = z; tm = t; } }
    const nm = [Math.cos(tm), Math.sin(tm), 0], tmv = norm3(cross3([0, 0, 1], nm));
    sv += onPlane(C, add3(mul3(nm, R * 1.003), [0, 0, -dep / 2 - 0.4]), tmv, [0, 0, 1], `<g fill="#1d1f23" font-weight="800" text-anchor="middle"><text x="0" y="${big ? 10 : 8}" font-size="${big ? 26 : 19}" letter-spacing="1">${esc(code)}</text><text x="0" y="${big ? 34 : 26}" font-size="${big ? 15 : 12}" letter-spacing="1" opacity=".7">${big ? "137 · 6512" : "TAIWAN"}</text></g>`);
    // front face
    const faceC = shadeN(zinc, C.rot([0, 0, 1]));
    sv += disc(C, [0, 0, 0], [0, 0, 1], R, A.lin([[0, shade(faceC, 0.18)], [0.5, faceC], [1, shade(faceC, -0.22)]], ...C.p([-R, R, 0]), ...C.p([R, -R, 0])));
    sv += `<path d="${discD(C, [0, 0, 0], [0, 0, 1], R * 0.985)}" fill="none" stroke="#fff" stroke-opacity=".35" stroke-width="1.2"/>`;
    sv += `<path d="${discD(C, [0, 0, 0], [0, 0, 1], R * 0.6)}" fill="none" stroke="#000" stroke-opacity=".12" stroke-width="1"/>`;
    // anti-rotation tab
    sv += prism(C, [R * 0.62, 0.4, 0], [0, 0, 1], [1, 0, 0], [0, 1, 0], [[-0.6, -0.9], [0.6, -0.9], [0.6, 0.9], [-0.6, 0.9]].map(([a, b]) => [a, b + R * 0.0]), 1.6, zinc).svg;
    // wiper travel dial printed on the face (live)
    const dr = R * 0.84, dial = (a0, a1) => { const pts = []; const n = Math.max(2, Math.round(Math.abs(a1 - a0) / (3 * DEG))); for (let i = 0; i <= n; i++) { const a = a0 + (a1 - a0) * i / n; pts.push([dr * 10 * Math.sin(a), -dr * 10 * Math.cos(a)]); } return pline(pts); };
    let dl = `<path class="cp-dialbg" d="${dial(-150 * DEG, 150 * DEG)}" stroke-width="5"/>`;
    for (let i = 0; i <= 10; i++) { const a = (-150 + 30 * i) * DEG; dl += `<path d="M${r2((dr - 0.9) * 10 * Math.sin(a))} ${r2(-(dr - 0.9) * 10 * Math.cos(a))} L${r2((dr - 0.35) * 10 * Math.sin(a))} ${r2(-(dr - 0.35) * 10 * Math.cos(a))}" stroke="#2a2d31" stroke-opacity=".55" stroke-width="2.2" stroke-linecap="round"/>`; }
    dl += `<path class="cp-dial" d="${dial(-150 * DEG, th)}" stroke-width="5"/>`;
    sv += onPlane(C, [0, 0, 0.02], [1, 0, 0], [0, 1, 0], dl);
    // washer + nut + threaded bushing
    sv += cylSide(A, C, [0, 0, 0], [0, 0, 1], 0.5, nutF * 0.62, steel) + disc(C, [0, 0, 0.5], [0, 0, 1], nutF * 0.62, shadeN(steel, C.rot([0, 0, 1])));
    sv += cylSide(A, C, [0, 0, 0.5], [0, 0, 1], Lb - 0.5, rb, brass);
    const hex = []; for (let i = 0; i < 6; i++) { const a = (30 + 60 * i) * DEG; hex.push([nutF / 2 / Math.cos(30 * DEG) * Math.cos(a), nutF / 2 / Math.cos(30 * DEG) * Math.sin(a)]); }
    sv += prism(C, [0, 0, 0.5], [0, 0, 1], [1, 0, 0], [0, 1, 0], hex, 2.2, steel).svg;
    sv += `<path d="${discD(C, [0, 0, 2.72], [0, 0, 1], rb * 1.05)}" fill="#3b3f45"/>`;
    for (let z = 2.9; z < Lb - 0.2; z += 0.55) sv += `<path d="${rimFront(C, [0, 0, z], [0, 0, 1], rb * 1.01)}" fill="none" stroke="#000" stroke-opacity=".28" stroke-width="1.6"/><path d="${rimFront(C, [0, 0, z + 0.22], [0, 0, 1], rb * 1.01)}" fill="none" stroke="#fff" stroke-opacity=".3" stroke-width="1"/>`;
    sv += disc(C, [0, 0, Lb], [0, 0, 1], rb, shadeN(brass, C.rot([0, 0, 1])));
    // shaft: knurled (pedal) or solid with a flat (amp)
    sv += cylSide(A, C, [0, 0, Lb], [0, 0, 1], Ls, rs, steel);
    if (!big) {
      const [u, v] = basis([0, 0, 1]);
      for (let i = 0; i < 36; i++) {
        const t = i / 36 * 2 * Math.PI, n = add3(mul3(u, Math.cos(t)), mul3(v, Math.sin(t)));
        if (C.rot(n)[2] < 0.08) continue;
        const a = C.p(add3([0, 0, Lb + Ls * 0.35], mul3(n, rs))), b = C.p(add3([0, 0, Lb + Ls], mul3(n, rs)));
        sv += `<path d="M${r2(a[0])} ${r2(a[1])} L${r2(b[0])} ${r2(b[1])}" stroke="#000" stroke-opacity="${r2(0.18 + 0.25 * (1 - C.rot(n)[2]))}" stroke-width="1.4"/>`;
      }
    }
    // shaft end, slot aligned with the wiper + bright index
    const endM = { c: "#d3d7dc", amb: 0.5, kd: 0.5, ks: 0.6, pw: 30 };
    sv += disc(C, [0, 0, Lb + Ls], [0, 0, 1], rs, A.lin([[0, "#f3f5f7"], [0.5, shadeN(endM, C.rot([0, 0, 1]))], [1, "#8e949b"]], ...C.p([-rs, rs, Lb + Ls]), ...C.p([rs, -rs, Lb + Ls])));
    const ind = big
      ? `<path d="M${r2(-rs * 10)} ${r2(-rs * 10 * 0.62)} H${r2(rs * 10)}" stroke="#7b8188" stroke-width="2" transform="rotate(${r2(th / DEG)})"/><path d="M0 0 L0 ${r2(-rs * 10 * 0.86)}" stroke="var(--accent, #e3a04b)" stroke-width="5" stroke-linecap="round" transform="rotate(${r2(th / DEG)})"/>`
      : `<rect x="-3.2" y="${r2(-rs * 10)}" width="6.4" height="${r2(rs * 20)}" fill="#2f3237" transform="rotate(${r2(th / DEG)})"/><rect x="2" y="${r2(-rs * 10)}" width="1.6" height="${r2(rs * 20)}" fill="#fff" opacity=".35" transform="rotate(${r2(th / DEG)})"/><circle cx="0" cy="${r2(-rs * 10 * 0.72)}" r="3.6" fill="var(--accent, #e3a04b)" transform="rotate(${r2(th / DEG)})"/>`;
    sv += onPlane(C, [0, 0, Lb + Ls + 0.01], [1, 0, 0], [0, 1, 0], ind);
    A.add(sv);
    const [n1, nw, n3] = p.nodes;
    const tags = [mkTag(o, n1, "LUG 1", { x: lugEnd[0][0], y: lugEnd[0][1] }), mkTag(o, nw, "WIPER", { x: lugEnd[1][0], y: lugEnd[1][1] }), mkTag(o, n3, "LUG 3", { x: lugEnd[2][0], y: lugEnd[2][1] })];
    if (nw === n1 || nw === n3) tags[nw === n1 ? 0 : 2].k += " = W";
    tagsRow(A, tags, lugEnd[1][1] + 24);
    const tp = code[0];
    return {
      A, name: `${p.text || fmtEng(+p.value, "Ω")} ${tp === "A" ? "audio-taper (log)" : tp === "C" ? "reverse-log" : tp === "W" ? "W-taper" : "linear"} potentiometer`,
      pkg: `${big ? "24 mm" : "16 mm"} rotary pot · marked ${code}`, chips: [code, big ? "24 mm" : "16 mm", `${Math.round(pos * 100)} %`],
    };
  }

  /* =====================================================================
     DIODES, LEDs
     ===================================================================== */
  function diodeStyle(p, o) {
    const m = String(p.model || "").toUpperCase(), pk = o.pkg, fam = o.fam || "";
    if (fam === "led" || o.pack === "t-1¾") return "led";
    if (fam === "germanium-diode") return "ge";
    if (fam === "schottky-diode") return "schottky";
    if (fam === "rectifier-diode" || o.pack === "do-41") return "do41";
    if (fam === "silicon-diode" && o.pack === "do-35") return /^(BAT|1N57|BAR|SD)/.test(m) ? "schottky" : "glass";
    if (/LED/.test(m) || /led/.test(pk)) return "led";
    if (/^(1N34|1N60|1N270|OA|AA1|D9|GE)/.test(m) || /germanium|\bge\b/.test(pk)) return "ge";
    if (/^(1N40|1N54|UF4|1N58|FR|BY)/.test(m) || /do-?41|rectifier/.test(pk)) return "do41";
    if (/^(BAT|1N57|BAR|SD)/.test(m) || /schottky/.test(pk)) return "schottky";
    return "glass";
  }
  function artDiode(p, o) {
    const st = diodeStyle(p, o);
    if (st === "led") return artLED(p, o);
    const A = new Art(640, 340);
    const m = String(p.model || "");
    const y = 140, yF = 266;
    const L = st === "ge" ? 300 : st === "do41" ? 250 : 230, R = st === "ge" ? 52 : st === "do41" ? 58 : 54;
    const x0 = 320 - L / 2, x1 = 320 + L / 2;
    A.shadow(320, yF + 4, 250, 12, 0.4, 10);
    A.shadow(320, y + R + 40, L * 0.5, 8, 0.18, 14);
    const ends = axialLeads(A, x0, x1, y, yF, st === "do41" ? 9 : 7, st === "ge" ? 80 : 100);
    const d = bodyPath(x0, x1, y, R, 1, 0);
    let s = "";
    if (st === "do41") {
      s += `<path d="${d}" fill="${FIG ? "#3a3c42" : "#17181b"}"/>`;
      s += `<rect x="${x1 - L * 0.22}" y="${y - R - 2}" width="${L * 0.1}" height="${2 * R + 4}" fill="#b9bdc3" clip-path="${A.clip(d)}"/>`;
      s += `<g fill="#b9bdc3" text-anchor="middle" font-weight="700" opacity=".85"><text x="${320 - L * 0.08}" y="${y - 4}" font-size="20">${esc(m.replace(/^1N/, "1N "))}</text><text x="${320 - L * 0.08}" y="${y + 20}" font-size="13">E3 · 52</text></g>`;
      s += `<path d="${d}" fill="${axialShade(A, y, R, 0.35)}"/><path d="${d}" fill="${endShade(A, x0, x1)}"/>`;
    } else {
      const tint = st === "ge" ? "#d7e6dc" : st === "schottky" ? "#a8aeb6" : "#e2741e";
      const op = st === "ge" ? 0.22 : st === "schottky" ? 0.5 : 0.62;
      // internals: slugs (dumet), die / whisker
      const slugL = st === "ge" ? L * 0.26 : L * 0.36, sr = st === "ge" ? R * 0.34 : R * 0.48;
      const slug = (xa, xb) => `<rect x="${r2(Math.min(xa, xb))}" y="${r2(y - sr)}" width="${r2(Math.abs(xb - xa))}" height="${r2(2 * sr)}" rx="${r2(sr * 0.25)}" fill="${A.lin([[0, "#5a3a2a"], [0.3, "#e0b394"], [0.5, "#9c6c50"], [1, "#3e2619"]], 0, y - sr, 0, y + sr)}"/>`;
      s += `<path d="${d}" fill="${tint}" opacity="${op * 0.55}"/>`;
      s += slug(x0 + 6, x0 + slugL) + slug(x1 - 6, x1 - slugL);
      if (st === "ge") {
        // germanium crystal on the left slug, cat's whisker spring from the right
        s += `<rect x="${r2(x0 + slugL)}" y="${r2(y - sr * 0.8)}" width="10" height="${r2(sr * 1.6)}" fill="#3a3d42"/><rect x="${r2(x0 + slugL + 1)}" y="${r2(y - sr * 0.8)}" width="3" height="${r2(sr * 1.6)}" fill="#9aa2ab"/>`;
        let wd = `M${r2(x1 - slugL)} ${y}`; const n = 7, span = (x1 - slugL) - (x0 + slugL + 10);
        for (let i = 1; i <= n; i++) wd += ` L${r2(x1 - slugL - span * (i - 0.5) / n)} ${r2(y + (i % 2 ? -1 : 1) * sr * 0.9)}`;
        wd += ` L${r2(x0 + slugL + 10)} ${y}`;
        s += `<path d="${wd}" fill="none" stroke="#c9b27a" stroke-width="2.2" stroke-linejoin="round"/>`;
      } else {
        s += `<rect x="${r2(320 - 7)}" y="${r2(y - sr * 0.6)}" width="14" height="${r2(sr * 1.2)}" fill="#2e2a28"/>`;
      }
      // glass: tint, rim, highlights
      s += `<path d="${d}" fill="${A.lin([[0, tint, op], [0.25, tint, op * 0.35], [0.5, tint, op * 0.25], [0.8, tint, op * 0.5], [1, shade(tint, -0.5), op]], 0, y - R, 0, y + R)}"/>`;
      s += `<path d="${d}" fill="${endShade(A, x0, x1)}" opacity=".7"/>`;
      // cathode band printed on the glass
      const bx = x1 - L * (st === "ge" ? 0.16 : 0.2), bw = L * (st === "ge" ? 0.07 : 0.11);
      s += `<g clip-path="${A.clip(d)}"><rect x="${r2(bx)}" y="${y - R - 2}" width="${r2(bw)}" height="${2 * R + 4}" fill="${st === "ge" ? "#18191b" : "#151515"}"/><rect x="${r2(bx)}" y="${y - R - 2}" width="${r2(bw)}" height="${2 * R + 4}" fill="${axialShade(A, y, R, 0.25)}"/></g>`;
      s += `<g clip-path="${A.clip(d)}">${streak(A, x0 + 10, y - R * 0.55, x1 - 10, y - R * 0.55, 5, 0.75)}${streak(A, x0 + 14, y + R * 0.6, x1 - 14, y + R * 0.6, 2.5, 0.35)}</g>`;
      s += `<path d="${d}" fill="none" stroke="${shade(tint, -0.4)}" stroke-opacity=".7" stroke-width="1.6"/>`;
      if (st === "ge") s += `<text x="320" y="${y + R + 30}" text-anchor="middle" font-size="16" font-weight="700" fill="var(--text-3, #6c6f78)" letter-spacing="1">${esc(m)}</text>`;
    }
    A.add(s);
    tagsRow(A, [mkTag(o, p.nodes[0], "ANODE", { x: ends[0][0], y: ends[0][1] }), mkTag(o, p.nodes[1], "CATHODE", { x: ends[1][0], y: ends[1][1] })], yF + 18);
    const nm = { ge: "germanium point-contact diode", do41: "silicon rectifier diode", schottky: "Schottky diode", glass: "silicon switching diode" }[st];
    const pk = { ge: "DO-7 glass", do41: "DO-41 epoxy", schottky: "DO-35 glass", glass: "DO-35 glass" }[st];
    return { A, name: `${m || "Diode"} ${nm}${p.type === "Dpair" ? " (antiparallel pair)" : ""}`, pkg: `${pk} · band marks the cathode`, chips: [pk.split(" ")[0], st === "ge" ? "Ge ≈ 0.3 V" : st === "schottky" ? "≈ 0.3 V" : "Si ≈ 0.6 V", p.type === "Dpair" ? "×2 antiparallel" : ""] };
  }

  function artLED(p, o) {
    const A = new Art(640, 380);
    const m = String(p.model || "LED_RED").toUpperCase(), col = /GREEN/.test(m) ? "#2fbf55" : /BLUE/.test(m) ? "#3d7bff" : /YELLOW|AMBER/.test(m) ? "#ffb21f" : /WHITE/.test(m) ? "#e9eef5" : "#e0261b";
    const va = o.V(p.nodes[0]), vk = o.V(p.nodes[1]);
    const lit = o.on && va != null && vk != null && va - vk > 1.4;
    const C = fitCam(-0.35, 0.3, cylPts(2.9, -9, 7.6), [220, 16, 420, 296], 30), s = C.s;
    A.shadowPath(discD(C, [0.5, -9, -0.5], [0, 1, 0], 3.4), 0.45, 9);
    const leg = (x, len) => { const a = C.p([x, 0, 0]), b = C.p([x, -len, 0]); return { d: `M${r2(a[0])} ${r2(a[1])} L${r2(b[0])} ${r2(b[1])}`, end: b }; };
    const la = leg(1.27, 9), lk = leg(-1.27, 8);
    A.add(wire(la.d, 5), wire(lk.d, 5));
    const epo = { c: col, amb: 0.55, kd: 0.5, ks: 0.6, pw: 30, env: 0.15 };
    let sv = "";
    // flange with the cathode flat
    sv += prism(C, [0, 0, 0], [0, 1, 0], [1, 0, 0], [0, 0, 1], (() => { const pts = []; for (let i = 0; i <= 40; i++) { const a = (-70 + 320 * i / 40) * DEG; pts.push([2.9 * Math.cos(a + Math.PI), 2.9 * Math.sin(a + Math.PI)]); } return pts.map(([x, z]) => [Math.max(x, -2.6), z]); })(), 1, epo).svg;
    sv += cylSide(A, C, [0, 1, 0], [0, 1, 0], 5.2, 2.5, epo, `opacity=".96"`);
    // dome
    const top = C.p([0, 6.2, 0]);
    sv += `<path d="${discD(C, [0, 6.2, 0], [0, 1, 0], 2.5)}" fill="${shadeN(epo, C.rot([0, 1, 0]))}"/>`;
    const dc = C.p([0, 7.3, 0]);
    sv += `<ellipse cx="${r2(dc[0])}" cy="${r2(dc[1] + s * 0.5)}" rx="${r2(2.5 * s)}" ry="${r2(1.7 * s)}" fill="${A.rad([[0, shade(col, 0.35)], [0.6, col], [1, shade(col, -0.3)]], dc[0] - 0.8 * s, dc[1] - 0.2 * s, 3 * s)}"/>`;
    // internals: anvil + post
    const a1 = C.p([-0.9, 1.4, 0]), a2 = C.p([-0.2, 5, 0]), p1 = C.p([1, 1.4, 0]), p2 = C.p([0.9, 4.4, 0]);
    sv += `<path d="M${r2(a1[0])} ${r2(a1[1])} L${r2(a2[0])} ${r2(a2[1])} L${r2(a2[0] - 18)} ${r2(a2[1] + 8)} L${r2(a1[0] - 8)} ${r2(a1[1])} Z" fill="#000" opacity=".28"/><path d="M${r2(p1[0])} ${r2(p1[1])} L${r2(p2[0])} ${r2(p2[1])}" stroke="#000" stroke-opacity=".25" stroke-width="7"/>`;
    sv += `<ellipse cx="${r2(top[0] - 0.9 * s)}" cy="${r2(top[1] - 0.2 * s)}" rx="${r2(0.4 * s)}" ry="${r2(1.4 * s)}" fill="#fff" opacity=".45" transform="rotate(-8 ${r2(top[0] - 0.9 * s)} ${r2(top[1])})"/>`;
    sv += streak(A, ...C.p([-1.6, 1.4, 1.8]), ...C.p([-1.6, 5.6, 1.8]), 5, 0.4);
    if (lit) sv += `<circle class="cp-glow" cx="${r2(top[0])}" cy="${r2(top[1] + 0.6 * s)}" r="${r2(5 * s)}" fill="${A.rad([[0, "#fff", 0.85], [0.18, col, 0.75], [1, col, 0]], top[0], top[1] + 0.6 * s, 5 * s)}"/>`;
    A.add(sv);
    tagsRow(A, [mkTag(o, p.nodes[0], "ANODE (long)", { x: la.end[0], y: la.end[1] }), mkTag(o, p.nodes[1], "CATHODE (flat)", { x: lk.end[0], y: lk.end[1] })], la.end[1] + 18);
    const cn = /GREEN/.test(m) ? "green" : /BLUE/.test(m) ? "blue" : /YELLOW|AMBER/.test(m) ? "yellow" : /WHITE/.test(m) ? "white" : "red";
    return { A, name: `5 mm ${cn} LED${/LED/.test(m) ? "" : ` (${m})`}`, pkg: "T-1¾ diffused epoxy · used as a clipping diode", chips: ["LED", cn === "red" ? "≈ 1.8 V" : "≈ 2 V+", lit ? "Lit" : "Dark"] };
  }

  /* =====================================================================
     TRANSISTORS (TO-92, TO-1 germanium cans)
     ===================================================================== */
  const BJT = [
    { re: /^2SC1815/, mk: ["C1815", "GR 6E"], pins: "ECB" }, { re: /^2SC828/, mk: ["C828", "R 8G"], pins: "ECB" }, { re: /^2SC/, mk: (m) => [m.replace(/^2S/, ""), "Y 6E"], pins: "ECB" },
    { re: /^2N50(88|89)/, mk: (m) => ["2N", m.slice(2)], pins: "EBC" }, { re: /^2N3904/, mk: ["2N", "3904"], pins: "EBC" }, { re: /^2N3906/, mk: ["2N", "3906"], pins: "EBC" },
    { re: /^2N5457|^2N5458|^2N5459/, mk: (m) => ["2N", m.slice(2)], pins: "DSG" }, { re: /^J20[1-2]|^J11[1-3]|^J30/, mk: (m) => [m, "54"], pins: "DSG" }, { re: /^MPF102/, mk: ["MPF", "102"], pins: "DSG" },
    { re: /^BC5/, mk: (m) => [m, "C 29"], pins: "CBE" }, { re: /^2N/, mk: (m) => ["2N", m.slice(2)], pins: "EBC" },
  ];
  const GE_CAN = /^(AC1|OC|NKT|2N404|2N1305|2N527|2N2613|GT|MP|AC12|ASY)/;
  function artTransistor(p, o) {
    const m = String(p.model || (p.type === "pnp" ? "2N3906" : p.type === "njf" ? "J201" : "2N3904")).toUpperCase();
    const fet = p.type === "njf" || p.type === "pjf";
    if (GE_CAN.test(m) || /germanium|to-?1|to-?5|can/.test(o.pkg)) return artTOcan(p, o, m);
    const spec = BJT.find((b) => b.re.test(m)) || { mk: [m.slice(0, 6), ""], pins: fet ? "DSG" : "EBC" };
    const mk = typeof spec.mk === "function" ? spec.mk(m) : spec.mk;
    const A = new Art(640, 420);
    const Hh = 4.9, legL = FIG ? 4.6 : 7.5;
    const C = fitCam(-0.5, 0.3, boxPts(-3.4, 3.4, -legL, Hh, -1.4, 1.0), [200, 14, 440, 330], 34), s = C.s;
    const ep = { c: "#1f2023", amb: 0.5, kd: 0.55, ks: 0.28, pw: 14, env: 0.12 };
    const leg = { c: "#c7cbd0", amb: 0.4, kd: 0.6, ks: 0.9, pw: 40, env: 0.25 };
    A.shadowPath(poly(hull(rrect(5.6, 4.2, 1.5).map(([x, z]) => C.p([x + 0.6, -legL, z - 0.6])))), 0.45, 10);
    // D-shaped section: flat face at z=+1, round back
    const D = []; D.push([-2.4, 1.0]); D.push([2.4, 1.0]);
    for (let i = 0; i <= 24; i++) { const a = -i / 24 * Math.PI; D.push([2.4 * Math.cos(a), 1.0 + 2.4 * Math.sin(a) * 0.98]); }
    D.pop(); // last equals (-2.4, 1.0)
    const legX = [-1.27, 0, 1.27], ends = [];
    legX.forEach((x, i) => {
      const bx = x + (i - 1) * 0.9;
      const pts = [[x, 0.2, 0.4], [x, -2.2, 0.4], [bx, -3.4, 0.4], [bx, -legL, 0.4]];
      const pp = pts.map(C.p);
      A.add(wire(`M${pp.map((q) => `${r2(q[0])} ${r2(q[1])}`).join(" L")}`, 0.5 * s, "#c3c7cd", 0.7));
      ends.push(pp[3]);
    });
    const pr = prism(C, [0, 0, 0], [0, 1, 0], [1, 0, 0], [0, 0, 1], D.map(([x, z]) => [x, z]), Hh, ep, { capM: { c: "#202124", amb: 0.55, kd: 0.45, ks: 0.15, pw: 10 } });
    let sv = pr.svg;
    // the flat face gets its own sheen, then the laser marking
    const fq = [[-2.4, 0, 1.0], [2.4, 0, 1.0], [2.4, Hh, 1.0], [-2.4, Hh, 1.0]].map(C.p);
    sv += `<path d="${poly(fq)}" fill="${A.lin(FIG ? [[0, "#4a4d54"], [0.55, "#3a3c42"], [1, "#2e3035"]] : [[0, "#2a2b2f"], [0.55, "#1c1d20"], [1, "#151618"]], fq[3][0], fq[3][1], fq[1][0], fq[1][1])}"/>`;
    sv += `<path d="${poly(fq)}" fill="none" stroke="#fff" stroke-opacity=".08" stroke-width="1.2"/>`;
    sv += onPlane(C, [0, Hh * 0.6, 1.01], [1, 0, 0], [0, 1, 0], `<g fill="#c9ccd1" text-anchor="middle" font-weight="700" opacity=".9"><text x="0" y="-2" font-size="10.5" letter-spacing=".4">${esc(mk[0])}</text><text x="0" y="12" font-size="9.5" letter-spacing=".4">${esc(mk[1] || "")}</text></g>`);
    // ejector pin dimple on top
    const tc = C.p([0, Hh + 0.01, -0.4]);
    sv += `<ellipse cx="${r2(tc[0])}" cy="${r2(tc[1])}" rx="${r2(0.9 * s)}" ry="${r2(0.9 * s * Math.sin(0.3) * 1.1)}" fill="#141517" stroke="#fff" stroke-opacity=".07"/>`;
    A.add(sv);
    // pin roles → nodes (netlist order: c,b,e or d,g,s)
    const roleIdx = fet ? { D: 0, G: 1, S: 2 } : { C: 0, B: 1, E: 2 };
    const roleName = { E: "EMITTER", B: "BASE", C: "COLLECTOR", D: "DRAIN", G: "GATE", S: "SOURCE" };
    tagsRow(A, spec.pins.split("").map((r, i) => mkTag(o, p.nodes[roleIdx[r]], roleName[r], { x: ends[i][0], y: ends[i][1] })), ends[1][1] + 22);
    const kind = fet ? `${p.type === "pjf" ? "P" : "N"}-channel JFET` : `${p.type.toUpperCase()} transistor`;
    return { A, name: `${m} ${kind}`, pkg: `TO-92 epoxy · pins ${spec.pins.split("").join("-")} (flat face, left to right)`, chips: ["TO-92", fet ? "JFET" : p.type.toUpperCase(), "Silicon"] };
  }
  function artTOcan(p, o, m) {
    const A = new Art(640, 420);
    const C = fitCam(-0.45, 0.42, cylPts(3.6, -8, 6).concat([[-2.6, -8, 0], [2.6, -8, 0]]), [200, 14, 440, 320], 30), s = C.s;
    const metal = { c: "#c3c8ce", amb: 0.38, kd: 0.6, ks: 0.9, pw: 36, env: 0.25 };
    A.shadowPath(discD(C, [0.8, -8, -0.8], [0, 1, 0], 3.6), 0.45, 9);
    const lx = [[-1.6, 0], [0, 1.4], [1.6, 0]], ends = [];
    lx.forEach(([x, z], i) => { const a = C.p([x * 0.6, 0, z * 0.5]), b = C.p([x * 1.6, -8, z * 0.5 + 0.6]); A.add(wire(`M${r2(a[0])} ${r2(a[1])} Q${r2((a[0] + b[0]) / 2)} ${r2(a[1] + 30)} ${r2(b[0])} ${r2(b[1])}`, 4.5, "#c9b07a", 0.6)); ends.push(b); });
    let sv = cylSide(A, C, [0, 0, 0], [0, 1, 0], 0.6, 3.6, metal) + disc(C, [0, 0.6, 0], [0, 1, 0], 3.6, shadeN(metal, C.rot([0, 1, 0])));
    sv += cylSide(A, C, [0, 0.6, 0], [0, 1, 0], 5.4, 3.0, metal);
    sv += disc(C, [0, 6, 0], [0, 1, 0], 3.0, A.lin([[0, "#f2f4f6"], [0.5, "#aeb4bb"], [1, "#7a8088"]], ...C.p([-3, 6, -3]), ...C.p([3, 6, 3])));
    const rd = C.p([1.2, 0.62, 2.4]);
    sv += `<ellipse cx="${r2(rd[0])}" cy="${r2(rd[1])}" rx="7" ry="4" fill="#c4221c"/>`;
    sv += onPlane(C, [0, 3.4, 3.01], [1, 0, 0], [0, 1, 0], `<g fill="#2a2c30" text-anchor="middle" font-weight="800"><text x="0" y="0" font-size="11">${esc(m)}</text></g>`);
    A.add(sv);
    const nm = ["EMITTER", "BASE", "COLLECTOR"], idx = [2, 1, 0];
    tagsRow(A, ends.map((e, i) => mkTag(o, p.nodes[idx[i]], nm[i] + (i === 2 ? " ●" : ""), { x: e[0], y: e[1] })), ends[0][1] + 34);
    return { A, name: `${m} germanium ${p.type.toUpperCase()} transistor`, pkg: "TO-1 metal can · red dot marks the collector", chips: ["TO-1", "Germanium", p.type.toUpperCase()] };
  }

  /* =====================================================================
     OP-AMPS (DIP-8)
     ===================================================================== */
  const OPA = [
    { re: /4558/, mk: ["JRC", "4558D", "8L2A"], dual: true, nm: "JRC4558D" }, { re: /TL072/, mk: ["TL072CP", "", "9AK"], dual: true, nm: "TL072CP" },
    { re: /TL071/, mk: ["TL071CP", "", "9AK"], dual: false, nm: "TL071CP" }, { re: /5532/, mk: ["NE5532P", "", "K28"], dual: true, nm: "NE5532P" },
    { re: /741/, mk: ["LM741CN", "", "8A3"], dual: false, nm: "LM741CN" }, { re: /308/, mk: ["LM308N", "", "75"], dual: false, nm: "LM308N" },
    { re: /4580/, mk: ["JRC", "4580D", "9C1"], dual: true, nm: "NJM4580D" }, { re: /OPA2134/, mk: ["OPA", "2134PA", ""], dual: true, nm: "OPA2134PA" },
    { re: /TL061|TL062/, mk: (m) => [m + "CP", "", "9AK"], dual: /2$/.test("TL062"), nm: "TL06x" }, { re: /IDEAL/, mk: ["IDEAL", "OP-AMP", ""], dual: true, nm: "Ideal op-amp" },
  ];
  function artOpamp(p, o) {
    const m = String(p.model || "IDEAL_OPAMP").toUpperCase();
    const spec = OPA.find((x) => x.re.test(m)) || { mk: [m.slice(0, 8), "", ""], dual: true, nm: m };
    const mk = typeof spec.mk === "function" ? spec.mk(m) : spec.mk;
    const A = new Art(640, 470);
    const Lb = 9.6, Wb = 6.4, H = 3.3, y0 = 0.6;
    const flip = !!FIG;
    const C = fitCam(flip ? -0.34 + Math.PI : -0.34, 0.7, boxPts(-Lb / 2 - 0.4, Lb / 2 + 0.4, -3.1, y0 + H, -Wb / 2 - 1, Wb / 2 + 1), [150, 78, 490, 386], 34), s = C.s;
    const ep = { c: "#1c1d20", amb: 0.5, kd: 0.5, ks: 0.22, pw: 12, env: 0.12 };
    const tin = { c: "#c9cdd2", amb: 0.4, kd: 0.6, ks: 0.9, pw: 40, env: 0.2 };
    A.shadowPath(poly(hull(rrect(Lb + 1.4, Wb + 3.6, 0.8).map(([x, z]) => C.p([x + 0.5, -3.1, z - 0.4])))), 0.5, 11);
    const px = [-3.81, -1.27, 1.27, 3.81];
    // which pins belong to this section
    const parts = (o.c.parts || []).filter((q) => q.type === "opamp" && q.ref.replace(/[AB]$/, "") === p.ref.replace(/[AB]$/, ""));
    const secB = /B$/.test(p.ref) && spec.dual;
    const pinOf = (q) => (spec.dual ? (/B$/.test(q.ref) ? { "+": 5, "−": 6, out: 7 } : { "+": 3, "−": 2, out: 1 }) : { "+": 3, "−": 2, out: 6 });
    const pinNode = {}, pinRole = {}, mine = new Set();
    parts.concat(parts.includes(p) ? [] : [p]).forEach((q) => {
      const map = pinOf(q);
      pinNode[map["+"]] = q.nodes[0]; pinNode[map["−"]] = q.nodes[1]; pinNode[map.out] = q.nodes[2];
      const sec = spec.dual ? (/B$/.test(q.ref) ? " B" : " A") : "";
      pinRole[map["+"]] = `+IN${sec}`; pinRole[map["−"]] = `−IN${sec}`; pinRole[map.out] = `OUT${sec}`;
      if (q === p) [map["+"], map["−"], map.out].forEach((k) => mine.add(k));
      if (q.nodes[3] != null) { pinNode[8] = q.nodes[3]; pinRole[8] = "V+"; } if (q.nodes[4] != null) { pinNode[4] = q.nodes[4]; pinRole[4] = "V−"; }
    });
    if (!spec.dual) { pinRole[7] = "V+"; pinNode[7] = pinNode[8]; delete pinNode[8]; pinRole[8] = "NC"; }
    // legs: pin 1-4 near side (z+), left→right; 8-5 far side
    const legSvg = (x, side, front) => {
      const z0 = side * Wb / 2, z1 = side * (Wb / 2 + 0.9), yl = y0 + 1.25;
      const sh = [[x - 0.75, yl, z0], [x + 0.75, yl, z0], [x + 0.75, yl, z1], [x - 0.75, yl, z1]].map(C.p);
      const v1 = [[x - 0.75, yl, z1], [x + 0.75, yl, z1], [x + 0.75, y0 - 0.5, z1], [x + 0.26, y0 - 0.9, z1], [x + 0.26, -3.1, z1], [x - 0.26, -3.1, z1], [x - 0.26, y0 - 0.9, z1], [x - 0.75, y0 - 0.5, z1]].map(C.p);
      const nc = C.rot([0, 0, side]);
      const c2 = shadeN(tin, nc[2] > 0 ? nc : mul3(nc, -1)), c1 = mix(c2, "#ffffff", 0.12);
      return { svg: `<path d="${poly(sh)}" fill="${c1}" stroke="${shade(c1, -0.3)}" stroke-width=".6"/><path d="${poly(v1)}" fill="${front ? A.lin([[0, shade(c2, -0.2)], [0.35, shade(c2, 0.3)], [1, shade(c2, -0.35)]], v1[0][0], 0, v1[1][0], 0) : shade(c2, -0.35)}" stroke="${shade(c2, -0.5)}" stroke-width=".6"/>`, tip: C.p([x, -3.1, z1]), sh: C.p([x, yl, z1]) };
    };
    const far = [8, 7, 6, 5].map((pin, i) => Object.assign(legSvg(px[i], -1, false), { pin }));
    const near = [1, 2, 3, 4].map((pin, i) => Object.assign(legSvg(px[i], 1, true), { pin }));
    (flip ? near : far).forEach((l) => A.add(l.svg));
    const pr = prism(C, [0, y0, 0], [0, 1, 0], [1, 0, 0], [0, 0, 1], rrect(Lb, Wb, 0.35, 3), H, ep, { capM: { c: "#202124", amb: 0.6, kd: 0.4, ks: 0.12, pw: 8 } });
    A.add(pr.svg);
    // top: notch, pin-1 dimple, marking
    const top = y0 + H + 0.01;
    let tp = "";
    const nt = []; for (let i = 0; i <= 16; i++) { const a = (-90 + 180 * i / 16) * DEG; nt.push(C.p([-Lb / 2 + 0.9 * Math.cos(a), top, 0.9 * Math.sin(a)])); }
    tp += `<path d="${poly(nt)}" fill="#0e0f11"/>`;
    const dm = C.p([-Lb / 2 + 1.5, top, Wb / 2 - 1.4]);
    tp += `<ellipse cx="${r2(dm[0])}" cy="${r2(dm[1])}" rx="${r2(0.55 * s)}" ry="${r2(0.55 * s * Math.sin(0.7))}" fill="#121315" stroke="#fff" stroke-opacity=".1"/>`;
    tp += onPlane(C, [0.4, top, 0], [1, 0, 0], [0, 0, -1], `<g fill="#cfd2d6" text-anchor="middle" font-weight="700" opacity=".88">${mk[1] ? `<text x="0" y="-10" font-size="13" letter-spacing="1">${esc(mk[0])}</text><text x="0" y="8" font-size="15" letter-spacing=".8">${esc(mk[1])}</text>` : `<text x="0" y="0" font-size="14" letter-spacing=".8">${esc(mk[0])}</text>`}<text x="0" y="${mk[1] ? 23 : 16}" font-size="9.5" opacity=".75" letter-spacing="1">${esc(mk[2] || "")}</text></g>`);
    // top edge highlight
    tp += `<path d="${poly(pr.top.map(C.p))}" fill="none" stroke="#fff" stroke-opacity=".1" stroke-width="1"/>`;
    A.add(tp);
    (flip ? far : near).forEach((l) => A.add(l.svg));
    const tagFor = (l) => {
      const n = pinNode[l.pin];
      const t = n != null ? mkTag(o, n, `${l.pin} ${pinRole[l.pin] || ""}`.trim()) : { node: "—", v: "", k: `${l.pin} ${pinRole[l.pin] || "NC"}` };
      t.dim = !mine.has(l.pin) && !(pinRole[l.pin] || "").startsWith("V");
      return t;
    };
    if (flip) tagsRow(A, far.concat(near).map((l) => Object.assign(tagFor(l), { x: (far.includes(l) ? l.tip : l.sh)[0], y: (far.includes(l) ? l.tip : l.sh)[1] })), 0);
    else {
      tagsRow(A, near.map((l) => Object.assign(tagFor(l), { x: l.tip[0], y: l.tip[1] })), near[0].tip[1] + 22);
      tagsRow(A, far.map((l) => Object.assign(tagFor(l), { x: l.sh[0], y: l.sh[1] })), Math.min(...far.map((l) => l.sh[1])) - 42, -1);
    }
    return {
      A, name: `${spec.nm} ${spec.dual ? "dual" : "single"} op-amp${spec.dual ? `, section ${secB ? "B" : "A"}` : ""}`,
      pkg: `DIP-8 · ${spec.dual ? `this section on pins ${secB ? "5-6-7" : "1-2-3"}, supply on 8 and 4` : "pins 2-3-6, supply on 7 and 4"}`, chips: ["DIP-8", spec.dual ? "Dual" : "Single", secB ? "Section B" : spec.dual ? "Section A" : ""],
    };
  }

  /* =====================================================================
     VACUUM TUBES
     ===================================================================== */
  const TUBES = [
    { re: /^(12AX7|ECC83|7025|5751)/, name: "12AX7", alt: "ECC83", base: "noval", D: 22, G: 46, glass: "mini", inner: "dual", pl: { w: 7, h: 12, c: "#6a6f74" }, kind: "dual triode", pins: { A: [1, 2, 3], B: [6, 7, 8] }, heat: [4, 5, 9] },
    { re: /^(12AT7|ECC81|6201)/, name: "12AT7", alt: "ECC81", base: "noval", D: 22, G: 46, glass: "mini", inner: "dual", pl: { w: 6.8, h: 11, c: "#858a8f", fins: true }, kind: "dual triode", pins: { A: [6, 7, 8], B: [1, 2, 3] }, heat: [4, 5, 9] },
    { re: /^(12AU7|ECC82|5814|6189)/, name: "12AU7", alt: "ECC82", base: "noval", D: 22, G: 46, glass: "mini", inner: "dual", pl: { w: 6.6, h: 12, c: "#6f7479" }, kind: "dual triode", pins: { A: [6, 7, 8], B: [1, 2, 3] }, heat: [4, 5, 9] },
    { re: /^(12AY7|6072)/, name: "12AY7", alt: "6072", base: "noval", D: 22, G: 46, glass: "mini", inner: "dual", pl: { w: 6.6, h: 10.5, c: "#7c8186" }, kind: "dual triode", pins: { A: [6, 7, 8], B: [1, 2, 3] }, heat: [4, 5, 9] },
    { re: /^(EL84|6BQ5)/, name: "EL84", alt: "6BQ5", base: "noval", D: 22, G: 58, glass: "mini", inner: "pentode", pl: { w: 12, h: 21, c: "#5f6469" }, kind: "power pentode", pins: { p: 7, g2: 9, g1: 2, k: 3 }, heat: [4, 5] },
    { re: /^(EL34|6CA7)/, name: "EL34", alt: "6CA7", base: "octal", D: 32, G: 86, glass: "tubular", inner: "pentode", pl: { w: 17, h: 40, c: "#3c3e42", dark: true }, kind: "power pentode", pins: { p: 3, g2: 4, g1: 5, k: 8 }, heat: [2, 7] },
    { re: /^(6L6|5881|KT66|7581)/, name: "6L6GC", base: "octal", D: 38, G: 82, glass: "dome", inner: "beam", pl: { w: 19, h: 38, c: "#666b71" }, kind: "beam power tetrode", pins: { p: 3, g2: 4, g1: 5, k: 8 }, heat: [2, 7] },
    { re: /^6V6/, name: "6V6GT", base: "octal", D: 30, G: 62, glass: "tubular", inner: "beam", pl: { w: 14, h: 27, c: "#62676d" }, kind: "beam power tetrode", pins: { p: 3, g2: 4, g1: 5, k: 8 }, heat: [2, 7] },
    { re: /^6550/, name: "6550", alt: "6550A", base: "octal", D: 46, G: 92, glass: "dome", inner: "beam", pl: { w: 22, h: 42, c: "#70757b" }, kind: "beam power tetrode", pins: { p: 3, g2: 4, g1: 5, k: 8 }, heat: [2, 7] },
    { re: /^(KT88|KT90|KT120|KT150)/, name: "KT88", base: "octal", D: 52, G: 96, glass: "coke", inner: "beam", pl: { w: 24, h: 42, c: "#6f747a" }, kind: "beam power tetrode", pins: { p: 3, g2: 4, g1: 5, k: 8 }, heat: [2, 7] },
    { re: /^(5AR4|GZ34)/, name: "GZ34", alt: "5AR4", base: "octal", D: 32, G: 76, glass: "dome", inner: "rect", pl: { w: 8.5, h: 22, c: "#7f8489" }, kind: "full-wave rectifier", pins: { a: [4, 6], k: [8] }, heat: [2, 8] },
    { re: /^5U4/, name: "5U4GB", base: "octal", D: 38, G: 80, glass: "dome", inner: "rect", pl: { w: 9.5, h: 24, c: "#4a4d51" }, kind: "full-wave rectifier", pins: { a: [4, 6], k: [2, 8] }, heat: [2, 8] },
    { re: /^(5Y3|GZ32|GZ37|5V4|5Z4)/, name: "5Y3GT", base: "octal", D: 32, G: 70, glass: "dome", inner: "rect", pl: { w: 8, h: 20, c: "#4a4d51" }, kind: "full-wave rectifier", pins: { a: [4, 6], k: [2, 8] }, heat: [2, 8] },
    { re: /^(EZ81|6CA4)/, name: "EZ81", alt: "6CA4", base: "noval", D: 22, G: 52, glass: "mini", inner: "rect", pl: { w: 6, h: 14, c: "#7f8489" }, kind: "full-wave rectifier", pins: { a: [1, 7], k: [3] }, heat: [4, 5] },
  ];
  function tubeSpec(model, type, fam) {
    const m = String(model || "").toUpperCase();
    const hit = TUBES.find((t) => t.re.test(m));
    if (hit) return hit;
    const byName = (n) => TUBES.find((t) => t.name === n);
    return type === "pentode" || /pentode|tetrode/.test(fam || "") ? byName("6L6GC") : (type === "D" || /rectifier/.test(fam || "")) ? byName("GZ34") : byName("12AX7");
  }
  // pin angle (base seen from above, 0 = toward the viewer, + = to the right)
  function pinAng(T, i) { return T.base === "noval" ? (180 + 36 * i) * DEG : (180 + 45 * i - 22.5) * DEG; }

  function artTube(p, o) {
    const T = tubeSpec(p.model, p.type, o.fam);
    const isRect = T.inner === "rect";
    const A = new Art(640, 500);
    const pinL = T.base === "noval" ? 7 : 10.5, baseH = T.base === "noval" ? 0 : 19;
    const tot = T.G + baseH + pinL + 4;
    const s = Math.min(430 / tot, 196 / T.D), W = T.D * s, cx = 320;
    const yTop = 24 + (430 - tot * s) / 2 + 6, yGb = yTop + T.G * s, yB1 = yGb + baseH * s;
    const ell = 0.2; // base ellipse ratio (slight view from above)
    const heat = o.on ? 1 : 0;
    // which section / pins this part uses
    let use = [];
    if (p.type === "triode") {
      const sib = (o.c.parts || []).filter((q) => q.type === "triode" && q.model === p.model && q.ref.replace(/[AB]$/, "") === p.ref.replace(/[AB]$/, ""));
      const sec = /B$/.test(p.ref) ? "B" : /A$/.test(p.ref) ? "A" : sib.indexOf(p) === 1 ? "B" : "A";
      const pn = T.pins[sec] || T.pins.A || [1, 2, 3];
      use = [[pn[0], "PLATE", p.nodes[0]], [pn[1], "GRID", p.nodes[1]], [pn[2], "CATHODE", p.nodes[2]]];
      T._sec = sec;
    } else if (p.type === "pentode") {
      const pn = T.pins.p ? T.pins : { p: 3, g2: 4, g1: 5, k: 8 };
      use = [[pn.p, "PLATE", p.nodes[0]], [pn.g2, "SCREEN", p.nodes[1]], [pn.g1, "GRID", p.nodes[2]], [pn.k, "CATHODE", p.nodes[3]]];
    } else {
      const sib = (o.c.parts || []).filter((q) => q.type === p.type && q.model === p.model);
      const k = Math.max(0, sib.indexOf(p)) % 2;
      const pa = (T.pins.a || [4, 6]), pk = (T.pins.k || [8])[(T.pins.k || [8]).length - 1];
      use = (p.nodes || []).length >= 3 ? [[pa[0], "PLATE 1", p.nodes[0]], [pa[1], "PLATE 2", p.nodes[1]], [pk, "CATHODE", p.nodes[2]]] : [[pa[k], "PLATE", p.nodes[0]], [pk, "CATHODE", p.nodes[1]]];
    }
    const kv = use.find((u) => u[1] === "CATHODE"), pv = use.find((u) => /^PLATE/.test(u[1]));
    const vk = kv ? o.V(kv[2]) : null, vp = pv ? o.V(pv[2]) : null;
    const conducting = vk == null || vp == null ? true : vp - vk > 5 || isRect;
    // ---- shadow
    A.shadow(cx, yB1 + pinL * s + 4, W * 0.75, W * 0.12, 0.5, 9);
    // ---- envelope path
    const xl = cx - W / 2, xr = cx + W / 2;
    const rr = T.glass === "dome" ? W * 0.5 : T.glass === "tubular" ? W * 0.3 : T.glass === "coke" ? W * 0.42 : W * 0.46;
    const by = W / 2 * ell;
    const glassBot = T.base === "octal" ? yGb + 6 * s : yGb;
    let env;
    if (T.glass === "coke") {
      const bw = W * 0.08;
      env = `M${xl + bw * 0.3} ${glassBot} C${xl - bw} ${glassBot - T.G * s * 0.25} ${xl - bw} ${yTop + T.G * s * 0.45} ${xl + bw * 0.4} ${yTop + T.G * s * 0.3} C${xl + bw} ${yTop + rr * 0.6} ${xl} ${yTop + rr * 0.5} ${xl + W * 0.04} ${yTop + rr * 0.45} C${xl + W * 0.1} ${yTop} ${cx - W * 0.2} ${yTop} ${cx} ${yTop} C${cx + W * 0.2} ${yTop} ${xr - W * 0.1} ${yTop} ${xr - W * 0.04} ${yTop + rr * 0.45} C${xr} ${yTop + rr * 0.5} ${xr - bw} ${yTop + rr * 0.6} ${xr - bw * 0.4} ${yTop + T.G * s * 0.3} C${xr + bw} ${yTop + T.G * s * 0.45} ${xr + bw} ${glassBot - T.G * s * 0.25} ${xr - bw * 0.3} ${glassBot} A${W / 2} ${by} 0 0 1 ${xl + bw * 0.3} ${glassBot} Z`;
    } else {
      env = `M${xl} ${glassBot} V${yTop + rr} C${xl} ${yTop + rr * 0.42} ${cx - W * 0.28} ${yTop} ${cx} ${yTop} C${cx + W * 0.28} ${yTop} ${xr} ${yTop + rr * 0.42} ${xr} ${yTop + rr} V${glassBot} A${W / 2} ${by} 0 0 1 ${xl} ${glassBot} Z`;
    }
    const clipEnv = A.clip(env);
    // ---- pins (back ones first)
    const Rp = (T.base === "noval" ? 5.95 : 8.7) * s;
    const nPins = T.base === "noval" ? 9 : 8;
    const pinTop = T.base === "noval" ? yGb - 2 * s : yB1;
    const pins = [];
    for (let i = 1; i <= nPins; i++) {
      const a = pinAng(T, i), x = cx + Rp * Math.sin(a), dz = Math.cos(a), yo = dz * Rp * ell;
      pins.push({ i, x, dz, y0: pinTop + yo, y1: pinTop + yo + pinL * s });
    }
    const pw = (T.base === "noval" ? 1.0 : 2.36) * s;
    const pinSvg = (q) => {
      const c = q.dz < 0 ? "#8f959c" : "#c9cdd2";
      return `<rect x="${r2(q.x - pw / 2)}" y="${r2(q.y0)}" width="${r2(pw)}" height="${r2(q.y1 - q.y0)}" rx="${r2(pw / 2)}" fill="${A.lin([[0, shade(c, -0.45)], [0.35, shade(c, 0.5)], [0.6, c], [1, shade(c, -0.5)]], q.x - pw / 2, 0, q.x + pw / 2, 0)}"/>`
        + (T.base === "octal" ? `<ellipse cx="${r2(q.x)}" cy="${r2(q.y1 - pw * 0.15)}" rx="${r2(pw * 0.42)}" ry="${r2(pw * 0.2)}" fill="#e3c78a" opacity=".85"/>` : "");
    };
    pins.filter((q) => q.dz < 0).forEach((q) => A.add(pinSvg(q)));
    if (T.base === "octal") {
      // key spigot
      const kw = 7.8 * s, ky0 = yB1 - 1 * s, ky1 = yB1 + 5 * s;
      A.add(`<rect x="${r2(cx - kw / 2)}" y="${r2(ky0)}" width="${r2(kw)}" height="${r2(ky1 - ky0)}" rx="${r2(kw * 0.12)}" fill="${A.lin([[0, "#050505"], [0.3, "#3b3c40"], [0.45, "#1b1c1f"], [1, "#030303"]], cx - kw / 2, 0, cx + kw / 2, 0)}"/><ellipse cx="${cx}" cy="${r2(ky1)}" rx="${r2(kw / 2)}" ry="${r2(kw / 2 * ell)}" fill="#141416"/><rect x="${r2(cx - 0.6 * s)}" y="${r2(ky0)}" width="${r2(1.2 * s)}" height="${r2(ky1 - ky0)}" fill="#2a2b2e"/>`);
    }
    // ---- back of the glass (very faint)
    A.add(`<path d="${env}" fill="${A.lin([[0, "#a8c0c4", 0.16], [0.5, "#dfeef0", 0.05], [1, "#a8c0c4", 0.16]], xl, 0, xr, 0)}"/>`);
    // ---- internals
    const iTop = yTop + rr * 0.95, iBot = glassBot - (T.base === "noval" ? 3 : 4) * s;
    const plH = T.pl.h * s, plW = T.pl.w * s;
    const plY0 = T.inner === "dual" ? iTop + (iBot - iTop) * 0.24 : iTop + (iBot - iTop) * 0.18;
    const plY1 = plY0 + plH;
    const micaT = plY0 - 2.6 * s, micaB = plY1 + 2.4 * s;
    const iw = W * 0.92;
    let g = "";
    // lead wires from the bottom mica down to the pins
    pins.forEach((q) => { const yb = T.base === "noval" ? yGb - 1 * s : glassBot - 2 * s; g += `<path d="M${r2(q.x * 0.55 + cx * 0.45)} ${r2(micaB + 1)} L${r2(q.x)} ${r2(yb + q.dz * Rp * ell)}" stroke="${q.dz < 0 ? "#6e7378" : "#9aa0a6"}" stroke-width="${r2(Math.max(1.2, 0.35 * s))}"/>`; });
    // support rods
    const rods = T.inner === "dual" ? [-0.36, -0.12, 0.12, 0.36] : T.inner === "rect" ? [-0.3, -0.08, 0.08, 0.3] : [-0.3, -0.12, 0.12, 0.3];
    rods.forEach((f, i) => { const x = cx + f * W; g += `<path d="M${r2(x)} ${r2(micaT - 2 * s)} V${r2(micaB + 2.5 * s)}" stroke="${T.inner === "pentode" || T.inner === "beam" ? (i === 1 || i === 2 ? "#b88a4a" : "#9ca2a8") : "#a7adb3"}" stroke-width="${r2(Math.max(1.3, 0.4 * s))}"/>`; });
    // heater/cathode glow (behind the plates, peeking out at the top/bottom and through gaps)
    const glowAt = (x, y, rx, ry, k = 1, cls = "") => heat ? `<ellipse class="cp-glow${cls}" cx="${r2(x)}" cy="${r2(y)}" rx="${r2(rx)}" ry="${r2(ry)}" fill="${A.rad([[0, "#fff1c9", 0.95 * k], [0.25, "#ffa040", 0.85 * k], [0.6, "#ff5a00", 0.35 * k], [1, "#ff3d00", 0]], x, y, Math.max(rx, ry), null, null, `translate(${r2(x)} ${r2(y)}) scale(${r2(rx / Math.max(rx, ry))} ${r2(ry / Math.max(rx, ry))}) translate(${r2(-x)} ${r2(-y)})`)}"/>` : "";
    const plates = T.inner === "dual" ? [cx - plW / 2 - 1.1 * s, cx + plW / 2 + 1.1 * s] : T.inner === "rect" ? [cx - plW / 2 - 0.9 * s, cx + plW / 2 + 0.9 * s] : [cx];
    const kHeat = conducting ? 1 : 0.7;
    plates.forEach((x, i) => { g += glowAt(x, plY0 - 0.6 * s, plW * 0.42, 2.6 * s, kHeat, i % 2 ? " b" : "") + glowAt(x, plY1 + 0.6 * s, plW * 0.36, 2.0 * s, kHeat * 0.8, i % 2 ? "" : " b"); });
    // plates
    const plate = (x) => {
      const x0 = x - plW / 2, c = T.pl.c;
      let t = "";
      if (T.inner === "beam") {
        // beam-forming plates (dark wings) behind the anode
        t += `<path d="M${r2(x0 - 2.2 * s)} ${r2(plY0 + 3 * s)} L${r2(x0 + 2 * s)} ${r2(plY0 + 1 * s)} V${r2(plY1 - 1 * s)} L${r2(x0 - 2.2 * s)} ${r2(plY1 - 3 * s)} Z M${r2(x0 + plW + 2.2 * s)} ${r2(plY0 + 3 * s)} L${r2(x0 + plW - 2 * s)} ${r2(plY0 + 1 * s)} V${r2(plY1 - 1 * s)} L${r2(x0 + plW + 2.2 * s)} ${r2(plY1 - 3 * s)} Z" fill="#2f3135"/>`;
      }
      const fl = (T.inner === "dual" ? 0.9 : 1.6) * s;
      t += `<rect x="${r2(x0 - fl)}" y="${r2(plY0 + 0.4 * s)}" width="${r2(plW + 2 * fl)}" height="${r2(plH - 0.8 * s)}" fill="${shade(c, -0.25)}"/>`;
      t += `<rect x="${r2(x0)}" y="${r2(plY0)}" width="${r2(plW)}" height="${r2(plH)}" rx="${r2(0.6 * s)}" fill="${A.lin([[0, shade(c, -0.6)], [0.14, shade(c, -0.1)], [0.32, shade(c, 0.22)], [0.5, shade(c, -0.12)], [0.8, shade(c, -0.4)], [1, shade(c, -0.7)]], x0, 0, x0 + plW, 0)}" stroke="#0d0e10" stroke-opacity=".55" stroke-width="1"/>`;
      t += `<rect x="${r2(x0)}" y="${r2(plY0)}" width="${r2(plW)}" height="${r2(plH)}" rx="${r2(0.6 * s)}" fill="${A.lin([[0, "#fff", 0.1], [0.12, "#fff", 0], [0.85, "#000", 0], [1, "#000", 0.3]], 0, plY0, 0, plY1)}"/>`;
      // embossed ribs / seams
      const ribs = T.inner === "dual" ? [0.5] : [0.3, 0.5, 0.7];
      ribs.forEach((f) => { const rx = x0 + plW * f; t += `<rect x="${r2(rx - 0.35 * s)}" y="${r2(plY0 + 0.8 * s)}" width="${r2(0.7 * s)}" height="${r2(plH - 1.6 * s)}" fill="#000" opacity=".22"/><rect x="${r2(rx + 0.35 * s)}" y="${r2(plY0 + 0.8 * s)}" width="${r2(0.35 * s)}" height="${r2(plH - 1.6 * s)}" fill="#fff" opacity=".16"/>`; });
      if (T.inner === "dual" || T.inner === "rect") {
        // the classic window slots in small-signal plates, glowing faintly
        [0.3, 0.64].forEach((f) => { const sy = plY0 + plH * f, sw = plW * 0.56, sh = Math.max(1.6, 0.7 * s), sx = x0 + plW * 0.22; t += `<rect x="${r2(sx)}" y="${r2(sy)}" width="${r2(sw)}" height="${r2(sh)}" rx="${r2(sh / 2)}" fill="#111214" opacity=".85"/>` + (heat ? `<rect class="cp-glow" x="${r2(sx + sw * 0.15)}" y="${r2(sy + sh * 0.2)}" width="${r2(sw * 0.7)}" height="${r2(sh * 0.6)}" rx="${r2(sh * 0.3)}" fill="${A.lin([[0, "#ff6a00", 0], [0.5, "#ffa04a", 0.85 * kHeat], [1, "#ff6a00", 0]], sx, 0, sx + sw, 0)}"/>` : ""); });
      }
      if (T.pl.fins) t += `<rect x="${r2(x0 - 2.4 * s)}" y="${r2(plY0 + plH * 0.2)}" width="${r2(1.4 * s)}" height="${r2(plH * 0.6)}" fill="${shade(c, -0.15)}"/><rect x="${r2(x0 + plW + 1 * s)}" y="${r2(plY0 + plH * 0.2)}" width="${r2(1.4 * s)}" height="${r2(plH * 0.6)}" fill="${shade(c, -0.15)}"/>`;
      if (T.inner === "pentode" || T.inner === "beam") {
        // welded side flanges with spot welds, embossed horizontal ribs, matte falloff toward the bottom
        const fw = 1.7 * s, fc = shade(c, 0.12);
        [x0 - fw, x0 + plW].forEach((fx) => {
          t += `<rect x="${r2(fx)}" y="${r2(plY0 + 0.6 * s)}" width="${r2(fw)}" height="${r2(plH - 1.2 * s)}" fill="${A.lin([[0, shade(fc, -0.35)], [0.5, shade(fc, 0.25)], [1, shade(fc, -0.4)]], fx, 0, fx + fw, 0)}" stroke="#0d0e10" stroke-opacity=".5" stroke-width=".8"/>`;
          for (let k = 0; k < 5; k++) t += `<circle cx="${r2(fx + fw / 2)}" cy="${r2(plY0 + plH * (0.12 + 0.19 * k))}" r="${r2(Math.max(1, 0.35 * s))}" fill="${shade(fc, -0.45)}"/>`;
        });
        [0.2, 0.5, 0.8].forEach((f) => { const ry = plY0 + plH * f; t += `<rect x="${r2(x0 + 0.5 * s)}" y="${r2(ry - 0.5 * s)}" width="${r2(plW - 1 * s)}" height="${r2(0.45 * s)}" fill="#fff" opacity=".13"/><rect x="${r2(x0 + 0.5 * s)}" y="${r2(ry)}" width="${r2(plW - 1 * s)}" height="${r2(0.55 * s)}" fill="#000" opacity=".3"/>`; });
        t += `<rect x="${r2(x0)}" y="${r2(plY0)}" width="${r2(plW)}" height="${r2(plH)}" fill="${A.lin([[0, "#000", 0], [0.6, "#000", 0.05], [1, "#000", 0.3]], 0, plY0, 0, plY1)}"/>`;
      }
      if (T.pl.dark) t += `<rect x="${r2(x0 + plW * 0.12)}" y="${r2(plY0 + plH * 0.08)}" width="${r2(plW * 0.76)}" height="${r2(1 * s)}" fill="#000" opacity=".35"/>`;
      return t;
    };
    plates.forEach((x) => { g += plate(x); });
    // grid radiator fins above power pentodes
    if (T.inner === "pentode" || T.inner === "beam") {
      [-1, 1].forEach((sd) => { const x = cx + sd * W * 0.12; g += `<rect x="${r2(x - 1.6 * s)}" y="${r2(micaT - 4.2 * s)}" width="${r2(3.2 * s)}" height="${r2(2.4 * s)}" fill="#1e1f22"/><rect x="${r2(x - 1.6 * s)}" y="${r2(micaT - 4.2 * s)}" width="${r2(3.2 * s)}" height="${r2(0.5 * s)}" fill="#fff" opacity=".12"/>`; });
    }
    // micas
    const mica = (y, t) => {
      // a mica disc seen slightly from above: translucent sheet, punched holes, serrated rim against the glass
      const rx = iw * 0.49, ry = Math.max(3, (W / 2) * ell * 0.95), th = Math.max(1.5, t * s * 0.6);
      let m = `<ellipse cx="${cx}" cy="${r2(y + th)}" rx="${r2(rx)}" ry="${r2(ry)}" fill="#8f8875" opacity=".55"/>`;
      m += `<ellipse cx="${cx}" cy="${r2(y)}" rx="${r2(rx)}" ry="${r2(ry)}" fill="${A.lin([[0, "#d9d2bf", 0.55], [0.3, "#f7f3e8", 0.72], [0.55, "#e6dfcc", 0.6], [1, "#bdb5a0", 0.55]], cx - rx, 0, cx + rx, 0)}" stroke="#fffaf0" stroke-opacity=".5" stroke-width="1"/>`;
      rods.forEach((fr) => { m += `<ellipse cx="${r2(cx + fr * W)}" cy="${r2(y)}" rx="${r2(Math.max(1.6, 0.5 * s))}" ry="${r2(Math.max(0.8, 0.25 * s))}" fill="#3b3a36" opacity=".7"/>`; });
      plates.forEach((x) => { m += `<rect x="${r2(x - plW * 0.42)}" y="${r2(y - 0.25 * s)}" width="${r2(plW * 0.84)}" height="${r2(Math.max(1, 0.5 * s))}" rx="1" fill="#4b4a45" opacity=".55"/>`; });
      for (let i = 0; i < 14; i++) { const a = (i + 0.5) / 14 * 2 * Math.PI; m += `<circle cx="${r2(cx + rx * 0.97 * Math.cos(a))}" cy="${r2(y + ry * 0.97 * Math.sin(a))}" r="${r2(Math.max(0.9, 0.3 * s))}" fill="#fffaf0" opacity=".45"/>`; }
      return m;
    };
    g += mica(micaT, 1.3) + mica(micaB, 1.2);
    // heater filament peeking out above the top mica
    if (T.inner !== "beam" || true) plates.forEach((x) => { g += `<path d="M${r2(x - 0.9 * s)} ${r2(micaT - 0.6 * s)} q${r2(0.45 * s)} ${r2(-1.6 * s)} ${r2(0.9 * s)} 0 q${r2(0.45 * s)} ${r2(-1.6 * s)} ${r2(0.9 * s)} 0" fill="none" stroke="${heat ? "#ffcf80" : "#d9d6cf"}" stroke-width="${r2(Math.max(1.2, 0.32 * s))}"${heat ? ` class="cp-glow"` : ""}/>`; });
    // getter (a small ring/cup on a wire above the top mica)
    const gy = yTop + rr * (T.glass === "dome" ? 0.75 : 0.85);
    g += `<path d="M${r2(cx + W * 0.2)} ${r2(micaT)} L${r2(cx + W * 0.12)} ${r2(gy + 1.4 * s)}" stroke="#9aa0a6" stroke-width="${r2(Math.max(1, 0.3 * s))}"/><ellipse cx="${r2(cx + W * 0.04)}" cy="${r2(gy + 1.2 * s)}" rx="${r2(W * 0.14)}" ry="${r2(0.9 * s)}" fill="none" stroke="#a9afb5" stroke-width="${r2(Math.max(1.4, 0.5 * s))}"/>`;
    // power tubes: blue fluorescence on the inner glass near the plate edges
    if (heat && conducting && (T.inner === "pentode" || T.inner === "beam")) {
      [-1, 1].forEach((sd) => { const x = cx + sd * W * 0.4; g += `<ellipse class="cp-glow b" cx="${r2(x)}" cy="${r2((plY0 + plY1) / 2)}" rx="${r2(W * 0.09)}" ry="${r2(plH * 0.48)}" fill="${A.rad([[0, "#7fb0ff", 0.32], [1, "#3c6cff", 0]], x, (plY0 + plY1) / 2, plH * 0.48, null, null, `translate(${r2(x)} ${r2((plY0 + plY1) / 2)}) scale(${r2(W * 0.09 / (plH * 0.48))} 1) translate(${r2(-x)} ${r2(-(plY0 + plY1) / 2)})`)}"/>`; });
    }
    A.add(`<g clip-path="${clipEnv}">${g}</g>`);
    // ---- front glass: getter flash, highlights, rim, print
    let f = "";
    const fy = yTop + rr * (T.glass === "dome" ? 0.62 : 0.78);
    let wave = `M${xl - 2} ${r2(fy)}`; for (let i = 1; i <= 8; i++) wave += ` Q${r2(xl + W * (i - 0.5) / 8)} ${r2(fy + (i % 2 ? 1 : -1) * 1.6 * s)} ${r2(xl + W * i / 8)} ${r2(fy + (i % 3 - 1) * 0.6 * s)}`;
    const flash = `M${xl - 2} ${yTop - 2} H${xr + 2} V${r2(fy)} ${wave.replace(/^M[^Q]+/, "").split(" Q").reverse().join(" Q").length ? "" : ""}`;
    // build the flash as the envelope top region down to a wavy edge
    let edge = []; for (let i = 0; i <= 16; i++) edge.push([xl + W * i / 16, fy + Math.sin(i * 1.7) * 1.4 * s + (i % 3) * 0.4 * s]);
    const flashD = `M${xl - 2} ${r2(yTop - 4)} H${xr + 2} V${r2(edge[16][1])} ${edge.slice().reverse().map((q) => `L${r2(q[0])} ${r2(q[1])}`).join(" ")} Z`;
    void flash;
    f += `<g clip-path="${clipEnv}"><path d="${flashD}" fill="${A.lin([[0, "#2a2d31"], [0.08, "#6b7178"], [0.16, "#eef1f3"], [0.22, "#aab1b8"], [0.3, "#4d5258"], [0.46, "#7d848b"], [0.6, "#c9ced3"], [0.7, "#8a9198"], [0.8, "#555a60"], [0.9, "#b4bac0"], [1, "#2a2d31"]], xl, 0, xr, 0)}" opacity=".96"/>`;
    f += `<path d="${flashD}" fill="${A.lin([[0, "#fff", 0.25], [0.35, "#fff", 0], [0.8, "#000", 0.12], [1, "#000", 0.4]], 0, yTop, 0, fy + 2 * s)}"/><path d="${pline(edge)}" fill="none" stroke="#1d2024" stroke-opacity=".55" stroke-width="${r2(Math.max(1.2, 0.35 * s))}"/>`;
    // soft haze below the flash edge
    const hz = edge.map((q) => [q[0], q[1] + 2.4 * s]);
    f += `<path d="M${edge.map((q) => `${r2(q[0])} ${r2(q[1] - 0.5)}`).join(" L")} ${hz.slice().reverse().map((q) => `L${r2(q[0])} ${r2(q[1])}`).join(" ")} Z" fill="#3a3e44" opacity=".22"/></g>`;
    // tip-off nipple on miniature tubes
    if (T.glass === "mini") f += `<path d="M${r2(cx - 1.4 * s)} ${r2(yTop + 0.5)} C${r2(cx - 1.2 * s)} ${r2(yTop - 2 * s)} ${r2(cx - 0.5 * s)} ${r2(yTop - 3.6 * s)} ${r2(cx)} ${r2(yTop - 3.8 * s)} C${r2(cx + 0.5 * s)} ${r2(yTop - 3.6 * s)} ${r2(cx + 1.2 * s)} ${r2(yTop - 2 * s)} ${r2(cx + 1.4 * s)} ${r2(yTop + 0.5)} Z" fill="${A.lin([[0, "#4b5056"], [0.4, "#c8ced3"], [1, "#2c2f33"]], cx - 1.4 * s, 0, cx + 1.4 * s, 0)}"/>`;
    // glass rim + highlights
    f += `<path d="${env}" fill="${A.lin([[0, "#d7e6ea", 0.42], [0.05, "#c5d6da", 0.12], [0.12, "#fff", 0], [0.88, "#fff", 0], [0.95, "#c5d6da", 0.14], [1, "#d7e6ea", 0.45]], xl, 0, xr, 0)}"/>`;
    f += `<g clip-path="${clipEnv}">`;
    f += `<rect x="${r2(xl + W * 0.12)}" y="${r2(yTop + rr * 0.35)}" width="${r2(W * 0.07)}" height="${r2(glassBot - yTop - rr * 0.6)}" rx="${r2(W * 0.035)}" fill="${A.lin([[0, "#fff", 0], [0.1, "#fff", 0.6], [0.85, "#fff", 0.45], [1, "#fff", 0]], 0, yTop + rr * 0.35, 0, glassBot)}"/>`;
    f += `<rect x="${r2(xl + W * 0.23)}" y="${r2(yTop + rr * 0.5)}" width="${r2(W * 0.025)}" height="${r2(glassBot - yTop - rr)}" fill="#fff" opacity=".18"/>`;
    f += `<rect x="${r2(xr - W * 0.13)}" y="${r2(yTop + rr * 0.6)}" width="${r2(W * 0.035)}" height="${r2(glassBot - yTop - rr)}" rx="${r2(W * 0.017)}" fill="#fff" opacity=".22"/>`;
    f += `<path d="M${r2(xl + W * 0.16)} ${r2(yTop + rr * 0.55)} Q${r2(cx - W * 0.1)} ${r2(yTop + rr * 0.04)} ${r2(cx + W * 0.12)} ${r2(yTop + rr * 0.08)}" stroke="#fff" stroke-opacity=".5" stroke-width="${r2(W * 0.035)}" stroke-linecap="round" fill="none"/>`;
    f += `</g><path d="${env}" fill="none" stroke="#e8f2f4" stroke-opacity=".35" stroke-width="1.4"/>`;
    // printed type
    const fz = Math.min(W * 0.16, 26);
    const room = T.base === "octal" ? 0 : (glassBot - 2 * s) - (micaB + 6);
    const ty = room > fz * 2.1 ? micaB + 6 + (room - fz * 1.6) / 2 + fz * 0.9 : plY0 + Math.max(fz * 1.4, plH * 0.22);
    f += `<g text-anchor="middle" fill="#f4f1ea" opacity=".82" font-weight="800" stroke="#1b1c1f" stroke-opacity=".38" stroke-width="${r2(Math.max(1, fz * 0.06))}" paint-order="stroke" stroke-linejoin="round"><text x="${cx}" y="${r2(ty)}" font-size="${r2(fz)}" letter-spacing="1">${esc(T.name)}</text>${T.alt ? `<text x="${cx}" y="${r2(ty + fz * 0.85)}" font-size="${r2(fz * 0.52)}" letter-spacing="2" font-weight="700">${esc(T.alt)}</text>` : ""}<path d="M${r2(cx - fz * 1.3)} ${r2(ty - fz * 1.05)} H${r2(cx + fz * 1.3)}" stroke="#f4f1ea" stroke-width="1.6" opacity=".7"/></g>`;
    // noval: pressed glass button at the bottom
    if (T.base === "noval") f += `<ellipse cx="${cx}" cy="${r2(yGb)}" rx="${r2(W / 2 * 0.96)}" ry="${r2(by * 0.9)}" fill="#cfe0e3" opacity=".18"/><path d="M${xl + 2} ${r2(yGb)} A${W / 2 - 2} ${by} 0 0 0 ${xr - 2} ${r2(yGb)}" fill="none" stroke="#fff" stroke-opacity=".3" stroke-width="1.6"/>`;
    A.add(f);
    // octal bakelite base
    if (T.base === "octal") {
      const bw = T.glass === "coke" ? W * 0.72 : W * 0.95, bx = cx - bw / 2, bb = by * (bw / W);
      let b = `<path d="M${r2(bx)} ${r2(yGb)} V${r2(yB1)} A${r2(bw / 2)} ${r2(bb)} 0 0 0 ${r2(bx + bw)} ${r2(yB1)} V${r2(yGb)} A${r2(bw / 2)} ${r2(bb)} 0 0 1 ${r2(bx)} ${r2(yGb)} Z" fill="${A.lin([[0, "#050506"], [0.16, "#2e2f33"], [0.26, "#5b5d62"], [0.34, "#26272a"], [0.7, "#141517"], [0.9, "#0a0a0b"], [1, "#030303"]], bx, 0, bx + bw, 0)}"/>`;
      b += `<path d="M${r2(bx)} ${r2(yGb + 3 * s)} A${r2(bw / 2)} ${r2(bb)} 0 0 0 ${r2(bx + bw)} ${r2(yGb + 3 * s)}" fill="none" stroke="#000" stroke-width="2.2" opacity=".7"/><path d="M${r2(bx)} ${r2(yGb + 3.5 * s)} A${r2(bw / 2)} ${r2(bb)} 0 0 0 ${r2(bx + bw)} ${r2(yGb + 3.5 * s)}" fill="none" stroke="#fff" stroke-width="1" opacity=".12"/>`;
      b += `<ellipse cx="${cx}" cy="${r2(yGb)}" rx="${r2(bw / 2)}" ry="${r2(bb)}" fill="#1a1b1e"/><ellipse cx="${cx}" cy="${r2(yGb)}" rx="${r2(W / 2 * 0.9)}" ry="${r2(by * 0.9)}" fill="#c8d8db" opacity=".12"/>`;
      b += `<text x="${cx}" y="${r2(yGb + (yB1 - yGb) * 0.62)}" text-anchor="middle" font-size="${r2(Math.min(14, 2.6 * s))}" font-weight="700" letter-spacing="2" fill="#d9d6cf" opacity=".55">${esc(T.name)}</text>`;
      A.add(b);
    }
    // front pins
    pins.filter((q) => q.dz >= 0).forEach((q) => A.add(pinSvg(q)));
    // pin tags in side columns
    const items = use.map(([pin, role, node]) => { const q = pins.find((z) => z.i === pin) || pins[0]; return Object.assign(mkTag(o, node, `PIN ${pin} · ${role}`), { x: q.x, y: q.y1 - 2, side: q.x < cx ? -1 : 1 }); });
    const L = items.filter((t) => t.side < 0), Rr = items.filter((t) => t.side > 0);
    const colY = Math.max(yTop + 40, glassBot - 120);
    if (L.length) tagsCol(A, L, -1, cx - W / 2 - 30, colY);
    if (Rr.length) tagsCol(A, Rr, 1, cx + W / 2 + 30, colY);
    const pinsTxt = use.map((u) => u[0]).join("-");
    return {
      A, name: `${T.name}${T.alt ? ` (${T.alt})` : ""} ${T.kind}${p.type === "triode" && T.inner === "dual" ? `, section ${T._sec}` : ""}`,
      pkg: `${T.base === "noval" ? "9-pin miniature (B9A)" : "Octal (K8A)"} · this ${p.type === "triode" ? "section" : "part"} on pins ${pinsTxt} · heater ${T.heat.join("-")}`,
      chips: [T.base === "noval" ? "Noval" : "Octal", heat ? "Heater on" : "Cold", vp != null && vk != null && !isRect ? `Vpk ${fmtV(vp - vk)}` : ""],
    };
  }

  /* =====================================================================
     MAGNETICS: transformers, chokes, toroids
     ===================================================================== */
  const WIRE_C = { blue: "#2b5fd1", red: "#c92a22", brown: "#7a4521", black: "#1d1d1f", green: "#2b8f43", yellow: "#e2b51c", white: "#e7e4dc", orange: "#e0701d", gray: "#8a8c90" };
  function clothWire(A, d, col) {
    return `<path d="${d}" fill="none" stroke="#000" stroke-opacity=".55" stroke-width="10.5" stroke-linecap="round"/><path d="${d}" fill="none" stroke="${col}" stroke-width="8" stroke-linecap="round"/><path d="${d}" fill="none" stroke="#fff" stroke-opacity=".28" stroke-width="2.4" stroke-linecap="round" transform="translate(-1.6 -1.2)"/><path d="${d}" fill="none" stroke="#000" stroke-opacity=".2" stroke-width="8" stroke-dasharray="1.2 2.2" stroke-linecap="butt"/>`;
  }
  function artMagnetic(p, o, kind) {
    // kind: "ot" | "pt" | "choke"
    const A = new Art(640, 440);
    const big = kind !== "choke";
    const Wc = big ? 76 : 56, Hc = big ? 64 : 48, Dc = big ? 46 : 34;
    const s = big ? 3.0 : 3.6, C = Cam(-0.55, 0.36, s, 318, big ? 216 : 214);
    const core = { c: "#46484d", amb: 0.5, kd: 0.6, ks: 0.32, pw: 16, env: 0.14 };
    const steel = { c: "#aeb3b9", amb: 0.4, kd: 0.6, ks: 0.8, pw: 30, env: 0.2 };
    const paper = { c: kind === "pt" ? "#1f2023" : "#d8c9a6", amb: 0.5, kd: 0.55, ks: 0.15, pw: 10 };
    A.shadowPath(poly(hull(rrect(Wc * 1.2, Dc * 1.5, 4).map(([x, z]) => C.p([x + 3, -Hc / 2, z - 2])))), 0.5, 12);
    // core (lamination stack along z)
    const pr = prism(C, [0, -Hc / 2, 0], [0, 1, 0], [1, 0, 0], [0, 0, 1], [[-Wc / 2, -Dc / 2], [Wc / 2, -Dc / 2], [Wc / 2, Dc / 2], [-Wc / 2, Dc / 2]], Hc, core);
    let sv = pr.svg;
    // lamination lines on the side faces (parallel to xy, stacked along z)
    for (let z = -Dc / 2 + 1.5; z < Dc / 2; z += 1.6) {
      const a = C.p([Wc / 2, -Hc / 2, z]), b = C.p([Wc / 2, Hc / 2, z]), c = C.p([-Wc / 2, Hc / 2, z]);
      sv += `<path d="M${r2(a[0])} ${r2(a[1])} L${r2(b[0])} ${r2(b[1])} L${r2(c[0])} ${r2(c[1])}" fill="none" stroke="#000" stroke-opacity=".28" stroke-width="1"/><path d="M${r2(a[0] + 1)} ${r2(a[1])} L${r2(b[0] + 1)} ${r2(b[1] + 0.6)}" fill="none" stroke="#fff" stroke-opacity=".06" stroke-width="1"/>`;
    }
    // E-I seam on the front face
    const sy = Hc / 2 - Hc * 0.2;
    sv += `<path d="M${C.p([-Wc / 2, sy, Dc / 2]).map(r2).join(" ")} L${C.p([Wc / 2, sy, Dc / 2]).map(r2).join(" ")}" stroke="#000" stroke-opacity=".45" stroke-width="1.4"/>`;
    // coil protruding from the front face
    const cw = Wc * 0.5, ch = Hc * 0.62, cz = Dc / 2, cd = big ? 9 : 7;
    const coil = prism(C, [0, -ch / 2 - Hc * 0.04, cz - 0.5], [0, 0, 1], [1, 0, 0], [0, 1, 0], rrect(cw, ch, 4, 5), cd, paper);
    sv += coil.svg;
    const lab = kind === "pt" ? ["POWER", o.label || "PRIMARY 120 V"] : kind === "choke" ? ["CHOKE", p.text || ""] : ["OUTPUT", "PUSH-PULL"];
    sv += onPlane(C, [0, -Hc * 0.04, cz + cd], [1, 0, 0], [0, 1, 0], `<g text-anchor="middle" font-weight="800" fill="${kind === "pt" ? "#d9d6cf" : "#2a2418"}" opacity=".85"><text x="0" y="-${big ? 34 : 26}" font-size="${big ? 34 : 30}" letter-spacing="3">${esc(lab[0])}</text><text x="0" y="${big ? 6 : 8}" font-size="${big ? 26 : 26}" letter-spacing="1.5">${esc(lab[1])}</text><path d="M-${big ? 120 : 90} ${big ? 30 : 30} H${big ? 120 : 90}" stroke="currentColor" stroke-width="3" opacity=".6"/></g>`);
    // tape band across the coil
    sv += `<path d="${poly([[-cw / 2, -Hc * 0.04 + ch * 0.28, cz + cd + 0.05], [cw / 2, -Hc * 0.04 + ch * 0.28, cz + cd + 0.05], [cw / 2, -Hc * 0.04 + ch * 0.36, cz + cd + 0.05], [-cw / 2, -Hc * 0.04 + ch * 0.36, cz + cd + 0.05]].map(C.p))}" fill="#000" opacity="${kind === "pt" ? 0 : 0.25}"/>`;
    // mounting channel brackets along the top, with feet
    // channel frame clamping the stack: straps over the top edges, feet with mounting holes at the base
    [-1, 1].forEach((sd) => {
      const z0 = sd * (Dc / 2 - 3), zl = sd > 0 ? Dc / 2 : -Dc / 2 - 1.2;
      sv += prism(C, [0, Hc / 2 - 9, zl], [0, 0, 1], [1, 0, 0], [0, 1, 0], [[-Wc / 2 - 0.6, 0], [Wc / 2 + 0.6, 0], [Wc / 2 + 0.6, 10.4], [-Wc / 2 - 0.6, 10.4]], 1.2, steel).svg;
      sv += prism(C, [0, Hc / 2, z0], [0, 1, 0], [1, 0, 0], [0, 0, 1], [[-Wc / 2 - 0.6, -3.2], [Wc / 2 + 0.6, -3.2], [Wc / 2 + 0.6, 3.2], [-Wc / 2 - 0.6, 3.2]], 1.4, steel).svg;
    });
    [-1, 1].forEach((sd) => {
      const zl = sd > 0 ? Dc / 2 : -Dc / 2 - 1.2;
      sv += prism(C, [0, -Hc / 2, zl], [0, 0, 1], [1, 0, 0], [0, 1, 0], [[-Wc / 2 - 0.6, 0], [Wc / 2 + 0.6, 0], [Wc / 2 + 0.6, 9], [-Wc / 2 - 0.6, 9]], 1.2, steel).svg;
    });
    [-1, 1].forEach((sx) => {
      sv += prism(C, [sx * (Wc / 2 + 6), -Hc / 2, 0], [0, 1, 0], [1, 0, 0], [0, 0, 1], [[-6, -Dc / 2 - 1.2], [6, -Dc / 2 - 1.2], [6, Dc / 2 + 1.2], [-6, Dc / 2 + 1.2]], 1.4, steel).svg;
      [-1, 1].forEach((sz) => { const h = C.p([sx * (Wc / 2 + 7), -Hc / 2 + 1.42, sz * Dc * 0.3]); sv += `<ellipse cx="${r2(h[0])}" cy="${r2(h[1])}" rx="${r2(2 * s)}" ry="${r2(2 * s * 0.45)}" fill="#141517"/>`; });
    });
    A.add(sv);
    // leads: colour code
    let leads = [];
    const nodes = p.nodes || [];
    if (kind === "choke" || nodes.length === 2) leads = [[nodes[0], "red", "IN"], [nodes[1], "black", "OUT"]];
    else {
      // windings: consecutive node pairs
      const w = []; for (let i = 0; i + 1 < nodes.length; i += 2) w.push([nodes[i], nodes[i + 1]]);
      const seen = new Map();
      w.forEach((pr2, wi) => pr2.forEach((n, k) => {
        if (seen.has(n)) return;
        let col, role;
        if (wi < w.length - 1 || w.length === 1) {
          const shared = w.some((q, j) => j !== wi && j < w.length - 1 && q.includes(n));
          col = shared ? "red" : seen.size === 0 ? "blue" : "brown"; role = shared ? "CT (B+)" : wi === 0 ? "PLATE A" : "PLATE B";
        } else { col = isGnd(n) ? "black" : "green"; role = isGnd(n) ? "COM" : "SPEAKER"; }
        if (w.length === 1) { col = k ? "brown" : "blue"; role = k ? "END" : "START"; }
        if (kind === "pt") {
          const shared = w.some((q, j) => j !== wi && q.includes(n));
          if (wi === 0 && w.length > 1) { col = k ? "white" : "black"; role = k ? "MAINS N" : "MAINS L"; }
          else { col = shared ? "yellow" : "red"; role = shared ? "HV CT" : "HV"; }
        }
        seen.set(n, 1); leads.push([n, col, role]);
      }));
    }
    const base = C.p([0, -Hc * 0.04 - ch / 2 + 2, cz + cd]);
    const n = leads.length, yEnd = 372;
    const ends = leads.map((L, i) => {
      const x0 = base[0] + (i - (n - 1) / 2) * 11, x1 = 320 + (i - (n - 1) / 2) * Math.min(110, 560 / Math.max(1, n));
      const d = `M${r2(x0)} ${r2(base[1])} C${r2(x0)} ${r2(base[1] + 60)} ${r2(x1)} ${r2(yEnd - 70)} ${r2(x1)} ${r2(yEnd)}`;
      A.add(clothWire(A, d, WIRE_C[L[1]]));
      return [x1, yEnd];
    });
    tagsRow(A, leads.map((L, i) => mkTag(o, L[0], leads.length > 3 ? L[2] : `${L[1].toUpperCase()} · ${L[2]}`, { x: ends[i][0], y: ends[i][1] })), yEnd + 14);
    if (kind === "choke") return { A, name: `${p.text || fmtEng(+p.value, "H")} filter choke`, pkg: "Open-frame EI laminated core", chips: ["Choke", p.text || "", ""] };
    if (kind === "pt") return { A, name: nodes.length > 2 ? "Power transformer" : "Power transformer secondary", pkg: `EI core · ${nodes.length > 2 ? "mains primary, centre-tapped high-voltage secondary" : "high-voltage winding"}`, chips: ["Mains", "AC"] };
    const turns = p.turns ? p.turns.map((t) => +t) : null;
    const ratio = turns && turns.length >= 2 ? turns[0] / turns[turns.length - 1] : null;
    return { A, name: "Output transformer", pkg: `Open-frame EI core · ${leads.length} leads${ratio ? ` · turns ratio ${ratio.toFixed(1)} : 1 (≈ ${Math.round(ratio * ratio * 8 / 100) * 100} Ω : 8 Ω per pair)` : ""}`, chips: ["Output", ratio ? `${ratio.toFixed(1)} : 1` : "", p.lm ? `Lm ${p.lm} H` : ""] };
  }
  function artToroid(p, o) {
    const A = new Art(640, 400);
    const cx = 320, cy = 150, R1 = 130, R0 = 58, k = 0.62, th = 24;
    A.shadow(cx + 10, cy + R1 * k + th + 16, R1 * 0.95, 16, 0.45, 12);
    const yF = 318;
    A.add(wire(`M${cx - 30} ${cy + R1 * k * 0.6} C${cx - 40} ${cy + R1 * k + 40} ${cx - 80} ${yF - 40} ${cx - 90} ${yF}`, 5, "#d38a52", 0.5), wire(`M${cx + 30} ${cy + R1 * k * 0.6} C${cx + 40} ${cy + R1 * k + 40} ${cx + 80} ${yF - 40} ${cx + 90} ${yF}`, 5, "#d38a52", 0.5));
    let s = `<ellipse cx="${cx}" cy="${cy + th}" rx="${R1}" ry="${R1 * k}" fill="#3a2414"/>`;
    s += `<path d="M${cx - R1} ${cy} V${cy + th} A${R1} ${R1 * k} 0 0 0 ${cx + R1} ${cy + th} V${cy} Z" fill="${A.lin([[0, "#4a2a12"], [0.3, "#c47a3c"], [0.5, "#8a4f22"], [1, "#2d1a0c"]], cx - R1, 0, cx + R1, 0)}"/>`;
    s += `<path d="M${cx - R1} ${cy} A${R1} ${R1 * k} 0 1 1 ${cx + R1} ${cy} A${R1} ${R1 * k} 0 1 1 ${cx - R1} ${cy} Z M${cx - R0} ${cy} A${R0} ${R0 * k} 0 1 0 ${cx + R0} ${cy} A${R0} ${R0 * k} 0 1 0 ${cx - R0} ${cy} Z" fill-rule="evenodd" fill="#2c6b46"/>`;
    // windings
    const N = 46;
    for (let i = 0; i < N; i++) {
      const a = i / N * 2 * Math.PI, ca = Math.cos(a), sa = Math.sin(a);
      const x0 = cx + R0 * ca, y0 = cy + R0 * k * sa, x1 = cx + R1 * ca, y1 = cy + R1 * k * sa;
      const lightness = 0.5 + 0.5 * (-ca * 0.6 - sa * 0.4);
      s += `<path d="M${r2(x0)} ${r2(y0)} L${r2(x1)} ${r2(y1)}${sa > 0 ? ` L${r2(x1)} ${r2(y1 + th)}` : ""}" stroke="${mix("#7a3f16", "#f7c08a", clamp(lightness, 0, 1))}" stroke-width="7.5" stroke-linecap="round" fill="none"/>`;
      s += `<path d="M${r2(x0)} ${r2(y0)} L${r2(x1)} ${r2(y1)}" stroke="#fff" stroke-opacity=".25" stroke-width="1.6" transform="translate(-1 -1)"/>`;
    }
    s += `<ellipse cx="${cx}" cy="${cy}" rx="${R0 - 3}" ry="${(R0 - 3) * k}" fill="#140c06" opacity=".8"/>`;
    A.add(s);
    tagsRow(A, [mkTag(o, p.nodes[0], "START", { x: cx - 90, y: yF }), mkTag(o, p.nodes[1], "FINISH", { x: cx + 90, y: yF })], yF + 14);
    return { A, name: `${p.text || fmtEng(+p.value, "H")} inductor`, pkg: "Toroidal core, enamelled copper winding", chips: ["Inductor", p.text || ""] };
  }

  /* =====================================================================
     SWITCHES, SOURCES, SPEAKER, LDR, GENERIC
     ===================================================================== */
  function artSwitch(p, o) {
    const A = new Art(640, 430);
    const closed = !!p.closed;
    const C = fitCam(-0.5, 0.3, boxPts(-8, 8, -6.5, 29, -4, 4), [200, 30, 440, 340], 16), s = C.s;
    const chrome = { c: "#d2d6db", amb: 0.32, kd: 0.55, ks: 1, pw: 60, env: 0.35 };
    const steel = { c: "#b4b9bf", amb: 0.38, kd: 0.6, ks: 0.85, pw: 34, env: 0.22 };
    const phen = { c: "#3a2c22", amb: 0.5, kd: 0.55, ks: 0.15, pw: 10 };
    A.shadowPath(poly(hull(rrect(15, 11, 2).map(([x, z]) => C.p([x + 1, -8, z - 1])))), 0.45, 10);
    const lx = [-4.7, 0, 4.7], ends = [];
    lx.forEach((x) => { const pts = [[x - 1, -1.5], [x + 1, -1.5], [x + 1, -6.5], [x - 1, -6.5]].map(([a, b]) => C.p([a, b, 0])); A.add(`<path d="${poly(pts)}" fill="${A.lin([[0, "#8e949a"], [0.5, "#e3e6e9"], [1, "#8e949a"]], pts[0][0], 0, pts[1][0], 0)}" stroke="#5a5f65" stroke-width=".8"/>`); const h = C.p([x, -4.6, 0.01]); A.add(`<ellipse cx="${r2(h[0])}" cy="${r2(h[1])}" rx="${r2(0.5 * s)}" ry="${r2(0.55 * s)}" fill="#26282b"/>`); ends.push(C.p([x, -6.5, 0])); });
    let sv = prism(C, [0, -1.6, 0], [0, 1, 0], [1, 0, 0], [0, 0, 1], rrect(13, 8, 0.6, 3), 1.6, phen).svg;
    sv += prism(C, [0, 0, 0], [0, 1, 0], [1, 0, 0], [0, 0, 1], rrect(13, 8, 0.8, 3), 9, steel).svg;
    sv += cylSide(A, C, [0, 9, 0], [0, 1, 0], 8, 3, { c: "#cfc8b6", amb: 0.38, kd: 0.6, ks: 0.9, pw: 40, env: 0.2 });
    for (let y = 9.4; y < 16.6; y += 0.6) sv += `<path d="${rimFront(C, [0, y, 0], [0, 1, 0], 3.02)}" fill="none" stroke="#000" stroke-opacity=".25" stroke-width="1.4"/>`;
    const hex = []; for (let i = 0; i < 6; i++) { const a = (30 + 60 * i) * DEG; hex.push([5 * Math.cos(a), 5 * Math.sin(a)]); }
    sv += prism(C, [0, 10.5, 0], [0, 1, 0], [1, 0, 0], [0, 0, 1], hex, 2.2, steel).svg;
    sv += disc(C, [0, 17, 0], [0, 1, 0], 3, shadeN(steel, C.rot([0, 1, 0])));
    const ang = (closed ? 1 : -1) * 20 * DEG, ax = [Math.sin(ang), Math.cos(ang), 0];
    sv += coneSide(A, C, [0, 16.6, 0], ax, 11, 1.2, 1.7, chrome);
    const tip = add3([0, 16.6, 0], mul3(ax, 11));
    const tp = C.p(tip);
    sv += `<circle cx="${r2(tp[0])}" cy="${r2(tp[1])}" r="${r2(1.75 * s)}" fill="${A.rad([[0, "#ffffff"], [0.35, "#dde1e5"], [0.8, "#8b9198"], [1, "#5b6067"]], tp[0] - 0.6 * s, tp[1] - 0.6 * s, 2.2 * s)}"/>`;
    const onL = C.p([7, 21, 0]), offL = C.p([-7, 21, 0]);
    sv += `<text class="cp-ann${closed ? " on" : ""}" x="${r2(onL[0] + 10)}" y="${r2(onL[1])}" text-anchor="start">${esc(o.ctlLabel ? "ON" : "CLOSED")}</text><text class="cp-ann${closed ? "" : " on"}" x="${r2(offL[0] - 10)}" y="${r2(offL[1])}" text-anchor="end">${esc(o.ctlLabel ? "OFF" : "OPEN")}</text>`;
    A.add(sv);
    tagsRow(A, [mkTag(o, p.nodes[0], "COMMON", { x: ends[1][0], y: ends[1][1] }), mkTag(o, p.nodes[1], closed ? "THROW · MADE" : "THROW · OPEN", { x: ends[closed ? 2 : 0][0], y: ends[closed ? 2 : 0][1] })], ends[1][1] + 24);
    return { A, name: `${closed ? "Closed" : "Open"} toggle switch${o.ctlLabel ? ` (${o.ctlLabel})` : ""}`, pkg: "Miniature bat-handle toggle · SPDT body", chips: ["Toggle", closed ? "Closed" : "Open"] };
  }

  function artBattery(p, o) {
    const A = new Art(640, 420);
    const C = fitCam(-0.45, 0.36, boxPts(-13.5, 13.5, 0, 52, -9, 9), [200, 76, 440, 410], 8), s = C.s;
    const body = { c: "#1d1e22", amb: 0.48, kd: 0.6, ks: 0.4, pw: 20, env: 0.12 };
    const steel = { c: "#c9cdd2", amb: 0.4, kd: 0.6, ks: 0.9, pw: 40, env: 0.25 };
    A.shadowPath(poly(hull(rrect(29, 20, 3).map(([x, z]) => C.p([x + 1.2, 0, z - 1])))), 0.5, 11);
    const pr = prism(C, [0, 0, 0], [0, 1, 0], [1, 0, 0], [0, 0, 1], rrect(26.5, 17.5, 2.2, 4), 48.5, body);
    let sv = pr.svg;
    // orange top band, wrapped label
    const band = prism(C, [0, 36, 0], [0, 1, 0], [1, 0, 0], [0, 0, 1], rrect(26.6, 17.6, 2.2, 4), 10, { c: "#e0781f", amb: 0.5, kd: 0.6, ks: 0.4, pw: 20 });
    sv += band.svg.replace(/<path d="[^"]*" fill="[^"]*"\/>/, "");
    sv += onPlane(C, [0, 22, 8.76], [1, 0, 0], [0, 1, 0], `<g text-anchor="middle" font-weight="800"><text x="0" y="40" font-size="120" fill="#f2f2ee" letter-spacing="-2">${esc(`${+Math.abs(+p.value || 9).toPrecision(2)}V`)}</text><text x="0" y="96" font-size="26" fill="#bfc2c7" letter-spacing="6">ALKALINE</text><text x="0" y="128" font-size="18" fill="#8d9096" letter-spacing="4">6LR61 · ${esc(fmtV(+p.value))}</text></g>`, 0.12);
    // terminals on top
    const tp = [6.35, -6.35];
    const tposC = C.p([tp[0], 48.5 + 3.5, 0]), tnegC = C.p([tp[1], 48.5 + 3.5, 0]);
    sv += cylSide(A, C, [tp[0], 48.5, 0], [0, 1, 0], 3, 2.6, steel) + disc(C, [tp[0], 51.5, 0], [0, 1, 0], 2.6, shadeN(steel, C.rot([0, 1, 0])));
    const hx = []; for (let i = 0; i < 6; i++) { const a = (60 * i) * DEG; hx.push([4 * Math.cos(a), 4 * Math.sin(a)]); }
    sv += prism(C, [tp[1], 48.5, 0], [0, 1, 0], [1, 0, 0], [0, 0, 1], hx, 3, steel).svg + `<path d="${discD(C, [tp[1], 51.52, 0], [0, 1, 0], 2.2)}" fill="#2a2c30"/>`;
    sv += onPlane(C, [0, 48.52, 0], [1, 0, 0], [0, 0, -1], `<g font-weight="800" fill="#f0efe9" text-anchor="middle"><text x="${6.35 * 10}" y="70" font-size="40">+</text><text x="${-6.35 * 10}" y="70" font-size="40">−</text></g>`);
    A.add(sv);
    const pos = isGnd(p.nodes[1]) ? 0 : 1;
    tagsRow(A, [mkTag(o, p.nodes[pos], "+ TERMINAL", { x: tposC[0], y: tposC[1] - 4 }), mkTag(o, p.nodes[1 - pos], "− TERMINAL", { x: tnegC[0], y: tnegC[1] - 4 })], 60, -1);
    return { A, name: `${p.text || fmtV(+p.value)} supply (battery)`, pkg: `${Math.abs(+p.value - 9) < 0.6 ? "PP3 / 6LR61 · " : ""}ideal DC source in the solve`, chips: ["DC", p.text || fmtV(+p.value)] };
  }
  // low-voltage reference rails (e.g. the TS808's 4.5 V Vref): drawn as the electrolytic that holds the rail up
  function artRail(p, o) {
    const m = String(p.label || "").match(/(\d+(?:\.\d+)?)\s*[µu]F/);
    const F = m ? +m[1] * 1e-6 : 47e-6;
    const r = capElecRadial({ ref: p.ref, type: "C", value: F, nodes: p.nodes }, o, F, null, Math.abs(+p.value || 0));
    const v = (p.text || fmtV(+p.value)).replace("-", "−");
    return Object.assign(r, { name: `${v} reference rail`, pkg: `Ideal DC node in the solve · decoupled by ${capText(F)} on the real board`, chips: ["DC rail", v, capText(F)] });
  }
  function artCan(p, o) {
    // high-voltage rails and bias supplies: a twist-lock filter can standing on the chassis
    const A = new Art(640, 430);
    const r = 17.5, h = 52;
    const C = fitCam(-0.42, 0.34, cylPts(r, 0, h + 3), [200, 14, 440, 330], 6), s = C.s;
    const al = { c: "#c4c9cf", amb: 0.38, kd: 0.6, ks: 0.85, pw: 30, env: 0.25 };
    const v = +p.value || 0, neg = v < 0;
    A.shadowPath(discD(C, [2, 0, -1], [0, 1, 0], r * 1.25), 0.5, 11);
    let sv = cylSide(A, C, [0, 0, 0], [0, 1, 0], h, r, al);
    sv += `<path d="${rimFront(C, [0, 3, 0], [0, 1, 0], r * 1.002)}" fill="none" stroke="#000" stroke-opacity=".35" stroke-width="2"/>`;
    sv += cylSide(A, C, [0, h, 0], [0, 1, 0], 2.5, r * 0.97, al) + disc(C, [0, h + 2.5, 0], [0, 1, 0], r * 0.97, A.lin([[0, "#f3f5f7"], [0.5, "#aab0b7"], [1, "#7c838b"]], ...C.p([-r, h, -r]), ...C.p([r, h, r])));
    sv += disc(C, [0, h + 2.55, 0], [0, 1, 0], r * 0.3, "#1e1f22");
    // stamped label wrapping the front
    let tf = 0, best = -9; const [u, vv] = basis([0, 1, 0]);
    for (let i = 0; i < 360; i++) { const t = i * DEG, n = add3(mul3(u, Math.cos(t)), mul3(vv, Math.sin(t))); const z = C.rot(n)[2]; if (z > best) { best = z; tf = t; } }
    const nF = add3(mul3(u, Math.cos(tf)), mul3(vv, Math.sin(tf))), tF = norm3(cross3([0, 1, 0], nF));
    sv += onPlane(C, add3(mul3(nF, r * 1.001), [0, h * 0.58, 0]), tF, [0, 1, 0], `<g text-anchor="middle" font-weight="800" fill="#24262a"><text x="0" y="-44" font-size="40" letter-spacing="3">${neg ? "BIAS" : "B+ SUPPLY"}</text><text x="0" y="26" font-size="78" letter-spacing="1">${esc((p.text || fmtV(v)).replace("-", "−"))}</text><text x="0" y="70" font-size="28" letter-spacing="3" opacity=".75">${esc(String(p.label || "FILTERED DC").toUpperCase().slice(0, 18))}</text></g>`);
    A.add(sv);
    const lug = C.p([0, 0, r]);
    const ends = [[lug[0] - 70, 368], [lug[0] + 70, 368]];
    A.add(wire(`M${r2(lug[0] - 10)} ${r2(lug[1] - 4)} C${r2(lug[0] - 20)} ${r2(lug[1] + 20)} ${ends[0][0]} ${ends[0][1] - 30} ${ends[0][0]} ${ends[0][1]}`, 5), wire(`M${r2(lug[0] + 10)} ${r2(lug[1] - 4)} C${r2(lug[0] + 20)} ${r2(lug[1] + 20)} ${ends[1][0]} ${ends[1][1] - 30} ${ends[1][0]} ${ends[1][1]}`, 5));
    tagsRow(A, [mkTag(o, p.nodes[0], neg ? "− RAIL" : "+ RAIL", { x: ends[0][0], y: ends[0][1] }), mkTag(o, p.nodes[1], "RETURN", { x: ends[1][0], y: ends[1][1] })], 376);
    return { A, name: `${p.label || (neg ? "Bias supply" : "High-voltage supply")} (${(p.text || fmtV(v)).replace("-", "−")})`, pkg: "Filtered DC rail · ideal source in the solve", chips: ["DC rail", (p.text || fmtV(v)).replace("-", "−")] };
  }

  function artSpeaker(p, o) {
    const A = new Art(640, 470);
    const txt = [p.model, p.of, p.label, p.ref, p.package, p.family, o.mi.note, o.mi.kind].filter(Boolean).join(" ").toLowerCase();
    const ten = /(^|[^0-9])10(["″”]|in| ?inch|\b)|p10|c10|10r/.test(txt), brit = /brit|celest|greenback|g12|v30|vintage ?30|\buk\b|alnico ?blue|h30|creamback/.test(txt);
    const Rf = ten ? 127 : 152, Rc = Rf * 0.78, Rd = ten ? 24 : 30, depth = ten ? 48 : 58;
    const pts = []; for (let i = 0; i < 24; i++) { const a = i / 24 * 2 * Math.PI; pts.push([Rf * Math.cos(a), Rf * Math.sin(a), 0], [56 * Math.cos(a), 56 * Math.sin(a), -118]); }
    const C = fitCam(-0.8, 0.12, pts, [96, 12, 544, 392], 2), s = C.s;
    const frame = brit ? { c: "#2e2f33", amb: 0.45, kd: 0.6, ks: 0.5, pw: 22, env: 0.12 } : { c: "#8a8f95", amb: 0.42, kd: 0.6, ks: 0.55, pw: 20, env: 0.15 };
    const cone = { c: brit ? "#5d5852" : "#35312c", amb: 0.55, kd: 0.6, ks: 0.06, pw: 8 };
    const mag = { c: brit ? "#2f6a3c" : "#2c2d30", amb: 0.45, kd: 0.6, ks: brit ? 0.45 : 0.3, pw: 18, env: 0.1 };
    A.shadow(330, 400, 190, 22, 0.45, 16);
    let sv = "";
    // magnet, back plate, basket legs
    sv += cylSide(A, C, [0, 0, -118], [0, 0, 1], 26, 52, mag) + disc(C, [0, 0, -118], [0, 0, -1], 52, shadeN(mag, C.rot([0, 0, -1])));
    sv += cylSide(A, C, [0, 0, -92], [0, 0, 1], 7, 58, frame);
    const ohms = /_8$/.test(String(p.model || "")) ? "8Ω" : "16Ω";
    if (brit) sv += onPlane(C, [0, -52.2, -105], [0, 0, 1], [1, 0, 0], `<text x="0" y="8" text-anchor="middle" font-size="60" font-weight="800" fill="#d9e6d6" opacity=".7" letter-spacing="4">${ohms}</text>`, 0.12);
    for (let i = 0; i < 4; i++) {
      const a = (45 + 90 * i) * DEG, ca = Math.cos(a), sa = Math.sin(a), w0 = 16, w1 = 10;
      const P = [[(Rf - 8) * ca - w0 * sa, (Rf - 8) * sa + w0 * ca, -4], [(Rf - 8) * ca + w0 * sa, (Rf - 8) * sa - w0 * ca, -4], [58 * ca + w1 * sa, 58 * sa - w1 * ca, -86], [58 * ca - w1 * sa, 58 * sa + w1 * ca, -86]];
      const c = shadeN(frame, C.rot(norm3([ca, sa, 0.9])));
      sv += `<path d="${poly(P.map(C.p))}" fill="${c}" stroke="${shade(c, -0.4)}" stroke-width="1.2"/>`;
    }
    sv += cylSide(A, C, [0, 0, -3], [0, 0, 1], 3, Rf, frame);
    // back of the cone (visible between the basket legs)
    sv += coneSide(A, C, [0, 0, -depth], [0, 0, 1], depth - 4, Rd + 4, Rc, { c: shade(cone.c, -0.15), amb: 0.5, kd: 0.5, ks: 0.04, pw: 6 });
    // cone interior
    const z1 = -6, z2 = -depth, N = 120;
    for (let i = 0; i < N; i++) {
      const a0 = i / N * 2 * Math.PI, a1 = (i + 1.08) / N * 2 * Math.PI, am = (a0 + a1) / 2;
      const n = norm3([-(z1 - z2) * Math.cos(am), -(z1 - z2) * Math.sin(am), Rc - Rd]);
      const q = [[Rc * Math.cos(a0), Rc * Math.sin(a0), z1], [Rc * Math.cos(a1), Rc * Math.sin(a1), z1], [Rd * Math.cos(a1), Rd * Math.sin(a1), z2], [Rd * Math.cos(a0), Rd * Math.sin(a0), z2]];
      const c = shadeN(cone, C.rot(n));
      sv += `<path d="${poly(q.map(C.p))}" fill="${c}" stroke="${c}" stroke-width=".9"/>`;
    }
    // ribs and paper fibre rings
    [0.18, 0.34, 0.52, 0.7].forEach((f, k) => { const rr = Rc + (Rd - Rc) * f, zz = z1 + (z2 - z1) * f; sv += `<path d="${discD(C, [0, 0, zz], [0, 0, 1], rr, 140)}" fill="none" stroke="#000" stroke-opacity="${k % 2 ? 0.16 : 0.26}" stroke-width="${k % 2 ? 1.4 : 2.4}"/><path d="${discD(C, [0, 0, zz + 0.6], [0, 0, 1], rr + 1.4, 140)}" fill="none" stroke="#fff" stroke-opacity=".07" stroke-width="1.2"/>`; });
    // voice-coil former + dust cap dome
    const dz = z2 + 1;
    sv += `<path d="${discD(C, [0, 0, dz], [0, 0, 1], Rd + 2)}" fill="#1a1816"/>`;
    const capC = brit ? "#7a756e" : "#24211f", dc = C.p([0, 0, dz + 9]);
    sv += `<path d="${discD(C, [0, 0, dz + 0.5], [0, 0, 1], Rd)}" fill="${A.rad([[0, shade(capC, 0.35)], [0.55, capC], [1, shade(capC, -0.45)]], dc[0] - Rd * s * 0.35, dc[1] - Rd * s * 0.4, Rd * s * 1.5)}"/>`;
    // surround: corrugated edge roll
    for (let k = 0; k < 5; k++) { const rr = Rc + 2 + k * ((Rf - 22 - Rc) / 4); sv += `<path d="${discD(C, [0, 0, -4 + (k % 2) * 1.2], [0, 0, 1], rr, 160)}" fill="none" stroke="${k % 2 ? shade(cone.c, 0.25) : shade(cone.c, -0.45)}" stroke-width="3"/>`; }
    // frame: flange + gasket + mounting holes
    const ann = (r0, r1, z, fill) => `<path d="${discD(C, [0, 0, z], [0, 0, 1], r1, 160)} ${discD(C, [0, 0, z], [0, 0, 1], r0, 160)}" fill-rule="evenodd" fill="${fill}"/>`;
    const fc = shadeN(frame, C.rot([0, 0, 1]));
    sv += ann(Rf - 22, Rf, 0, A.lin([[0, shade(fc, 0.3)], [0.45, fc], [1, shade(fc, -0.35)]], ...C.p([-Rf, Rf, 0]), ...C.p([Rf, -Rf, 0])));
    for (let i = 0; i < 8; i++) { const a = (22.5 + 45 * i) * DEG; sv += `<path d="${discD(C, [(Rf - 9) * Math.cos(a), (Rf - 9) * Math.sin(a), 0.1], [0, 0, 1], 3.6, 24)}" fill="#111214"/>`; }
    sv += ann(Rf - 26, Rf - 13, 1.5, brit ? "#1d1d1f" : "#2a2622");
    // terminal strip on the frame edge
    const tA = (-38) * DEG, tB = (-26) * DEG;
    const t1 = C.p([(Rf + 1) * Math.cos(tA), (Rf + 1) * Math.sin(tA), -6]), t2 = C.p([(Rf + 1) * Math.cos(tB), (Rf + 1) * Math.sin(tB), -6]);
    sv += `<rect x="${r2(t1[0] - 6)}" y="${r2(t1[1] - 6)}" width="12" height="20" rx="2" fill="#d8bf86" stroke="#6d5a2e"/><rect x="${r2(t2[0] - 6)}" y="${r2(t2[1] - 6)}" width="12" height="20" rx="2" fill="#d8bf86" stroke="#6d5a2e"/><circle cx="${r2(t2[0])}" cy="${r2(t2[1] - 12)}" r="4.5" fill="#d22a22"/>`;
    A.add(sv);
    const nodes = p.nodes || [];
    const size = ten ? '10"' : '12"';
    if (p.type === "speaker") {
      // the electro-mechanical device (docs/CIRCUIT_ENGINE.md §10.21): its third pin is the cone's velocity (m/s)
      tagsRow(A, [mkTag(o, nodes[0], "+ (RED DOT)", { x: t2[0], y: t2[1] + 14 }), mkTag(o, nodes[1] != null ? nodes[1] : "0", "−", { x: t1[0], y: t1[1] + 14 }),
        mkTag(o, nodes[2], "CONE m/s", { x: dc[0], y: dc[1] })], 412);
      const CARD = { CELESTION_G12M25_8: "Celestion G12M-25 Greenback 8 Ω", CELESTION_G12M25_16: "Celestion G12M-25 Greenback 16 Ω", CELESTION_V30_16: "Celestion Vintage 30 16 Ω",
        JENSEN_P12Q_8: "Jensen P12Q 8 Ω", JENSEN_C12N_8: "Jensen C12N 8 Ω", EMINENCE_LEGEND1258_8: "Eminence Legend 1258 8 Ω" };
      const ns = Math.max(1, +p.series || 1), np = Math.max(1, +p.parallel || 1), n = ns * np;
      return { A, name: `${CARD[p.model] || `${size} speaker${p.model ? ` (${p.model})` : ""}`}${n > 1 ? ` × ${n}` : ""}`,
        pkg: `Electro-mechanical model: Bl(x), Kms(x), Le(x) and voice-coil heating; the cone's velocity drives the cab${n > 1 ? ` · ${n} drivers as one (${ns} in series × ${np} in parallel)` : ""}`,
        chips: [size, brit ? "British" : "American", "Nonlinear", n > 1 ? `${ns}s${np}p` : ""] };
    }
    tagsRow(A, [mkTag(o, nodes[0], "+ (RED DOT)", { x: t2[0], y: t2[1] + 14 }), mkTag(o, nodes[1] != null ? nodes[1] : "0", "−", { x: t1[0], y: t1[1] + 14 })], 412);
    return { A, name: `${size} ${brit ? "British" : "American"} guitar speaker (load)`, pkg: `${brit ? "Pressed-steel black basket, ceramic magnet" : "Hammertone pressed-steel basket, paper cone"} · electrical load on the OT secondary`, chips: [size, brit ? "British" : "American", "Load"] };
  }

  function artSpring(p, o) {
    const A = new Art(640, 400);
    const Lt = 425, Wt = 110, Ht = 33;
    const C = fitCam(-0.42, 0.62, boxPts(-Lt / 2 - 6, Lt / 2 + 6, 0, Ht, -Wt / 2, Wt / 2), [40, 30, 600, 300], 2), s = C.s;
    const pan = { c: "#c9c6b6", amb: 0.45, kd: 0.6, ks: 0.7, pw: 28, env: 0.2 };
    const blk = { c: "#1d1e21", amb: 0.5, kd: 0.55, ks: 0.3, pw: 14 };
    A.shadowPath(poly(hull(rrect(Lt + 20, Wt + 18, 6).map(([x, z]) => C.p([x + 4, 0, z - 3])))), 0.5, 12);
    let sv = "";
    // floor (inside) + walls: far walls first, then springs, then near walls
    const fl = [[-Lt / 2, 1, -Wt / 2], [Lt / 2, 1, -Wt / 2], [Lt / 2, 1, Wt / 2], [-Lt / 2, 1, Wt / 2]].map(C.p);
    sv += `<path d="${poly(fl)}" fill="${A.lin([[0, "#8f8c7f"], [0.5, "#b5b2a3"], [1, "#7d7a6e"]], fl[0][0], fl[0][1], fl[2][0], fl[2][1])}"/>`;
    const wall = (P, n) => `<path d="${poly(P.map(C.p))}" fill="${shadeN(pan, C.rot(n))}" stroke="#6e6b60" stroke-width=".8"/>`;
    sv += wall([[-Lt / 2, 0, -Wt / 2], [Lt / 2, 0, -Wt / 2], [Lt / 2, Ht, -Wt / 2], [-Lt / 2, Ht, -Wt / 2]], [0, 0, 1]);
    sv += wall([[Lt / 2, 0, -Wt / 2], [Lt / 2, 0, Wt / 2], [Lt / 2, Ht, Wt / 2], [Lt / 2, Ht, -Wt / 2]], [-1, 0, 0]);
    // transducers at both ends
    [-1, 1].forEach((sd) => {
      sv += prism(C, [sd * (Lt / 2 - 26), 1, 0], [0, 1, 0], [1, 0, 0], [0, 0, 1], rrect(34, 62, 3, 3), 22, blk).svg;
      sv += prism(C, [sd * (Lt / 2 - 26), 23, 0], [0, 1, 0], [1, 0, 0], [0, 0, 1], rrect(14, 30, 2, 3), 4, { c: "#8a8d92", amb: 0.4, kd: 0.6, ks: 0.8, pw: 30 }).svg;
    });
    // two springs (helices)
    [-14, 14].forEach((zc) => {
      const pts = [];
      for (let x = -Lt / 2 + 44; x <= Lt / 2 - 44; x += 0.8) { const t = x * 1.9; pts.push(C.p([x, 18 + 4 * Math.sin(t), zc + 4 * Math.cos(t)])); }
      const d = pline(pts);
      sv += `<path d="${d}" fill="none" stroke="#2d2f33" stroke-width="3.6"/><path d="${d}" fill="none" stroke="#c7ccd2" stroke-width="2"/><path d="${d}" fill="none" stroke="#fff" stroke-opacity=".5" stroke-width=".8" transform="translate(-.5 -.6)"/>`;
      [-1, 1].forEach((sd) => { const a = C.p([sd * (Lt / 2 - 44), 18, zc]), b = C.p([sd * (Lt / 2 - 30), 18, zc * 0.6]); sv += `<path d="M${r2(a[0])} ${r2(a[1])} L${r2(b[0])} ${r2(b[1])}" stroke="#c7ccd2" stroke-width="2.2"/>`; });
    });
    // near walls with a folded rim
    sv += wall([[-Lt / 2, 0, Wt / 2], [Lt / 2, 0, Wt / 2], [Lt / 2, Ht, Wt / 2], [-Lt / 2, Ht, Wt / 2]], [0, 0, 1]);
    sv += wall([[-Lt / 2, 0, -Wt / 2], [-Lt / 2, 0, Wt / 2], [-Lt / 2, Ht, Wt / 2], [-Lt / 2, Ht, -Wt / 2]], [-1, 0, 0]);
    sv += `<path d="${pline([[-Lt / 2, Ht, -Wt / 2], [-Lt / 2, Ht, Wt / 2], [Lt / 2, Ht, Wt / 2]].map(C.p))}" fill="none" stroke="#fff" stroke-opacity=".5" stroke-width="1.6"/>`;
    // label on the near wall
    sv += onPlane(C, [0, Ht / 2, Wt / 2 + 0.05], [1, 0, 0], [0, 1, 0], `<rect x="-640" y="-110" width="1280" height="220" rx="16" fill="#efe9d8"/><g text-anchor="middle" font-weight="800" fill="#2b2618"><text x="0" y="-24" font-size="96" letter-spacing="10">REVERB</text><text x="0" y="70" font-size="76" letter-spacing="6">${esc(String(p.model || "4AB3C1B").toUpperCase().slice(0, 12))}</text></g>`, 0.05);
    // RCA jacks on the left end
    const jk = [C.p([-Lt / 2 - 1, Ht * 0.5, -22]), C.p([-Lt / 2 - 1, Ht * 0.5, 22])];
    jk.forEach((q, i) => { sv += `<ellipse cx="${r2(q[0])}" cy="${r2(q[1])}" rx="${r2(4.5 * s)}" ry="${r2(5.2 * s)}" fill="${i ? "#c42a22" : "#e8e4d8"}" stroke="#222" stroke-width="1"/><ellipse cx="${r2(q[0])}" cy="${r2(q[1])}" rx="${r2(2.4 * s)}" ry="${r2(2.8 * s)}" fill="#c9cdd2"/><ellipse cx="${r2(q[0])}" cy="${r2(q[1])}" rx="${r2(0.9 * s)}" ry="${r2(1 * s)}" fill="#1b1c1f"/>`; });
    A.add(sv);
    const nodes = p.nodes || [], half = Math.max(1, Math.floor(nodes.length / 2));
    const tags = nodes.slice(0, 4).map((n, i) => mkTag(o, n, i < half ? `INPUT${half > 1 ? (i ? " −" : " +") : ""}` : `OUTPUT${nodes.length - half > 1 ? (i - half ? " −" : " +") : ""}`, { x: jk[i < half ? 0 : 1][0] + (i % 2) * 8, y: jk[i < half ? 0 : 1][1] + 6 * s }));
    tagsRow(A, tags, 318);
    return { A, name: `Spring reverb tank${p.model ? ` (${p.model})` : ""}`, pkg: "Long tank, two springs · input and output transducers", chips: ["Reverb tank", "2 springs"] };
  }

  function artLDR(p, o) {
    const A = new Art(640, 380);
    const roach = /ROACH|VTL|NSL|VACTROL/.test(String(p.model || "").toUpperCase()) || /vactrol|roach|optocoupler|neon-ldr/.test(o.pkg) || (p.nodes || []).length >= 4;
    const nodes = p.nodes || [];
    if (roach) {
      const y = 140, L = 290, R = 52, x0 = 320 - L / 2, x1 = 320 + L / 2, yF = 290;
      A.shadow(320, yF + 4, L * 0.62, 12, 0.42, 10);
      const ds = [-14, 14];
      const ends = [];
      ds.forEach((dy, i) => { A.add(wire(`M${x0 + 4} ${y + dy} H${x0 - 40 - i * 16} Q${x0 - 60 - i * 16} ${y + dy} ${x0 - 60 - i * 16} ${y + dy + 20} V${yF}`, 5)); ends.push([x0 - 60 - i * 16, yF]); });
      ds.forEach((dy, i) => { A.add(wire(`M${x1 - 4} ${y + dy} H${x1 + 40 + i * 16} Q${x1 + 60 + i * 16} ${y + dy} ${x1 + 60 + i * 16} ${y + dy + 20} V${yF}`, 5)); ends.push([x1 + 60 + i * 16, yF]); });
      const d = bodyPath(x0, x1, y, R, 1, 0);
      A.add(`<path d="${d}" fill="#141416"/><path d="${d}" fill="${axialShade(A, y, R, 0.5)}"/><g clip-path="${A.clip(d)}">${streak(A, x0 + 20, y - R * 0.5, x1 - 20, y - R * 0.5, 6, 0.4)}</g><path d="M${x0 + 30} ${y - R + 2} q12 30 0 ${2 * R - 4} M${x1 - 30} ${y - R + 2} q-12 30 0 ${2 * R - 4}" stroke="#000" stroke-opacity=".6" stroke-width="3" fill="none"/>`);
      tagsRow(A, [mkTag(o, nodes[0], "LDR", { x: ends[1][0], y: ends[1][1] }), mkTag(o, nodes[1], "LDR", { x: ends[0][0], y: ends[0][1] }), mkTag(o, nodes[2], "LAMP +", { x: ends[2][0], y: ends[2][1] }), mkTag(o, nodes[3], "LAMP −", { x: ends[3][0], y: ends[3][1] })], yF + 16);
      return { A, name: `${p.model || "Opto-coupler"} (lamp + LDR “roach”)`, pkg: "Neon/LED and CdS cell sealed in black heat-shrink", chips: ["Optocoupler", "LDR"] };
    }
    const cx = 320, cy = 140, R = 96, yF = 300;
    A.shadow(cx + 8, yF, R * 0.9, 10, 0.4, 9);
    A.add(wire(`M${cx - 30} ${cy + R * 0.6} V${yF}`, 5.5), wire(`M${cx + 30} ${cy + R * 0.6} V${yF}`, 5.5));
    let s = `<ellipse cx="${cx + 5}" cy="${cy + 8}" rx="${R}" ry="${R * 0.98}" fill="#8a8170"/><circle cx="${cx}" cy="${cy}" r="${R}" fill="${A.rad([[0, "#fbf7ec"], [0.8, "#e4dccb"], [1, "#b8ae98"]], cx - 20, cy - 24, R * 1.1)}"/>`;
    // interdigitated electrodes + CdS serpentine
    let zz = `M${cx - R * 0.62} ${cy - R * 0.5}`;
    for (let i = 0; i < 9; i++) { const yy = cy - R * 0.5 + i * R * 0.125; zz += ` H${i % 2 ? cx - R * 0.62 : cx + R * 0.62} V${r2(yy + R * 0.125)}`; }
    s += `<circle cx="${cx}" cy="${cy}" r="${R * 0.78}" fill="#c9cdd2"/><path d="${zz}" fill="none" stroke="#c4501f" stroke-width="7" stroke-linejoin="round"/>`;
    s += `<circle cx="${cx}" cy="${cy}" r="${R * 0.82}" fill="${A.rad([[0, "#fff", 0.28], [0.5, "#fff", 0.05], [1, "#fff", 0]], cx - 30, cy - 36, R * 0.9)}"/>`;
    A.add(s);
    tagsRow(A, [mkTag(o, nodes[0], "PIN 1", { x: cx - 30, y: yF }), mkTag(o, nodes[1], "PIN 2", { x: cx + 30, y: yF })], yF + 14);
    return { A, name: "CdS photoresistor (LDR)", pkg: "Cadmium-sulphide cell, ceramic body", chips: ["LDR"] };
  }

  function artGeneric(p, o) {
    const A = new Art(640, 420);
    const nodes = (p.nodes || []).slice(0, 8), n = Math.max(2, nodes.length);
    const Wb = Math.max(22, n * 6.5);
    const C = fitCam(-0.38, 0.5, boxPts(-Wb / 2, Wb / 2, -7, 10, -7, 7), [160, 20, 480, 320], 14), s = C.s;
    const ep = { c: "#18191c", amb: 0.5, kd: 0.55, ks: 0.4, pw: 20, env: 0.12 };
    const tin = { c: "#d2b46a", amb: 0.42, kd: 0.6, ks: 0.9, pw: 40, env: 0.2 };
    A.shadowPath(poly(hull(rrect(Wb + 3, 18, 2).map(([x, z]) => C.p([x + 1, -7, z - 1])))), 0.5, 11);
    const xs = nodes.map((_, i) => (i - (nodes.length - 1) / 2) * Math.min(6.5, (Wb - 6) / Math.max(1, nodes.length - 1)));
    const ends = xs.map((x) => { const a = C.p([x, 0.2, 5]), b = C.p([x, -7, 5]); A.add(wire(`M${r2(a[0])} ${r2(a[1])} L${r2(b[0])} ${r2(b[1])}`, 0.8 * s, "#d2b46a", 0.6)); return b; });
    void tin;
    const pr = prism(C, [0, 0, 0], [0, 1, 0], [1, 0, 0], [0, 0, 1], rrect(Wb, 14, 2, 5), 10, ep, { capM: { c: "#1c1d20", amb: 0.6, kd: 0.45, ks: 0.2, pw: 10 } });
    let sv = pr.svg;
    const typeName = { vcvs: "VCVS", vccs: "VCCS", sub: String(p.of || "SUB").toUpperCase() }[p.type] || String(p.type || "PART").toUpperCase();
    sv += onPlane(C, [0, 10.02, 0], [1, 0, 0], [0, 0, -1], `<rect x="${-Wb * 4.2}" y="-46" width="${Wb * 8.4}" height="92" rx="10" fill="#c8a24a" opacity=".92"/><rect x="${-Wb * 4.2 + 6}" y="-40" width="${Wb * 8.4 - 12}" height="80" rx="7" fill="none" stroke="#3a2c0a" stroke-opacity=".5" stroke-width="2"/><g text-anchor="middle" font-weight="800" fill="#2a2008"><text x="0" y="-6" font-size="30" letter-spacing="3">${esc(typeName)}</text><text x="0" y="26" font-size="20" letter-spacing="2">${esc(p.gain != null ? `GAIN ${p.gain}` : p.model || p.ref)}</text></g>`);
    A.add(sv);
    const roles = p.type === "vcvs" || p.type === "vccs" ? ["OUT +", "OUT −", "CTRL +", "CTRL −"] : nodes.map((_, i) => `PIN ${i + 1}`);
    tagsRow(A, nodes.map((nd, i) => mkTag(o, nd, roles[i] || `PIN ${i + 1}`, { x: ends[i][0], y: ends[i][1] })), Math.max(...ends.map((e) => e[1])) + 22);
    return { A, name: `${typeName === "VCVS" ? "Voltage-controlled voltage source" : typeName === "VCCS" ? "Voltage-controlled current source" : `${typeName} block`}`, pkg: "Behavioural element (no physical package)", chips: ["Behavioural"] };
  }

  /* =====================================================================
     dispatcher
     ===================================================================== */
  function vKind(p) {
    const v = +p.value || 0;
    if (p.ac) return "pt";
    if (Math.abs(v) > 30 || v < 0) return "can";
    return /ref|divider|bias|µF|uF/i.test(`${p.label || ""} ${p.ref || ""}`) && !/supply|batt/i.test(p.label || "") ? "rail" : "battery";
  }
  function classify(p, o) {
    const t = p.type, m = String(p.model || "").toUpperCase(), mk = String(o.mi.kind || "").toLowerCase();
    const fam = o.fam || "", pk = o.pack || "", of = String(p.of || "").toLowerCase();
    if (/reverb|spring|tank/.test(fam + " " + pk + " " + of) || /^(4AB|8AB|9AB|4FB|8EB|ACCUTRONICS|TANK)/.test(m) || t === "spring" || t === "tank" || t === "reverb") return "spring";
    if (fam === "speaker" || /speaker/.test(pk) || t === "speaker" || (t === "sub" && /speaker|spk|cab/.test(of))) return "speaker";
    if (fam === "vacuum-rectifier" || fam === "triode" || fam === "power-pentode" || pk === "noval-b9a" || pk === "octal") return "tube";
    if (fam === "optocoupler" || pk === "neon-ldr") return "ldr";
    if (fam === "transformer" || pk === "frame-transformer" || pk === "mains-transformer") return pk === "mains-transformer" ? "pt" : "ot";
    if (fam === "choke" || pk === "frame-choke") return "choke";
    if (fam === "inductor" || pk === "wound-inductor") return "toroid";
    if (fam === "op-amp" || pk === "dip-8") return "opamp";
    if (/^bjt|jfet|fet/.test(fam) || pk === "to-92") return "transistor";
    if (/diode|^led$/.test(fam)) return "diode";
    if (fam === "potentiometer" || pk === "panel-pot") return "pot";
    if (fam === "switch" || pk === "toggle") return "switch";
    if (fam === "source" || pk === "supply-rail") return vKind(p);
    if (t === "R") return "resistor";
    if (t === "C") return "cap";
    if (t === "pot") return "pot";
    if (t === "L") return /toroid/.test(o.pkg) ? "toroid" : (+p.value >= 1 || /choke/.test(o.pkg)) ? "choke" : "toroid";
    if (t === "D" || t === "Dpair") return (/^(5AR4|GZ3|5Y3|5U4|EZ8|6CA4|5V4|5Z4)/.test(m) || mk === "rectifier" || mk === "vacuum_diode") ? "tube" : "diode";
    if (t === "npn" || t === "pnp" || t === "njf" || t === "pjf") return "transistor";
    if (t === "opamp") return "opamp";
    if (t === "triode" || t === "pentode" || t === "tetrode") return "tube";
    if (t === "transformer") return "ot";
    if (t === "switch") return "switch";
    if (t === "V") return vKind(p);
    if (t === "ldr") return "ldr";
    return "generic";
  }
  // The drawing's shape class, from the part alone (no DC): "axial" parts lie along their wire (resistors, diodes,
  // axial capacitors); everything else stands on it. The canvas sizes its slots from this before anything is drawn.
  function figClass(p, opts = {}) {
    const c = opts.circuit || {}, mi = (p.model && (c.models || {})[p.model]) || {};
    const o = { c, parts: c.parts || [], mi, pkg: [p.package, p.family, p.style, mi.package, mi.family, mi.style].filter(Boolean).join(" ").toLowerCase(),
      fam: String(p.family || mi.family || "").toLowerCase(), pack: String(p.package || mi.package || "").toLowerCase(), V: () => null,
      tubes: (c.parts || []).some((q) => q.type === "triode" || q.type === "pentode" || q.type === "tetrode") };
    const kind = classify(p, o);
    if (kind === "resistor") return "axial";
    if (kind === "diode") return diodeStyle(p, o) === "led" ? "radial" : "axial";
    if (kind === "cap") { const st = capStyle(p, o); return st === "elec-ax" || st === "mallory" ? "axial" : "radial"; }
    return kind;
  }
  function renderArt(p, opts) {
    const c = opts.circuit || {}, parts = c.parts || [], models = c.models || {};
    const mi = (p.model && models[p.model]) || {};
    const pkg = [p.package, p.family, p.style, mi.package, mi.family, mi.style].filter(Boolean).join(" ").toLowerCase();
    const fam = String(p.family || mi.family || "").toLowerCase(), pack = String(p.package || mi.package || "").toLowerCase();
    const dc = opts.dc || {};
    const ctl = p.control && (c.controls || []).find((k) => k.id === p.control);
    const o = {
      c, parts, mi, pkg, fam, pack, on: opts.on !== false,
      tubes: parts.some((q) => q.type === "triode" || q.type === "pentode" || q.type === "tetrode"),
      V: (n) => (n == null ? null : isGnd(n) ? 0 : dc[n] != null && isFinite(+dc[n]) ? +dc[n] : null),
      ctlLabel: ctl ? ctl.label : "", label: p.label || "",
    };
    const kind = classify(p, o);
    let r;
    try {
      r = ({ resistor: artResistor, cap: artCap, pot: artPot, diode: artDiode, transistor: artTransistor, opamp: artOpamp, tube: artTube,
        ot: (q, oo) => artMagnetic(q, oo, "ot"), choke: (q, oo) => artMagnetic(q, oo, "choke"), pt: (q, oo) => artMagnetic(Object.assign({}, q, { nodes: q.nodes }), oo, "pt"),
        toroid: artToroid, spring: artSpring, rail: artRail, switch: artSwitch, battery: artBattery, can: artCan, speaker: artSpeaker, ldr: artLDR, generic: artGeneric }[kind] || artGeneric)(p, o);
    } catch (e) {
      if (typeof console !== "undefined") console.error("circuit-parts", p.ref, e);
      r = artGeneric(p, o);
    }
    r.kind = kind;
    return r;
  }
  function render(p, opts = {}) {
    injectCss();
    const r = renderArt(p, opts);
    return { svg: r.A.svg(r.name), kind: r.kind, name: r.name, pkg: r.pkg, chips: (r.chips || []).filter(Boolean), bands: r.bands || null };
  }

  // The compact figure the circuit canvas draws in its Illustrated view (same art, no tags, straight axial leads).
  function figure(p, opts = {}) {
    injectCss();
    const prev = FIG;
    FIG = { anchors: [], reach: opts.reach != null ? opts.reach : 30 };
    try {
      const r = renderArt(p, opts);
      const A = r.A;
      return { defs: A.defs.join(""), shadow: A.sh.join(""), body: A.out.join(""), w: A.w, h: A.h, anchors: FIG.anchors, kind: r.kind, name: r.name };
    } finally { FIG = prev; }
  }
  window.CircuitParts = { render, figure, figClass, colorBands, capCode, potCode, classify: (p, opts = {}) => classify(p, { mi: {}, pkg: "", fam: String(p.family || "").toLowerCase(), pack: String(p.package || "").toLowerCase(), c: opts.circuit || {} }), fmtV };
})();
