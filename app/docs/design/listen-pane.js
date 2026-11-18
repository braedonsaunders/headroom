/* Headroom — right pane concept "Listen" (docs/design/listen-pane.*).
 * A self-contained mock: ListenPane.mount(el, {state}) renders the pane and drives it with simulated analysis
 * (E minor, i–VI–III–VII). The real pane would take the same model from an engine /api/listen route. */
(function () {
  "use strict";
  const SHARP = ["C", "C♯", "D", "D♯", "E", "F", "F♯", "G", "G♯", "A", "A♯", "B"];
  const FLAT = ["C", "D♭", "D", "E♭", "E", "F", "G♭", "G", "A♭", "A", "B♭", "B"];
  const SCALES = {
    pent: { label: "Pent", name: "minor pentatonic", iv: [0, 3, 5, 7, 10] },
    blues: { label: "Blues", name: "blues", iv: [0, 3, 5, 6, 7, 10] },
    nat: { label: "Natural", name: "natural minor", iv: [0, 2, 3, 5, 7, 8, 10] },
    dor: { label: "Dorian", name: "dorian", iv: [0, 2, 3, 5, 7, 9, 10] },
  };
  // Simulated progression in E minor: Em – Cmaj7 – G – D (i – VI – III – VII)
  const PROG = [
    { name: "Em", sup: "", roman: "i", pcs: [4, 7, 11], root: 4 },
    { name: "C", sup: "maj7", roman: "VI", pcs: [0, 4, 7, 11], root: 0 },
    { name: "G", sup: "", roman: "III", pcs: [7, 11, 2], root: 7 },
    { name: "D", sup: "", roman: "VII", pcs: [2, 6, 9], root: 2 },
  ];
  const OPEN = [64, 59, 55, 50, 45, 40];          // high e … low E (MIDI)
  const STR_NAMES = ["e", "B", "G", "D", "A", "E"];
  const KEY = { root: 4, mode: "minor", conf: 0.86, alt: "G major" };
  const ICON = {
    lock: '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5"><rect x="3.5" y="7" width="9" height="6.5" rx="1.5"/><path d="M5.5 7V5a2.5 2.5 0 015 0v2"/></svg>',
    expand: '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5"><path d="M9.5 2.5h4v4M6.5 13.5h-4v-4M13.5 2.5L9 7M2.5 13.5L7 9"/></svg>',
    close: '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6"><path d="M4 4l8 8M12 4l-8 8"/></svg>',
  };
  const nm = (pc) => SHARP[((pc % 12) + 12) % 12];

  function fretboardSVG() {
    const W = 252, x0 = 22, fw = (W - x0 - 3) / 12, top = 8, gap = 14.2;
    let s = `<svg class="lp-fb" viewBox="0 0 ${W} 96" xmlns="http://www.w3.org/2000/svg">`;
    [3, 5, 7, 9].forEach((f) => { s += `<circle class="inl" cx="${x0 + (f - 0.5) * fw}" cy="${top + gap * 2.5}" r="2.6"/>`; });
    s += `<circle class="inl" cx="${x0 + 11.5 * fw}" cy="${top + gap * 1.5}" r="2.6"/><circle class="inl" cx="${x0 + 11.5 * fw}" cy="${top + gap * 3.5}" r="2.6"/>`;
    for (let f = 1; f <= 12; f++) s += `<line class="frt" x1="${x0 + f * fw}" x2="${x0 + f * fw}" y1="${top}" y2="${top + gap * 5}"/>`;
    s += `<line class="nut" x1="${x0}" x2="${x0}" y1="${top - 1}" y2="${top + gap * 5 + 1}"/>`;
    for (let i = 0; i < 6; i++) s += `<line class="str" x1="${x0}" x2="${W - 2}" y1="${top + i * gap}" y2="${top + i * gap}" stroke-width="${0.6 + i * 0.22}"/>`;
    [3, 5, 7, 9, 12].forEach((f) => { s += `<text class="fn" x="${x0 + (f - 0.5) * fw}" y="${top + gap * 5 + 12}">${f}</text>`; });
    for (let i = 0; i < 6; i++) for (let f = 0; f <= 12; f++) {
      const cx = f === 0 ? x0 - 10 : x0 + (f - 0.5) * fw, cy = top + i * gap;
      s += `<g data-s="${i}" data-f="${f}" data-pc="${(OPEN[i] + f) % 12}"><circle class="dot" cx="${cx}" cy="${cy}" r="6"/><text class="lbl" x="${cx}" y="${cy + 0.3}"></text></g>`;
    }
    return s + "</svg>";
  }

  function knob(v, label, val) {
    const a0 = -135, a = a0 + v * 270, r = 13, c = 17;
    const pt = (deg, rr) => { const t = (deg - 90) * Math.PI / 180; return [c + rr * Math.cos(t), c + rr * Math.sin(t)]; };
    const [sx, sy] = pt(a0, r), [ex, ey] = pt(a, r), [px, py] = pt(a, 8);
    const big = a - a0 > 180 ? 1 : 0;
    return `<div class="lp-kn"><svg viewBox="0 0 34 34"><circle cx="17" cy="17" r="13" fill="none" stroke="var(--line-2)" stroke-width="2.4"/>
      <path d="M${sx} ${sy} A${r} ${r} 0 ${big} 1 ${ex} ${ey}" fill="none" stroke="var(--accent)" stroke-width="2.4" stroke-linecap="round"/>
      <circle cx="17" cy="17" r="9" fill="var(--card-2)" stroke="var(--line-3)"/><line x1="17" y1="17" x2="${px}" y2="${py}" stroke="var(--text)" stroke-width="1.6" stroke-linecap="round"/></svg>
      <b>${val}</b>${label}</div>`;
  }

  function html() {
    return `<div class="lp">
    <div class="card lp-tun-card">
      <div class="lp-row" style="margin-bottom:6px"><span class="lp-eyebrow">Tuner</span><span class="lp-row" style="gap:6px"><span class="lp-chip" data-k="ref">A = 440</span><button class="lp-ib" title="Full-screen tuner, mutes output (U)">${ICON.expand}</button></span></div>
      <div class="lp-tun">
        <div class="lp-note" data-k="note">E<sub>2</sub></div>
        <div>
          <div class="lp-cents" data-k="cents"></div>
          <div class="lp-tun-meta"><span data-k="cval"><b>+2</b> ¢</span><span data-k="hz">82.5 Hz</span></div>
        </div>
      </div>
      <div class="lp-trail-wrap"><canvas class="lp-trail" data-k="trail"></canvas><span class="lp-trail-tag" data-k="bend"></span></div>
      <div class="lp-strings" data-k="strings">${STR_NAMES.slice().reverse().map((n, i) => `<span data-i="${5 - i}">${n}</span>`).join("")}</div>
    </div>

    <div class="card lp-harm">
      <div class="lp-row"><span class="lp-eyebrow">Key</span><span class="lp-row" style="gap:6px"><span class="lp-chip" data-k="keystate"><i></i>listening</span><button class="lp-ib" title="Lock the key (stop following)">${ICON.lock}</button></span></div>
      <div class="lp-key">
        <div><div class="lp-key-name">E <em>minor</em></div><div class="lp-key-alt">or <b>G major</b> · relative</div></div>
        <div class="lp-conf"><svg viewBox="0 0 38 38"><circle cx="19" cy="19" r="15.5" fill="none" stroke="var(--line-2)" stroke-width="3"/><circle data-k="confarc" cx="19" cy="19" r="15.5" fill="none" stroke="var(--accent)" stroke-width="3" stroke-linecap="round" stroke-dasharray="97.4" stroke-dashoffset="13.6"/></svg><span data-k="conf">86%</span></div>
      </div>
      <div class="lp-chord">
        <div class="lp-chord-name" data-k="chord">Em</div>
        <div class="lp-chord-side"><span class="lp-roman" data-k="roman">i</span><span class="lp-tones" data-k="tones"></span></div>
      </div>
      <div class="lp-prog" data-k="prog"></div>
      <div class="lp-sec"><span class="lp-eyebrow">Scale</span>
        <span class="lp-seg" data-k="scales">${Object.entries(SCALES).map(([k, v]) => `<button data-sc="${k}">${v.label}</button>`).join("")}</span></div>
      ${fretboardSVG()}
      <div class="lp-fb-legend"><span><i style="background:var(--accent)"></i>Root</span><span><i style="background:rgba(227,160,75,.35);box-shadow:inset 0 0 0 1px rgba(227,160,75,.7)"></i>In this chord</span><span><i style="background:#fff3dc;box-shadow:0 0 4px var(--accent)"></i>Playing</span></div>
    </div>

    <div class="card lp-tone">
      <div class="lp-row" style="margin-bottom:7px"><span class="lp-eyebrow">Tone</span><span class="lp-seg"><button class="on">Spectrum</button><button>Scope</button></span></div>
      <canvas class="lp-spec" data-k="spec"></canvas>
      <div class="lp-bands"><span>80</span><span>250</span><span>1k</span><span>4k</span><span>12k</span></div>
      <div class="lp-tone-read"><span>Tight lows</span><span>Mids ↑ 800</span><span>Top ↓ 5.6k</span></div>
    </div>

    <div class="card lp-sig">
      <div class="lp-row" style="margin-bottom:6px"><span class="lp-eyebrow">Input</span><span class="lp-chip" data-k="peak">peak −11 dBFS</span></div>
      <div class="lp-meter"><div class="sweet" style="left:${(54 / 72) * 100}%;width:${(12 / 72) * 100}%"></div><div class="floor" style="width:${(4 / 72) * 100}%"></div><div class="gate" style="left:${(14 / 72) * 100}%" title="Noise gate threshold −58 dB"></div><div class="fill" data-k="fill"></div><div class="pk" data-k="pk"></div></div>
      <div class="lp-scale"><span>−72</span><span class="g">gate −58</span><span>−36</span><span class="s">sweet spot</span><span>0</span></div>
      <div class="lp-health" data-k="health"><i></i><span><b>Input level is right.</b> Peaks land in the sweet spot.</span></div>
      <div class="lp-stats">
        <div><div class="k">Floor</div><div class="v">−68<small>dB</small></div></div>
        <div><div class="k">Latency</div><div class="v">5.8<small>ms</small></div></div>
        <div><div class="k">CPU</div><div class="v" data-k="cpu">9<small>%</small></div></div>
      </div>
    </div>

    <div class="lp-drawer">
      <div class="grab"></div>
      <div class="lp-row"><div><h4>Marshall JCM800 2203</h4><div class="sub">Amp · slot 2 of 4 · 384 kHz internal</div></div><button class="lp-ib" title="Close (Esc)">${ICON.close}</button></div>
      <div class="lp-dr-ctls">${knob(.62, "Pre-amp", "6.2")}${knob(.5, "Bass", "5.0")}${knob(.7, "Middle", "7.0")}${knob(.58, "Treble", "5.8")}${knob(.45, "Presence", "4.5")}${knob(.69, "Master", "6.9")}${knob(.3, "Sag", "30%")}${knob(.5, "Bias", "0.0")}</div>
      <div class="lp-eyebrow">Unit presets</div>
      <div class="lp-dr-presets"><span>Clean</span><span class="on">Crunch</span><span>Hi Gain</span><span>Lead</span><span>Plexi Push</span></div>
      <div class="lp-dr-acts"><button>Bypass</button><button>Open details</button><button>Remove</button></div>
    </div>
  </div>`;
  }

  function mount(root, opts) {
    opts = opts || {};
    root.innerHTML = html();
    const q = (k) => root.querySelector(`[data-k="${k}"]`);
    const pane = root.querySelector(".lp");
    const st = { scale: opts.scale || "pent", chord: 0, hot: [], cents: 2, peak: -60, lvl: -60, t0: performance.now(), frozen: !!opts.freezeAt };
    if (opts.state === "silent") pane.classList.add("silent");
    if (opts.state === "selected") pane.classList.add("sel");

    // cents scale
    const cents = q("cents");
    let ch = `<div class="zone" style="left:${50 - 5}%;width:10%"></div>`;
    for (let c = -50; c <= 50; c += 10) ch += `<div class="tk${c === 0 ? " c" : ""}" style="left:${50 + c}%"></div>`;
    cents.innerHTML = ch + `<div class="nd" style="left:52%"></div>`;
    const needle = cents.querySelector(".nd");

    const fbDots = [...root.querySelectorAll(".lp-fb g")];
    function paintFretboard() {
      const sc = SCALES[st.scale].iv.map((i) => (KEY.root + i) % 12);
      const chord = PROG[st.chord].pcs;
      fbDots.forEach((g) => {
        const pc = +g.dataset.pc, s = +g.dataset.s, f = +g.dataset.f;
        const inScale = sc.includes(pc), dot = g.firstChild, lbl = g.lastChild;
        g.style.display = inScale ? "" : "none";
        if (!inScale) return;
        const hot = st.hot.some((h) => h.s === s && h.f === f);
        const cls = hot ? "hot" : pc === KEY.root ? "root" : chord.includes(pc) ? "ct" : "";
        dot.setAttribute("class", "dot " + cls);
        lbl.setAttribute("class", "lbl " + cls);
        lbl.textContent = nm(pc).replace("♯", "#");
      });
      root.querySelectorAll("[data-sc]").forEach((b) => b.classList.toggle("on", b.dataset.sc === st.scale));
    }
    root.querySelector('[data-k="scales"]').addEventListener("click", (e) => {
      const b = e.target.closest("[data-sc]"); if (!b) return; st.scale = b.dataset.sc; paintFretboard();
    });

    function paintChord() {
      const c = PROG[st.chord];
      q("chord").innerHTML = `${c.name}${c.sup ? `<sup>${c.sup}</sup>` : ""}`;
      q("roman").textContent = c.roman + (c.sup ? "maj7" : "");
      q("tones").innerHTML = c.pcs.map((pc) => `<span class="${pc === c.root ? "r" : ""}">${nm(pc)}</span>`).join("");
      const hist = [];
      for (let k = 3; k >= 0; k--) hist.push((st.chord - k + 400) % 4);
      q("prog").innerHTML = hist.map((i, n) => `<div class="${n === 3 ? "now" : ""}"><b>${PROG[i].name}${PROG[i].sup ? `<small>${PROG[i].sup}</small>` : ""}</b><i>${PROG[i].roman}</i></div>`).join("") +
        `<div class="ghost"><b>?</b><i>next</i></div>`;
    }

    // sounding notes: chord tones played in the 7th–10th fret box
    function pickHot() {
      const c = PROG[st.chord], cand = [];
      for (let s = 0; s < 6; s++) for (let f = 7; f <= 10; f++) if (c.pcs.includes((OPEN[s] + f) % 12)) cand.push({ s, f });
      const n = 1 + (Math.random() < 0.35 ? 1 : 0);
      st.hot = [];
      for (let k = 0; k < n && cand.length; k++) st.hot.push(cand.splice(Math.floor(Math.random() * cand.length), 1)[0]);
      const h = st.hot[0];
      if (h) {
        const midi = OPEN[h.s] + h.f, oct = Math.floor(midi / 12) - 1;
        q("note").innerHTML = `${nm(midi).replace("♯", "<sup>♯</sup>")}<sub>${oct}</sub>`;
        q("hz").textContent = `${(440 * Math.pow(2, (midi - 69) / 12)).toFixed(1)} Hz`;
        root.querySelectorAll('[data-k="strings"] span').forEach((sp) => sp.classList.toggle("on", +sp.dataset.i === h.s));
      }
    }

    // pitch trail: last 3 s of detected pitch on a semitone grid (bends show where they land)
    const trail = q("trail"), hist = [];
    st.bend = null;
    function trailPush(t) {
      const h = st.hot[0]; if (!h) return;
      let m = OPEN[h.s] + h.f + st.cents / 100;
      if (st.bend) {                                   // whole-step bend: rise, hold slightly sharp
        const k = Math.min(1, (t - st.bend.t0) / 260);
        m = st.bend.from + k * k * (3 - 2 * k) * (2 + st.bend.miss / 100) + Math.sin(t / 55) * 0.05 * k;
        if (t - st.bend.t0 > 900) st.bend = null;
      }
      hist.push({ t, m });
      while (hist.length && t - hist[0].t > 3000) hist.shift();
    }
    function drawTrail(t) {
      const dpr = window.devicePixelRatio || 1, w = trail.clientWidth, h = trail.clientHeight;
      if (!w) return;
      if (trail.width !== w * dpr) { trail.width = w * dpr; trail.height = h * dpr; }
      const g = trail.getContext("2d"); g.setTransform(dpr, 0, 0, dpr, 0, 0); g.clearRect(0, 0, w, h);
      if (!hist.length) return;
      const cs = getComputedStyle(root), acc = cs.getPropertyValue("--accent").trim(), grid = cs.getPropertyValue("--scope-grid").trim();
      const last = hist[hist.length - 1].m, mid = st.trailMid = st.trailMid == null ? last : st.trailMid + (last - st.trailMid) * 0.06;
      const span = 7, y = (m) => h / 2 - ((m - mid) / span) * h, x = (tt) => w - ((t - tt) / 3000) * w;
      g.strokeStyle = grid; g.lineWidth = 1;
      for (let s = Math.floor(mid - span / 2); s <= mid + span / 2; s++) { const yy = Math.round(y(s)) + .5; g.beginPath(); g.moveTo(0, yy); g.lineTo(w, yy); g.stroke(); }
      g.beginPath();
      hist.forEach((p, i) => { const prev = hist[i - 1]; if (i && Math.abs(p.m - prev.m) > 1.2 && p.t - prev.t < 40 && !prev.bend) { g.moveTo(x(p.t), y(p.m)); } else i ? g.lineTo(x(p.t), y(p.m)) : g.moveTo(x(p.t), y(p.m)); });
      g.strokeStyle = acc; g.lineWidth = 1.8; g.lineJoin = "round"; g.shadowColor = acc; g.shadowBlur = 6; g.stroke(); g.shadowBlur = 0;
      g.fillStyle = acc; g.beginPath(); g.arc(w - 2, y(last), 2.6, 0, 7); g.fill();
    }

    // spectrum
    const cv = q("spec");
    function curve(f, t, input) {
      const lf = Math.log10(f);
      if (input) return -38 - 6 * Math.abs(lf - 2.6) - (f > 3000 ? (lf - 3.48) * 14 : 0) + 3 * Math.sin(t * 2 + lf * 9);
      let d = -22;
      d -= f < 110 ? (2.04 - lf) * 40 : 0;                        // tight lows
      d += 7 * Math.exp(-Math.pow((lf - 2.9) / 0.22, 2));         // mid hump ~800 Hz
      d += 3.5 * Math.exp(-Math.pow((lf - 3.4) / 0.12, 2));       // presence 2.5k
      d -= f > 5600 ? (lf - 3.75) * 70 : 0;                       // cab roll-off
      d += 2.2 * Math.sin(t * 3 + lf * 13) + 1.4 * Math.sin(t * 5.3 + lf * 23);
      return d;
    }
    function drawSpec(t) {
      const dpr = window.devicePixelRatio || 1, w = cv.clientWidth, h = cv.clientHeight;
      if (cv.width !== w * dpr) { cv.width = w * dpr; cv.height = h * dpr; }
      const g = cv.getContext("2d"); g.setTransform(dpr, 0, 0, dpr, 0, 0); g.clearRect(0, 0, w, h);
      const cs = getComputedStyle(root);
      const acc = cs.getPropertyValue("--accent").trim(), grid = cs.getPropertyValue("--scope-grid").trim(), inc = cs.getPropertyValue("--scope-in").trim();
      const fx = (f) => (Math.log10(f / 40) / Math.log10(16000 / 40)) * w, dy = (d) => h - ((d + 72) / 72) * h;
      g.strokeStyle = grid; g.lineWidth = 1;
      [100, 1000, 10000].forEach((f) => { g.beginPath(); g.moveTo(fx(f) + .5, 0); g.lineTo(fx(f) + .5, h); g.stroke(); });
      [-12, -24, -36, -48, -60].forEach((d) => { g.beginPath(); g.moveTo(0, dy(d) + .5); g.lineTo(w, dy(d) + .5); g.stroke(); });
      const silent = pane.classList.contains("silent");
      const path = (input) => { g.beginPath(); for (let x = 0; x <= w; x += 2) { const f = 40 * Math.pow(16000 / 40, x / w); const d = silent ? -80 : curve(f, t, input); x ? g.lineTo(x, dy(d)) : g.moveTo(x, dy(d)); } };
      path(true); g.strokeStyle = inc; g.lineWidth = 1.2; g.setLineDash([3, 3]); g.stroke(); g.setLineDash([]);
      path(false); g.lineTo(w, h); g.lineTo(0, h); g.closePath();
      const gr = g.createLinearGradient(0, 0, 0, h); gr.addColorStop(0, acc + "66"); gr.addColorStop(1, acc + "05");
      g.fillStyle = gr; g.fill();
      path(false); g.strokeStyle = acc; g.lineWidth = 1.6; g.stroke();
    }

    function meter(t) {
      const silent = pane.classList.contains("silent");
      const beat = (t / 1000) * (92 / 60) * 2;
      const env = Math.exp(-6 * (beat % 1));
      st.lvl = silent ? -66 : -30 + 20 * env + 2 * Math.sin(t / 90);
      st.peak = Math.max(st.peak - 0.08, st.lvl);
      const pct = (d) => Math.max(0, Math.min(100, ((d + 72) / 72) * 100));
      q("fill").style.width = pct(st.lvl) + "%";
      q("pk").style.left = pct(st.peak) + "%";
      q("peak").textContent = silent ? "no signal" : `peak ${Math.round(st.peak)} dBFS`.replace("-", "−");
    }

    paintChord(); pickHot(); paintFretboard();
    if (opts.state === "silent") {
      q("note").innerHTML = "–"; q("cval").innerHTML = "play a string"; q("hz").textContent = "";
      q("keystate").classList.add("idle"); q("keystate").innerHTML = "<i></i>held · 14 s ago";
      st.hot = []; paintFretboard();
      root.querySelectorAll('[data-k="strings"] span').forEach((sp) => sp.classList.remove("on"));
      q("health").classList.add("warn");
      q("health").innerHTML = `<i></i><span><b>Nothing coming in.</b> Check the cable and the input channel (Settings → Audio).</span>`;
    }
    let lastChord = performance.now(), lastHot = 0;
    function frame(t) {
      if (!root.isConnected) return;
      const live = opts.state !== "silent" && !opts.still;
      if (live) {
        if (t - lastChord > 2600) { lastChord = t; st.chord = (st.chord + 1) % 4; paintChord(); paintFretboard(); }
        if (t - lastHot > 330 && !st.bend) {
          lastHot = t; pickHot(); paintFretboard();
          if (Math.random() < 0.16) {                  // simulate a whole-step bend and report where it landed
            const h = st.hot[0], from = OPEN[h.s] + h.f, miss = Math.round((Math.random() - 0.35) * 22);
            st.bend = { t0: t, from, miss };
            setTimeout(() => { const tag = q("bend"); tag.innerHTML = `bend → <b>${nm(from + 2)}</b> ${miss >= 0 ? "+" : "−"}${Math.abs(miss)}¢`; tag.classList.toggle("off", Math.abs(miss) > 8); tag.classList.add("show"); setTimeout(() => tag.classList.remove("show"), 2200); }, 300);
          }
        }
        trailPush(t);
        st.cents += (Math.random() - 0.5) * 2.2 - st.cents * 0.08;
        const c = Math.round(st.cents);
        needle.style.left = 50 + Math.max(-50, Math.min(50, st.cents)) + "%";
        q("cval").innerHTML = `<b>${c > 0 ? "+" : c < 0 ? "−" : "±"}${Math.abs(c)}</b> ¢`;
        root.querySelector(".lp-tun").classList.toggle("intune", Math.abs(st.cents) < 5);
        q("cpu").innerHTML = `${(8.5 + Math.sin(t / 700)).toFixed(0)}<small>%</small>`;
      }
      if (live) drawTrail(t);
      if (!opts.still || !st.drawn) { drawSpec(opts.still ? 1.3 : t / 1000); meter(opts.still ? 140 : t); st.drawn = true; }
      requestAnimationFrame(frame);
    }
    requestAnimationFrame(frame);
    return { setState(s) { pane.classList.toggle("silent", s === "silent"); pane.classList.toggle("sel", s === "selected"); } };
  }
  window.ListenPane = { mount };
})();
