/* Headroom — right pane "Listen" (listen-pane.css). The engine analyses the DRY input (GET /api/listen):
 *   Tuner      always-on: note, cents (0.1), strobe strip, 3 s pitch trail in cents (fading tail, bright "now"
 *              head, ±3 ¢ band) with bend landing; strings become
 *              per-string meters when a strum lights every string (polyphonic). Expand → full tuner (needle /
 *              strobe / polyphonic LED columns, mute while tuning). Guitar 6/7/8, bass 4/5/6, ukulele, mandolin,
 *              violin, cello, banjo, chromatic; alternate tunings; A4 reference.
 *   Key        detected key + confidence, live chord + Roman numeral, progression (with a guess at what's next),
 *              scale picker + the scale's notes (lock or pick the key by hand). The neck view lives on the
 *              Fretboard page (fretboard-view.js), which reads the same settings through window.ListenPane.
 *   Tone       rig output spectrum over the dry guitar (or the classic scope).
 *   Input      level vs. the sweet spot, noise floor + gate threshold on one meter, plain-language advice.
 *   Chain      the old chain list (hidden by default).
 * Cards can be hidden and dragged into any order (persisted). API for Settings: window.ListenPane. */
(function () {
  "use strict";
  const LS_KEY = "hr.listen.v1";
  const CARDS = [
    { id: "tuner", label: "Tuner" },
    { id: "key", label: "Key & scale" },
    { id: "tone", label: "Tone" },
    { id: "input", label: "Input" },
    { id: "chain", label: "Chain" },
  ];
  const DEF = { order: CARDS.map((c) => c.id), hidden: ["chain"], inst: "guitar6", tuning: "std", a4: 440, mode: "auto", display: "needle", scale: "pent", tone: "spectrum", keyLock: null, tunerMute: true };

  /* ---------- instruments ---------- */
  const INSTR = {
    guitar6: { label: "Guitar", t: { std: ["Standard", "E2 A2 D3 G3 B3 E4"], dropd: ["Drop D", "D2 A2 D3 G3 B3 E4"], eb: ["E♭ Standard", "Eb2 Ab2 Db3 Gb3 Bb3 Eb4"], d: ["D Standard", "D2 G2 C3 F3 A3 D4"], dropcs: ["Drop C♯", "C#2 G#2 C#3 F#3 A#3 D#4"], dropc: ["Drop C", "C2 G2 C3 F3 A3 D4"], c: ["C Standard", "C2 F2 Bb2 Eb3 G3 C4"], dropb: ["Drop B", "B1 F#2 B2 E3 G#3 C#4"], b: ["B Standard", "B1 E2 A2 D3 F#3 B3"], dadgad: ["DADGAD", "D2 A2 D3 G3 A3 D4"], openg: ["Open G", "D2 G2 D3 G3 B3 D4"], opend: ["Open D", "D2 A2 D3 F#3 A3 D4"], opene: ["Open E", "E2 B2 E3 G#3 B3 E4"], openc: ["Open C", "C2 G2 C3 G3 C4 E4"] } },
    guitar7: { label: "7-string", t: { std: ["Standard", "B1 E2 A2 D3 G3 B3 E4"], dropa: ["Drop A", "A1 E2 A2 D3 G3 B3 E4"], a: ["A Standard", "A1 D2 G2 C3 F3 A3 D4"] } },
    guitar8: { label: "8-string", t: { std: ["Standard", "F#1 B1 E2 A2 D3 G3 B3 E4"], drope: ["Drop E", "E1 B1 E2 A2 D3 G3 B3 E4"] } },
    bass4: { label: "Bass", t: { std: ["Standard", "E1 A1 D2 G2"], dropd: ["Drop D", "D1 A1 D2 G2"], eb: ["E♭ Standard", "Eb1 Ab1 Db2 Gb2"], d: ["D Standard", "D1 G1 C2 F2"], bead: ["BEAD", "B0 E1 A1 D2"] } },
    bass5: { label: "5-string bass", t: { std: ["Standard", "B0 E1 A1 D2 G2"], high: ["High C", "E1 A1 D2 G2 C3"] } },
    bass6: { label: "6-string bass", t: { std: ["Standard", "B0 E1 A1 D2 G2 C3"] } },
    uke: { label: "Ukulele", t: { std: ["Standard (re-entrant)", "G4 C4 E4 A4"], lowg: ["Low G", "G3 C4 E4 A4"], bari: ["Baritone", "D3 G3 B3 E4"] } },
    mandolin: { label: "Mandolin", t: { std: ["Standard", "G3 D4 A4 E5"] } },
    violin: { label: "Violin", t: { std: ["Standard", "G3 D4 A4 E5"] } },
    cello: { label: "Cello", t: { std: ["Standard", "C2 G2 D3 A3"] } },
    banjo: { label: "Banjo", t: { std: ["Open G", "G4 D3 G3 B3 D4"] } },
    chromatic: { label: "Chromatic", t: { any: ["Any instrument", ""] } },
  };
  const PC = { C: 0, D: 2, E: 4, F: 5, G: 7, A: 9, B: 11 };
  const parseNote = (s) => { const m = /^([A-G])([#b]?)(-?\d)$/.exec(s); if (!m) return null; return 12 * (+m[3] + 1) + PC[m[1]] + (m[2] === "#" ? 1 : m[2] === "b" ? -1 : 0); };

  /* ---------- note & chord spelling ---------- */
  const SH = ["C", "C♯", "D", "D♯", "E", "F", "F♯", "G", "G♯", "A", "A♯", "B"];
  const FL = ["C", "D♭", "D", "E♭", "E", "F", "G♭", "G", "A♭", "A", "B♭", "B"];
  const MIX = ["C", "C♯", "D", "E♭", "E", "F", "F♯", "G", "A♭", "A", "B♭", "B"];
  const FLAT_MAJ = new Set([5, 10, 3, 8, 1, 6]), FLAT_MIN = new Set([2, 7, 0, 5, 10, 3]);
  function names(key) { if (!key) return MIX; return (key.minor ? FLAT_MIN : FLAT_MAJ).has(key.root) ? FL : SH; }
  const Q = {
    maj: { iv: [0, 4, 7], s: "" }, min: { iv: [0, 3, 7], s: "m" }, "5": { iv: [0, 7], s: "5" }, "7": { iv: [0, 4, 7, 10], s: "7" },
    maj7: { iv: [0, 4, 7, 11], s: "maj7" }, m7: { iv: [0, 3, 7, 10], s: "m7" }, sus2: { iv: [0, 2, 7], s: "sus2" }, sus4: { iv: [0, 5, 7], s: "sus4" },
    dim: { iv: [0, 3, 6], s: "°" }, aug: { iv: [0, 4, 8], s: "+" }, m7b5: { iv: [0, 3, 6, 10], s: "m7♭5" }, add9: { iv: [0, 2, 4, 7], s: "add9" },
    "6": { iv: [0, 4, 7, 9], s: "6" }, m6: { iv: [0, 3, 7, 9], s: "m6" }, dim7: { iv: [0, 3, 6, 9], s: "°7" }, "9": { iv: [0, 2, 4, 7, 10], s: "9" }, m9: { iv: [0, 2, 3, 7, 10], s: "m9" },
  };
  const isMinorQ = (q) => ["min", "m7", "m6", "m9", "dim", "m7b5", "dim7"].includes(q);
  function chordParts(c, key) {
    const N = names(key), q = Q[c.quality] || Q.maj;
    let sfx = q.s, base = N[c.root];
    const main = sfx.startsWith("m") && !sfx.startsWith("maj") ? "m" : "";
    const sup = main ? sfx.slice(1) : sfx;
    return { base: base + main, sup, slash: c.bass != null && c.bass >= 0 && c.bass !== c.root ? "/" + N[c.bass] : "" };
  }
  function chordText(c, key) { const p = chordParts(c, key); return p.base + p.sup + p.slash; }
  const ROMAN = ["I", "II", "III", "IV", "V", "VI", "VII"];
  function roman(c, key) {
    if (!key) return "";
    const deg = (c.root - key.root + 12) % 12;
    const ref = key.minor ? [0, 2, 3, 5, 7, 8, 10] : [0, 2, 4, 5, 7, 9, 11];
    let i = ref.indexOf(deg), acc = "";
    if (i < 0) { const lo = ref.findIndex((r) => r === deg + 1); if (lo >= 0) { i = lo; acc = "♭"; } else { i = ref.findIndex((r) => r === deg - 1); acc = "♯"; } }
    if (i < 0) return "";
    let r = ROMAN[i];
    if (isMinorQ(c.quality)) r = r.toLowerCase();
    const tail = { dim: "°", dim7: "°7", m7b5: "ø7", aug: "+", "7": "7", maj7: "M7", m7: "7", "9": "9", m9: "9" }[c.quality] || "";
    return acc + r + tail;
  }
  const SCALES = {
    pent: { l: "Pent", min: [0, 3, 5, 7, 10], maj: [0, 2, 4, 7, 9] },
    blues: { l: "Blues", min: [0, 3, 5, 6, 7, 10], maj: [0, 2, 3, 4, 7, 9] },
    nat: { l: "Natural", lMaj: "Major", min: [0, 2, 3, 5, 7, 8, 10], maj: [0, 2, 4, 5, 7, 9, 11] },
    mode: { l: "Dorian", lMaj: "Mixo", min: [0, 2, 3, 5, 7, 9, 10], maj: [0, 2, 4, 5, 7, 9, 10] },
  };

  /* ---------- icons ---------- */
  const IC = {
    grip: '<svg class="lp-grip" viewBox="0 0 8 12" fill="currentColor"><circle cx="2" cy="2" r="1"/><circle cx="6" cy="2" r="1"/><circle cx="2" cy="6" r="1"/><circle cx="6" cy="6" r="1"/><circle cx="2" cy="10" r="1"/><circle cx="6" cy="10" r="1"/></svg>',
    hide: '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round"><path d="M1.5 8s2.4-4.5 6.5-4.5S14.5 8 14.5 8 12.1 12.5 8 12.5 1.5 8 1.5 8z"/><path d="M2.5 13.5l11-11"/></svg>',
    expand: '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5"><path d="M9.5 2.5h4v4M6.5 13.5h-4v-4M13.5 2.5L9 7M2.5 13.5L7 9"/></svg>',
    lock: '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5"><rect x="3.5" y="7" width="9" height="6.5" rx="1.5"/><path d="M5.5 7V5a2.5 2.5 0 015 0v2"/></svg>',
    unlock: '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5"><rect x="3.5" y="7" width="9" height="6.5" rx="1.5"/><path d="M5.5 7V5a2.5 2.5 0 014.8-1"/></svg>',
    close: '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6"><path d="M4 4l8 8M12 4l-8 8"/></svg>',
  };

  /* ---------- state ---------- */
  const S = {
    cfg: load(), d: null, ok: null, fails: 0, lastPoll: 0, polling: false, built: false,
    needle: 0, needleT: 0, strobe: 0, strobeBig: [0, 0, 0], lastFrame: 0, bandsIn: null, bandsOut: null,
    lastNote: null, holdT: 0, lastBendT: -1, wantT: -1e9, sentCfg: "", listeners: new Set(), tuner: { open: false, mutedByMe: false },
  };
  function load() { try { return Object.assign({}, DEF, JSON.parse(localStorage.getItem(LS_KEY) || "{}")); } catch (e) { return Object.assign({}, DEF); } }
  function save() { try { localStorage.setItem(LS_KEY, JSON.stringify(S.cfg)); } catch (e) { /* private mode */ } S.listeners.forEach((f) => { try { f(api.state()); } catch (e) { /* listener */ } }); if (S.built) setTimeout(kick); }   // tone / card changes can start or end the loops
  const $id = (id) => document.getElementById(id);
  const q1 = (sel, root) => (root || document).querySelector(sel);
  const escH = (s) => String(s).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
  const base = () => (typeof API_BASE !== "undefined" ? API_BASE : "http://127.0.0.1:17770");
  const getJSON = (p) => fetch(base() + p, { cache: "no-store" }).then((r) => { if (!r.ok) throw new Error(r.status); return r.json(); });
  const refOff = () => 12 * Math.log2((S.cfg.a4 || 440) / 440);  // semitones the reference shifts every target
  function inst() { return INSTR[S.cfg.inst] || INSTR.guitar6; }
  function tuningArr() { const t = inst().t[S.cfg.tuning] || Object.values(inst().t)[0]; return t[1] ? t[1].split(" ").map(parseNote) : []; }
  function tuningLabel() { const i = inst(), t = i.t[S.cfg.tuning] || Object.values(i.t)[0]; return S.cfg.inst === "chromatic" ? "Chromatic" : `${i.label} · ${t[0]}`; }
  function shortTuning() {
    const i = inst(), t = i.t[S.cfg.tuning] || Object.values(i.t)[0];
    if (S.cfg.inst === "chromatic") return "Chromatic";
    if (S.cfg.inst === "guitar6") return t[0];
    return t[0] === "Standard" ? i.label : `${i.label.replace("-string bass", "-str bass")} · ${t[0]}`;
  }
  function curKey() {
    if (S.cfg.keyLock) return S.cfg.keyLock;
    const k = S.d && S.d.key;
    return k && k.root != null ? { root: k.root, minor: !!k.minor } : null;
  }
  function noteHTML(midi, key, octave = true) {
    const pc = ((midi % 12) + 12) % 12, n = names(key)[pc], o = Math.floor(midi / 12) - 1;
    return `${n[0]}${n[1] ? `<span class="acc">${n[1]}</span>` : ""}${octave ? `<sub>${o}</sub>` : ""}`;
  }

  /* =====================================================================
     BUILD
     ===================================================================== */
  function hdr(card, title, right) {
    return `<header class="lp-hd" data-hd="${card}">${IC.grip}<span class="lp-hd-t">${title}</span><span class="lp-hd-r">${right || ""}<button class="lp-ib lp-hide" data-act="hide" title="Hide this card (bring it back from the bottom of the pane)" aria-label="Hide ${title}">${IC.hide}</button></span></header>`;
  }
  function build() {
    const pane = $id("lp");
    if (!pane || S.built) return;
    S.built = true;
    const card = (id) => q1(`.lp-card[data-card="${id}"]`, pane);

    // Tuner
    let ticks = "";
    for (let c = -50; c <= 50; c += 10) ticks += `<i class="tk${c === 0 ? " c" : ""}" style="left:${50 + c}%"></i>`;
    card("tuner").classList.add("lp-tun-card");
    card("tuner").innerHTML = hdr("tuner", "Tuner", `<button class="lp-pick" data-act="tuning" title="Instrument, tuning and reference pitch"></button><button class="lp-ib" data-act="expand" title="Open the full tuner (mutes the output while you tune)">${IC.expand}</button>`) +
      `<div class="lp-tun" data-st="none">
        <div class="lp-note" data-k="note">–</div>
        <div><div class="lp-strip"><div class="lp-strobe" data-k="strobe"></div><div class="zone"></div>${ticks}<div class="nd" data-k="nd" style="left:50%"></div></div>
        <div class="lp-tun-meta"><span data-k="cents">play a string</span><span data-k="hz"></span></div></div>
      </div>
      <div class="lp-trail-wrap"><canvas class="lp-trail" data-k="trail"></canvas><span class="lp-trail-tag" data-k="bend"></span></div>
      <div class="lp-strs" data-k="strs"></div>`;

    // the trail draws only while it is on screen (the pane scrolls; frame() skips it otherwise)
    if (window.IntersectionObserver) new IntersectionObserver((es) => { es.forEach((e) => { S.trVis = e.isIntersecting; }); }).observe(q1('[data-k="trail"]', card("tuner")));

    // Key
    card("key").innerHTML = hdr("key", "Key", `<span class="lp-chip idle" data-k="kstate"><i></i>listening</span><button class="lp-ib" data-act="lock" data-k="lockbtn" title="Lock the key">${IC.unlock}</button>`) +
      `<div class="lp-key"><div style="min-width:0"><button class="lp-key-name wait" data-act="pickkey" data-k="kname" title="Pick the key yourself">Listening…</button><div class="lp-key-alt" data-k="kalt">Play for a few seconds</div></div>
        <div class="lp-conf"><svg viewBox="0 0 38 38"><circle cx="19" cy="19" r="15.5" fill="none" stroke="var(--line-2)" stroke-width="3"/><circle class="v" data-k="confarc" cx="19" cy="19" r="15.5" fill="none" stroke="var(--accent)" stroke-width="3" stroke-linecap="round" stroke-dasharray="97.4" stroke-dashoffset="97.4"/></svg><span data-k="conf">–</span></div></div>
      <div class="lp-chord stale" data-k="chordbox"><div class="lp-chord-name" data-k="chord">–</div><div class="lp-chord-side"><span class="lp-roman" data-k="roman">strum a chord</span><span class="lp-tones" data-k="tones"></span></div></div>
      <div class="lp-prog" data-k="prog"></div>
      <div class="lp-sec"><span class="lp-seg lp-scales" data-k="scales" title="Scale (the Fretboard page shows it on the neck)"></span></div>
      <div class="lp-scale-notes" data-k="snotes"></div>`;

    // Tone (keeps the classic scope canvas the rest of the app draws into)
    const tone = card("tone"), scopeCv = $id("top-wave-canvas");
    tone.innerHTML = hdr("tone", "Tone", `<span class="lp-seg" data-k="toneview"><button data-tv="spectrum">Spectrum</button><button data-tv="scope">Scope</button></span>`) +
      `<canvas class="lp-spec" data-k="spec"></canvas><div data-k="scopeslot"></div>
      <div class="lp-bands" data-k="bands"></div><div class="lp-tone-read" data-k="toneread"></div>`;
    if (scopeCv) { scopeCv.classList.add("lp-scope"); q1('[data-k="scopeslot"]', tone).replaceWith(scopeCv); }

    // Input
    card("input").innerHTML = hdr("input", "Input", `<span class="lp-chip" data-k="peak">–</span>`) +
      `<div class="lp-meter"><div class="sweet" style="left:${pct(-18)}%;width:${pct(-6) - pct(-18)}%"></div><div class="floor" data-k="floor"></div><div class="gate" data-k="gate" hidden></div><div class="fill" data-k="fill"></div><div class="pk" data-k="pk"></div></div>
      <div class="lp-scale"><span style="left:0">−72</span><span class="g" data-k="gatelbl" style="left:${pct(-50)}%"></span><span style="left:${pct(-36)}%">−36</span><span class="s" style="left:${pct(-12)}%">sweet spot</span><span style="left:100%">0</span></div>
      <div class="lp-health idle" data-k="health"><i></i><span>Waiting for the engine…</span></div>
      <div class="lp-stats"><div><div class="k">Floor</div><div class="v" data-k="sfloor">–</div></div><div><div class="k">Latency</div><div class="v" data-k="slat">–</div></div><div><div class="k">CPU</div><div class="v" data-k="scpu">–</div></div></div>`;

    // Chain: header around the existing list (#chain-count / #topology-flow keep their handlers)
    const chain = card("chain"), list = $id("topology-flow"), cnt = $id("chain-count");
    const h = document.createElement("div");
    h.innerHTML = hdr("chain", "Chain", "");
    const head = h.firstElementChild;
    if (cnt) { cnt.classList.add("lp-chip"); q1(".lp-hd-r", head).prepend(cnt); }
    chain.prepend(head);
    if (list && list.parentElement !== chain) chain.append(list);

    wire(pane);
    applyLayout();
    paintStatic();
    sendCfg(true);
  }
  const pct = (db) => Math.max(0, Math.min(100, ((db + 72) / 72) * 100));

  /* ---------- layout: order / hidden ---------- */
  function applyLayout() {
    const pane = $id("lp");
    if (!pane) return;
    const known = CARDS.map((c) => c.id);
    const order = S.cfg.order.filter((id) => known.includes(id));
    known.forEach((id) => { if (!order.includes(id)) order.push(id); });
    S.cfg.order = order;
    order.forEach((id) => { const c = q1(`.lp-card[data-card="${id}"]`, pane); if (c) { pane.append(c); c.hidden = S.cfg.hidden.includes(id); } });
    const add = $id("lp-add");
    if (add) {
      const hid = CARDS.filter((c) => S.cfg.hidden.includes(c.id));
      add.hidden = !hid.length;
      add.innerHTML = hid.length ? `<span>Show</span>${hid.map((c) => `<button data-show="${c.id}" title="Show the ${escH(c.label)} card">+ ${escH(c.label)}</button>`).join("")}` : "";
    }
    if (typeof drawScope === "function") requestAnimationFrame(() => { try { drawScope(ui.lastWaveIn || [], ui.lastWaveOut || []); } catch (e) { /* scope */ } });
  }
  function setVisible(id, on) {
    const h = new Set(S.cfg.hidden);
    on ? h.delete(id) : h.add(id);
    S.cfg.hidden = [...h];
    save(); applyLayout();
  }
  function setOrder(ids) { S.cfg.order = ids.slice(); save(); applyLayout(); }
  function resetLayout() { S.cfg.order = DEF.order.slice(); S.cfg.hidden = DEF.hidden.slice(); save(); applyLayout(); }

  /* ---------- wiring ---------- */
  function wire(pane) {
    pane.addEventListener("click", (e) => {
      const b = e.target.closest("[data-act],[data-sc],[data-tv]");
      if (!b) return;
      const card = b.closest(".lp-card"), id = card && card.dataset.card;
      if (b.dataset.sc) { S.cfg.scale = b.dataset.sc; save(); paintStatic(); paintKey(); return; }
      if (b.dataset.tv) { S.cfg.tone = b.dataset.tv; save(); paintStatic(); return; }
      switch (b.dataset.act) {
        case "hide": setVisible(id, false); break;
        case "expand": openTuner(); break;
        case "tuning": tuningMenu(b); break;
        case "ref": refMenu(b); break;
        case "lock": toggleLock(); break;
        case "pickkey": keyMenu(b); break;
      }
    });
    pane.addEventListener("contextmenu", (e) => {
      const h = e.target.closest(".lp-hd");
      if (!h || typeof showMenu !== "function") return;
      e.preventDefault();
      const id = h.dataset.hd, i = S.cfg.order.indexOf(id), vis = S.cfg.order.filter((x) => !S.cfg.hidden.includes(x));
      const vi = vis.indexOf(id);
      const move = (dir) => { const o = vis.slice(); const j = vi + dir; [o[vi], o[j]] = [o[j], o[vi]]; setOrder([...o, ...S.cfg.order.filter((x) => S.cfg.hidden.includes(x))]); };
      showMenu(e.clientX, e.clientY, [
        { h: (CARDS.find((c) => c.id === id) || {}).label || "Card" },
        { label: "Move up", disabled: vi <= 0, onClick: () => move(-1) },
        { label: "Move down", disabled: vi < 0 || vi >= vis.length - 1, onClick: () => move(1) },
        { label: "Hide card", onClick: () => setVisible(id, false) },
        { sep: true },
        ...CARDS.filter((c) => S.cfg.hidden.includes(c.id)).map((c) => ({ label: `Show ${c.label}`, onClick: () => setVisible(c.id, true) })),
        { label: "Reset pane layout", disabled: i < 0, onClick: resetLayout },
      ]);
    });
    const add = $id("lp-add");
    if (add) add.addEventListener("click", (e) => { const b = e.target.closest("[data-show]"); if (b) setVisible(b.dataset.show, true); });
    // drag a card by its header to reorder
    pane.addEventListener("pointerdown", (e) => {
      const h = e.target.closest(".lp-hd");
      if (!h || e.button !== 0 || e.target.closest("button, .lp-seg, .lp-pick")) return;
      const card = h.closest(".lp-card");
      const sx = e.clientX, sy = e.clientY;
      let ph = null, offY = 0, raf = 0, lastY = sy;
      const scroller = $id("side-right");
      const move = (me) => {
        lastY = me.clientY;
        if (!ph) {
          if (Math.hypot(me.clientX - sx, me.clientY - sy) < 5) return;
          const r = card.getBoundingClientRect();
          offY = sy - r.top;
          ph = document.createElement("div");
          ph.className = "lp-ph";
          ph.style.height = r.height + "px";
          card.before(ph);
          Object.assign(card.style, { left: r.left + "px", top: r.top + "px", width: r.width + "px" });
          card.classList.add("lp-drag");
          document.body.style.cursor = "grabbing";
          const tick = () => {   // auto-scroll near the pane's edges
            const sr = scroller.getBoundingClientRect();
            if (lastY < sr.top + 30) scroller.scrollTop -= 8; else if (lastY > sr.bottom - 30) scroller.scrollTop += 8;
            raf = requestAnimationFrame(tick);
          };
          raf = requestAnimationFrame(tick);
        }
        card.style.top = me.clientY - offY + "px";
        const others = [...pane.querySelectorAll(".lp-card:not(.lp-drag):not([hidden])")];
        const before = others.find((o) => { const r = o.getBoundingClientRect(); return me.clientY < r.top + r.height / 2; });
        if (before) { if (ph.nextElementSibling !== before) before.before(ph); } else if (pane.lastElementChild !== ph) pane.append(ph);
      };
      const up = () => {
        window.removeEventListener("pointermove", move);
        window.removeEventListener("pointerup", up);
        window.removeEventListener("pointercancel", up);
        cancelAnimationFrame(raf);
        if (!ph) return;
        ph.replaceWith(card);
        card.classList.remove("lp-drag");
        card.style.left = card.style.top = card.style.width = "";
        document.body.style.cursor = "";
        S.cfg.order = [...pane.querySelectorAll(".lp-card")].map((c) => c.dataset.card);
        save(); applyLayout();
      };
      window.addEventListener("pointermove", move);
      window.addEventListener("pointerup", up);
      window.addEventListener("pointercancel", up);
    });
  }

  /* ---------- menus ---------- */
  function menuAt(el, items) { if (typeof showMenu !== "function") return; const r = el.getBoundingClientRect(); showMenu(r.left, r.bottom + 4, items); }
  function tuningMenu(el) {
    const items = [];
    Object.entries(INSTR).forEach(([k, v]) => {
      items.push({ h: v.label });
      Object.entries(v.t).forEach(([tk, t]) => items.push({ label: (S.cfg.inst === k && S.cfg.tuning === tk ? "✓ " : "") + t[0], sub: t[1] ? t[1].replace(/b(\d)/g, "♭$1").replace(/#/g, "♯") : "", onClick: () => setTuning(k, tk) }));
    });
    items.push({ h: "Reference A4" });
    [432, 438, 440, 441, 442, 443, 444].forEach((f) => items.push({ label: `${S.cfg.a4 === f ? "✓ " : ""}${f} Hz`, sub: f === 440 ? "concert" : `${f > 440 ? "+" : "−"}${Math.abs(1200 * Math.log2(f / 440)).toFixed(1)} ¢`, onClick: () => setRef(f) }));
    items.push({ label: "Other… (full tuner)", onClick: openTuner });
    menuAt(el, items);
  }
  function setTuning(i, t) { S.cfg.inst = i; S.cfg.tuning = t; save(); sendCfg(true); paintStatic(); }
  function refMenu(el) {
    menuAt(el, [{ h: "Reference A4" }, ...[432, 435, 436, 438, 439, 440, 441, 442, 443, 444, 445, 446].map((f) => ({ label: `${S.cfg.a4 === f ? "✓ " : ""}${f} Hz`, sub: f === 440 ? "concert" : f === 432 ? "" : `${(1200 * Math.log2(f / 440)).toFixed(1)} ¢`, onClick: () => setRef(f) }))]);
  }
  function setRef(f) { S.cfg.a4 = Math.max(415, Math.min(466, Math.round(f * 10) / 10)); save(); sendCfg(true); paintStatic(); }
  function keyMenu(el) {
    const items = [{ h: "Key" }, { label: (S.cfg.keyLock ? "" : "✓ ") + "Detect automatically", onClick: () => { S.cfg.keyLock = null; save(); paintKey(); } }, { sep: true }];
    for (const minor of [false, true]) for (let r = 0; r < 12; r++) {
      const k = { root: r, minor };
      const on = S.cfg.keyLock && S.cfg.keyLock.root === r && S.cfg.keyLock.minor === minor;
      items.push({ label: `${on ? "✓ " : ""}${names(k)[r]} ${minor ? "minor" : "major"}`, onClick: () => { S.cfg.keyLock = k; save(); paintKey(); } });
    }
    menuAt(el, items);
  }
  function toggleLock() {
    if (S.cfg.keyLock) S.cfg.keyLock = null;
    else { const k = curKey(); if (!k) { if (typeof toast === "function") toast("No key yet — play a little first, or pick one from the key name."); return; } S.cfg.keyLock = k; }
    save(); paintKey();
  }
  function sendCfg(force) {
    const strings = S.cfg.inst === "chromatic" ? [] : tuningArr();
    const sig = `${S.cfg.a4}|${strings.join(",")}`;
    if (!force && sig === S.sentCfg) return;
    S.sentCfg = sig;
    getJSON(`/api/listen/set?a4=${S.cfg.a4}&strings=${strings.join(",")}`).catch(() => { S.sentCfg = ""; });
  }

  /* =====================================================================
     PAINT — static bits (depend on settings, not on audio)
     ===================================================================== */
  function paintStatic() {
    const pane = $id("lp");
    if (!pane) return;
    const tc = q1('.lp-card[data-card="tuner"]', pane);
    q1('[data-act="tuning"]', tc).textContent = shortTuning() + (S.cfg.a4 !== 440 ? ` · ${S.cfg.a4 % 1 ? S.cfg.a4.toFixed(1) : S.cfg.a4}` : "") + " ▾";
    // strings row
    const st = S.cfg.inst === "chromatic" ? [] : tuningArr();
    q1('[data-k="strs"]', tc).innerHTML = st.map((m, i) => `<div class="lp-str" data-i="${i}"><em>–</em><span class="bar"><i></i></span><b>${noteHTML(m + 0, null, false).replace(/<span class="acc">(.+?)<\/span>/, "$1")}</b></div>`).join("");
    q1('[data-k="strs"]', tc).hidden = !st.length;
    // tone toggle
    const tone = q1('.lp-card[data-card="tone"]', pane);
    tone.querySelectorAll("[data-tv]").forEach((b) => b.classList.toggle("on", b.dataset.tv === S.cfg.tone));
    q1('[data-k="spec"]', tone).hidden = S.cfg.tone !== "spectrum";
    const sc = $id("top-wave-canvas");
    if (sc) sc.hidden = S.cfg.tone !== "scope";
    q1('[data-k="bands"]', tone).innerHTML = S.cfg.tone === "spectrum"
      ? "<span>80</span><span>250</span><span>1k</span><span>4k</span><span>12k</span>"
      : '<span class="lg"><i style="background:var(--scope-in)"></i>In</span><span class="lg"><i style="background:var(--accent)"></i>Out</span>';
    if (S.cfg.tone === "scope" && typeof drawScope === "function") requestAnimationFrame(() => { try { drawScope(ui.lastWaveIn || [], ui.lastWaveOut || []); } catch (e) { /* scope */ } });
    paintKey();
    if (S.tuner.open) paintTunerStatic();
  }

  /* =====================================================================
     POLL
     ===================================================================== */
  /* Idle CPU (assets/activity.js): the loop runs only while something shows Listen data — window/editor awake
     and the pane shown, the big tuner open, or another view asking through want(). Otherwise it ends: no
     /api/listen polls (the engine's analysis thread disarms ~4 s later) and no draws. kick() restarts it on
     anything that can bring it back (HRActivity, pane collapse / Live full screen via a MutationObserver,
     tuner, want(), layout and tone changes). While the input is silent it polls at 4 Hz and draws once per
     reading after a short settle, instead of every frame. It also drives the Tone card's scope (index.html). */
  function paneOn() {
    const ws = q1(".workspace");
    return !(ws && ws.classList.contains("r-collapsed")) && !document.body.classList.contains("hr-live-full");
  }
  function awake() { return window.HRActivity ? HRActivity.awake() : !document.hidden; }
  function visible() {
    return awake() && (S.tuner.open || paneOn() || performance.now() - S.wantT < 600);
  }
  function scopeOn() {
    const tone = S.built && q1('.lp-card[data-card="tone"]');
    return !!tone && !tone.hidden && S.cfg.tone === "scope" && paneOn();
  }
  function kick() {
    if (!S.raf && S.built && visible()) { clearTimeout(S.idleT); S.idleT = 0; S.raf = requestAnimationFrame(loop); }
    if (window.HRScope) HRScope.kick();
  }
  const quiet = () => S.ok === true && !S.tuner.open && !!(S.d && S.d.level) && !S.d.level.signal;
  async function poll() {
    if (S.polling) return;
    S.polling = true;
    try {
      const d = await getJSON("/api/listen");
      if (d && d.ok) {
        if (S.ok !== true) sendCfg(true);
        S.ok = true; S.fails = 0;
        if (!d.warming) { S.d = d; S.dT = performance.now(); paintData(); }
      } else { S.ok = false; }
    } catch (e) {
      if (++S.fails > 3) { S.ok = e && e.message === "404" ? "old" : false; paintData(); }
    }
    S.polling = false;
  }
  function loop(t) {
    S.raf = 0;
    if (!S.built || !visible()) return;   // asleep / nothing shows it: stop (kick() restarts)
    const q = quiet();
    if (!q) S.quietT = 0; else if (!S.quietT) S.quietT = t;
    const every = S.ok === "old" ? 4000 : S.ok === false ? 1000 : q ? 250 : 45;
    if (t - S.lastPoll > every) { S.lastPoll = t; poll(); }
    if (!q || t - S.quietT < 700 || S.trBusy) { frame(t); S.raf = requestAnimationFrame(loop); return; }   // (the trail's tail scrolls out / head settles at full rate)
    // silent and settled: one frame per new reading, then sleep until the next poll is due
    if ((S.dT || 0) !== S.drawnT) { S.drawnT = S.dT; frame(t); }
    S.idleT = setTimeout(() => { S.idleT = 0; kick(); }, Math.max(20, every - (t - S.lastPoll)));
  }

  /* =====================================================================
     PAINT — data
     ===================================================================== */
  function tunerModel() {
    const d = S.d, st = S.cfg.inst === "chromatic" ? [] : tuningArr();
    const off = refOff();
    const m = { state: "none", strings: st, poly: false, polyStrs: [] };
    if (!d) return m;
    const forcePoly = S.cfg.mode === "poly", forceMono = S.cfg.mode === "mono";
    m.poly = st.length > 1 && !forceMono && (forcePoly || (d.poly && d.poly.active));
    if (m.poly) m.polyStrs = (d.poly && d.poly.strings) || [];
    const p = d.pitch;
    if (p && (!d.chordal || forceMono)) {
      const mref = p.midi - off, note = Math.round(mref);
      m.state = "note"; m.midi = note; m.cents = (mref - note) * 100; m.hz = p.hz; m.stable = p.stable;
      if (Math.abs(m.cents) < 1.5 && p.stable) m.state = "in";
      // nearest string (for highlighting and "tune up to …" hints)
      let best = -1, bd = 99;
      st.forEach((t, i) => { const dd = mref - t; if (Math.abs(dd) < Math.abs(bd)) { bd = dd; best = i; } });
      if (best >= 0 && Math.abs(bd) < 2.6) { m.string = best; m.strDiff = bd; }
    }
    return m;
  }
  function paintData() {
    const pane = $id("lp");
    if (!pane) return;
    paintTuner(pane);
    paintKey();
    paintTone(pane);
    paintInput(pane);
    if (S.tuner.open) paintTunerData();
  }

  function paintTuner(pane) {
    const tc = q1('.lp-card[data-card="tuner"]', pane), box = q1(".lp-tun", tc);
    if (S.ok === "old") { setH(q1('[data-k="cents"]', tc), "needs the updated engine"); return; }
    const m = tunerModel();
    S.tm = m;
    tc.classList.toggle("poly", m.poly);
    box.dataset.st = m.state;
    const key = curKey();
    if (m.state !== "none") {
      setH(q1('[data-k="note"]', tc), noteHTML(m.midi, key));
      const c = m.cents, cs = `${c > 0.05 ? "+" : c < -0.05 ? "−" : "±"}${Math.abs(c).toFixed(1)}`;
      let hint = "";
      if (m.string != null && Math.abs(m.strDiff) > 0.5) hint = ` <span class="hint">${m.strDiff < 0 ? "↑" : "↓"} ${escH(names(key)[((S.cfg.inst === "chromatic" ? 0 : m.strings[m.string]) % 12 + 12) % 12])}</span>`;
      setH(q1('[data-k="cents"]', tc), `<b>${cs}</b> ¢${hint}`);
      setH(q1('[data-k="hz"]', tc), `${m.hz.toFixed(2)} Hz`);
      S.needleT = Math.max(-50, Math.min(50, c));
    } else {
      setH(q1('[data-k="note"]', tc), S.d && S.d.chordal ? "♪" : "–");
      setH(q1('[data-k="cents"]', tc), S.d && S.d.chordal ? (m.strings.length > 1 ? "strum open strings to tune all" : "chord") : S.d && S.d.level && S.d.level.signal ? "listening…" : "play a string");
      setH(q1('[data-k="hz"]', tc), "");
    }
    // strings
    const cols = tc.querySelectorAll(".lp-str");
    cols.forEach((col, i) => {
      if (m.poly) {
        const ps = m.polyStrs[i];
        const has = ps && ps.present && ps.cents != null;
        col.classList.toggle("miss", !has);
        col.classList.toggle("in", !!has && Math.abs(ps.cents) < 2);
        col.classList.remove("on", "near");
        setH(q1("em", col), has ? `${ps.cents > 0 ? "+" : ps.cents < 0 ? "−" : ""}${Math.abs(ps.cents).toFixed(Math.abs(ps.cents) < 10 ? 1 : 0)}` : "–");
        const dot = q1(".bar i", col);
        dot.style.top = `${50 - Math.max(-50, Math.min(50, has ? ps.cents : 0))}%`;
      } else {
        col.classList.remove("miss", "in");
        col.classList.toggle("on", m.string === i && Math.abs(m.strDiff) <= 0.5);
        col.classList.toggle("near", m.string === i && Math.abs(m.strDiff) > 0.5);
      }
    });
    // bend tag
    const bt = q1('[data-k="bend"]', tc), b = S.d && S.d.bend;
    if (b && b.age < 2.2) {
      const tgt = b.to - Math.round(refOff());
      const c = b.cents - (refOff() - Math.round(refOff())) * 100;
      setH(bt, `bend → <b>${names(key)[((tgt % 12) + 12) % 12]}</b> ${c >= 0 ? "+" : "−"}${Math.abs(c).toFixed(0)}¢`);
      bt.classList.toggle("off", Math.abs(c) > 8);
      bt.classList.add("show");
    } else bt.classList.remove("show");
  }

  // ---- key / chord / scale ----
  function paintKey() {
    const pane = $id("lp");
    if (!pane) return;
    const kc = q1('.lp-card[data-card="key"]', pane), d = S.d;
    const key = curKey(), dk = d && d.key;
    const kname = q1('[data-k="kname"]', kc);
    const chip = q1('[data-k="kstate"]', kc);
    q1('[data-k="lockbtn"]', kc).innerHTML = S.cfg.keyLock ? IC.lock : IC.unlock;
    q1('[data-k="lockbtn"]', kc).classList.toggle("on", !!S.cfg.keyLock);
    q1('[data-k="lockbtn"]', kc).title = S.cfg.keyLock ? "Unlock: follow what you play" : "Lock the key";
    if (key) {
      kname.classList.remove("wait");
      setH(kname, `${escH(names(key)[key.root])} <em>${key.minor ? "minor" : "major"}</em>`);
      const rel = key.minor ? { root: (key.root + 3) % 12, minor: false } : { root: (key.root + 9) % 12, minor: true };
      let alt = `relative <b>${escH(names(rel)[rel.root])} ${rel.minor ? "minor" : "major"}</b>`;
      if (!S.cfg.keyLock && dk && dk.alt_root >= 0 && !(dk.alt_root === rel.root && !!dk.alt_minor === rel.minor)) {
        const a = { root: dk.alt_root, minor: !!dk.alt_minor };
        alt = `or <b>${escH(names(a)[a.root])} ${a.minor ? "minor" : "major"}</b>`;
      }
      setH(q1('[data-k="kalt"]', kc), alt);
    } else {
      kname.classList.add("wait");
      setH(kname, S.ok === "old" ? "Update the engine" : "Listening…");
      setH(q1('[data-k="kalt"]', kc), dk && dk.evidence_s > 0.2 ? "Keep playing…" : "Play for a few seconds");
    }
    // state chip + confidence
    let conf = 0;
    if (S.cfg.keyLock) { chip.className = "lp-chip lock"; setH(chip, "<i></i>locked"); conf = 1; }
    else if (dk && dk.root != null) {
      conf = dk.conf || 0;
      if (dk.state === "held") { chip.className = "lp-chip idle"; setH(chip, `<i></i>held · ${fmtAge(d.level && d.level.silent_s)}`); }
      else if (dk.state === "listening") { chip.className = "lp-chip"; setH(chip, "<i></i>settling"); }
      else { chip.className = "lp-chip"; setH(chip, "<i></i>listening"); }
    } else { chip.className = "lp-chip idle"; setH(chip, "<i></i>listening"); conf = dk ? Math.min(0.99, (dk.evidence_s || 0) / 4) * 0.4 : 0; }
    q1('[data-k="confarc"]', kc).setAttribute("stroke-dashoffset", (97.4 * (1 - conf)).toFixed(1));
    setH(q1('[data-k="conf"]', kc), S.cfg.keyLock ? "✓" : key ? `${Math.round(conf * 100)}%` : "–");
    // chord
    const c = d && d.chord, box = q1('[data-k="chordbox"]', kc);
    if (c && c.root != null) {
      const p = chordParts(c, key);
      setH(q1('[data-k="chord"]', kc), `${escH(p.base)}${p.sup ? `<sup>${escH(p.sup)}</sup>` : ""}${p.slash ? `<small>${escH(p.slash)}</small>` : ""}`);
      setH(q1('[data-k="roman"]', kc), escH(roman(c, key) || chordName(c.quality)));
      const N = names(key);
      setH(q1('[data-k="tones"]', kc), (Q[c.quality] || Q.maj).iv.map((iv) => { const pc = (c.root + iv) % 12; return `<span class="${iv === 0 ? "r" : ""}">${escH(N[pc])}</span>`; }).join(""));
      box.classList.toggle("stale", !d.chordal && !(d.level && d.level.signal && c.age < 1.5));
    } else {
      setH(q1('[data-k="chord"]', kc), "–"); setH(q1('[data-k="roman"]', kc), "strum a chord"); setH(q1('[data-k="tones"]', kc), "");
      box.classList.add("stale");
    }
    // progression: the last few chords, the current one, then a guess at the next (if the progression repeats)
    const prog = (d && d.prog) || [];
    const seq = prog.slice(-3);
    const cells = seq.map((x) => ({ c: x, cls: "" }));
    if (c && c.root != null) cells.push({ c, cls: "now" });
    let next = null;
    if (c && c.root != null && prog.length >= 3) {
      const same = (a, b) => a.root === b.root && a.quality === b.quality;
      for (let i = prog.length - 2; i >= 0; i--) if (same(prog[i], c)) { next = prog[i + 1]; break; }
      if (next && same(next, c)) next = null;
    }
    if (next) cells.push({ c: next, cls: "next" });
    setH(q1('[data-k="prog"]', kc), cells.length ? cells.slice(-5).map(({ c: x, cls }) => {
      const p = chordParts(x, key);
      return `<div class="${cls}" title="${cls === "next" ? "Probably next (from the last time round)" : escH(chordText(x, key))}"><b>${escH(p.base)}${p.sup ? `<small>${escH(p.sup)}</small>` : ""}</b><i>${escH(roman(x, key) || (cls === "next" ? "next?" : "·"))}</i></div>`;
    }).join("") : "");
    // scale buttons + the scale's notes (root first; chord tones marked)
    const minor = key ? key.minor : true;
    setH(q1('[data-k="scales"]', kc), Object.entries(SCALES).map(([k, v]) => `<button data-sc="${k}" class="${S.cfg.scale === k ? "on" : ""}">${(!minor && v.lMaj) || v.l}</button>`).join(""));
    let sn = '<span class="w">The scale appears once the key is found</span>';
    if (key) {
      const N = names(key), ct = c && c.root != null ? (Q[c.quality] || Q.maj).iv.map((i) => (c.root + i) % 12) : [];
      sn = (SCALES[S.cfg.scale] || SCALES.pent)[key.minor ? "min" : "maj"].map((i) => { const pc = (key.root + i) % 12; return `<span class="${i === 0 ? "r" : ct.includes(pc) ? "ct" : ""}">${escH(N[pc])}</span>`; }).join("");
    }
    setH(q1('[data-k="snotes"]', kc), sn);
  }
  const chordName = (q) => ({ "5": "power chord", sus2: "suspended", sus4: "suspended" }[q] || "");
  function fmtAge(s) { s = +s || 0; return s < 60 ? `${Math.round(s)} s` : `${Math.round(s / 60)} min`; }

  // ---- tone ----
  function paintTone(pane) {
    const d = S.d, tone = q1('.lp-card[data-card="tone"]', pane);
    if (!d || !d.spec) return;
    const t = d.tone, rd = q1('[data-k="toneread"]', tone);
    if (S.cfg.tone !== "spectrum" || !t) { setH(rd, ""); return; }
    const k = (hz) => (hz >= 1000 ? `${(hz / 1000).toFixed(hz >= 10000 ? 0 : 1)}k` : `${Math.round(hz)}`);
    const pills = [];
    pills.push(t.low_hz >= 105 ? "Tight lows" : t.low_hz <= 60 ? "Deep lows" : `Lows from ${k(t.low_hz)}`);
    pills.push(`Mids ↑ ${k(t.mid_hz)}`);
    pills.push(t.top_hz >= 15000 ? "Open top" : `Top ↓ ${k(t.top_hz)}`);
    setH(rd, pills.map((p) => `<span>${escH(p)}</span>`).join(""));
  }

  // ---- input ----
  function gateThreshold() {
    const slots = (typeof rackState !== "undefined" && rackState.slots) || [];
    for (const s of slots) {
      if (s.bypassed || !/gate/i.test(`${s.reference_name} ${s.display_name}`)) continue;
      const p = (s.params || []).find((x) => /thresh/i.test(`${x.name} ${x.id}`));
      const m = p && /(-?\d+(?:\.\d+)?)\s*dB/i.exec(p.fmt || "");
      if (m) return +m[1];
    }
    return null;
  }
  function paintInput(pane) {
    const ic = q1('.lp-card[data-card="input"]', pane), d = S.d;
    const health = q1('[data-k="health"]', ic);
    // The status follows the level, which jitters around thresholds: a new message must hold for ~0.8 s before it
    // replaces the shown one, so the text (and the card) never flickers between states.
    const say = (cls, html) => {
      const key = cls + "|" + html, now = performance.now();
      if (health._shown === key) { health._pend = null; return; }
      if (health._shown != null && (!health._pend || health._pend.key !== key)) { health._pend = { key, t: now }; return; }
      if (health._shown != null && now - health._pend.t < 800) return;
      health._shown = key; health._pend = null;
      health.className = "lp-health " + cls; setH(q1("span", health), html);
    };
    if (S.ok === "old") { say("idle", "This engine build has no input analysis yet. Rebuild Headroom to turn it on."); return; }
    if (!d || !d.level) { say("idle", S.ok === false ? "Can’t reach the audio engine." : "Waiting for the engine…"); return; }
    const L = d.level, live = d.source === "live";
    const fmt = (v) => (v == null ? "–" : `${v < 0 ? "−" : ""}${Math.abs(v).toFixed(0)}`);
    setH(q1('[data-k="peak"]', ic), L.signal || L.hold_db > -70 ? `peak ${fmt(L.hold_db)} dBFS` : live ? "no signal" : "DI player");
    q1('[data-k="pk"]', ic).style.left = `calc(${pct(L.hold_db)}% - 1px)`;
    q1('[data-k="floor"]', ic).style.width = L.floor_db != null ? `${pct(L.floor_db)}%` : "0";
    const g = gateThreshold(), gEl = q1('[data-k="gate"]', ic);
    gEl.hidden = g == null;
    if (g != null) gEl.style.left = `${pct(g)}%`;
    setH(q1('[data-k="gatelbl"]', ic), g != null ? `gate ${fmt(g)}` : "");
    if (g != null) q1('[data-k="gatelbl"]', ic).style.left = `${pct(g)}%`;
    setH(q1('[data-k="sfloor"]', ic), L.floor_db != null ? `${fmt(L.floor_db)}<small>dB</small>` : "–");
    setH(q1('[data-k="slat"]', ic), d.latency_ms > 0 ? `${d.latency_ms.toFixed(1)}<small>ms</small>` : "–");
    setH(q1('[data-k="scpu"]', ic), d.cpu != null ? `${Math.round(d.cpu * 100)}<small>%</small>` : "–");
    const peak = L.hold_db;
    if (L.clip) say("bad", `<b>Clipping at the input.</b> Turn the interface gain down about ${Math.max(3, Math.ceil(peak + 9))} dB.`);
    else if (!L.signal && (L.silent_s || 0) > 2.5) {
      if (L.hum_hz) say("warn", `<b>${L.hum_hz} Hz hum</b> (${fmt(L.hum_db)} dB) while you’re not playing. Try the other pickup position, touch the strings, or move away from the screen.`);
      else if (live) say("warn", "<b>Nothing coming in.</b> Check the cable and the input channel (Settings → Audio).");
      else say("idle", "<b>DI player is stopped.</b> Press play, or switch to live input in Settings → Audio.");
    }
    else if (peak > -4) say("warn", `<b>Very hot.</b> Peaks reach ${fmt(peak)} dBFS. Lower the interface gain about ${Math.ceil(peak + 9)} dB to leave headroom.`);
    else if (L.signal && peak < -26) say("warn", `<b>Low input.</b> Peaks only reach ${fmt(peak)} dBFS. Raise the interface gain about ${Math.round(-12 - peak)} dB so amps react like a real guitar.`);
    else if (g != null && L.floor_db != null && g < L.floor_db + 3) say("warn", `<b>Gate sits under the noise.</b> Your floor is ${fmt(L.floor_db)} dB; raise the gate threshold to about ${fmt(L.floor_db + 6)} dB.`);
    else if (L.signal) say("", `<b>Input level is right.</b> Peaks land in the sweet spot${live ? "" : " (DI player)"}.`);
    else say("idle", live ? "Play something to check your level." : "DI player is quiet.");
  }

  function setH(el, v) { if (el && el._v !== v) { el._v = v; el.innerHTML = v; } }

  /* =====================================================================
     FRAME — 60 fps animation from the latest data
     ===================================================================== */
  function frame(t) {
    const dt = Math.min(0.1, (t - (S.lastFrame || t)) / 1000);
    S.lastFrame = t;
    const pane = $id("lp");
    if (!pane) return;
    const m = S.tm;
    // needle + strobe
    const live = m && m.state !== "none";
    S.needle += ((live ? S.needleT : S.needle) - S.needle) * Math.min(1, dt * 14);
    const tc = q1('.lp-card[data-card="tuner"]', pane);
    if (tc && !tc.hidden) {
      q1('[data-k="nd"]', tc).style.left = `${50 + S.needle}%`;
      if (live) S.strobe += S.needleT * dt * 6;
      q1('[data-k="strobe"]', tc).style.backgroundPosition = `${S.strobe.toFixed(1)}px 0`;
      if (S.trVis !== false) drawTrail(q1('[data-k="trail"]', tc), dt); else S.trBusy = false;
    } else S.trBusy = false;
    const tone = q1('.lp-card[data-card="tone"]', pane);
    if (tone && !tone.hidden && S.cfg.tone === "spectrum") drawSpec(q1('[data-k="spec"]', tone), dt);
    const ic = q1('.lp-card[data-card="input"]', pane);
    if (ic && !ic.hidden && S.d && S.d.level) {
      S.meter = S.meter == null ? -72 : S.meter + ((S.d.level.peak_db) - S.meter) * Math.min(1, dt * (S.d.level.peak_db > S.meter ? 30 : 8));
      q1('[data-k="fill"]', ic).style.width = `${pct(S.meter)}%`;
    }
    if (S.tuner.open) tunerFrame(dt);
  }
  function canvasCtx(cv) {
    const dpr = window.devicePixelRatio || 1, w = cv.clientWidth, h = cv.clientHeight;
    if (!w || !h) return null;
    if (cv.width !== Math.round(w * dpr) || cv.height !== Math.round(h * dpr)) { cv.width = Math.round(w * dpr); cv.height = Math.round(h * dpr); }
    const g = cv.getContext("2d");
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, w, h);
    return { g, w, h };
  }
  function tokens() {
    if (!S.tok || S.tokT !== document.body.className) {
      const cs = getComputedStyle(document.body), v = (k) => cs.getPropertyValue(k).trim();
      S.tok = { acc: v("--accent"), grid: v("--scope-grid"), inp: v("--scope-in"), t2: v("--text-2"), t3: v("--text-3"), line: v("--line-2"),
        ok: rgb(v("--lp-tr-ok")), warn: rgb(v("--lp-tr-warn")), bad: rgb(v("--lp-tr-bad")), idle: rgb(v("--lp-tr-idle")), font: cs.fontFamily };
      S.tokT = document.body.className;
      S.trCol = null;
    }
    return S.tok;
  }
  function rgb(c) { const m = /^#([0-9a-f]{6})$/i.exec(c || ""); const n = m ? parseInt(m[1], 16) : 0x888888; return [n >> 16, (n >> 8) & 255, n & 255]; }

  /* ---- pitch trail: the last 3 s of pitch, in cents around the note being played ----
     Time runs right to left; "now" is the bright head at the right edge. Each sample is placed by its engine
     time (trail ends at d.t) on a clock smoothed across polls, drawn ~60 ms behind the newest reading, so the
     trace scrolls at display rate without the step-jumps of redrawing on each poll. The tail fades and thins
     with age. Cents use a centre-expanded scale (|c|/50)^0.6, so the ±3 ¢ band is a few pixels tall on a
     38 px strip while ±50 still reaches the edges (beyond that, e.g. the note before a bend, runs off the strip). Stray frames (octave slips, one-frame outliers, blips under
     3 frames) are dropped and the rest gets a light 3-tap smooth; vibrato survives. The graph re-centres on a
     new note only past ±58 ¢ (hysteresis) and slides there. On silence the head eases to the centre and dims. */
  const TR = { span: 3, lag: 0.06, gut: 24, pad: 3, ok: 3, hyst: 0.58 };
  const trY = (c, mid, half) => { const a = Math.min(Math.abs(c), 150) / 50; return mid - Math.sign(c) * Math.pow(a, 0.6) * half; };
  function trColor(tk, c) {   // |cents| → [r,g,b]: green ≤ 3, amber to ~20, red beyond (short blends, no flicker at the edges)
    if (!S.trCol) {
      S.trCol = [];
      const mix = (a, b, u) => a.map((x, i) => Math.round(x + (b[i] - x) * u));
      for (let k = 0; k <= 60; k++) {
        const c = k / 2;
        S.trCol.push(c <= 3 ? tk.ok : c < 5 ? mix(tk.ok, tk.warn, (c - 3) / 2) : c <= 18 ? tk.warn : c < 24 ? mix(tk.warn, tk.bad, (c - 18) / 6) : tk.bad);
      }
    }
    return S.trCol[Math.min(60, Math.round(Math.abs(c) * 2))];
  }
  const rgba = (c, a) => `rgba(${c[0]},${c[1]},${c[2]},${a.toFixed(3)})`;
  function trailSamples(tr, off) {   // engine trail → cleaned MIDI values (reference applied), null = unvoiced
    const src = tr.midi || [], n = src.length, v = new Array(n);
    for (let i = 0; i < n; i++) v[i] = src[i] == null ? null : src[i] - off;
    // one-frame outliers: a sample far from neighbours that agree with each other (both sides, or two on one
    // side at the edge of a run)
    const far = (x, y) => Math.abs(x - y) > 0.25, near = (x, y) => x != null && y != null && !far(x, y);
    for (let i = 0; i < n; i++) {
      const b = v[i];
      if (b == null) continue;
      const a = v[i - 1], c = v[i + 1];
      if (near(a, c) && far(b, (a + c) / 2)) v[i] = (a + c) / 2;
      else if (a == null && near(c, v[i + 2]) && far(b, c)) v[i] = c;
      else if (c == null && i < n - 1 && near(a, v[i - 2]) && far(b, a)) v[i] = a;
    }
    // blips: voiced runs shorter than 3 frames, or a short run sitting an octave-ish away from both sides
    for (let i = 0; i < n;) {
      if (v[i] == null) { i++; continue; }
      let j = i; while (j < n && v[j] != null) j++;
      if (j - i < 3 && j < n) for (let k = i; k < j; k++) v[k] = null;   // (a run still growing at the head stays)
      i = j;
    }
    // adaptive smooth: a wide Gaussian where the pitch is steady (detector jitter of a few tenths of a cent
    // reads as a scribble on the expanded centre scale), easing to a 3-tap where it moves (vibrato, bends)
    const o = new Array(n), GW = [0.04, 0.08, 0.14, 0.18, 0.2];   // σ ≈ 2 frames, half-window 4
    for (let i = 0; i < n; i++) {
      const b = v[i];
      if (b == null) { o[i] = null; continue; }
      let sw = 0, sv = 0, lo = b, hi = b;
      for (let k = -4; k <= 4; k++) {
        const x = v[i + k];
        if (x == null || Math.abs(x - b) > 0.4) continue;
        const wt = GW[4 - Math.abs(k)];
        sw += wt; sv += wt * x;
        if (x < lo) lo = x; if (x > hi) hi = x;
      }
      const a = v[i - 1], c = v[i + 1];
      const s3 = a != null && c != null && Math.abs(a - b) < 0.4 && Math.abs(c - b) < 0.4 ? (a + 2 * b + c) / 4 : b;
      const u = Math.max(0, Math.min(1, ((hi - lo) * 100 - 3) / 6));   // spread 3 → 9 ¢: wide → narrow
      o[i] = (sv / sw) * (1 - u) + s3 * u;
    }
    return o;
  }
  function drawTrail(cv, dt) {
    const c = cv && canvasCtx(cv);
    S.trBusy = false;
    if (!c) return;
    const { g, w, h } = c, tk = tokens(), now = performance.now();
    const x0 = TR.gut, xH = w - 7, pw = xH - x0, mid = h / 2, half = h / 2 - TR.pad;
    const tr = S.d && S.d.trail, step = ((tr && tr.step_ms) || 20) / 1000;
    const vals = tr ? trailSamples(tr, refOff()) : [], n = vals.length;
    // engine clock → display time (smoothed; never runs backwards except on a real reset)
    let tD = null;
    if (tr && S.dT) {
      const tEnd = typeof S.d.t === "number" ? S.d.t : S.dT / 1000;
      if (S.trSeen !== S.dT) {
        S.trSeen = S.dT;
        const off = tEnd - S.dT / 1000;
        S.trClk = S.trClk == null || Math.abs(off - S.trClk) > 0.25 ? off : S.trClk + (off - S.trClk) * 0.12;
      }
      tD = now / 1000 + S.trClk - TR.lag;
      if (S.trTD != null && tD < S.trTD && S.trTD - tD < 0.5) tD = S.trTD;
      S.trTD = tD;
      S.trEnd = tEnd;
    }
    // the head: interpolated at display time
    let head = null;
    if (tD != null && n) {
      const f = (n - 1) - (S.trEnd - tD) / step, i0 = Math.floor(f);
      if (i0 >= 0 && i0 < n - 1) { const a = vals[i0], b = vals[i0 + 1]; head = a != null && b != null ? a + (b - a) * (f - i0) : (b != null && f - i0 > 0.5 ? b : a); }
      else if (i0 >= n - 1) { head = vals[n - 1]; if (head == null && vals[n - 2] != null) head = vals[n - 2]; }
    }
    // centre note with hysteresis; slide to a new one
    const k = Math.min(1, dt * 14);
    if (head != null) {
      if (S.trNote == null || (S.trSilT || 0) > 0.25) { S.trNote = Math.round(head); S.trC = S.trNote; }
      else if (Math.abs(head - S.trNote) > TR.hyst) S.trNote = Math.round(head);
      S.trSilT = 0;
    } else S.trSilT = (S.trSilT || 0) + dt;
    if (S.trNote != null) S.trC = S.trC == null ? S.trNote : S.trC + (S.trNote - S.trC) * k;
    const C = S.trC;
    // head state: ease toward the reading; on silence toward the centre, dimmed
    const live = head != null;
    const hc = live ? (head - C) * 100 : 0;
    S.trH = S.trH == null ? hc : S.trH + (hc - S.trH) * Math.min(1, dt * (live ? 22 : 5));
    S.trA = (S.trA || 0) + ((live ? 1 : 0) - (S.trA || 0)) * Math.min(1, dt * (live ? 16 : 4));
    const hcol = live ? trColor(tk, S.trH) : tk.idle, inTune = live && Math.abs(S.trH) <= TR.ok;
    S.trInA = (S.trInA || 0) + ((inTune ? 1 : 0) - (S.trInA || 0)) * Math.min(1, dt * 10);

    // ---- background: scale, band, grid ----
    g.font = `600 8px ${tk.font}`;
    g.textBaseline = "middle";
    // in-tune band (±3 ¢), brighter while the head is inside it
    const yb0 = trY(TR.ok, mid, half), yb1 = trY(-TR.ok, mid, half);
    g.fillStyle = rgba(tk.ok, 0.09 + 0.16 * S.trInA);
    g.fillRect(x0, yb0, pw + 7, yb1 - yb0);
    g.fillStyle = rgba(tk.ok, 0.25 + 0.4 * S.trInA);
    g.fillRect(x0, Math.round(yb0) - 0.5, pw + 7, 1); g.fillRect(x0, Math.round(yb1) - 0.5, pw + 7, 1);
    // grid: ±10, ±25 faint; ±50 at the edges
    g.fillStyle = tk.grid;
    [10, 25, -10, -25].forEach((cc) => g.fillRect(x0, Math.round(trY(cc, mid, half)), pw + 7, 1));
    g.fillStyle = tk.line;
    [50, -50].forEach((cc) => g.fillRect(x0, Math.round(trY(cc, mid, half)) - (cc > 0 ? 0 : 1), pw + 7, 1));
    // gutter: +50 / note / −50, and short ticks at ±10 / ±25
    g.fillStyle = tk.line; g.fillRect(x0 - 1, 0, 1, h);
    [10, 25, -10, -25].forEach((cc) => g.fillRect(x0 - 3, Math.round(trY(cc, mid, half)), 2, 1));
    g.fillStyle = tk.t3; g.textAlign = "right";
    g.fillText("+50", x0 - 4, trY(50, mid, half) + 3.5);
    g.fillText("−50", x0 - 4, trY(-50, mid, half) - 3);
    if (S.trNote != null) {
      const N = names(curKey()), pc = ((S.trNote % 12) + 12) % 12, oct = Math.floor(S.trNote / 12) - 1;
      const nm = N[pc], ow = 5;
      g.globalAlpha = 0.45 + 0.55 * S.trA;
      g.fillStyle = inTune ? rgba(tk.ok, 1) : tk.t2;
      g.font = `700 10px ${tk.font}`;
      const tw = g.measureText(nm).width;
      g.textAlign = "left";
      const lx = x0 - 4 - ow - tw;
      g.fillText(nm, lx, mid + 0.5);
      g.font = `600 7px ${tk.font}`; g.fillStyle = tk.t3;
      g.fillText(String(oct), lx + tw + 0.5, mid + 3.5);
      g.globalAlpha = 1;
    }
    if (C == null || tD == null) return;

    // ---- the trace: segments fade and thin with age; colour follows each sample's error ----
    g.save();
    g.beginPath(); g.rect(x0, 0, pw + 7, h); g.clip();
    g.lineCap = "round";
    const xs = (ts) => xH - ((tD - ts) / TR.span) * pw;
    let px = null, py = null, pc = 0, any = false;
    for (let i = 0; i < n; i++) {
      const v = vals[i];
      if (v == null) { px = null; continue; }
      const ts = S.trEnd - (n - 1 - i) * step;
      if (ts > tD) break;   // newer than the display time: the head covers it
      const age = tD - ts;
      if (age > TR.span) { px = null; continue; }
      const cc = (v - C) * 100, xx = xs(ts), yy = trY(cc, mid, half);
      if (px != null && Math.abs(cc - pc) < 80) {
        const u = 1 - age / TR.span, a = Math.pow(u, 1.6);
        g.strokeStyle = rgba(trColor(tk, (cc + pc) / 2), 0.08 + 0.92 * a);
        g.lineWidth = 0.7 + 1.9 * a;
        g.beginPath(); g.moveTo(px, py); g.lineTo(xx, yy); g.stroke();
      }
      px = xx; py = yy; pc = cc; any = true;
    }
    // join the newest sample to the head
    const hy = trY(Math.max(-52, Math.min(52, S.trH)), mid, half);
    if (live && px != null && Math.abs(pc - S.trH) < 80) { g.strokeStyle = rgba(hcol, 1); g.lineWidth = 2.6; g.beginPath(); g.moveTo(px, py); g.lineTo(xH, hy); g.stroke(); }
    g.restore();
    // ---- "now": faint cursor line, soft glow, bright dot ----
    const ha = 0.3 + 0.7 * S.trA;
    g.fillStyle = rgba(hcol, 0.18 * ha); g.fillRect(xH - 0.5, 0, 1, h);
    const gr = g.createRadialGradient(xH, hy, 0, xH, hy, 9);
    gr.addColorStop(0, rgba(hcol, 0.55 * S.trA)); gr.addColorStop(1, rgba(hcol, 0));
    g.fillStyle = gr; g.beginPath(); g.arc(xH, hy, 9, 0, 7); g.fill();
    g.fillStyle = rgba(hcol, ha); g.beginPath(); g.arc(xH, hy, live ? 3.4 : 2.6, 0, 7); g.fill();
    if (live) {   // hot core (dark) / a crisp rim against the light strip
      if (document.body.classList.contains("light")) { g.strokeStyle = `rgba(255,255,255,${(0.9 * S.trA).toFixed(3)})`; g.lineWidth = 1; g.beginPath(); g.arc(xH, hy, 3.9, 0, 7); g.stroke(); }
      else { g.fillStyle = `rgba(255,255,255,${(0.75 * S.trA).toFixed(3)})`; g.beginPath(); g.arc(xH, hy, 1.4, 0, 7); g.fill(); }
    }
    // keep drawing every frame while anything moves (tail still on screen, head still settling)
    S.trBusy = any || live || S.trA > 0.02 || Math.abs(S.trH) > 0.3 || S.trInA > 0.02;
  }
  function drawSpec(cv, dt) {
    const c = cv && canvasCtx(cv);
    if (!c) return;
    const { g, w, h } = c, tk = tokens(), sp = S.d && S.d.spec;
    const fx = (f) => (Math.log10(f / 40) / Math.log10(16000 / 40)) * w, dy = (db) => h - ((db + 84) / 78) * h;
    g.strokeStyle = tk.grid; g.lineWidth = 1;
    [100, 1000, 10000].forEach((f) => { g.beginPath(); g.moveTo(Math.round(fx(f)) + 0.5, 0); g.lineTo(Math.round(fx(f)) + 0.5, h); g.stroke(); });
    [-18, -36, -54, -72].forEach((db) => { g.beginPath(); g.moveTo(0, Math.round(dy(db)) + 0.5); g.lineTo(w, Math.round(dy(db)) + 0.5); g.stroke(); });
    if (!sp || !sp.in) return;
    const nb = sp.in.length;
    if (!S.bandsIn || S.bandsIn.length !== nb) { S.bandsIn = sp.in.slice(); S.bandsOut = sp.out.slice(); }
    const k = Math.min(1, dt * 18);
    for (let i = 0; i < nb; i++) { S.bandsIn[i] += (sp.in[i] - S.bandsIn[i]) * k; S.bandsOut[i] += (sp.out[i] - S.bandsOut[i]) * k; }
    const path = (arr) => { g.beginPath(); for (let i = 0; i < nb; i++) { const xx = ((i + 0.5) / nb) * w, yy = dy(Math.max(-90, arr[i])); i ? g.lineTo(xx, yy) : g.moveTo(xx, yy); } };
    path(S.bandsIn); g.strokeStyle = tk.inp; g.lineWidth = 1.2; g.setLineDash([3, 3]); g.stroke(); g.setLineDash([]);
    path(S.bandsOut); g.lineTo(w, h); g.lineTo(0, h); g.closePath();
    const gr = g.createLinearGradient(0, 0, 0, h); gr.addColorStop(0, hexA(tk.acc, 0.42)); gr.addColorStop(1, hexA(tk.acc, 0.02));
    g.fillStyle = gr; g.fill();
    path(S.bandsOut); g.strokeStyle = tk.acc; g.lineWidth = 1.6; g.stroke();
  }
  function hexA(c, a) { const m = /^#([0-9a-f]{6})$/i.exec(c); if (!m) return c; const n = parseInt(m[1], 16); return `rgba(${n >> 16},${(n >> 8) & 255},${n & 255},${a})`; }

  /* =====================================================================
     FULL TUNER
     ===================================================================== */
  function buildTuner() {
    if ($id("lpt")) return;
    const el = document.createElement("div");
    el.className = "lpt none"; el.id = "lpt"; el.hidden = true;
    el.setAttribute("role", "dialog"); el.setAttribute("aria-label", "Tuner");
    // needle gauge: ±50 cents over ±60°
    const cx = 310, cy = 300, r = 260, A = (c) => (c / 50) * 60 * Math.PI / 180;
    const pt = (c, rr) => [cx + rr * Math.sin(A(c)), cy - rr * Math.cos(A(c))];
    let g = `<svg class="lpt-gauge" viewBox="0 0 620 330">`;
    const [zx0, zy0] = pt(-2, r - 18), [zx1, zy1] = pt(2, r - 18);
    g += `<path class="zone" d="M${zx0} ${zy0} A${r - 18} ${r - 18} 0 0 1 ${zx1} ${zy1}"/>`;
    const [ax0, ay0] = pt(-50, r), [ax1, ay1] = pt(50, r);
    g += `<path class="arc" d="M${ax0} ${ay0} A${r} ${r} 0 0 1 ${ax1} ${ay1}"/>`;
    for (let c = -50; c <= 50; c += 5) {
      const major = c % 10 === 0, [x0, y0] = pt(c, r - (major ? 22 : 12)), [x1, y1] = pt(c, r);
      g += `<line class="tk${major ? " m" : ""}" x1="${x0}" y1="${y0}" x2="${x1}" y2="${y1}"/>`;
      if (major) { const [tx, ty] = pt(c, r + 16); g += `<text class="tl" x="${tx}" y="${ty + 4}">${c > 0 ? "+" : c < 0 ? "−" : ""}${Math.abs(c)}</text>`; }
    }
    g += `<line class="nd" data-k="gnd" x1="${cx}" y1="${cy}" x2="${cx}" y2="${cy - r + 26}"/><circle class="hub" cx="${cx}" cy="${cy}" r="9"/></svg>`;
    el.innerHTML = `<div class="lpt-in">
      <div class="lpt-hd"><h3>Tuner</h3>
        <button class="lp-pick" data-act="tuning"></button>
        <span class="lpt-ref"><button data-act="ref-" title="Lower the reference 0.5 Hz">−</button><span data-k="ref"></span><button data-act="ref+" title="Raise the reference 0.5 Hz">+</button></span>
        <span class="lp-seg" data-k="mode"><button data-mode="auto" title="Single notes, and every string at once when you strum them open">Auto</button><button data-mode="mono">Single</button><button data-mode="poly" title="Strum all open strings">Strum</button></span>
        <span class="lp-seg" data-k="disp"><button data-disp="needle">Needle</button><button data-disp="strobe">Strobe</button></span>
        <span class="sp"></span>
        <button class="lpt-mute" data-act="mute" title="Mute the output while the tuner is open"><i></i>Mute while tuning</button>
        <button class="lpt-close" data-act="close" title="Close (Esc)">${IC.close}</button></div>
      <div class="lpt-stage">
        <div class="lpt-mono" data-k="mono">${g}
          <div class="lpt-strobe" data-k="strobe" hidden><div></div><div></div><div></div></div>
          <div class="lpt-read"><div class="lpt-note" data-k="note">–</div><div class="lpt-nums"><b data-k="cents">–</b><span data-k="hz"></span><span data-k="hint"></span></div></div>
        </div>
        <div class="lpt-poly" data-k="poly" hidden></div>
        <div class="lpt-hint" data-k="phint" hidden>Strum all the open strings</div>
      </div>
      <div class="lpt-strs" data-k="strs"></div>
      <div class="lpt-foot"><span data-k="foot"></span><span><kbd>Esc</kbd> close</span></div>
    </div>`;
    document.body.append(el);
    el.addEventListener("pointerdown", (e) => { if (e.target === el) closeTuner(); });
    el.addEventListener("click", (e) => {
      const b = e.target.closest("[data-act],[data-mode],[data-disp]");
      if (!b) return;
      if (b.dataset.mode) { S.cfg.mode = b.dataset.mode; save(); paintTunerStatic(); paintData(); return; }
      if (b.dataset.disp) { S.cfg.display = b.dataset.disp; save(); paintTunerStatic(); return; }
      switch (b.dataset.act) {
        case "close": closeTuner(); break;
        case "tuning": tuningMenu(b); break;
        case "ref-": setRef(S.cfg.a4 - 0.5); break;
        case "ref+": setRef(S.cfg.a4 + 0.5); break;
        case "mute": S.cfg.tunerMute = !S.cfg.tunerMute; save(); applyMute(); paintTunerStatic(); break;
      }
    });
  }
  function paintTunerStatic() {
    const el = $id("lpt");
    if (!el) return;
    q1('[data-act="tuning"]', el).textContent = tuningLabel() + " ▾";
    setH(q1('[data-k="ref"]', el), `A4 ${S.cfg.a4.toFixed(1)} Hz`);
    el.querySelectorAll("[data-mode]").forEach((b) => b.classList.toggle("on", b.dataset.mode === S.cfg.mode));
    el.querySelectorAll("[data-disp]").forEach((b) => b.classList.toggle("on", b.dataset.disp === S.cfg.display));
    q1('[data-act="mute"]', el).classList.toggle("on", !!S.cfg.tunerMute);
    q1(".lpt-gauge", el).style.display = S.cfg.display === "needle" ? "" : "none";
    q1('[data-k="strobe"]', el).hidden = S.cfg.display !== "strobe";
    const st = S.cfg.inst === "chromatic" ? [] : tuningArr();
    q1('[data-k="strs"]', el).innerHTML = st.map((m, i) => `<span data-i="${i}">${noteHTML(m, null)}</span>`).join("");
    q1('[data-k="poly"]', el).innerHTML = st.map((m, i) => `<div class="lpt-ps" data-i="${i}"><div class="num">–</div><div class="lpt-leds">${Array.from({ length: 21 }, (_, k) => `<i class="${k === 10 ? "c" : ""}"></i>`).join("")}</div><div class="nm">${noteHTML(m, null)}</div></div>`).join("");
    q1('[data-k="poly"]', el).querySelectorAll(".nm .acc").forEach((a) => { a.style.fontSize = "13px"; a.style.top = "0"; });
    setH(q1('[data-k="foot"]', el), st.length > 1 ? "Strum all open strings to tune them at once." : S.cfg.inst === "chromatic" ? "Chromatic: any note, any instrument." : "");
  }
  function paintTunerData() {
    const el = $id("lpt");
    if (!el) return;
    const m = S.tm || tunerModel(), key = curKey();
    const poly = m.poly && m.strings.length > 1;
    q1('[data-k="mono"]', el).hidden = poly;
    q1('[data-k="poly"]', el).hidden = !poly;
    q1('[data-k="phint"]', el).hidden = !(S.cfg.mode === "poly" && poly && !m.polyStrs.some((p) => p && p.present));
    el.classList.toggle("poly", poly);
    el.classList.toggle("in", !poly && m.state === "in");
    el.classList.toggle("none", !poly && m.state === "none");
    if (!poly) {
      if (m.state !== "none") {
        setH(q1('[data-k="note"]', el), noteHTML(m.midi, key));
        const c = m.cents;
        setH(q1('[data-k="cents"]', el), `${c > 0.05 ? "+" : c < -0.05 ? "−" : "±"}${Math.abs(c).toFixed(1)}<small>¢</small>`);
        setH(q1('[data-k="hz"]', el), `${m.hz.toFixed(2)} Hz`);
        const hint = m.string != null ? (Math.abs(m.strDiff) > 0.5 ? `${m.strDiff < 0 ? "↑ tune up to" : "↓ tune down to"} ${names(key)[((m.strings[m.string] % 12) + 12) % 12]}` : m.state === "in" ? "In tune" : `String ${m.strings.length - m.string}`) : m.state === "in" ? "In tune" : "";
        setH(q1('[data-k="hint"]', el), hint);
        q1('[data-k="hint"]', el).className = m.string != null && Math.abs(m.strDiff) > 0.5 ? "hint" : "";
      } else {
        setH(q1('[data-k="note"]', el), "–");
        setH(q1('[data-k="cents"]', el), "–");
        setH(q1('[data-k="hz"]', el), S.d && S.d.chordal ? "Hearing a chord: strum open strings, or play one string" : "Play a single string");
        setH(q1('[data-k="hint"]', el), "");
      }
    } else {
      q1('[data-k="poly"]', el).querySelectorAll(".lpt-ps").forEach((col, i) => {
        const ps = m.polyStrs[i], has = ps && ps.present && ps.cents != null;
        col.classList.toggle("miss", !has);
        col.classList.toggle("in", !!has && Math.abs(ps.cents) < 2);
        setH(q1(".num", col), has ? `${ps.cents > 0 ? "+" : ps.cents < 0 ? "−" : ""}${Math.abs(ps.cents).toFixed(1)}` : "–");
        const lit = has ? Math.round(10 - Math.max(-50, Math.min(50, ps.cents)) / 5) : -1;
        col.querySelectorAll(".lpt-leds i").forEach((led, k) => led.classList.toggle("lit", k === lit || (has && Math.abs(ps.cents) < 2 && Math.abs(k - 10) <= 1)));
      });
    }
    q1('[data-k="strs"]', el).querySelectorAll("span").forEach((s, i) => s.classList.toggle("on", !poly && m.state !== "none" && m.string === i && Math.abs(m.strDiff) <= 0.5));
  }
  function tunerFrame(dt) {
    const el = $id("lpt");
    if (!el) return;
    const m = S.tm, live = m && m.state !== "none" && !m.poly;
    const nd = q1('[data-k="gnd"]', el);
    if (nd) nd.setAttribute("transform", `rotate(${(S.needle / 50) * 60} 310 300)`);
    if (S.cfg.display === "strobe") {
      const c = live ? S.needleT : 0;
      const rows = el.querySelectorAll(".lpt-strobe div");
      rows.forEach((r, i) => { S.strobeBig[i] += c * dt * [4, 8, 16][i]; r.style.backgroundPosition = `${S.strobeBig[i].toFixed(1)}px 0`; });
    }
  }
  async function applyMute() {
    if (!S.tuner.open) return;
    const audioOn = () => typeof audioCfg !== "undefined" && audioCfg && audioCfg.current && audioCfg.current.monitor !== false;
    if (S.cfg.tunerMute && !S.tuner.mutedByMe && (typeof audioCfg === "undefined" || !audioCfg || audioOn())) {
      S.tuner.mutedByMe = true;
      if (typeof setAudio === "function") await setAudio({ monitor: 0 }); else await getJSON("/api/audio/set?monitor=0").catch(() => {});
    } else if (!S.cfg.tunerMute && S.tuner.mutedByMe) restoreMute();
  }
  async function restoreMute() {
    if (!S.tuner.mutedByMe) return;
    S.tuner.mutedByMe = false;
    if (typeof setAudio === "function") await setAudio({ monitor: 1 }); else await getJSON("/api/audio/set?monitor=1").catch(() => {});
  }
  function openTuner() {
    buildTuner();
    const el = $id("lpt");
    S.tuner.open = true;
    kick();
    el.hidden = false;
    requestAnimationFrame(() => el.classList.add("open"));
    paintTunerStatic(); paintTunerData();
    applyMute();
  }
  function closeTuner() {
    const el = $id("lpt");
    if (!el || !S.tuner.open) return;
    S.tuner.open = false;
    el.classList.remove("open");
    setTimeout(() => { if (!S.tuner.open) el.hidden = true; }, 220);
    restoreMute();
  }
  document.addEventListener("keydown", (e) => {
    if (S.tuner.open && e.key === "Escape") { e.preventDefault(); e.stopPropagation(); closeTuner(); }
  }, true);
  window.addEventListener("beforeunload", () => { if (S.tuner.mutedByMe) restoreMute(); });

  /* =====================================================================
     API (Settings window, Live view)
     ===================================================================== */
  const api = {
    cards: () => CARDS.map((c) => ({ id: c.id, label: c.label, visible: !S.cfg.hidden.includes(c.id), index: S.cfg.order.indexOf(c.id) })).sort((a, b) => a.index - b.index),
    setVisible, setOrder, reset: resetLayout,
    instruments: () => Object.entries(INSTR).map(([id, v]) => ({ id, label: v.label, tunings: Object.entries(v.t).map(([tid, t]) => ({ id: tid, label: t[0], notes: t[1] })) })),
    setTuning, setReference: setRef,
    // (scale / keyLock / key are additions for the Fretboard page; the older fields are unchanged)
    state: () => ({ cards: api.cards(), instrument: S.cfg.inst, tuning: S.cfg.tuning, a4: S.cfg.a4, mode: S.cfg.mode, display: S.cfg.display, tunerMute: S.cfg.tunerMute, scale: S.cfg.scale, keyLock: S.cfg.keyLock, key: curKey() }),
    onChange: (fn) => { S.listeners.add(fn); return () => S.listeners.delete(fn); },
    openTuner, closeTuner, isTunerOpen: () => S.tuner.open,
    latest: () => S.d,          // raw /api/listen data
    want: () => { S.wantT = performance.now(); kick(); },   // keep polling while another view shows tuner data
    scopeOn,                    // index.html's scope loop runs only while this is true
    fresh: () => S.ok === true && !!S.d && performance.now() - (S.dT || 0) < 400,
    tuner: () => S.tm || null,  // { state, midi, cents, hz, string, poly, … } with the reference applied
  };
  window.ListenPane = api;

  function init() {
    build();
    // pane collapse (.workspace.r-collapsed) and Live full screen (body.hr-live-full) restart / end the loops
    const mo = new MutationObserver(kick), ws = q1(".workspace");
    if (ws) mo.observe(ws, { attributes: true, attributeFilter: ["class"] });
    mo.observe(document.body, { attributes: true, attributeFilter: ["class"] });
    if (window.HRActivity) HRActivity.on(kick);
    kick();
  }
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", init); else init();
})();
