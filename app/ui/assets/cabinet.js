/* Headroom — Cabinet unit faceplate (table 900100) with its Room & Stereo section (doubler, amp in the room),
 * the IR Loader's "Import .wav" (table 168000) and the rig tempo control in the top bar.
 *
 * Loaded by index.html after its own script. It reuses index.html's globals (ctlHTML, esc, unitCls,
 * moreBtn, drawerHTML, XBTN, PWR_SVG, MIC_SVG, RENDER, rackState, API_BASE, applyRackState, toast) so
 * every control keeps the standard drag / menu / keyboard behaviour.
 *
 * Engine contract: docs/ENGINE_API.md — "Cabinet unit" and "Rig tempo".
 */
(function () {
  "use strict";

  const CSS = `
.cab.studio-cab .hw-body { grid-template-columns: minmax(0, 1fr) var(--plate-w, 372px); }
.cab.studio-cab .cab-grille { height: auto; min-height: var(--gh, 176px); }
.cab.studio-cab.compact .cab-grille { min-height: 0; height: var(--gh); }
.cab.studio-cab .cs-knobs.b { border-top: none; justify-content: flex-start; gap: 2px 10px; padding-top: 0; }
.cab.studio-cab .spk-wrap { grid-template-columns: repeat(var(--sc, 2), 1fr); grid-template-rows: repeat(var(--sr, 1), 1fr); }
.cab.studio-cab .cab-brand { position: absolute; z-index: 4; left: 14px; bottom: 9px; font: 800 10px/1 var(--font, system-ui); letter-spacing: 2.4px;
  text-transform: uppercase; color: rgba(255,255,255,0.72); text-shadow: 0 1px 2px rgba(0,0,0,0.9); }
.cab.studio-cab .cab-brand small { display: block; margin-top: 4px; letter-spacing: 0.6px; font-weight: 600; opacity: 0.7; text-transform: none; font-size: 10px; }
.cab.studio-cab .mic { pointer-events: none; }
.cab.studio-cab .cab-grille[data-cab-open] { cursor: pointer; }
.cab.studio-cab .cab-grille[data-cab-open]:hover .vign { box-shadow: inset 0 0 40px rgba(0,0,0,0.55), inset 0 0 0 2px color-mix(in srgb, var(--accent) 60%, transparent); }
/* a mic on the grille: drawn as itself, its head at the spot it's aimed at, the body angled away up and out */
.cab.studio-cab .mic.art { width: 50px; height: 21px; margin: -10.5px 0 0 -46px; }
.cab.studio-cab .mic.art svg { transform-origin: 92% 50%; filter: drop-shadow(0 3px 3px rgba(0,0,0,0.75)); }
.cab.studio-cab .mic.art.a svg { transform: rotate(150deg); }
.cab.studio-cab .mic.art.b svg { transform: rotate(30deg); }
.cab.studio-cab .mic.art b { top: -14px; transform: none; }
.cab.studio-cab .mic.art.a b { left: auto; right: -30px; top: -22px; }
.cab.studio-cab .mic.art.b b { left: -6px; top: -22px; }
.cab.studio-cab .mic.room { width: auto; height: auto; margin: 0; }
.cab.studio-cab .mic.room b { position: static; transform: none; background: #c9c3b6; }
.cab.studio-cab .cs-sum { display: flex; flex-direction: column; gap: 4px; }
.cab.studio-cab .cs-sum div { display: grid; grid-template-columns: 18px minmax(0, 1fr); align-items: baseline; gap: 6px; font-size: 11.5px; }
.cab.studio-cab .cs-sum b { width: 16px; height: 16px; border-radius: 50%; font: 800 9px/16px var(--font, system-ui); text-align: center; background: var(--accent); color: #1a1206; }
.cab.studio-cab .cs-sum b.b { background: #9fd3ff; color: #0b1a26; }
.cab.studio-cab .cs-sum b.cab { border-radius: 4px; width: auto; padding: 0 3px; background: rgba(255,255,255,0.12); color: #d3d5da; }
.cab.studio-cab .cs-sum span { font-weight: 650; color: #e3e4e6; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.cab.studio-cab .cs-sum em { font-style: normal; color: #8d9097; font-weight: 500; }
.cab.studio-cab .cs-open { align-self: flex-start; height: 28px; padding: 0 12px; border-radius: 7px; border: 0; background: var(--accent); color: var(--accent-ink, #1a1206);
  font-size: 11.5px; font-weight: 700; cursor: pointer; }
.cab.studio-cab .cs-open:hover { filter: brightness(1.07); }
.cab.studio-cab .mic.b b { background: #9fd3ff; }
.cab.studio-cab .mic.room { width: 24px; height: 24px; margin: -12px 0 0 -12px; }
.cab.studio-cab .mic.room b { background: #c9c3b6; }
.cab.studio-cab .mic .cable { position: absolute; left: 50%; top: 100%; width: 2px; height: 60px; margin-left: -1px;
  background: linear-gradient(180deg, #111, rgba(17,17,17,0)); z-index: -1; }
.cab.studio-cab .cs-chip { display: inline-flex; align-items: center; gap: 6px; font-size: 10px; font-weight: 700; letter-spacing: 0.4px;
  text-transform: uppercase; padding: 3px 7px; border-radius: 5px; background: rgba(255,255,255,0.06); color: #c9cbd0; }
.cab.studio-cab .cs-chip.warn { background: rgba(239,90,76,0.16); color: #ffb0a6; }
.cab.studio-cab .cs-chip.approx { background: rgba(227,160,75,0.16); color: #f2c58a; }
.cab.studio-cab .cs-row { display: grid; grid-template-columns: 46px minmax(0, 1fr); align-items: center; gap: 8px; }
.cab.studio-cab .cs-row > .cs-k { font-size: 9.5px; font-weight: 800; letter-spacing: 1.2px; color: #8d9097; text-transform: uppercase; }
.cab.studio-cab .cs-row { align-items: start; }
.cab.studio-cab .cs-row > .cs-k { padding-top: 13px; }
.cab.studio-cab .cs-row .ctl.sel { width: 100%; align-items: stretch; }
.cab.studio-cab .cs-row .ctl.sel .sel-box { width: auto; max-width: none; margin: 4px 0 2px; }
.cab.studio-cab .cs-row .ctl.sel .sel-v { text-align: left; padding: 0 6px; font-size: 11px; }
.cab.studio-cab .cs-row .ctl.sel > .ctl-lbl { display: none; }
.cab.studio-cab .cs-mic { display: flex; flex-direction: column; gap: 5px; min-width: 0; }
.cab.studio-cab .cs-mic .ctl.btnrow { align-items: flex-start; }
.cab.studio-cab .cs-mic .ctl.btnrow .ctl-lbl, .cab.studio-cab .cs-mic .ctl.btnrow .ctl-val { display: none; }
.cab.studio-cab .cs-note { font-size: 10.5px; color: #9a9da4; line-height: 1.35; }
.cab.studio-cab .cs-fixed { font-size: 10.5px; color: #8d9097; }
.cab.studio-cab .cs-knobs { display: flex; flex-wrap: wrap; justify-content: space-between; gap: 2px 4px; padding-top: 2px; border-top: 1px solid rgba(255,255,255,0.06); }
.cab.studio-cab .cs-pair { align-self: flex-start; font-size: 11px; font-weight: 650; height: 24px; padding: 0 9px; border-radius: 6px;
  border: 1px solid rgba(255,255,255,0.14); background: rgba(255,255,255,0.04); color: #e3e4e6; cursor: pointer; }
.cab.studio-cab .cs-pair:hover { background: rgba(255,255,255,0.09); }
.cab .cs-convert { height: 22px; padding: 0 8px; border-radius: 6px; border: 1px solid rgba(255,255,255,0.16); background: rgba(255,255,255,0.05);
  color: #e3e4e6; font-size: 10.5px; font-weight: 650; cursor: pointer; flex-shrink: 0; }
.cab .cs-convert:hover { background: rgba(255,255,255,0.1); }
.ua .ua-tools .ua-cabmics { height: 22px; padding: 0 9px; border: 0; border-radius: 6px; background: rgba(12, 12, 14, 0.62); color: #f1f1f3; font: 650 10.5px/22px var(--font, system-ui);
  box-shadow: inset 0 0 0 1px rgba(255,255,255,0.14); -webkit-backdrop-filter: blur(6px); backdrop-filter: blur(6px); cursor: pointer; white-space: nowrap; }
.ua .ua-tools .ua-cabmics:hover { background: var(--accent); color: var(--accent-ink, #1a1206); }
.sr-card .ua-tools .ua-cabmics { display: none; }
.cab.studio-cab .cs-approx { font-size: 10.5px; color: #f2c58a; line-height: 1.35; }
.cab.studio-cab .cs-rs { border-top: 1px solid rgba(255,255,255,0.06); padding-top: 7px; }
.cab.studio-cab .cs-rs-h { display: flex; align-items: center; gap: 7px; font-size: 9.5px; font-weight: 800; letter-spacing: 1.2px; color: #8d9097; text-transform: uppercase; }
.cab.studio-cab .cs-rs-h small { margin-left: auto; font-size: 9.5px; font-weight: 600; letter-spacing: 0.2px; text-transform: none; color: #6f727a; }
.cab.studio-cab .cs-rs.on .cs-rs-h { color: #d3d5da; }
.cab.studio-cab .cs-rs .led { width: 6px; height: 6px; }
.cab.studio-cab .cs-rs .led.off { background: radial-gradient(circle at 40% 35%, #55585e 0, #2a2c30 70%); box-shadow: 0 0 0 1px rgba(0,0,0,0.55); }
.cab.studio-cab .cs-knobs.rs { border-top: none; padding-top: 0; }
.cab.studio-cab .cs-rs-k { transition: opacity 0.15s; }
.cab.studio-cab .cs-rs-k.dim { opacity: 0.42; }
.cab .cs-ir-row { display: flex; align-items: center; gap: 8px; margin: 2px 0 6px; }
.cab .cs-ir-row .cs-import { display: inline-flex; align-items: center; gap: 6px; height: 24px; }
.cab .cs-ir-row .cs-import svg { width: 11px; height: 11px; }
.cab .cs-ir-row small { font-size: 10px; color: #8d9097; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }

.hr-tempo { display: flex; align-items: center; gap: 2px; padding: 2px; border-radius: 7px; background: var(--inset); box-shadow: inset 0 0 0 1px var(--line);
  flex-shrink: 0; -webkit-app-region: no-drag; }
.hr-tempo .tp-k { font-size: 9.5px; font-weight: 800; letter-spacing: 1px; color: var(--text-3, #7b7e86); padding: 0 4px 0 6px; }
.hr-tempo button { height: 24px; min-width: 22px; border: none; border-radius: 5px; background: transparent; color: var(--text-2); font-size: 13px; font-weight: 700; padding: 0 5px; cursor: pointer; }
.hr-tempo button:hover { background: var(--hover, rgba(255,255,255,0.06)); color: var(--text); }
.hr-tempo .tp-tap { font-size: 10px; letter-spacing: 1px; padding: 0 8px; background: var(--card-2, rgba(255,255,255,0.06)); color: var(--text); box-shadow: inset 0 0 0 1px var(--line-2); }
.hr-tempo .tp-tap.hit { background: var(--accent); color: var(--accent-ink, #1a1206); }
.hr-tempo input { width: 46px; height: 24px; border: none; background: transparent; color: var(--text); font: 650 13px/1 var(--mono, ui-monospace, SFMono-Regular, Menlo, monospace);
  text-align: center; font-variant-numeric: tabular-nums; outline: none; border-radius: 5px; }
.hr-tempo input:focus { background: var(--card-2, rgba(255,255,255,0.06)); box-shadow: inset 0 0 0 1px var(--accent); }
.hr-tempo .tp-led { width: 7px; height: 7px; border-radius: 50%; margin: 0 6px 0 3px; background: var(--accent); opacity: 0.18; animation: hrBeat var(--beat, 0.5s) steps(1, end) infinite; }
.hr-tempo.host .tp-k::after { content: " HOST"; color: var(--accent); }
.hr-tempo.host button, .hr-tempo.host input { pointer-events: none; opacity: 0.55; }
@keyframes hrBeat { 0% { opacity: 1; box-shadow: 0 0 8px var(--accent); } 18% { opacity: 0.18; box-shadow: none; } }
`;
  const style = document.createElement("style");
  style.textContent = CSS;
  document.head.appendChild(style);

  /* ───────────────────────── Cabinet unit ───────────────────────── */
  const LAYOUT = { // speakers → columns × rows, grille height, speaker diameter
    "1x12": [1, 1, 168, 132], "2x12": [2, 1, 176, 140], "4x12": [2, 2, 270, 112], "4x10": [2, 2, 236, 98],
    "8x10": [4, 2, 236, 86], "1x15": [1, 1, 186, 156], "2x15": [2, 1, 186, 150],
  };
  // Where each recorded position sits on the speaker (fraction of the radius, angle).
  const POS = { cap: [0, 0], capedge: [0.42, -35], cone: [0.78, -35], room: [0, 0], fixed: [0.3, -35] };

  const P = (s, id) => (s.params || []).find((p) => p.id === id) || null;
  const RS = () => (typeof rackState !== "undefined" ? rackState : null); // index.html's top-level `let`

  // Always a drop-down (index.html renders 2–3 option selectors as toggles): same markup and
  // data-kind as its own selector, so its menu / search popover / keyboard handling applies.
  function selHTML(s, p, label) {
    return `<div class="ctl sel" data-kind="sel" role="slider" aria-valuetext="${esc(displayVal(p))}" ${ctlAttrs(s, p)}>
      <div class="sel-box"><button tabindex="-1" data-d="-1" aria-label="Previous">‹</button><span class="sel-v">${esc(displayVal(p))}</span><button tabindex="-1" data-d="1" aria-label="Next">›</button></div><span class="ctl-lbl">${esc(label || p.name)}</span></div>`;
  }
  const KNOB = (s, id, label) => { const p = P(s, id); return p ? ctlHTML(s, p, { knob: "alu", size: 30, scale: "arc", label }) : ""; };

  function micIcon(cls, label, x, y, opacity, title, m) {
    const art = m && window.HRRoom && HRRoom.micArt ? HRRoom.micArt(m) : "";   // each mic drawn as itself (assets/stage-room.js)
    return `<span class="mic ${cls}${art ? " art" : ""}" style="left:${x.toFixed(1)}%;top:${y.toFixed(1)}%;opacity:${opacity.toFixed(2)}" title="${esc(title)}">${art || MIC_SVG}<b>${esc(label)}</b>${art ? "" : `<i class="cable"></i>`}</span>`;
  }

  function grilleHTML(s, cb, ampD) {
    const C = RENDER.compact;
    const [cols, rows, gh0, sd0] = LAYOUT[cb.speakers] || LAYOUT["4x12"];
    const gh = C ? Math.min(132, gh0) : gh0, sd = C ? Math.round(sd0 * 0.72) : sd0;
    const inherit = ampD && ampD.form === "amp";
    const grille = inherit ? (ampD.cabGrille || ampD.grille || "g-dark") : "g-dark";
    let spk = "";
    for (let k = 0; k < cols * rows; k++) spk += `<span class="spk"></span>`;
    // Speaker centres in % of the grille box (spk-wrap inset 10px/14px ≈ 4%).
    const centre = (k) => [4 + ((k % cols) + 0.5) * (92 / cols), 5 + (Math.floor(k / cols) + 0.5) * (90 / rows)];
    const place = (k, pos) => {
      const [fr, ang] = POS[pos] || POS.fixed;
      const [cx, cy] = centre(k);
      const rPctX = (sd / 2) / Math.max(1, 560 / cols) * 100 / cols * fr; // approximate: speaker radius in % of width
      const rPctY = (sd / 2) / gh * 100 * fr;
      const a = (ang * Math.PI) / 180;
      return [cx + Math.cos(a) * rPctX * cols, cy + Math.sin(a) * rPctY];
    };
    let mics = "";
    const pB = P(s, "blend"), blend = pB ? pB.norm : 0.5;
    // both mics on the first speaker, where the Cab & mics panel shows them; the quieter one in the blend a little dimmer
    if (cb.mic_a) {
      const [x, y] = place(0, cb.mic_a.position);
      mics += micIcon("a", "A", x, y, cb.mic_b ? 0.6 + 0.4 * Math.cos(blend * Math.PI / 2) : 1, `Mic A: ${cb.mic_a.label} · ${cb.mic_a.position_label}`, cb.mic_a);
    }
    if (cb.mic_b) {
      let [x, y] = place(0, cb.mic_b.position);
      const cx0 = 4 + 0.5 * (92 / cols);
      x = 2 * cx0 - x + (cb.mic_a && cb.mic_a.position === cb.mic_b.position && cb.mic_b.position === "cap" ? -3 : 0); y += 4;   // mirrored: B comes in from the other side
      mics += micIcon("b", "B", x, y, 0.6 + 0.4 * Math.sin(blend * Math.PI / 2), `Mic B: ${cb.mic_b.label} · ${cb.mic_b.position_label}`, cb.mic_b);
    }
    const pr = P(s, "room");
    if (cb.room_available && pr && pr.norm > 0.01) mics += micIcon("room", "Room", 88, 10, 0.5 + 0.5 * pr.norm, `Room mic ${pr.fmt || ""}`);
    const brand = C ? "" : `<div class="cab-brand">${esc(cb.speakers || "")} cabinet<small>${esc(cb.label || "")}</small></div>`;
    return `<div class="cab-grille" style="--gh:${gh}px" data-cab-open="${s.slot_idx}" title="Cab &amp; mics — choose the cabinet and place the mics">
        <div class="spk-wrap" style="--sc:${cols};--sr:${rows};--sd:${sd}px">${spk}</div>
        <div class="cloth ${grille}"></div><div class="vign"></div>${brand}${mics}
      </div>`;
  }

  function chip(cb) {
    if (cb.status === "no_content") return `<span class="cs-chip warn">No cabinet content installed</span>`;
    if (cb.status === "missing") return `<span class="cs-chip warn" title="${esc(cb.stored_cab_id)}">Cabinet not installed on this machine</span>`;
    if (cb.approximation) return `<span class="cs-chip approx">Approximation · no IR</span>`;
    if (cb.kind === "matched") return `<span class="cs-chip">Matched to amp · 2 mics</span>`;
    if (cb.kind === "capture") return `<span class="cs-chip">IR capture · ${esc(cb.group)}</span>`;
    return `<span class="cs-chip">Studio cabinet · recorded mics</span>`;
  }

  function summaryHTML(s, cb) {
    const fixed = cb.kind === "matched" || cb.kind === "capture";
    const line = (L, m) => `<div><b class="${L.toLowerCase()}">${L}</b><span>${m ? esc(m.label.replace(/\s*\((Close|Matched)[^)]*\)\s*$/, "")) : "off"}${m ? ` <em>· ${esc(fixed ? "fixed by the capture" : m.position_label || "")}</em>` : ""}</span></div>`;
    const rows = RENDER.compact ? line("A", cb.mic_a) : line("A", cb.mic_a) + (cb.kind === "capture" ? "" : line("B", cb.mic_b));
    return `<div class="cs-sum">${rows}</div><button class="cs-open" data-cab-open="${s.slot_idx}" title="Choose the cabinet, the microphones and where they sit">Cab &amp; mics…</button>`;
  }
  function micBlock(s, which, cb) {
    const mic = which === "A" ? cb.mic_a : cb.mic_b;
    const pm = P(s, which === "A" ? "micA" : "micB"), pp = P(s, which === "A" ? "posA" : "posB");
    if (!pm) return "";
    let pos = "";
    if (mic && pp) {
      const n = Array.isArray(pp.labels) ? pp.labels.length : 0;
      pos = n > 1 ? ctlHTML(s, pp, { btnrow: true }) : `<span class="cs-fixed">${esc(mic.position_label)}${mic.position === "fixed" ? "" : " only"}</span>`;
    }
    const note = mic && mic.note && !RENDER.compact ? `<div class="cs-note">${esc(mic.note)}</div>` : "";
    // Mic B carries its own blend / distance / polarity controls.
    const extra = which === "B" && mic && !RENDER.compact
      ? `<div class="cs-knobs b">${KNOB(s, "blend", "A ↔ B")}${KNOB(s, "alignB", "Distance")}${KNOB(s, "phaseB", "Invert")}</div>` : "";
    return `<div class="cs-row"><span class="cs-k">Mic ${which}</span><div class="cs-mic">${selHTML(s, pm)}${pos}${note}${extra}</div></div>`;
  }

  function cabHTML(s, d, i, ampD) {
    const cb = s.cabinet || { status: "ok", speakers: "4x12" };
    const C = RENDER.compact;
    const inherit = ampD && ampD.form === "amp";
    const tolex = inherit ? ampD.tolex : "#171717";
    const pipe = inherit ? ampD.pipe : "rgba(255,255,255,0.16)";
    const head = `<div class="cab-head"><button class="pwr-btn" data-act="bypass" title="Power: bypass" aria-label="Power">${PWR_SVG}</button>
      <div style="flex:1;min-width:0"><div class="cab-title">${esc(s.custom_name || "Cabinet")}</div><div class="cab-sub">${esc(cb.label || s.display_name)}</div></div></div>`;
    // Cabinet, mics and positions are chosen in one place, the Cab & mics panel (assets/stage-room.js); the plate
    // says what's on it and keeps the tone controls.
    let plate = head + `<div>${chip(cb)}</div>` + summaryHTML(s, cb);
    if (!C) {
      if (cb.approximation && cb.approximation_note) plate += `<div class="cs-approx">${esc(cb.approximation_note)}</div>`;
      const knobs = (cb.room_available ? KNOB(s, "room", "Room") : "") + KNOB(s, "width", "Width") + KNOB(s, "lowcut", "Low cut") + KNOB(s, "highcut", "High cut") + KNOB(s, "level", "Level");
      plate += `<div class="cs-knobs">${knobs}</div>`;
      plate += roomStereoHTML(s);
    }
    return `<div class="${unitCls(s, `cab studio-cab${C ? " compact" : ""}`)}" data-unit="${s.slot_idx}" style="--i:${i};--tolex:${tolex};--pipe:${pipe};--plate-w:${C ? 260 : 372}px">
      <div class="hw-body">${grilleHTML(s, cb, ampD)}<div class="cab-plate">${plate}</div></div>${drawerHTML(s)}${XBTN}</div>`;
  }

  /* Room & Stereo — engine params tagged section "Room & Stereo" (doubler, its tightness, amp in the room, de-mic).
     Nothing renders until the engine reports them. 0 = off for doubler / air / de-mic. */
  const RS_ORDER = ["doubler", "doublerDelay", "air", "demic"];
  const RS_LABEL = { doubler: "Doubler", doublerDelay: "Tightness", air: "Amp in room", demic: "De-mic" };
  const RS_SWITCHED = ["doubler", "air", "demic"];
  function roomStereoParams(s) {
    return (s.params || []).filter((p) => /room\s*&\s*stereo/i.test(p.section || ""))
      .sort((a, b) => ((RS_ORDER.indexOf(a.id) + 1) || 99) - ((RS_ORDER.indexOf(b.id) + 1) || 99));
  }
  const rsOff = (s, id) => { const p = P(s, id); return !p || p.norm <= 0.001; };
  function roomStereoHTML(s) {
    const ps = roomStereoParams(s);
    if (!ps.length) return "";
    const on = RS_SWITCHED.some((id) => !rsOff(s, id));
    const parts = [!rsOff(s, "doubler") && "doubled", !rsOff(s, "air") && "in the room"].filter(Boolean);
    const knobs = ps.map((p) => {
      const dim = p.id === "doublerDelay" && rsOff(s, "doubler");
      return `<div class="cs-rs-k${dim ? " dim" : ""}" title="${esc(p.name)}${RS_SWITCHED.includes(p.id) ? " — turn fully down for off" : ""}">${ctlHTML(s, p, { knob: "alu", size: 30, scale: "arc", label: RS_LABEL[p.id] || p.name })}</div>`;
    }).join("");
    return `<div class="cs-rs${on ? " on" : ""}"><div class="cs-rs-h"><i class="led${on ? " green" : " off"}"></i>Room &amp; Stereo<small>${parts.length ? esc(parts.join(" · ")) : "off"}</small></div><div class="cs-knobs rs">${knobs}</div></div>`;
  }

  // The grille or "Cab & mics…" on a Cabinet faceplate (chain, sheet or the Room's card) → the Cab & mics panel.
  document.addEventListener("click", (e) => {
    const o = e.target.closest && e.target.closest("[data-cab-open]");
    if (!o || !window.HRRoom || !HRRoom.micPanel) return;
    e.preventDefault(); e.stopPropagation();
    const slot = +o.dataset.cabOpen;
    HRRoom.micPanel(slot, o.closest("[data-unit]"));
  }, true);
  document.addEventListener("click", async (e) => {
    const b = e.target.closest && e.target.closest("[data-cab-pair]");
    if (!b) return;
    e.preventDefault();
    e.stopPropagation();
    try {
      const r = await fetch(`${API_BASE}/api/cabinet?slot=${b.dataset.slot}&cab=${encodeURIComponent(b.dataset.cabPair)}`, { cache: "no-store" });
      const d = await r.json();
      if (d && d.slots) applyRackState(d);
      else if (typeof toast === "function") toast("This cabinet needs the updated engine.");
    } catch (err) { /* engine offline */ }
  }, true);

  /* Legacy cabinet units (Amp-Matched Cab, Classic Amp-Matched Cab, Studio Cab Room, Cabinet IR Loader): one click
     swaps them for an equivalent Cabinet unit, where mics and positions can be chosen. */
  const CONVERTIBLE = new Set([156000, 88000, 119000, 168000]);
  const IR_LOADER = 168000;
  const bridge = () => !!(window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.headroom);
  /* IR Loader: bring your own .wav impulse responses (native picker in the app; they land in the User IRs folder). */
  function importIr() {
    if (bridge()) { window.webkit.messageHandlers.headroom.postMessage({ action: "importIr" }); return; }
    fetch(`${API_BASE}/api/irs?rescan=1`, { cache: "no-store" }).then((r) => r.json()).then((d) => {
      if (typeof toast === "function") toast(d && d.folder ? `Importing works in the Headroom app — or copy .wav files into ${d.folder.replace(/^\/Users\/[^/]+/, "~")}` : "Importing IRs works in the Headroom app.");
    }).catch(() => {});
  }
  function revealIrFolder() { if (bridge()) window.webkit.messageHandlers.headroom.postMessage({ action: "revealIrFolder" }); }
  const irCount = { n: null };
  function refreshIrCount() {
    fetch(`${API_BASE}/api/irs`, { cache: "no-store" }).then((r) => r.json()).then((d) => { if (d && typeof d.count === "number") { irCount.n = d.count; document.querySelectorAll(".cs-ir-n").forEach((x) => { x.textContent = irCountTxt(); }); } }).catch(() => {});
  }
  const irCountTxt = () => (irCount.n == null ? "" : irCount.n ? `${irCount.n} of your own in the list` : "Yours appear in the IR list");
  window.headroomIrImported = function (count) {
    if (typeof toast === "function") toast(count ? `Imported ${count} IR${count === 1 ? "" : "s"} — pick ${count === 1 ? "it" : "them"} in the IR Loader` : "No .wav files were imported.");
    refreshIrCount();
    if (typeof scheduleRackRefresh === "function") scheduleRackRefresh(150);
  };
  async function convert(slotIdx) {
    try {
      const r = await fetch(`${API_BASE}/api/cabinet/convert?slot=${slotIdx}`, { cache: "no-store" });
      const d = await r.json();
      if (d && d.slots) applyRackState(d);
      else if (typeof toast === "function") toast(d && d.error ? "This cabinet can't be converted." : "Converting needs the updated engine.");
    } catch (e) { /* offline */ }
  }
  function menuItems(s) {
    if (!s) return [];
    const out = [];
    if (s.table_id === IR_LOADER) {
      out.push({ label: "Import .wav IRs…", sub: bridge() ? "" : "In the app", onClick: importIr });
      if (bridge()) out.push({ label: "Show User IRs folder", onClick: revealIrFolder });
    }
    if (s.table_id === 900100 && window.HRRoom && HRRoom.micPanel) out.push({ label: "Cab & mics…", onClick: () => HRRoom.micPanel(s.slot_idx) });
    if (CONVERTIBLE.has(s.table_id)) out.push({ label: "Cab & mics: make it mikeable", onClick: () => { if (window.HRRoom && HRRoom.micPanel) HRRoom.micPanel(s.slot_idx, null, { convert: true }); else convert(s.slot_idx); } });
    return out;
  }
  const IMPORT_SVG = `<svg viewBox="0 0 12 12" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"><path d="M6 1.8v6M3.6 5.6 6 8l2.4-2.4M2 10.2h8"/></svg>`;
  const openMikeable = (slot, el) => { if (window.HRRoom && HRRoom.micPanel) HRRoom.micPanel(slot, el, { convert: true }); else convert(slot); };
  function decorate() {
    // classic cabs drawn as illustrated faces: a "Cab & mics" button with the power button (shows on hover)
    document.querySelectorAll("#rack-bay .unit.ua[data-ua-tid], #sheet-body .unit.ua[data-ua-tid]").forEach((u) => {
      if (!CONVERTIBLE.has(+u.dataset.uaTid)) return;
      const tools = u.querySelector(".ua-tools");
      if (!tools || tools.querySelector(".ua-cabmics")) return;
      const b = document.createElement("button");
      b.className = "ua-cabmics";
      b.textContent = "Cab & mics";
      b.title = "Make it mikeable: swaps in the Cabinet unit (same sound to start), puts real mics on the same speakers and opens Cab & mics";
      b.addEventListener("click", (e) => { e.stopPropagation(); openMikeable(+u.dataset.unit, u); });
      tools.appendChild(b);
    });
    document.querySelectorAll("#rack-bay .unit.cab[data-unit], #sheet-body .unit.cab[data-unit]").forEach((u) => {
      if (u.classList.contains("studio-cab")) return;
      const s = typeof getSlot === "function" ? getSlot(Number(u.dataset.unit)) : null;
      const head = u.querySelector(".cab-head");
      if (!s || !head) return;
      if (s.table_id === IR_LOADER && !u.querySelector(".cs-ir-row") && !u.classList.contains("compact")) {
        const row = document.createElement("div");
        row.className = "cs-ir-row";
        row.innerHTML = `<button class="cs-convert cs-import" title="Choose .wav impulse responses (files or folders) to add to the IR list">${IMPORT_SVG}Import .wav…</button><small class="cs-ir-n">${irCountTxt()}</small>`;
        row.querySelector("button").addEventListener("click", (e) => { e.stopPropagation(); importIr(); });
        head.insertAdjacentElement("afterend", row);
        if (irCount.n == null) refreshIrCount();
      }
      if (CONVERTIBLE.has(s.table_id) && !head.querySelector(".cs-mics")) {
        const b = document.createElement("button");
        b.className = "cs-convert cs-mics";
        b.textContent = "Cab & mics…";
        b.title = "Make it mikeable: swaps in the Cabinet unit (same sound to start), puts real mics on the same speakers and opens Cab & mics";
        b.addEventListener("click", (e) => { e.stopPropagation(); openMikeable(s.slot_idx, u); });
        head.appendChild(b);
      }
    });
  }
  const bayObserver = new MutationObserver(() => decorate());
  const watchBay = () => { const bay = document.getElementById("rack-bay"); if (bay) { bayObserver.observe(bay, { childList: true }); decorate(); } };
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", watchBay); else watchBay();

  /* ───────────────────────── Rig tempo ───────────────────────── */
  const tempo = { el: null, input: null, bpm: 120, editing: false, taps: [], sentAt: 0 };

  function fmtBpm(v) { return (Math.round(v * 10) / 10).toFixed(1); }
  function paint() {
    if (!tempo.el) return;
    if (!tempo.editing) tempo.input.value = fmtBpm(tempo.bpm);
    tempo.el.style.setProperty("--beat", `${(60 / Math.max(20, tempo.bpm)).toFixed(4)}s`);
  }
  async function send(bpm) {
    bpm = Math.min(300, Math.max(20, Math.round(bpm * 10) / 10));
    tempo.bpm = bpm;
    tempo.sentAt = Date.now();
    const rs = RS();
    if (rs && rs.tempo) rs.tempo.bpm = rs.tempo.rig_bpm = bpm;
    paint();
    try { await fetch(`${API_BASE}/api/tempo?bpm=${bpm}`, { cache: "no-store" }); } catch (e) { /* offline */ }
  }
  function tap() {
    const now = performance.now();
    if (tempo.taps.length && now - tempo.taps[tempo.taps.length - 1] > 2000) tempo.taps = [];
    tempo.taps.push(now);
    if (tempo.taps.length > 6) tempo.taps.shift();
    const btn = tempo.el.querySelector(".tp-tap");
    btn.classList.add("hit");
    setTimeout(() => btn.classList.remove("hit"), 90);
    if (tempo.taps.length >= 2) {
      const n = tempo.taps.length - 1;
      send(60000 / ((tempo.taps[n] - tempo.taps[0]) / n));
    }
  }
  function mountTempo() {
    const bar = document.querySelector(".topbar");
    if (!bar || document.querySelector(".hr-tempo")) return;
    const el = document.createElement("div");
    el.className = "hr-tempo no-drag";
    el.title = "Rig tempo: delays, LFOs, the gate and synced predelays follow it when their Sync is on. Saved with the rig.";
    el.innerHTML = `<span class="tp-k">BPM</span><button data-d="-1" aria-label="Slower">−</button><input aria-label="Tempo (BPM)" inputmode="decimal" spellcheck="false" value="120.0"><button data-d="1" aria-label="Faster">+</button><button class="tp-tap" title="Tap tempo">TAP</button><i class="tp-led" aria-hidden="true"></i>`;
    const slot = document.getElementById("tb-tempo-slot");   // index.html's header reserves a slot (it folds into ⋯ when narrow)
    if (slot) slot.appendChild(el);
    else { const anchor = bar.querySelector(".tb-right"); bar.insertBefore(el, anchor || null); }
    tempo.el = el;
    tempo.input = el.querySelector("input");
    el.addEventListener("click", (e) => {
      const b = e.target.closest("button");
      if (!b) return;
      if (b.classList.contains("tp-tap")) tap();
      else send(tempo.bpm + Number(b.dataset.d) * (e.shiftKey ? 0.1 : 1));
    });
    tempo.input.addEventListener("focus", () => { tempo.editing = true; tempo.input.select(); });
    tempo.input.addEventListener("blur", () => {
      tempo.editing = false;
      const v = parseFloat(tempo.input.value);
      if (isFinite(v) && Math.abs(v - tempo.bpm) > 1e-6) send(v); else paint();
    });
    tempo.input.addEventListener("keydown", (e) => {
      e.stopPropagation(); // keep rig shortcuts (space, [, ]) out of the field
      if (e.key === "Enter") tempo.input.blur();
      else if (e.key === "Escape") { tempo.input.value = fmtBpm(tempo.bpm); tempo.input.blur(); }
      else if (e.key === "ArrowUp" || e.key === "ArrowDown") {
        e.preventDefault();
        send(tempo.bpm + (e.key === "ArrowUp" ? 1 : -1) * (e.shiftKey ? 0.1 : 1));
        tempo.input.value = fmtBpm(tempo.bpm);
      }
    });
    tempo.input.addEventListener("wheel", (e) => { e.preventDefault(); send(tempo.bpm + (e.deltaY < 0 ? 1 : -1)); }, { passive: false });
    paint();
    // Follow the engine (preset loads bring their own tempo; a plugin host may drive it).
    // HRActivity.every: no wakeups while the window / plugin editor is hidden (assets/activity.js).
    (window.HRActivity ? HRActivity.every.bind(HRActivity) : (ms, f) => setInterval(f, ms))(300, () => {
      const rs = RS(), t = rs && rs.tempo;
      if (!t || tempo.editing || Date.now() - tempo.sentAt < 800) return;
      el.classList.toggle("host", t.source === "host");
      if (typeof t.bpm === "number" && Math.abs(t.bpm - tempo.bpm) > 1e-6) { tempo.bpm = t.bpm; paint(); }
    });
  }
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", mountTempo);
  else mountTempo();

  // Part of index.html's structure key: a cabinet / mic / position change re-renders the unit (its
  // mic list, positions and grille drawing depend on the selection, not just on knob values).
  function sig(s) {
    const c = s && s.cabinet;
    if (!c) return "";
    const m = (x) => (x ? `${x.id}@${x.position}` : "-");
    const rs = roomStereoParams(s).length ? `|rs:${RS_SWITCHED.map((id) => (rsOff(s, id) ? 0 : 1)).join("")}` : "";   // on/off flips the LED, the dimmed knob and the caption
    return `|${c.status}|${c.cab_id}|${m(c.mic_a)}|${m(c.mic_b)}${rs}`;
  }
  window.HRCabinet = { cabHTML, sig, menuItems, convert, isCabinet: (s) => s && s.table_id === 900100,
    importIr, revealIrFolder };   // importIr / revealIrFolder: also used by Settings → Library & Content (assets/settings.js)
  window.HRTempo = { set: send, tap, get: () => tempo.bpm };
})();
