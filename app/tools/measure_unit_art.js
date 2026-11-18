async (page) => {
  await page.goto('http://127.0.0.1:8791/assets/units/index.json');
  const idx = JSON.parse(await page.evaluate(() => document.body.innerText));
  const ONLY = [];   // refresh_unit_art.sh <tid>...: measure just these units (empty: all)
  if (ONLY.length) idx.units = Object.fromEntries(Object.entries(idx.units).filter(([t]) => ONLY.includes(+t)));
  await page.goto('http://127.0.0.1:8791/index.html?port=1');
  const out = await page.evaluate(async (units) => {
    const res = {};
    const KIND = [[/send/i, "send"], [/return|rtn/i, "return"], [/exp(ression)?|ctl|pedal-in/i, "exp"], [/dc|9v|power-?jack|psu|adapter/i, "power"],
      [/out(put)?s?\b|line-?out|output/i, "out"], [/in(put)?s?\b|inst|hi(gh)?\b|lo(w)?\b|normal|bright|norm/i, "in"]];
    const kindOf = (t) => { for (const [re, k] of KIND) if (re.test(t)) return k; return ""; };
    const LABEL = /^(inputs?|in|outputs?|out|send|return|rtn|hi|high|lo|low|exp|expression|dc\s*9v|9v\s*dc|9v|inst|instrument|mono|stereo|[lr]|left|right|normal|bright|line out)$/i;
    async function findJacks(url, cvb) {
      const txt = await (await fetch(url)).text();
      const host = document.createElement("div");
      host.style.cssText = "position:absolute;left:0;top:0;width:1000px;visibility:hidden";
      host.innerHTML = txt;
      document.body.appendChild(host);
      const svg = host.querySelector("svg");
      const [vx, vy, vw, vh] = svg.getAttribute("viewBox").split(/[ ,]+/).map(Number);
      svg.setAttribute("width", 1000); svg.setAttribute("height", 1000 * vh / vw); svg.setAttribute("preserveAspectRatio", "none");
      const sr = svg.getBoundingClientRect(), kx = vw / sr.width, ky = vh / sr.height;
      const box = (el) => { const r = el.getBoundingClientRect(); return { x: vx + (r.left - sr.left + r.width / 2) * kx, y: vy + (r.top - sr.top + r.height / 2) * ky, w: r.width * kx, h: r.height * ky }; };
      const round = [...svg.querySelectorAll("circle, ellipse")].filter((c) => !c.closest("defs, mask, clipPath, pattern")).map((c) => ({ el: c, ...box(c) }))
        .filter((c) => c.w > vw * 0.008 && c.w < vw * 0.09 && Math.abs(c.w - c.h) < Math.max(c.w, c.h) * 0.35);
      const jacks = [];
      const add = (x, y, k, label, src) => { if (jacks.some((j) => Math.hypot(j.x - x, j.y - y) < vw * 0.02)) return; jacks.push({ k: k || "", x: +x.toFixed(1), y: +y.toFixed(1), label: label || "", src }); };
      // 1) elements whose id names a jack
      svg.querySelectorAll("[id]").forEach((el) => {
        const id = el.id;
        if (!/jack|socket/i.test(id) || el.closest("defs")) return;
        const b = box(el);
        if (!b.w || b.w > vw * 0.5) return;
        if (/jacks/i.test(id)) {   // a group of several jacks: use the round sockets inside it
          const inner = round.filter((c) => el.contains(c.el)).sort((p, q) => q.w - p.w);
          const seen = [];
          inner.forEach((c) => { if (!seen.some((s) => Math.hypot(s.x - c.x, s.y - c.y) < c.w)) { seen.push(c); add(c.x, c.y, "", "", "group:" + id); } });
          if (!inner.length) add(b.x, b.y, kindOf(id), "", "id:" + id);
          return;
        }
        add(b.x, b.y, kindOf(id.replace(/jack|socket/gi, "")) || "in", "", "id:" + id);
      });
      // 2) labels next to a socket
      svg.querySelectorAll("text").forEach((t) => {
        const s = (t.textContent || "").trim();
        if (!LABEL.test(s) || t.closest("defs")) return;
        const b = box(t);
        let best = null, bd = Infinity;
        round.forEach((c) => { const d = Math.hypot(c.x - b.x, c.y - b.y); if (d < bd) { bd = d; best = c; } });
        const near = best && bd < Math.max(vw, vh) * 0.08;
        const j = near && jacks.find((q) => Math.hypot(q.x - best.x, q.y - best.y) < best.w);
        if (j) { if (!j.label) { j.label = s; j.k = kindOf(s) || j.k; } return; }
        if (near) add(best.x, best.y, kindOf(s), s, "label:" + s);
      });
      host.remove();
      // which way the cable leaves: the nearest edge of the hardware, or straight out of the front panel
      const [cx, cy, cw, ch] = cvb;
      jacks.forEach((j) => {
        const d = { left: (j.x - cx) / cw, right: (cx + cw - j.x) / cw, top: (j.y - cy) / ch, bottom: (cy + ch - j.y) / ch };
        const e = Object.entries(d).sort((a, b) => a[1] - b[1])[0];
        j.edge = e[1] < 0.12 ? e[0] : "front";
      });
      return jacks;
    }
    for (const [tid, u] of Object.entries(units)) {
      const [x0, y0, W, H] = u.vb;
      const img = new Image();
      img.src = `/assets/units/${tid}.${u.h}.svg`;
      try { await img.decode(); } catch (e) { continue; }
      const cw = 1600, ch = Math.round(1600 * H / W);
      const c = document.createElement('canvas'); c.width = cw; c.height = ch;
      const g = c.getContext('2d'); g.drawImage(img, 0, 0, cw, ch);
      const d = g.getImageData(0, 0, cw, ch).data;
      let l = cw, t = ch, r = -1, b = -1;
      for (let y = 0; y < ch; y++) for (let x = 0; x < cw; x++) if (d[(y * cw + x) * 4 + 3] > 48) { if (x < l) l = x; if (x > r) r = x; if (y < t) t = y; if (y > b) b = y; }
      if (r < 0) continue;
      const sx = W / cw, sy = H / ch, pad = 0.006 * Math.max(W, H);
      const nx = Math.max(x0, x0 + l * sx - pad), ny = Math.max(y0, y0 + t * sy - pad);
      const nr = Math.min(x0 + W, x0 + (r + 1) * sx + pad), nb = Math.min(y0 + H, y0 + (b + 1) * sy + pad);
      res[tid] = { k: u.k, vb: [+nx.toFixed(1), +ny.toFixed(1), +(nr - nx).toFixed(1), +(nb - ny).toFixed(1)], from: u.vb };
      // Jacks: find the sockets the drawing has (ids like jack-input / side-jacks, labels like INPUT / OUT / HI / LO next
      // to a round socket) in the live SVG DOM, in the drawing's own coordinates.
      try { res[tid].jacks = await findJacks(`/assets/units/${tid}.${u.h}.svg`, [nx, ny, nr - nx, nb - ny]); } catch (e) { res[tid].jacks = []; }
      // No sockets named in the drawing: look for them in the silhouette instead. Jacks stick out of a stompbox's
      // sides (or top): rows/columns where the outline bulges past the body's straight edge.
      if (u.kind === "pedal" && !res[tid].jacks.some((j) => /^(id|group)/.test(j.src))) {
        const op = (x, y) => d[(y * cw + x) * 4 + 3] > 48;
        const lm = [], rm = [], tm = [];
        for (let y = t; y <= b; y++) { let a = -1, z = -1; for (let x = l; x <= r; x++) if (op(x, y)) { if (a < 0) a = x; z = x; } lm[y] = a; rm[y] = z; }
        for (let x = l; x <= r; x++) { let a = -1; for (let y = t; y <= b; y++) if (op(x, y)) { a = y; break; } tm[x] = a; }
        const med = (arr, from, to) => { const v = arr.slice(from, to).filter((q) => q >= 0).sort((p, q) => p - q); return v[Math.floor(v.length / 2)]; };
        const H0 = b - t, W0 = r - l;
        const bodyL = med(lm, t + Math.round(H0 * 0.3), t + Math.round(H0 * 0.9)), bodyR = med(rm, t + Math.round(H0 * 0.3), t + Math.round(H0 * 0.9));
        const bodyT = med(tm, l + Math.round(W0 * 0.3), l + Math.round(W0 * 0.7));
        const runs = (pred, from, to, minLen, maxLen) => { const o = []; let s0 = -1; for (let i = from; i <= to + 1; i++) { const on = i <= to && pred(i); if (on && s0 < 0) s0 = i; if (!on && s0 >= 0) { if (i - s0 >= minLen && i - s0 <= maxLen) o.push([s0, i - 1]); s0 = -1; } } return o; };
        const thr = Math.max(3, cw * 0.012);
        const bumps = [];
        runs((y) => lm[y] >= 0 && lm[y] < bodyL - thr, t, b, ch * 0.012, ch * 0.16).forEach(([a, z]) => bumps.push({ side: "left", px: Math.min(...lm.slice(a, z + 1).filter((q) => q >= 0)), py: (a + z) / 2 }));
        runs((y) => rm[y] > bodyR + thr, t, b, ch * 0.012, ch * 0.16).forEach(([a, z]) => bumps.push({ side: "right", px: Math.max(...rm.slice(a, z + 1)), py: (a + z) / 2 }));
        if (!bumps.length) runs((x) => tm[x] >= 0 && tm[x] < bodyT - thr, l, r, cw * 0.012, cw * 0.18).forEach(([a, z]) => bumps.push({ side: "top", px: (a + z) / 2, py: Math.min(...tm.slice(a, z + 1).filter((q) => q >= 0)) }));
        const found = bumps.filter((q) => q.side !== "top" || q.py < t + H0 * 0.25).map((q) => ({ k: "", x: +(x0 + q.px * sx).toFixed(1), y: +(y0 + q.py * sy).toFixed(1), label: "", src: "bump:" + q.side, edge: q.side }));
        if (found.length >= 1) res[tid].jacks = res[tid].jacks.filter((j) => /^label/.test(j.src) && j.k).concat(found);
      }
    }
    return res;
  }, idx.units);
  return JSON.stringify(out);
}
