/* Headroom — Fretboard view: a big interactive neck that lights up with the key  (assets/fretboard-view.js → #view-fretboard)
 *
 *   - Key: follows the key the engine detects (GET /api/listen → key), or lock one by hand (root + major/minor).
 *   - Scales: auto (key scale / pentatonic / blues follow major↔minor), the 7 modes, harmonic & melodic minor,
 *     pentatonics, blues, Mixolydian ♭6, Phrygian dominant, whole tone, diminished (H–W / W–H), chromatic.
 *     Labels: note names, scale degrees, intervals or none. Dots are colour-coded by degree; the root is emphasised.
 *   - Positions: CAGED shapes (pentatonic / blues boxes) and 3-notes-per-string, one at a time with a slider
 *     (dim or hide the rest, optionally following your hand).
 *   - Live: the note you're playing lights up where you're most likely fretting it (hand position inferred from
 *     recent notes; other places for the same pitch get a faint ring), a short fading trail, the vibrating part of
 *     the string shimmers. While a chord sounds, a likely grip is shown. Chord-tone overlay of the live chord or of
 *     any diatonic chord (I ii iii … strip), or arpeggio-only.
 *   - Practice: targets (find the ♭3 …, by eye or by ear), "only notes I haven't played", click a note to hear a
 *     soft reference tone (WebAudio in the page, never the engine).
 *   - Neck: rosewood / maple / ebony, dots or blocks, 22/24 frets, fit 12 / 15 / all, real (equal-tempered) or even
 *     spacing, left-handed, high or low string on top, capo; tuning shared with the tuner (Listen pane).
 *
 * Data: window.ListenPane (assets/listen-pane.js): want() keeps its /api/listen poll alive, latest() is the raw
 * reading, state()/instruments()/setTuning()/onChange() for the tuning. Without ListenPane it polls /api/listen itself.
 * Idle CPU (assets/activity.js): the loop runs only while this tab is shown and the window is awake; nothing
 * ticks, polls or animates otherwise. The wood is a static SVG drawn into an <img> (rasterised once); only the notes layer repaints.
 */
(function () {
  "use strict";
  const LS_KEY = "hr.fretboard.v1";
  const clamp = (v, a, b) => Math.max(a, Math.min(b, v));
  const now = () => performance.now();
  const f1 = (v) => Math.round(v * 10) / 10;
  const escH = (s) => String(s ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
  const say = (m) => { if (typeof toast === "function") toast(m); };
  const typingIn = (a) => a && ((a.tagName === "INPUT" && a.type !== "range") || a.tagName === "SELECT" || a.tagName === "TEXTAREA" || a.isContentEditable);
  const base = () => (typeof API_BASE !== "undefined" ? API_BASE : "http://127.0.0.1:17770");
  const awake = () => (window.HRActivity ? HRActivity.awake() : document.visibilityState !== "hidden");

  /* =====================================================================
     MUSIC THEORY
     ===================================================================== */
  const LET = ["C", "D", "E", "F", "G", "A", "B"], NAT = [0, 2, 4, 5, 7, 9, 11];
  const ACC = { "-2": "♭♭", "-1": "♭", 0: "", 1: "♯", 2: "♯♯" };
  const SH = ["C", "C♯", "D", "D♯", "E", "F", "F♯", "G", "G♯", "A", "A♯", "B"];
  const FL = ["C", "D♭", "D", "E♭", "E", "F", "G♭", "G", "A♭", "A", "B♭", "B"];
  const MIX = ["C", "C♯", "D", "E♭", "E", "F", "F♯", "G", "A♭", "A", "B♭", "B"];
  const CH_DEG = ["1", "b2", "2", "b3", "3", "4", "b5", "5", "b6", "6", "b7", "7"];
  const CH_IV = ["R", "m2", "M2", "m3", "M3", "P4", "TT", "P5", "m6", "M6", "m7", "M7"];
  const IVN = { 1: "R", b2: "m2", 2: "M2", "#2": "A2", b3: "m3", 3: "M3", 4: "P4", "#4": "A4", b5: "d5", 5: "P5", "#5": "A5", b6: "m6", 6: "M6", bb7: "d7", b7: "m7", 7: "M7" };
  const ROMAN = ["I", "II", "III", "IV", "V", "VI", "VII"];
  const pretty = (l) => String(l).replace(/bb/g, "♭♭").replace(/b/g, "♭").replace(/#/g, "♯");
  function degParse(l) {
    const m = /^([b#]*)(\d+)$/.exec(l);
    const acc = [...m[1]].reduce((a, c) => a + (c === "#" ? 1 : -1), 0), num = +m[2];
    return { acc, num, semi: (NAT[(num - 1) % 7] + acc + 120) % 12 };
  }
  const SC = {
    ionian: { n: "Major (Ionian)", t: "major", degs: "1 2 3 4 5 6 7" },
    dorian: { n: "Dorian", t: "Dorian", degs: "1 2 b3 4 5 6 b7" },
    phrygian: { n: "Phrygian", t: "Phrygian", degs: "1 b2 b3 4 5 b6 b7" },
    lydian: { n: "Lydian", t: "Lydian", degs: "1 2 3 #4 5 6 7" },
    mixolydian: { n: "Mixolydian", t: "Mixolydian", degs: "1 2 3 4 5 6 b7" },
    aeolian: { n: "Natural minor (Aeolian)", t: "natural minor", degs: "1 2 b3 4 5 b6 b7" },
    locrian: { n: "Locrian", t: "Locrian", degs: "1 b2 b3 4 b5 b6 b7" },
    harmonic: { n: "Harmonic minor", t: "harmonic minor", degs: "1 2 b3 4 5 b6 7" },
    melodic: { n: "Melodic minor", t: "melodic minor", degs: "1 2 b3 4 5 6 7" },
    pentmaj: { n: "Major pentatonic", t: "major pentatonic", degs: "1 2 3 5 6", box: "maj", parent: "ionian" },
    pentmin: { n: "Minor pentatonic", t: "minor pentatonic", degs: "1 b3 4 5 b7", box: "min", parent: "aeolian" },
    bluesmin: { n: "Minor blues", t: "blues", degs: "1 b3 4 b5 5 b7", box: "min", parent: "aeolian" },
    bluesmaj: { n: "Major blues", t: "major blues", degs: "1 2 b3 3 5 6", box: "maj", parent: "ionian" },
    mixb6: { n: "Mixolydian ♭6", t: "Mixolydian ♭6", degs: "1 2 3 4 5 b6 b7" },
    phrdom: { n: "Phrygian dominant", t: "Phrygian dominant", degs: "1 b2 3 4 5 b6 b7" },
    wholetone: { n: "Whole tone", t: "whole tone", degs: "1 2 3 #4 #5 b7" },
    dimhw: { n: "Diminished (half–whole)", t: "diminished H–W", degs: "1 b2 b3 3 #4 5 6 b7" },
    dimwh: { n: "Diminished (whole–half)", t: "diminished W–H", degs: "1 2 b3 4 b5 b6 6 7" },
    chromatic: { n: "Chromatic", t: "chromatic", degs: "1 b2 2 b3 3 4 b5 5 b6 6 b7 7" },
  };
  Object.values(SC).forEach((s) => { s.degs = s.degs.split(" "); s.semis = s.degs.map((d) => degParse(d).semi); });
  const AUTO = {
    key: { n: "Key scale", sub: "major or natural minor, follows the key", maj: "ionian", min: "aeolian", sfx: "" },
    pent: { n: "Pentatonic", sub: "follows the key", maj: "pentmaj", min: "pentmin", sfx: " pentatonic" },
    blues: { n: "Blues", sub: "follows the key", maj: "bluesmaj", min: "bluesmin", sfx: " blues" },
  };
  const SCALE_MENU = [
    ["Follow the key", ["key", "pent", "blues"]],
    ["Major & the modes", ["ionian", "dorian", "phrygian", "lydian", "mixolydian", "aeolian", "locrian"]],
    ["Minor", ["harmonic", "melodic"]],
    ["Pentatonic & blues", ["pentmin", "pentmaj", "bluesmin", "bluesmaj"]],
    ["More colours", ["mixb6", "phrdom", "wholetone", "dimhw", "dimwh", "chromatic"]],
  ];
  // the order the scale cycles in with [ and ]
  const SCALE_CYCLE = SCALE_MENU.flatMap((g) => g[1]);

  function spellWith(rootPc, li, degs) {
    return degs.map((l) => {
      const d = degParse(l), L = (li + d.num - 1) % 7, pc = (rootPc + d.semi) % 12;
      return { letter: LET[L], acc: ((pc - NAT[L] + 18) % 12) - 6, pc };
    });
  }
  function bestSpelling(rootPc, degs, prefer) {
    let best = null;
    for (let li = 0; li < 7; li++) {
      const a = ((rootPc - NAT[li] + 18) % 12) - 6;
      if (Math.abs(a) > 1) continue;
      const sp = spellWith(rootPc, li, degs);
      let cost = 0;
      sp.forEach((x) => { cost += Math.abs(x.acc) + (Math.abs(x.acc) > 1 ? 6 : 0); });
      if (prefer != null && li === prefer) cost -= 0.5;
      if (a < 0 && rootPc !== 6) cost -= 0.1;
      if (a > 0 && rootPc === 6) cost -= 0.1;
      if (!best || cost < best.cost) best = { cost, li, sp };
    }
    return best;
  }
  const nm = (x) => x.letter + ACC[x.acc];

  // chord qualities (engine's set + the diatonic 7ths)
  const Q = {
    maj: { iv: [0, 4, 7], s: "" }, min: { iv: [0, 3, 7], s: "m" }, "5": { iv: [0, 7], s: "5" }, "7": { iv: [0, 4, 7, 10], s: "7" },
    maj7: { iv: [0, 4, 7, 11], s: "maj7" }, m7: { iv: [0, 3, 7, 10], s: "m7" }, sus2: { iv: [0, 2, 7], s: "sus2" }, sus4: { iv: [0, 5, 7], s: "sus4" },
    dim: { iv: [0, 3, 6], s: "°" }, aug: { iv: [0, 4, 8], s: "+" }, m7b5: { iv: [0, 3, 6, 10], s: "m7♭5" }, add9: { iv: [0, 2, 4, 7], s: "add9" },
    "6": { iv: [0, 4, 7, 9], s: "6" }, m6: { iv: [0, 3, 7, 9], s: "m6" }, dim7: { iv: [0, 3, 6, 9], s: "°7" }, "9": { iv: [0, 2, 4, 7, 10], s: "9" },
    m9: { iv: [0, 2, 3, 7, 10], s: "m9" }, mmaj7: { iv: [0, 3, 7, 11], s: "m(maj7)" }, augmaj7: { iv: [0, 4, 8, 11], s: "+maj7" }, aug7: { iv: [0, 4, 8, 10], s: "+7" },
  };
  const ARP_NUM = { 0: 1, 1: 2, 2: 2, 3: 3, 4: 3, 5: 4, 6: 5, 7: 5, 8: 5, 9: 6, 10: 7, 11: 7 };
  const MINORQ = new Set(["min", "m7", "m6", "m9", "dim", "m7b5", "dim7", "mmaj7"]);
  function chordIvLabel(q, semi) {
    if (q === "dim7" && semi === 9) return "♭♭7";
    if ((q === "sus4") && semi === 5) return "4";
    return { 0: "R", 1: "♭9", 2: "9", 3: "♭3", 4: "3", 5: "11", 6: "♭5", 7: "5", 8: "♯5", 9: "6", 10: "♭7", 11: "7" }[semi];
  }
  function triadQuality(a, b, c, d, sev) {
    const t = (b - a + 12) % 12, f = (c - a + 12) % 12, s = d == null ? null : (d - a + 12) % 12;
    if (t === 4 && f === 7) return sev ? (s === 11 ? "maj7" : "7") : "maj";
    if (t === 3 && f === 7) return sev ? (s === 11 ? "mmaj7" : "m7") : "min";
    if (t === 3 && f === 6) return sev ? (s === 9 ? "dim7" : "m7b5") : "dim";
    if (t === 4 && f === 8) return sev ? (s === 11 ? "augmaj7" : "aug7") : "aug";
    if (t === 2 && f === 7) return "sus2";
    if (t === 5 && f === 7) return "sus4";
    return sev ? "7" : "maj";
  }

  /* ---------- instruments (the tuner's list comes from ListenPane.instruments()) ---------- */
  const PC = { C: 0, D: 2, E: 4, F: 5, G: 7, A: 9, B: 11 };
  const parseNote = (s) => { const m = /^([A-G])([#b]?)(-?\d)$/.exec(s); if (!m) return null; return 12 * (+m[3] + 1) + PC[m[1]] + (m[2] === "#" ? 1 : m[2] === "b" ? -1 : 0); };
  const FALLBACK_INSTR = [
    { id: "guitar6", label: "Guitar", tunings: [{ id: "std", label: "Standard", notes: "E2 A2 D3 G3 B3 E4" }, { id: "dropd", label: "Drop D", notes: "D2 A2 D3 G3 B3 E4" }, { id: "dadgad", label: "DADGAD", notes: "D2 A2 D3 G3 A3 D4" }, { id: "openg", label: "Open G", notes: "D2 G2 D3 G3 B3 D4" }] },
    { id: "guitar7", label: "7-string", tunings: [{ id: "std", label: "Standard", notes: "B1 E2 A2 D3 G3 B3 E4" }] },
    { id: "bass4", label: "Bass", tunings: [{ id: "std", label: "Standard", notes: "E1 A1 D2 G2" }] },
    { id: "bass5", label: "5-string bass", tunings: [{ id: "std", label: "Standard", notes: "B0 E1 A1 D2 G2" }] },
  ];
  const UNFRETTED = new Set(["chromatic", "violin", "cello", "banjo"]);
  const MAXF = { uke: 15, mandolin: 20 };

  /* ---------- palette (neck materials) ---------- */
  const WOODS = {
    rosewood: { n: "Rosewood", base: ["#61382a", "#4a2a1d", "#341d13"], grain: "#170904", ga: 0.62, fig: "#9a6342", fa: 0.2, pore: "#0d0503", bind: "#eadfc6", dots: "pearl" },
    ebony: { n: "Ebony", base: ["#2a2420", "#1b1715", "#110e0c"], grain: "#000000", ga: 0.5, fig: "#544a42", fa: 0.17, pore: "#000000", bind: "#eadfc6", dots: "pearl" },
    maple: { n: "Maple", base: ["#f2d7a6", "#e6c38b", "#d4ab6e"], grain: "#9b6a33", ga: 0.36, fig: "#fff3d6", fa: 0.38, pore: "#7a5228", bind: null, dots: "black" },
  };
  const INLAY_FRETS = [3, 5, 7, 9, 15, 17, 19, 21], DOUBLE = [12, 24];

  /* =====================================================================
     PREFS + STATE
     ===================================================================== */
  const DEF = {
    follow: true, lock: null, lastKey: null, scale: "pent", labels: "note", color: "degree",
    wood: "rosewood", inlay: "dots", frets: 24, zoom: "all", spacing: "real", lefty: false, highTop: true, capo: 0,
    psys: "all", pidx: 1, phide: false, phand: false,
    chordLive: true, chordSel: null, omode: "tones", sev: false,
    targets: false, tHide: false, unplayed: false, sound: true, trail: true,
  };
  const prefs = (() => { try { return Object.assign({}, DEF, JSON.parse(localStorage.getItem(LS_KEY) || "{}")); } catch (e) { return Object.assign({}, DEF); } })();
  let saveT = 0;
  const save = () => { clearTimeout(saveT); saveT = setTimeout(() => { try { localStorage.setItem(LS_KEY, JSON.stringify(prefs)); } catch (e) { /* private mode */ } }, 250); };

  const F = {
    built: false, visible: false, root: null, els: {},
    d: null, lastD: null, det: null, chord: null,
    loopT: 0, wantT: 0, ownBusy: false,
    tun: null, G: null, staticSig: "", cells: [], cellMap: new Map(), M: null, pos: null, posSig: "",
    ws: 0, wsManual: false,
    hand: null, prevS: null, lastOnsetT: 0, lastOnsetAbs: -1, lastMono: null, onsets: [],
    hot: null, hotAlt: [], grip: null, gripSig: "", playedSig: 0,
    played: new Set(), target: null, tScore: 0, tLast: -1,
    hover: null, flash: null,
    stats: { ticks: 0, data: 0, paints: 0, rebuilds: 0 },
  };

  /* =====================================================================
     TUNING / KEY / MODEL
     ===================================================================== */
  function tuningInfo() {
    const lp = window.ListenPane;
    let list = FALLBACK_INSTR, inst = "guitar6", tid = "std";
    if (lp && lp.state && lp.instruments) { try { const st = lp.state(); inst = st.instrument; tid = st.tuning; list = lp.instruments(); } catch (e) { /* older pane */ } }
    let I = list.find((i) => i.id === inst), T = I && (I.tunings.find((t) => t.id === tid) || I.tunings[0]);
    let note = "";
    if (!I || !T || !T.notes || UNFRETTED.has(inst)) {
      note = `The tuner is set to ${I ? I.label.toLowerCase() : inst}, so this shows a guitar in standard tuning.`;
      I = list.find((i) => i.id === "guitar6") || FALLBACK_INSTR[0]; T = I.tunings[0];
    }
    const notes = T.notes.split(" ").map(parseNote).filter((x) => x != null);
    const short = I.id === "guitar6" ? T.label : T.label === "Standard" ? I.label : `${I.label} · ${T.label}`;
    return { inst: I.id, tid: T.id, label: `${I.label} · ${T.label}`, short, notes, note, list, maxF: Math.min(prefs.frets, MAXF[I.id] || 24), bass: /^bass/.test(I.id) };
  }
  function curKey() {
    if (!prefs.follow && prefs.lock) return { root: prefs.lock.root, minor: !!prefs.lock.minor, src: "lock" };
    if (F.det) return { root: F.det.root, minor: F.det.minor, src: "live" };
    const k = prefs.lastKey || { root: 9, minor: true };
    return { root: k.root, minor: !!k.minor, src: "last" };
  }
  function keySpelling(root, minor) { return bestSpelling(root, (minor ? SC.aeolian : SC.ionian).degs, null); }
  function keyName(k) { return nm(keySpelling(k.root, k.minor).sp[0]); }
  function resolveScale(id, minor) {
    const a = AUTO[id];
    const sid = a ? (minor ? a.min : a.maj) : SC[id] ? id : "pentmin";
    return { id: sid, auto: a ? id : null, def: SC[sid] };
  }

  function computeModel() {
    const key = curKey(), rs = resolveScale(prefs.scale, key.minor), sc = rs.def;
    const ks = keySpelling(key.root, key.minor);
    const sp = bestSpelling(key.root, sc.degs, ks.li);
    const names = sp.sp.map(nm);
    const fb = names.some((n) => n.includes("♭")) ? FL : names.some((n) => n.includes("♯")) ? SH : MIX;
    const info = [];
    for (let pc = 0; pc < 12; pc++) {
      const semi = (pc - key.root + 12) % 12;
      info[pc] = { in: false, lbl: pretty(CH_DEG[semi]), iv: CH_IV[semi], name: fb[pc], num: degParse(CH_DEG[semi]).num, semi };
    }
    sc.degs.forEach((d, i) => {
      const pc = (key.root + sc.semis[i]) % 12;
      info[pc] = { in: true, lbl: pretty(d), iv: IVN[d] || CH_IV[sc.semis[i]], name: names[i], num: degParse(d).num, semi: sc.semis[i], i };
    });
    const pcs = sc.semis.map((s) => (key.root + s) % 12);
    const rootName = names[0], kName = nm(ks.sp[0]);
    const title = rs.auto ? `${kName} ${key.minor ? "minor" : "major"}${AUTO[rs.auto].sfx}` : `${rootName} ${sc.t}`;
    // diatonic chords: from the scale when it has 7 notes, else from the key's major / natural minor
    const parent = sc.degs.length === 7 ? sc : SC[sc.parent || (key.minor ? "aeolian" : "ionian")];
    const psp = spellWith(key.root, sp.li, parent.degs);
    const dia = parent.degs.map((d, i) => {
      const pp = (j) => (key.root + parent.semis[(i + j) % 7]) % 12;
      const q = triadQuality(pp(0), pp(2), pp(4), prefs.sev ? pp(6) : null, prefs.sev);
      const dp = degParse(d);
      let r = ROMAN[dp.num - 1];
      if (MINORQ.has(q)) r = r.toLowerCase();
      const tail = { dim: "°", aug: "+", m7b5: "ø7", dim7: "°7", maj7: "M7", "7": "7", m7: "7", mmaj7: "M7", augmaj7: "+M7", aug7: "+7", sus2: "sus2", sus4: "sus4" }[q] || "";
      return { root: pp(0), quality: q, roman: (dp.acc < 0 ? "♭" : dp.acc > 0 ? "♯" : "") + r + tail, name: nm(psp[i]) + Q[q].s };
    });
    return { key, rs, sc, info, pcs, names, fb, rootName, kName, title, dia, pcSet: new Set(pcs) };
  }
  function pcName(pc) { const M = F.M; return M ? M.info[pc].name : MIX[pc]; }
  function chordName(c) { return pcName(c.root) + (Q[c.quality] || Q.maj).s + (c.bass != null && c.bass >= 0 && c.bass !== c.root ? "/" + pcName(c.bass) : ""); }
  function romanOf(c) {
    const M = F.M;
    const d = M.dia.find((x) => x.root === c.root && MINORQ.has(x.quality) === MINORQ.has(c.quality));
    if (d) return d.roman.replace(/[M+°ø]?7$|sus\d$/, "") + ({ dim: "°", aug: "+", m7b5: "ø7", dim7: "°7", "7": "7", maj7: "M7", m7: "7" }[c.quality] || "");
    const semi = (c.root - M.key.root + 12) % 12;
    const lab = CH_DEG[semi], dp = degParse(lab);
    let r = ROMAN[dp.num - 1];
    if (MINORQ.has(c.quality)) r = r.toLowerCase();
    return (dp.acc < 0 ? "♭" : "") + r + ({ dim: "°", aug: "+", "7": "7" }[c.quality] || "");
  }
  function overlayChord() {
    let c = null;
    if (prefs.chordSel) c = prefs.chordSel;
    else if (prefs.chordLive && F.chord) c = F.chord;
    if (!c) return null;
    const q = Q[c.quality] ? c.quality : "maj";
    const ivs = new Map();
    Q[q].iv.forEach((s) => ivs.set((c.root + s) % 12, chordIvLabel(q, s)));
    return { root: c.root, quality: q, bass: c.bass, pcs: ivs, name: chordName({ root: c.root, quality: q, bass: c.bass }), pinned: !!prefs.chordSel };
  }

  /* =====================================================================
     POSITIONS (CAGED / boxes, 3 notes per string)
     ===================================================================== */
  function posSystems(M) {
    const sc = M.sc, n = sc.degs.length;
    return { caged: !!sc.box || n === 7, nps: n >= 6 && n < 12, boxes: !!sc.box };
  }
  function computePositions() {
    const M = F.M, T = F.tun, capo = prefs.capo, maxF = T.maxF;
    const sys = posSystems(M);
    let kind = prefs.psys;
    if ((kind === "caged" && !sys.caged) || (kind === "nps" && !sys.nps)) kind = "all";
    const sig = [kind, M.key.root, M.sc.degs.join(), T.notes.join(), capo, maxF].join("|");
    if (sig === F.posSig) return F.pos;
    F.posSig = sig;
    if (kind === "all") return (F.pos = { kind, list: [] });
    const tun = T.notes, low = tun[0];
    const inSet = (pcs) => { const s = new Set(pcs); return (m) => s.has(((m % 12) + 12) % 12); };
    const nps = (startPc, k, isIn) => {
      const f0 = capo + ((((startPc - (low + capo)) % 12) + 12) % 12);
      let m = low + f0;
      const out = [];
      for (let s = 0; s < tun.length; s++) for (let j = 0; j < k; j++) {
        if (s || j) { m++; while (!isIn(m)) m++; }
        out.push({ s, f: m - tun[s], m });
      }
      if (out.some((x) => x.f < capo)) out.forEach((x) => { x.f += 12; x.m += 12; });
      return out;
    };
    const span = (notes) => ({ lo: Math.min(...notes.map((x) => x.f)), hi: Math.max(...notes.map((x) => x.f)) });
    const std6 = tun.length === 6 && tun.slice(1).map((v, i) => v - tun[i]).join() === "5,5,5,4,5";
    const isIn = inSet(M.pcs);
    const list = [];
    if (kind === "caged") {
      const pentSemis = (M.sc.box || (M.sc.semis.includes(4) ? "maj" : "min")) === "maj" ? [0, 2, 4, 7, 9] : [0, 3, 5, 7, 10];
      const shapes = pentSemis[1] === 2 ? ["E", "D", "C", "A", "G"] : ["G", "E", "D", "C", "A"];
      const pentIn = inSet(pentSemis.map((s) => (M.key.root + s) % 12));
      pentSemis.forEach((ps, k) => {
        const box = nps((M.key.root + ps) % 12, 2, pentIn);
        const { lo, hi } = span(box);
        let notes;
        if (M.sc.box) {
          notes = box.slice();
          const have = new Set(notes.map((x) => x.m));
          for (let s = 0; s < tun.length; s++) for (let f = Math.max(capo, lo); f <= hi; f++) { const m = tun[s] + f; if (isIn(m) && !have.has(m)) { notes.push({ s, f, m }); have.add(m); } }
        } else {
          const cand = [];
          for (let s = 0; s < tun.length; s++) for (let f = Math.max(capo, lo - 1); f <= hi + 1; f++) { const m = tun[s] + f; if (isIn(m)) cand.push({ s, f, m, inner: f >= lo && f <= hi }); }
          const cnt = new Array(tun.length).fill(0);
          cand.forEach((c) => { if (c.inner) cnt[c.s]++; });
          const byM = new Map();
          cand.filter((c) => c.inner).forEach((c) => { const o = byM.get(c.m); if (!o || cnt[c.s] < cnt[o.s]) byM.set(c.m, c); });
          cand.filter((c) => !c.inner).forEach((c) => { if (!byM.has(c.m)) byM.set(c.m, c); });
          notes = [...byM.values()];
        }
        const sem = degParse(CH_DEG[ps]).num;
        list.push({ notes, name: M.sc.box ? `Box ${k + 1}` : std6 ? `${shapes[k]} shape` : `Position ${k + 1}`, sub: M.sc.box && std6 ? `${shapes[k]} shape` : `from the ${M.info[(M.key.root + ps) % 12].lbl}`, sem });
      });
    } else {
      M.sc.semis.forEach((s, k) => {
        const notes = nps((M.key.root + s) % 12, 3, isIn);
        list.push({ notes, name: `Position ${k + 1}`, sub: `from the ${pretty(M.sc.degs[k])}` });
      });
    }
    // octave copies that fit on the neck
    list.forEach((p) => {
      p.inst = [];
      for (const sh of [-12, 0, 12, 24]) {
        const ns = p.notes.map((x) => ({ s: x.s, f: x.f + sh, m: x.m + sh }));
        if (ns.every((x) => x.f >= capo && x.f <= maxF)) p.inst.push(Object.assign({ notes: ns }, span(ns)));
      }
      if (!p.inst.length) { const ns = p.notes.filter((x) => x.f <= maxF); if (ns.length) p.inst.push(Object.assign({ notes: ns }, span(ns))); }
      p.keys = new Set(p.inst.flatMap((i) => i.notes.map((x) => x.s + ":" + x.f)));
    });
    return (F.pos = { kind, list });
  }
  function curPos() {
    const P = F.pos;
    if (!P || P.kind === "all" || !P.list.length) return null;
    const i = clamp(prefs.pidx, 1, P.list.length);
    return Object.assign({ idx: i }, P.list[i - 1]);
  }

  /* =====================================================================
     BUILD
     ===================================================================== */
  const IC = {
    chev: '<svg class="chev" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"><path d="M4.5 6.5L8 10l3.5-3.5"/></svg>',
    lock: '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5"><rect x="3.5" y="7" width="9" height="6.5" rx="1.5"/><path d="M5.5 7V5a2.5 2.5 0 015 0v2"/></svg>',
    target: '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.4"><circle cx="8" cy="8" r="5.6"/><circle cx="8" cy="8" r="2.6"/><circle cx="8" cy="8" r=".6" fill="currentColor"/></svg>',
    unplayed: '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.4"><circle cx="4.5" cy="5" r="2"/><circle cx="11.5" cy="5" r="2" stroke-dasharray="1.6 1.4"/><circle cx="4.5" cy="11.5" r="2" stroke-dasharray="1.6 1.4"/><circle cx="11.5" cy="11.5" r="2"/></svg>',
    sound: '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"><path d="M2.5 6h2.2L8 3.2v9.6L4.7 10H2.5z" fill="currentColor" stroke="none"/><path d="M10.5 5.6a3.4 3.4 0 0 1 0 4.8"/><path d="M12.4 3.8a6 6 0 0 1 0 8.4"/></svg>',
    mute: '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"><path d="M2.5 6h2.2L8 3.2v9.6L4.7 10H2.5z" fill="currentColor" stroke="none"/><path d="M10.5 6l3.5 4M14 6l-3.5 4"/></svg>',
    gear: '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.4"><circle cx="8" cy="8" r="2.2"/><path d="M8 1.8v1.6M8 12.6v1.6M14.2 8h-1.6M3.4 8H1.8M12.4 3.6l-1.1 1.1M4.7 11.3l-1.1 1.1M12.4 12.4l-1.1-1.1M4.7 4.7L3.6 3.6"/></svg>',
    eye: '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.4"><path d="M1.5 8s2.4-4.5 6.5-4.5S14.5 8 14.5 8 12.1 12.5 8 12.5 1.5 8 1.5 8z"/><circle cx="8" cy="8" r="2"/></svg>',
    hand: '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round"><path d="M5 8.5V3.8a1 1 0 012 0V8M7 7.5V2.8a1 1 0 012 0V7.5M9 7.6V3.6a1 1 0 012 0v5.2M11 6.6a1 1 0 012 0v3.2c0 2.6-2 4.4-4.5 4.4-1.7 0-2.8-.8-3.6-2L3.3 9.6a1 1 0 011.6-1.1L5 8.6"/></svg>',
    dim: '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.4"><circle cx="5" cy="8" r="2.4"/><circle cx="11" cy="8" r="2.4" opacity=".4"/></svg>',
    hide: '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.4"><circle cx="5" cy="8" r="2.4"/><circle cx="11" cy="8" r="2.4" stroke-dasharray="1.4 1.6" opacity=".55"/></svg>',
    prev: '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"><path d="M9.5 4L5.5 8l4 4"/></svg>',
    next: '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"><path d="M6.5 4l4 4-4 4"/></svg>',
  };
  function build() {
    if (F.built) return true;
    const host = document.getElementById("view-fretboard");
    if (!host) return false;
    F.built = true;
    host.innerHTML = `<div class="fbv">
      <header class="fbv-head">
        <div class="fbv-key">
          <button class="fbv-kname" data-act="key" title="Pick the key, or follow what you play"><span data-k="kname">–</span>${IC.chev}</button>
          <div class="fbv-ksub"><button class="fbv-chip" data-act="lock" data-k="kchip" title="Lock / follow the key"><i></i>listening</button><span class="fbv-kalt" data-k="kalt"></span></div>
        </div>
        <button class="fbv-scale" data-act="scale" title="Scale  ( [ and ] step through them )"><span class="fbv-cap">Scale</span><b data-k="sname">–</b>${IC.chev}</button>
        <div class="fbv-tones" data-k="tones" title="Click a note to find it on the neck"></div>
        <div class="seg fbv-labels" data-k="labels" title="What the dots say"><button data-lab="note">Notes</button><button data-lab="deg">Degrees</button><button data-lab="iv">Intervals</button><button data-lab="none">Off</button></div>
      </header>
      <div class="fbv-chords">
        <span class="fbv-cap fbv-cap-c">Chords</span>
        <button class="fbv-live" data-act="livechord" data-k="live" title="Overlay the chord you're playing"></button>
        <div class="fbv-dia" data-k="dia"></div>
        <button class="fbv-tg" data-act="sev" data-k="sev" title="Triads or 7th chords">7ths</button>
        <div class="seg fbv-omode" data-k="omode" title="Chord overlay: highlight the chord tones in the scale, or show the arpeggio alone"><button data-om="tones"><span class="lw">Chord </span>tones</button><button data-om="arp">Arp<span class="lw">eggio</span></button></div>
        <span class="fbv-sp"></span>
        <div class="fbv-prac">
          <button class="fbv-tg" data-act="targets" data-k="targets" title="Target practice: find the degree it asks for">${IC.target}<span>Targets</span></button>
          <button class="fbv-tg" data-act="unplayed" data-k="unplayed" title="Show only the notes you haven't played yet">${IC.unplayed}<span>Unplayed</span></button>
          <button class="fbv-ib" data-act="sound" data-k="sound" title="Click a note to hear it (a soft tone in this window, not through the rig)"></button>
        </div>
      </div>
      <div class="fbv-neck" data-k="neck">
        <img class="fbv-wood" data-k="wood" alt="" aria-hidden="true" draggable="false">
        <div class="fbv-fx fbv-under"><i class="fbv-hand" data-k="hand"></i></div>
        <svg class="fbv-notes" data-k="notes" xmlns="http://www.w3.org/2000/svg" role="img" aria-label="Fretboard"></svg>
        <div class="fbv-fx" data-k="fx"></div>
        <div class="fbv-status"><span class="fbv-now" data-k="now"></span><span class="fbv-pst" data-k="pst"></span></div>
      </div>
      <footer class="fbv-foot">
        <span class="fbv-cap fbv-cap-p">Positions</span>
        <div class="seg" data-k="psys"><button data-ps="all">All</button><button data-ps="caged" data-k="pcaged">CAGED</button><button data-ps="nps" title="3 notes per string">3 NPS</button></div>
        <div class="fbv-pos" data-k="posbox">
          <button class="fbv-ib sm" data-act="pprev" title="Previous position (←)">${IC.prev}</button>
          <input type="range" class="fbv-slider" data-k="pslider" min="1" max="5" step="1" value="1" aria-label="Position">
          <button class="fbv-ib sm" data-act="pnext" title="Next position (→)">${IC.next}</button>
          <span class="fbv-plabel" data-k="plabel"></span>
          <button class="fbv-ib" data-act="phide" data-k="phide"></button>
          <button class="fbv-ib" data-act="phand" data-k="phand" title="Follow my hand: pick the position you're playing in">${IC.hand}</button>
        </div>
        <span class="fbv-sp"></span>
        <button class="fbv-pill" data-act="tuning" data-k="tuning" title="Instrument and tuning (shared with the tuner)"></button>
        <button class="fbv-pill" data-act="capo" data-k="capo" title="Capo"></button>
        <div class="seg fbv-zoom" data-k="zoom" title="Fit frets"><button data-z="12">12</button><button data-z="15">15</button><button data-z="all" data-k="zall">24</button></div>
        <button class="fbv-ib" data-act="neck" title="Neck: wood, inlays, frets, spacing, left-handed…">${IC.gear}</button>
      </footer>
      <div class="fbv-pop" data-k="pop" hidden></div>
    </div>`;
    F.root = host.firstElementChild;
    F.root.querySelectorAll("[data-k]").forEach((el) => { F.els[el.dataset.k] = el; });
    wire();
    new ResizeObserver(() => { if (F.visible) schedule(true); }).observe(F.els.neck);
    return true;
  }

  /* =====================================================================
     GEOMETRY + STATIC NECK
     ===================================================================== */
  function gauge(midi, bass) {
    const g = 0.010 * Math.pow(2, ((64 - midi) / 12) * 1.1);
    return { g, wound: bass || g > 0.0195 };
  }
  function geometry(W, H) {
    const T = F.tun, n = T.notes.length, maxF = T.maxF;
    const zoomN = prefs.zoom === "all" ? maxF : Math.min(+prefs.zoom, maxF);
    const ws = clamp(F.ws, 0, Math.max(0, maxF - zoomN)), we = Math.min(maxF, ws + zoomN);
    const narrow = W < 760;
    const labW = narrow ? 20 : 26, openW = ws === 0 ? clamp(W * 0.045, 28, 46) : 8, padT = 24, fnH = 20;
    const x0 = labW + openW, extW = we === maxF ? clamp(W * 0.02, 12, 22) : 0, x1 = W - 10 - extW;
    const L = (f) => (prefs.spacing === "real" ? 1 - Math.pow(2, -f / 12) : f);
    const La = L(ws), Lb = L(we);
    const fx = (f) => x0 + ((L(f) - La) / (Lb - La)) * (x1 - x0);
    const X = (x) => (prefs.lefty ? W - x : x);
    const taper = (f) => 1 + 0.12 * ((1 - Math.pow(2, -f / 12)) / (1 - Math.pow(2, -24 / 12)));
    const avail = H - padT - fnH - 8;
    const gapMax = n <= 4 ? 78 : n === 5 ? 70 : n === 6 ? 64 : n === 7 ? 56 : 50;
    const gTop = Math.min(gapMax, avail / (n - 1 + 0.96), (W / 2.85) / (n - 1 + 0.96));
    const gap = (f) => gTop * taper(f) / taper(we);
    const hb = (f) => gap(f) * ((n - 1) / 2 + 0.48);
    const cy = padT + (avail + 8) / 2;
    const row = (s) => (prefs.highTop ? n - 1 - s : s);
    const y = (s, f) => cy + (row(s) - (n - 1) / 2) * gap(f);
    const cellF = (f) => (f === 0 ? 0 : f - (prefs.spacing === "real" ? 0.44 : 0.5));   // fractional fret of a dot
    const cx = (f) => {
      if (f === 0) return ws === 0 ? labW + openW * 0.5 : null;
      if (f <= ws || f > we) return null;
      if (prefs.capo && f === prefs.capo) return fx(f) - Math.min(fx(f) - fx(f - 1), 40) * 0.36;
      return fx(f - 1) + (fx(f) - fx(f - 1)) * (prefs.spacing === "real" ? 0.56 : 0.5);
    };
    const fwAt = (f) => (f <= 0 ? openW : fx(f) - fx(f - 1));
    const rBase = Math.min(gTop * 0.43 * taper(ws) / taper(we), 19);
    return { W, H, n, maxF, ws, we, zoomN, labW, openW, padT, fnH, x0, x1, extW, fx, X, gap, hb, cy, y, cx, cellF, fwAt, rBase, row, narrow, boardEnd: we === maxF ? x1 + extW * 0.75 : W };
  }
  function boardPoly(G) {
    const top = [], bot = [];
    const xs = [];
    for (let f = G.ws; f <= G.we; f++) xs.push([G.fx(f), f]);
    xs.push([G.boardEnd, G.we]);
    if (G.ws > 0) xs.unshift([G.labW - 6, G.ws]);
    xs.forEach(([x, f]) => { top.push([G.X(x), G.cy - G.hb(f)]); bot.push([G.X(x), G.cy + G.hb(f)]); });
    return { top, bot, d: "M" + top.concat(bot.reverse()).map((p) => `${f1(p[0])},${f1(p[1])}`).join("L") + "Z" };
  }
  function stringPts(G, s, fromX, toX) {
    // the string is straight from nut to bridge; sample its y at the window's frets
    const pts = [];
    const fAt = (x) => { // inverse of fx, approx by search over frets
      for (let f = G.ws; f < G.we; f++) if (x <= G.fx(f + 1)) return f + (x - G.fx(f)) / Math.max(1e-6, G.fx(f + 1) - G.fx(f));
      return G.we;
    };
    const xs = [fromX];
    for (let f = G.ws; f <= G.we; f++) { const x = G.fx(f); if (x > fromX && x < toX) xs.push(x); }
    xs.push(toX);
    xs.forEach((x) => pts.push([G.X(x), G.y(s, clamp(x < G.x0 ? G.ws : fAt(x), G.ws, G.we))]));
    return pts;
  }
  function renderStatic(G) {
    const T = F.tun, W = WOODS[prefs.wood] || WOODS.rosewood, bp = boardPoly(G);
    const top = G.cy - G.hb(G.we) - 2, bh = G.hb(G.we) * 2 + 4;
    const fadeA = G.X(G.labW + 2), fadeB = G.X(G.labW + G.openW * 0.75);
    let s = `<defs>
      <linearGradient id="fbv-wb" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="${W.base[0]}"/><stop offset=".5" stop-color="${W.base[1]}"/><stop offset="1" stop-color="${W.base[2]}"/></linearGradient>
      <linearGradient id="fbv-light" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#fff" stop-opacity=".16"/><stop offset=".06" stop-color="#fff" stop-opacity=".05"/><stop offset=".45" stop-color="#fff" stop-opacity="0"/><stop offset=".9" stop-color="#000" stop-opacity=".16"/><stop offset="1" stop-color="#000" stop-opacity=".42"/></linearGradient>
      <filter id="fbv-grain" filterUnits="userSpaceOnUse" x="0" y="${f1(top)}" width="${G.W}" height="${f1(bh)}" color-interpolation-filters="sRGB">
        <feTurbulence type="fractalNoise" baseFrequency="0.0032 0.14" numOctaves="4" seed="11"/>
        <feColorMatrix type="matrix" values="0 0 0 0 ${hexF(W.grain, 0)} 0 0 0 0 ${hexF(W.grain, 1)} 0 0 0 0 ${hexF(W.grain, 2)} 0 0 3.4 0 -1.45"/>
      </filter>
      <filter id="fbv-fig" filterUnits="userSpaceOnUse" x="0" y="${f1(top)}" width="${G.W}" height="${f1(bh)}" color-interpolation-filters="sRGB">
        <feTurbulence type="fractalNoise" baseFrequency="0.0011 0.035" numOctaves="3" seed="4"/>
        <feColorMatrix type="matrix" values="0 0 0 0 ${hexF(W.fig, 0)} 0 0 0 0 ${hexF(W.fig, 1)} 0 0 0 0 ${hexF(W.fig, 2)} 2.6 0 0 0 -1.05"/>
      </filter>
      <filter id="fbv-pore" filterUnits="userSpaceOnUse" x="0" y="${f1(top)}" width="${G.W}" height="${f1(bh)}" color-interpolation-filters="sRGB">
        <feTurbulence type="fractalNoise" baseFrequency="0.09 0.9" numOctaves="1" seed="2"/>
        <feColorMatrix type="matrix" values="0 0 0 0 ${hexF(W.pore, 0)} 0 0 0 0 ${hexF(W.pore, 1)} 0 0 0 0 ${hexF(W.pore, 2)} 0 6 0 0 -3.9"/>
      </filter>
      <filter id="fbv-blur" x="-5%" y="-30%" width="110%" height="160%"><feGaussianBlur stdDeviation="1.3"/></filter>
      <filter id="fbv-shadow" x="-5%" y="-30%" width="110%" height="170%"><feGaussianBlur stdDeviation="9"/></filter>
      <clipPath id="fbv-clip"><path d="${bp.d}"/></clipPath>
      <linearGradient id="fbv-fret" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#4c4e52"/><stop offset=".3" stop-color="#d9dce0"/><stop offset=".48" stop-color="#ffffff"/><stop offset=".72" stop-color="#9da0a6"/><stop offset="1" stop-color="#3e4044"/></linearGradient>
      <linearGradient id="fbv-fsh" x1="${prefs.lefty ? 1 : 0}" y1="0" x2="${prefs.lefty ? 0 : 1}" y2="0"><stop offset="0" stop-color="#000" stop-opacity=".42"/><stop offset="1" stop-color="#000" stop-opacity="0"/></linearGradient>
      <linearGradient id="fbv-nut" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#cdbf9f"/><stop offset=".35" stop-color="#f7f1e2"/><stop offset=".7" stop-color="#e6dbc1"/><stop offset="1" stop-color="#b9a983"/></linearGradient>
      <radialGradient id="fbv-pearl" cx=".38" cy=".34" r=".75"><stop offset="0" stop-color="#ffffff"/><stop offset=".45" stop-color="#f0ebe1"/><stop offset=".8" stop-color="#d4cbbb"/><stop offset="1" stop-color="#b5ab9a"/></radialGradient>
      <linearGradient id="fbv-iri" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#ffb7d6" stop-opacity=".35"/><stop offset=".5" stop-color="#b9ffe9" stop-opacity=".18"/><stop offset="1" stop-color="#a9c4ff" stop-opacity=".35"/></linearGradient>
      <radialGradient id="fbv-black" cx=".38" cy=".34" r=".75"><stop offset="0" stop-color="#4a423c"/><stop offset=".6" stop-color="#1b1714"/><stop offset="1" stop-color="#0a0807"/></radialGradient>
      <linearGradient id="fbv-plain" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#5e6369"/><stop offset=".32" stop-color="#f7f9fb"/><stop offset=".55" stop-color="#c3c8ce"/><stop offset="1" stop-color="#43474c"/></linearGradient>
      <linearGradient id="fbv-wound" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#55504a"/><stop offset=".32" stop-color="#efe8db"/><stop offset=".55" stop-color="#b7ae9d"/><stop offset="1" stop-color="#3a352f"/></linearGradient>
      <pattern id="fbv-wind" width="2.1" height="12" patternUnits="userSpaceOnUse" patternTransform="rotate(-62)"><rect width=".85" height="12" fill="#000" fill-opacity=".38"/></pattern>
      <linearGradient id="fbv-fade" gradientUnits="userSpaceOnUse" x1="${f1(fadeA)}" y1="0" x2="${f1(fadeB)}" y2="0"><stop offset="0" stop-color="#fff" stop-opacity="0"/><stop offset="1" stop-color="#fff" stop-opacity="1"/></linearGradient>
      <mask id="fbv-smask" maskUnits="userSpaceOnUse" x="0" y="0" width="${G.W}" height="${G.H}"><rect width="${G.W}" height="${G.H}" fill="url(#fbv-fade)"/></mask>
    </defs>`;
    // shadow under the neck
    s += `<path d="${bp.d}" transform="translate(0 7)" filter="url(#fbv-shadow)" fill="${document.body.classList.contains("light") ? "rgba(40,30,20,.42)" : "rgba(0,0,0,.75)"}"/>`;
    // the board: base, figure, grain, pores, light
    s += `<g clip-path="url(#fbv-clip)"><path d="${bp.d}" fill="url(#fbv-wb)"/>
      <rect x="0" y="${f1(top)}" width="${G.W}" height="${f1(bh)}" filter="url(#fbv-fig)" opacity="${W.fa}"/>
      <rect x="0" y="${f1(top)}" width="${G.W}" height="${f1(bh)}" filter="url(#fbv-grain)" opacity="${W.ga}"/>
      <rect x="0" y="${f1(top)}" width="${G.W}" height="${f1(bh)}" filter="url(#fbv-pore)" opacity=".5"/>`;
    // inlays
    for (let f = Math.max(1, G.ws + 1); f <= G.we; f++) {
      const dbl = DOUBLE.includes(f);
      if (!dbl && !INLAY_FRETS.includes(f)) continue;
      const xa = G.fx(f - 1), xb = G.fx(f), xm = (xa + xb) / 2, fw = xb - xa, gp = G.gap(f - 0.5), cyy = G.cy + (G.n % 2 && !dbl && prefs.inlay !== "blocks" ? gp / 2 : 0);
      const fill = W.dots === "black" ? "url(#fbv-black)" : "url(#fbv-pearl)";
      if (prefs.inlay === "blocks") {
        const bw = Math.max(6, fw * 0.58), bhh = G.hb(f - 0.5) * 1.5;
        s += `<rect x="${f1(G.X(xm) - bw / 2)}" y="${f1(cyy - bhh / 2)}" width="${f1(bw)}" height="${f1(bhh)}" rx="1.6" fill="${fill}"/>`;
        if (W.dots !== "black") s += `<rect x="${f1(G.X(xm) - bw / 2)}" y="${f1(cyy - bhh / 2)}" width="${f1(bw)}" height="${f1(bhh)}" rx="1.6" fill="url(#fbv-iri)"/>`;
        if (dbl) s += `<rect x="${f1(G.X(xm) - bw / 2)}" y="${f1(cyy - 1)}" width="${f1(bw)}" height="2" fill="${W.base[1]}" opacity=".85"/>`;
      } else {
        const r = Math.min(gp * 0.24, fw * 0.24, 9);
        const dd = G.n >= 6 ? (G.n % 2 ? 1.5 : G.n === 6 ? 1 : 2) : 0.5 * (G.n - 2);
        const ys = dbl ? [cyy - gp * dd, cyy + gp * dd] : [cyy];
        ys.forEach((yy) => {
          s += `<circle cx="${f1(G.X(xm))}" cy="${f1(yy)}" r="${f1(r)}" fill="${fill}"/>`;
          if (W.dots !== "black") s += `<circle cx="${f1(G.X(xm))}" cy="${f1(yy)}" r="${f1(r)}" fill="url(#fbv-iri)"/>`;
        });
      }
    }
    // fret wire + its shadow
    const fwire = clamp(G.rBase * 0.17, 2, 3.6);
    for (let f = G.ws + 1; f <= G.we; f++) {
      const x = G.fx(f), h = G.hb(f) + 1;
      const sx = prefs.lefty ? G.X(x) - fwire / 2 - fwire * 2.2 : G.X(x) + fwire / 2;
      s += `<rect x="${f1(sx)}" y="${f1(G.cy - h)}" width="${f1(fwire * 2.2)}" height="${f1(h * 2)}" fill="url(#fbv-fsh)"/>`;
      s += `<rect x="${f1(G.X(x) - fwire / 2)}" y="${f1(G.cy - h)}" width="${f1(fwire)}" height="${f1(h * 2)}" fill="url(#fbv-fret)"/>`;
    }
    s += `<path d="${bp.d}" fill="url(#fbv-light)"/></g>`;
    // binding / edges
    const edge = (pts) => "M" + pts.map((p) => `${f1(p[0])},${f1(p[1])}`).join("L");
    if (W.bind) s += `<path d="${edge(bp.top)}" fill="none" stroke-width="1.6" stroke-opacity=".85" stroke="${W.bind}"/><path d="${edge(bp.bot.slice().reverse())}" fill="none" stroke-width="1.6" stroke-opacity=".85" stroke="${W.bind}" opacity=".7"/>`;
    else s += `<path d="${edge(bp.top)}" fill="none" stroke="#fff" stroke-opacity=".35" stroke-width="1"/><path d="${edge(bp.bot.slice().reverse())}" fill="none" stroke="#000" stroke-opacity=".3" stroke-width="1.2"/>`;
    // nut
    if (G.ws === 0) {
      const nw = clamp(G.rBase * 0.42, 5, 8), h = G.hb(0) + 1.5, xn = prefs.lefty ? G.X(G.x0) : G.X(G.x0) - nw;
      s += `<rect x="${f1(xn)}" y="${f1(G.cy - h)}" width="${f1(nw)}" height="${f1(h * 2)}" rx="1.2" fill="url(#fbv-nut)"/>`;
      s += `<rect x="${f1(prefs.lefty ? xn - 4 : xn + nw)}" y="${f1(G.cy - h)}" width="4" height="${f1(h * 2)}" fill="url(#fbv-fsh)"/>`;
    }
    // capo shade (the strings behind it don't ring)
    if (prefs.capo && prefs.capo > G.ws && prefs.capo <= G.we) {
      const xc = G.cx(prefs.capo), xa = G.fx(G.ws);
      const l = Math.min(G.X(xa), G.X(xc)), r = Math.max(G.X(xa), G.X(xc));
      s += `<rect x="${f1(l)}" y="${f1(G.cy - G.hb(prefs.capo) - 2)}" width="${f1(r - l)}" height="${f1(G.hb(prefs.capo) * 2 + 4)}" fill="#000" opacity=".38"/>`;
    }
    // strings: shadow, body, winding
    const sStart = G.labW + 2, sEnd = G.boardEnd;
    let sh = "", body = "";
    T.notes.forEach((m, si) => {
      const gg = gauge(m, F.tun.bass);
      const w = clamp(0.9 + gg.g * 58, 1.1, G.gap(G.ws) * 0.15);
      const pts = stringPts(G, si, sStart, sEnd);
      const line = pts.map((p) => `${f1(p[0])},${f1(p[1])}`).join(" ");
      sh += `<polyline points="${pts.map((p) => `${f1(p[0] + 1.2)},${f1(p[1] + w * 0.6 + 1.6)}`).join(" ")}" stroke-width="${f1(w + 0.6)}"/>`;
      const poly = pts.map((p) => `${f1(p[0])},${f1(p[1] - w / 2)}`).concat(pts.slice().reverse().map((p) => `${f1(p[0])},${f1(p[1] + w / 2)}`)).join(" ");
      body += `<polygon points="${poly}" fill="url(#fbv-${gg.wound ? "wound" : "plain"})"/>`;
      if (gg.wound && w > 1.8) body += `<polygon points="${poly}" fill="url(#fbv-wind)"/>`;
      void line;
    });
    s += `<g mask="url(#fbv-smask)"><g clip-path="url(#fbv-clip)"><g fill="none" stroke="#000" stroke-opacity=".55" stroke-linecap="round" filter="url(#fbv-blur)">${sh}</g></g>${body}</g>`;
    // capo bar on top of the strings
    if (prefs.capo && prefs.capo > G.ws && prefs.capo <= G.we) {
      const xc = G.X(G.cx(prefs.capo)), h = G.hb(prefs.capo) + 7, cw = clamp(G.rBase * 0.75, 8, 13);
      s += `<rect x="${f1(xc - cw / 2 + 2)}" y="${f1(G.cy - h + 3)}" width="${f1(cw)}" height="${f1(h * 2)}" rx="${f1(cw / 2)}" fill="#000" opacity=".35" filter="url(#fbv-blur)"/>`;
      s += `<rect fill="#26272b" stroke="#8b8e94" stroke-width="1" x="${f1(xc - cw / 2)}" y="${f1(G.cy - h)}" width="${f1(cw)}" height="${f1(h * 2)}" rx="${f1(cw / 2)}"/>`;
      s += `<rect x="${f1(xc - cw / 2 + 2)}" y="${f1(G.cy - h + 3)}" width="${f1(cw * 0.28)}" height="${f1(h * 2 - 6)}" rx="1.5" fill="#fff" opacity=".14"/>`;
    }
    return s;
  }
  // string names (tuning) and fret numbers: live in the notes layer (theme colours, and the wood stays an image)
  function renderLabels(G) {
    let s = "";
    F.tun.notes.forEach((m, si) => {
      s += `<text class="fbv-sl" data-sl="${si}" x="${f1(G.X(G.labW * 0.48))}" y="${f1(G.y(si, G.ws) + 0.5)}">${escH(strName(m + (prefs.capo || 0)))}</text>`;   // with a capo: what the open string sounds
    });
    const yNum = G.cy + G.hb(G.we) + 15;
    for (let f = G.ws + 1; f <= G.we; f++) {
      const x = G.cx(f) ?? (G.fx(f - 1) + G.fx(f)) / 2, mk = INLAY_FRETS.includes(f) || DOUBLE.includes(f);
      if (G.fwAt(f) < 14 && !mk) continue;
      s += `<text class="fbv-fn${mk ? " mk" : ""}" x="${f1(G.X(x))}" y="${f1(yNum)}">${f}</text>`;
    }
    if (G.ws > 0) s += `<text class="fbv-fn mk" x="${f1(G.X(G.labW + 2))}" y="${f1(yNum)}" text-anchor="${prefs.lefty ? "end" : "start"}">${G.ws}</text>`;
    return s;
  }
  function strName(m) {
    const M = F.M, pc = m % 12;
    if (!M) return MIX[pc];
    if (M.info[pc].in) return M.info[pc].name;
    const ks = keySpelling(M.key.root, M.key.minor).sp.map(nm);
    return ks.some((n) => n.includes("♭")) ? FL[pc] : ks.some((n) => n.includes("♯")) ? SH[pc] : MIX[pc];
  }
  function hexF(hex, i) { return (parseInt(hex.slice(1 + i * 2, 3 + i * 2), 16) / 255).toFixed(3); }

  /* ---------- the notes layer: one persistent element per string × fret ---------- */
  function renderCells(G) {
    const T = F.tun, cells = [];
    let s = `<defs>
      <radialGradient id="fbv-glow"><stop offset="0" stop-color="var(--fbv-hot)" stop-opacity=".95"/><stop offset=".35" stop-color="var(--fbv-hot)" stop-opacity=".45"/><stop offset="1" stop-color="var(--fbv-hot)" stop-opacity="0"/></radialGradient>
      <linearGradient id="fbv-handg" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="var(--fbv-hot)" stop-opacity="0"/><stop offset=".25" stop-color="var(--fbv-hot)" stop-opacity=".16"/><stop offset=".75" stop-color="var(--fbv-hot)" stop-opacity=".16"/><stop offset="1" stop-color="var(--fbv-hot)" stop-opacity="0"/></linearGradient>
    </defs>`;
    const top = G.cy - G.hb(G.we) - 4, bh = G.hb(G.we) * 2 + 8;
    s += renderLabels(G) + `<g class="fbv-shape" data-k2="shape"></g>`;
    s += `<g class="fbv-cells">`;
    T.notes.forEach((open, si) => {
      for (let f = 0; f <= G.maxF; f++) {
        const x = G.cx(f);
        if (x == null) continue;
        if (prefs.capo && f > 0 && f < prefs.capo) continue;
        if (prefs.capo && f === 0) continue;
        const yy = G.y(si, G.cellF(f));
        const fw = f === 0 ? G.openW : G.fwAt(f);
        const r = Math.max(5, Math.min(G.rBase, fw * 0.44));
        const fs = clamp(r * 0.86, 7.5, 15);
        const key = si + ":" + f;
        cells.push({ key, s: si, f, m: open + f, x: G.X(x), y: yy, r, cls: "", txt: null, el: null });
        s += `<g class="fbc" data-c="${key}" transform="translate(${f1(G.X(x))} ${f1(yy)})"><g class="fbc-i"><circle class="h" r="${f1(r * 2.3)}"/><circle class="o" r="${f1(r + 3.2)}"/><circle class="sd" cy="1.6" r="${f1(r + 0.4)}"/><circle class="b" r="${f1(r)}"/><text class="t" y="${f1(fs * 0.36)}" style="font-size:${f1(fs)}px"></text></g></g>`;
      }
    });
    s += `</g>`;
    F.handBox = { top, bh };
    return { svg: s, cells };
  }

  function layout(force) {
    if (!F.built || !F.visible) return;
    const neck = F.els.neck, W = Math.round(neck.clientWidth), H = Math.round(neck.clientHeight);
    if (W < 50 || H < 50) return;
    F.tun = tuningInfo();
    F.M = computeModel();
    computePositions();
    autoWindow();
    const sig = [W, H, F.ws, prefs.zoom, prefs.frets, prefs.spacing, prefs.lefty, prefs.highTop, prefs.wood, prefs.inlay, prefs.capo, F.tun.notes.join(), F.tun.maxF, F.tun.notes.map(strName).join(), document.body.classList.contains("light")].join("|");
    if (sig !== F.staticSig || force === "hard") {
      F.staticSig = sig;
      F.stats.rebuilds++;
      const G = (F.G = geometry(W, H));
      const wood = F.els.wood, notes = F.els.notes;
      notes.setAttribute("viewBox", `0 0 ${W} ${H}`);
      notes.setAttribute("width", W); notes.setAttribute("height", H);
      // explicit pixel sizes: a percentage-sized SVG root is re-laid out on every document layout (the Listen pane
      // updates its DOM every frame while it polls, which made each of those cost ~1 ms here)
      notes.style.width = wood.style.width = W + "px"; notes.style.height = wood.style.height = H + "px";
      // the wood is a static picture: an <img> is rasterised once and never re-laid out or repainted with the notes
      wood.width = W; wood.height = H;
      wood.src = "data:image/svg+xml;charset=utf-8," + encodeURIComponent(`<svg xmlns="http://www.w3.org/2000/svg" width="${W}" height="${H}" viewBox="0 0 ${W} ${H}">${renderStatic(G)}</svg>`);
      const rc = renderCells(G);
      F.root.classList.add("fbv-instant");
      notes.innerHTML = rc.svg;
      F.cells = rc.cells;
      F.cellMap = new Map();
      const els = notes.querySelectorAll(".fbc");
      F.cells.forEach((c, i) => { c.el = els[i]; c.txtEl = els[i].querySelector("text"); F.cellMap.set(c.key, c); });
      F.layers = { shape: notes.querySelector('[data-k2="shape"]'), hand: F.els.hand };
      F.els.fx.querySelectorAll(".fbv-tr, .fbv-tl, .fbv-rg").forEach((e) => e.remove());
      F.mkSig = "";
      F.els.hand.style.top = f1(F.handBox.top) + "px"; F.els.hand.style.height = f1(F.handBox.bh) + "px";
      F.shapeSig = ""; F.handSig = ""; F.slHot = null;
      F.root.classList.toggle("fbv-narrow", W < 940);
      F.root.classList.toggle("fbv-tight", W < 760);
      paint(true);
      requestAnimationFrame(() => requestAnimationFrame(() => F.root && F.root.classList.remove("fbv-instant")));
      return;
    }
    paint();
  }
  let schedT = 0, schedForce = false;
  function schedule(force) {
    if (force) schedForce = true;
    if (schedT) return;
    schedT = requestAnimationFrame(() => { schedT = 0; const fz = schedForce; schedForce = false; if (fz) layout(); else { F.M = computeModel(); computePositions(); if (autoWindow()) layout(); else paint(); } });
  }
  // keep the isolated position / your hand inside a zoomed window
  function autoWindow() {
    const T = F.tun, maxF = T.maxF, zoomN = prefs.zoom === "all" ? maxF : Math.min(+prefs.zoom, maxF);
    const old = F.ws;
    if (zoomN >= maxF) F.ws = 0;
    else {
      F.ws = clamp(F.ws, 0, maxF - zoomN);
      const p = curPos();
      const inWin = (lo, hi) => lo >= (F.ws === 0 ? 0 : F.ws + 1) && hi <= F.ws + zoomN;
      if (p && p.inst.length && !p.inst.some((i) => inWin(i.lo, i.hi))) {
        const near = p.inst.slice().sort((a, b) => Math.abs(a.lo - F.ws) - Math.abs(b.lo - F.ws))[0];
        F.ws = clamp(near.lo <= 1 ? 0 : near.lo - 1, 0, maxF - zoomN);
      } else if (!p && F.hand != null && now() - F.lastOnsetT < 1500 && (F.hand < F.ws + 1 || F.hand > F.ws + zoomN - 2)) {
        F.ws = clamp(Math.round(F.hand - zoomN / 2), 0, maxF - zoomN);
      }
    }
    return F.ws !== old;
  }

  /* =====================================================================
     PAINT
     ===================================================================== */
  function paint(full) {
    if (!F.G || !F.M) return;
    F.stats.paints++;
    paintHead();
    paintChords();
    paintFoot();
    paintCells(full);
    paintShape();
    paintStatus();
  }
  const setH = (el, v) => { if (el && el._h !== v) { el._h = v; el.innerHTML = v; } };
  const setT = (el, v) => { if (el && el._t !== v) { el._t = v; el.textContent = v; } };
  const tog = (el, c, on) => { if (el && el.classList.contains(c) !== !!on) el.classList.toggle(c, !!on); };
  const setAttr = (el, a, v) => { if (el && el.getAttribute(a) !== v) el.setAttribute(a, v); };

  function paintHead() {
    const M = F.M, E = F.els, k = M.key, det = F.det;
    setH(E.kname, `${escH(M.kName)} <em>${k.minor ? "minor" : "major"}</em>`);
    tog(E.kname, "wait", k.src === "last");
    let chip, cls = "";
    if (k.src === "lock") { chip = `${IC.lock}locked`; cls = "lock"; }
    else if (!det) chip = "<i></i>listening…";
    else if (det.state === "listening") { chip = `<i></i>settling · ${Math.round((det.conf || 0) * 100)}%`; cls = "live"; }
    else if (det.state === "held") { chip = `<i></i>held · ${Math.round((det.conf || 0) * 100)}%`; cls = "held"; }
    else { chip = `<i></i>following · ${Math.round((det.conf || 0) * 100)}%`; cls = "live"; }
    setH(E.kchip, chip);
    const kc = "fbv-chip " + cls;
    if (E.kchip.className !== kc) E.kchip.className = kc;
    setAttr(E.kchip, "title", k.src === "lock" ? "Locked: click to follow the key you play" : "Following the detected key: click to lock it");
    let alt = "";
    if (k.src === "lock" && det && (det.root !== k.root || det.minor !== k.minor)) alt = `hearing ${keyName(det)} ${det.minor ? "minor" : "major"}`;
    else if (k.src === "live" && det && det.alt_root != null && det.alt_root >= 0) alt = `or ${keyName({ root: det.alt_root, minor: !!det.alt_minor })} ${det.alt_minor ? "minor" : "major"}`;
    else if (k.src === "last") alt = "play a few seconds to find the key";
    setT(E.kalt, alt);
    // scale
    setH(E.sname, escH(M.rs.auto ? `${AUTO[M.rs.auto].n}${M.rs.auto !== "key" ? "" : ""}` : M.sc.n) + (M.rs.auto ? `<small>${escH(M.title)}</small>` : `<small>${escH(M.title)}</small>`));
    // tones legend
    const ov = overlayChord();
    tog(E.tones, "many", M.sc.degs.length > 7);
    setH(E.tones, M.sc.degs.length > 8 ? `<span class="fbv-tones-all">all 12 notes · root ${escH(M.rootName)}</span>` : M.pcs.map((pc) => {
      const I = M.info[pc], ct = ov && ov.pcs.has(pc);
      const tgt = prefs.targets && F.target && F.target.pc === pc && !prefs.tHide;
      return `<button class="fbv-tone c${prefs.color === "degree" || pc === M.key.root ? I.num : "n"}${pc === M.key.root ? " root" : ""}${ct ? " ct" : ""}${tgt ? " tgt" : ""}" data-tone="${pc}"><b>${escH(I.name)}</b><i>${escH(I.lbl)}</i></button>`;
    }).join(""));
    E.labels.querySelectorAll("[data-lab]").forEach((b) => tog(b, "on", b.dataset.lab === prefs.labels));
  }

  function paintChords() {
    const M = F.M, E = F.els, ov = overlayChord(), live = F.chord;
    const liveOn = !prefs.chordSel && prefs.chordLive;
    if (live) setH(E.live, `<span class="dot"></span><b>${escH(chordName(live))}</b><i>${escH(romanOf(live))}</i>`);
    else setH(E.live, `<span class="dot"></span><b>Live</b><i>${prefs.chordLive ? "–" : "off"}</i>`);
    tog(E.live, "on", liveOn && !!live);
    tog(E.live, "armed", liveOn && !live);
    tog(E.live, "sounding", !!(live && F.d && F.d.chordal));
    setH(E.dia, M.dia.map((c, i) => {
      const pinned = prefs.chordSel && prefs.chordSel.root === c.root && prefs.chordSel.quality === c.quality;
      const now_ = live && live.root === c.root && MINORQ.has(live.quality) === MINORQ.has(c.quality);
      return `<button class="fbv-ch${pinned ? " on" : ""}${now_ ? " now" : ""}" data-ci="${i}" title="Overlay ${escH(c.name)} (click again to clear)"><i>${escH(c.roman)}</i><b>${escH(c.name)}</b></button>`;
    }).join(""));
    tog(E.sev, "on", prefs.sev);
    E.omode.querySelectorAll("[data-om]").forEach((b) => tog(b, "on", b.dataset.om === prefs.omode));
    tog(E.omode, "off", !ov);
    tog(E.targets, "on", prefs.targets);
    tog(E.unplayed, "on", prefs.unplayed);
    setH(E.sound, prefs.sound ? IC.sound : IC.mute);
    tog(E.sound, "on", prefs.sound);
  }

  function paintFoot() {
    const M = F.M, E = F.els, P = F.pos, sys = posSystems(M), T = F.tun;
    const kind = P ? P.kind : "all";
    E.psys.querySelectorAll("[data-ps]").forEach((b) => {
      const ps = b.dataset.ps;
      tog(b, "on", ps === kind);
      const dis = (ps === "caged" && !sys.caged) || (ps === "nps" && !sys.nps);
      if (b.disabled !== dis) b.disabled = dis;
    });
    setT(E.pcaged, sys.boxes ? "Boxes" : "CAGED");
    setAttr(E.pcaged, "title", sys.boxes ? "Pentatonic boxes 1–5" : "CAGED shapes");
    const p = curPos();
    if (E.posbox.hidden !== !p) E.posbox.hidden = !p;
    if (p) {
      if (E.pslider.max !== String(P.list.length)) E.pslider.max = String(P.list.length);
      if (+E.pslider.value !== p.idx) E.pslider.value = String(p.idx);
      const ins = p.inst.map((i) => `${i.lo}–${i.hi}`).join(", ");
      setH(E.plabel, `<b>${escH(p.name)}</b><span>${escH(p.sub)} · fr ${escH(ins)}</span>`);
      setH(E.phide, prefs.phide ? IC.hide : IC.dim);
      setAttr(E.phide, "title", prefs.phide ? "Other notes hidden: click to dim them instead" : "Other notes dimmed: click to hide them");
      tog(E.phand, "on", prefs.phand);
    }
    setT(E.tuning, T.short + (prefs.capo ? "" : "") + " ▾");
    setT(E.capo, prefs.capo ? `Capo ${prefs.capo} ▾` : "No capo ▾");
    tog(E.capo, "on", !!prefs.capo);
    setT(E.zall, String(T.maxF));
    E.zoom.querySelectorAll("[data-z]").forEach((b) => { tog(b, "on", b.dataset.z === String(prefs.zoom)); const dis = b.dataset.z !== "all" && +b.dataset.z >= T.maxF; if (b.disabled !== dis) b.disabled = dis; });
  }

  function paintCells() {
    const M = F.M, ov = overlayChord(), p = curPos(), root = M.key.root;
    const hotKey = F.hot && F.hot.cell ? F.hot.cell.key : null;
    const alt = new Set(F.hotAlt), grip = new Map((F.grip || []).map((g) => [g.key, g]));
    const tgt = prefs.targets && F.target ? F.target.pc : null;
    const flash = F.flash && now() - F.flash.t < 900 ? F.flash.pc : null;
    for (const c of F.cells) {
      const pc = c.m % 12, I = M.info[pc];
      let show = I.in, dim = false, ct = false, ctx = false;
      if (ov) {
        ct = ov.pcs.has(pc);
        if (prefs.omode === "arp") { show = ct; if (ct && !I.in) ctx = true; }
        else { if (show && !ct) dim = true; if (ct && !I.in) { show = true; ctx = true; } }
      }
      if (p && !p.keys.has(c.key)) { if (prefs.phide) show = false; else dim = true; }
      if (prefs.unplayed && F.played.has(c.m)) show = false;
      const hot = c.key === hotKey, gr = grip.get(c.key), isAlt = alt.has(c.key);
      const arp = ov && prefs.omode === "arp" && ct;
      const num = arp ? ARP_NUM[(pc - ov.root + 12) % 12] : I.num, isRoot = arp ? pc === ov.root : pc === root;
      let cls = "fbc";
      if (show || hot || gr) {
        cls += " on";
        cls += prefs.color === "degree" || isRoot ? ` c${I.in || ctx ? num : "x"}` : (I.in || arp ? " cn" : " cx");
        if (!I.in && !ctx) cls += " out";
        if (isRoot) cls += " root";
        if (dim && !hot && !gr) cls += " dim";
        if (ct && !arp) cls += ctx ? " ct ctx" : " ct";
        else if (arp && ctx) cls += " ctx2";
      } else if (isAlt) cls += " ghost";
      if (hot) cls += " hot";
      else if (gr) cls += " grip";
      if (isAlt) cls += " alt";
      if (flash != null && pc === flash) cls += " flash";
      if (c.cls !== cls) { c.cls = cls; c.el.setAttribute("class", cls); }
      let txt = "";
      if (prefs.labels !== "none" || hot) {
        const lab = hot && prefs.labels === "none" ? "note" : prefs.labels;
        if (lab === "note") txt = I.name;
        else if (lab === "deg") txt = ov && prefs.omode === "arp" && ct ? ov.pcs.get(pc) : I.lbl;
        else if (lab === "iv") txt = ov && prefs.omode === "arp" && ct ? ov.pcs.get(pc) : I.iv;
      }
      if (c.txt !== txt) { c.txt = txt; c.txtEl.textContent = txt; }
    }
    paintMarkers();
    // tuning labels light with the string you're on
    const hs = F.hot && F.hot.cell ? F.hot.cell.s : -1;
    if (F.slHot !== hs) { F.slHot = hs; F.els.notes.querySelectorAll(".fbv-sl").forEach((t) => t.classList.toggle("on", +t.dataset.sl === hs)); }
  }

  function paintShape() {
    const p = curPos(), G = F.G, L = F.layers;
    if (!L) return;
    const sig = p ? `${p.idx}|${F.posSig}|${F.staticSig}` : "";
    if (sig === F.shapeSig) return;
    F.shapeSig = sig;
    if (!p) { L.shape.innerHTML = ""; return; }
    let s = "";
    p.inst.forEach((ins) => {
      const rows = new Map();
      ins.notes.forEach((x) => { const c = F.cellMap.get(x.s + ":" + x.f); if (!c) return; const r = G.row(x.s); const o = rows.get(r) || { a: c, b: c }; if (G.X(c.x) < G.X(o.a.x)) o.a = c; if (G.X(c.x) > G.X(o.b.x)) o.b = c; rows.set(r, o); });
      if (!rows.size) return;
      const rs = [...rows.keys()].sort((a, b) => a - b);
      const left = rs.map((r) => rows.get(r).a), right = rs.map((r) => rows.get(r).b).reverse();
      const pts = left.concat(right).map((c) => `${f1(c.x)},${f1(c.y)}`).join(" ");
      s += `<polygon points="${pts}" stroke-width="${f1(G.rBase * 2 + 10)}"/>`;
    });
    L.shape.innerHTML = s;
  }

  function paintStatus() {
    const E = F.els, M = F.M;
    let nowTxt = "";
    if (F.hot && F.hot.cell) {
      const c = F.hot.cell, I = M.info[c.m % 12];
      nowTxt = `<b>${escH(I.name)}<sub>${Math.floor(c.m / 12) - 1}</sub></b> string ${F.tun.notes.length - c.s} · ${c.f === 0 ? "open" : c.f === prefs.capo ? "at the capo" : "fret " + c.f} · <em class="${I.in ? "" : "out"}">${escH(I.in ? I.lbl : "outside the scale")}</em>`;
    } else if (F.grip && F.chord) {
      const fs = F.grip.filter((g) => g.f > prefs.capo).map((g) => g.f);
      nowTxt = `<b>${escH(chordName(F.chord))}</b> likely grip${fs.length ? ` around fret ${Math.min(...fs)}` : " in open position"}`;
    } else if (F.hover) {
      const c = F.hover, I = M.info[c.m % 12];
      nowTxt = `<span class="hv">${escH(I.name)}<sub>${Math.floor(c.m / 12) - 1}</sub> · string ${F.tun.notes.length - c.s}, ${c.f === 0 ? "open" : "fret " + c.f} · ${escH(I.in ? I.lbl + " · " + I.iv : "not in the scale")}</span>`;
    } else if (F.tun.note) nowTxt = `<span class="hv">${escH(F.tun.note)}</span>`;
    setH(E.now, nowTxt);
    let pst = "";
    if (prefs.targets && F.target) {
      const I = M.info[F.target.pc];
      const ok = F.target.hitT && now() - F.target.hitT < 900;
      pst += `<span class="fbv-tgt${ok ? " ok" : ""}">${ok ? "✓ " : ""}Find the <b>${escH(I.lbl)}</b>${prefs.tHide ? "" : ` <span>(${escH(I.name)})</span>`}</span><button data-act="hear" title="Hear the root, then the target">Hear it</button><button data-act="thide" title="${prefs.tHide ? "Show the targets on the neck" : "Ear mode: hide the targets on the neck"}">${prefs.tHide ? "Show" : "By ear"}</button><button data-act="tskip">Skip</button><span class="fbv-score">${F.tScore} found</span>`;
    }
    if (prefs.unplayed) {
      const left = F.cells.filter((c) => c.cls.includes(" on") && !c.cls.includes(" dim") && !c.cls.includes(" out")).length;
      const total = F.cells.filter((c) => { const pc = c.m % 12; return M.info[pc].in; }).length;
      pst += `<span class="fbv-left">${left} left${total ? ` of ${total}` : ""}</span><button data-act="reset" title="Start again">Reset</button>`;
    }
    setH(E.pst, pst);
  }

  /* =====================================================================
     LIVE: data → notes, hand position, trail
     ===================================================================== */
  function candidates(midi) {
    const T = F.tun, capo = prefs.capo, out = [];
    T.notes.forEach((open, s) => {
      const f = midi - open;
      if (f < capo || f > T.maxF) return;
      if (capo && f > 0 && f < capo) return;
      const c = F.cellMap.get(s + ":" + f);
      out.push({ s, f, m: midi, key: s + ":" + f, cell: c || null });
    });
    return out;
  }
  function place(midi, commit) {
    const cands = candidates(midi);
    if (!cands.length) return null;
    const capo = prefs.capo, hand = F.hand, p = curPos(), recent = now() - F.lastOnsetT < 700;
    let best = null;
    for (const c of cands) {
      let cost = 0;
      const open = c.f === capo;
      if (hand == null) cost += open ? 1.6 : Math.abs(c.f - (capo + 5)) * 0.3;
      else if (open) cost += 0.9 + Math.max(0, hand - capo - 4) * 0.3;
      else { const d = c.f - hand; cost += Math.abs(d) * 0.55 + (d < -2 || d > 3 ? 2.2 : 0); }
      if (F.prevS != null && recent) cost += Math.abs(c.s - F.prevS) * 0.22;
      if (p) cost += p.keys.has(c.key) ? -0.6 : 1.4;
      if (!c.cell) cost += 0.5;   // outside the zoomed window
      if (!best || cost < best.cost) best = Object.assign({ cost }, c);
    }
    if (commit) {
      if (best.f !== capo) F.hand = F.hand == null ? best.f : F.hand * 0.6 + best.f * 0.4;
      F.prevS = best.s;
    }
    return { best, alts: cands.filter((c) => c.key !== best.key).map((c) => c.key) };
  }
  function refOff(d) { return 12 * Math.log2(((d && d.a4) || 440) / 440); }

  function onData(d) {
    F.stats.data++;
    F.d = d;
    const k = d.key;
    const prevDet = F.det;
    if (k && k.root != null && k.root >= 0) {
      F.det = { root: k.root, minor: !!k.minor, conf: k.conf, alt_root: k.alt_root, alt_minor: k.alt_minor, state: k.state };
      if (!prefs.lastKey || prefs.lastKey.root !== k.root || prefs.lastKey.minor !== !!k.minor) { prefs.lastKey = { root: k.root, minor: !!k.minor }; save(); }
    } else F.det = null;
    const c = d.chord;
    F.chord = c && c.root != null && c.root >= 0 && (d.chordal || (c.age != null && c.age < 8)) ? { root: c.root, quality: c.quality, bass: c.bass } : null;
    const keyChanged = !prevDet !== !F.det || (prevDet && F.det && (prevDet.root !== F.det.root || prevDet.minor !== F.det.minor));
    if (keyChanged && prefs.follow) { F.M = computeModel(); computePositions(); }
    // single notes
    const off = refOff(d);
    let onsets = [];
    const tr = d.trail && d.trail.midi;
    if (!d.chordal && tr && tr.length && typeof d.t === "number") {
      const step = (d.trail.step_ms || 20) / 1000, n = tr.length;
      if (d.t + 0.5 < F.lastT) F.lastOnsetAbs = -1;   // engine restarted
      F.lastT = d.t;
      let run = null;
      const runs = [];
      for (let i = 0; i < n; i++) {
        const r = tr[i] == null ? null : Math.round(tr[i] - off);
        if (run && run.r === r) run.i1 = i;
        else { if (run) runs.push(run); run = { r, i0: i, i1: i }; }
      }
      if (run) runs.push(run);
      for (const R of runs) {
        if (R.r == null || R.i0 === 0 || R.i1 - R.i0 < 2) continue;
        const tAbs = d.t - (n - 1 - R.i0) * step;
        if (tAbs > F.lastOnsetAbs + step * 0.5) { onsets.push({ midi: R.r, t: tAbs }); F.lastOnsetAbs = tAbs; }
      }
      if (F.lastOnsetAbs < 0) F.lastOnsetAbs = d.t - 0.01;
    } else if (!d.chordal && d.pitch && d.pitch.midi != null && d.pitch.conf > 0.6) {
      const r = Math.round(d.pitch.midi - off);
      if (r !== F.lastMono) onsets.push({ midi: r, t: 0 });
      F.lastMono = r;
    }
    if (!d.pitch) F.lastMono = null;
    if (onsets.length > 6) onsets = onsets.slice(-6);
    onsets.forEach((o) => onset(o.midi));
    // what's sounding now
    const pm = !d.chordal && d.pitch && d.pitch.midi != null && (d.pitch.conf == null || d.pitch.conf > 0.5) ? Math.round(d.pitch.midi - off) : null;
    if (pm != null) {
      const last = F.onsets[F.onsets.length - 1];
      let cell = last && last.midi === pm && last.cell ? last.cell : null;
      let alts = [];
      if (!cell) { const pl = place(pm, false); if (pl && pl.best.cell) { cell = pl.best.cell; alts = pl.alts; } }
      else alts = last.alts || [];
      F.hot = cell ? { midi: pm, cell } : null;
      F.hotAlt = cell ? alts : [];
      F.hotT = now();
    } else if (F.hot && now() - F.hotT > 140) { F.hot = null; F.hotAlt = []; }
    // a chord: the likely grip
    if (d.chordal && F.chord) {
      const gs = `${F.chord.root}|${F.chord.quality}|${F.chord.bass}|${Math.round(F.hand ?? -1)}|${F.staticSig}|${F.posSig}`;
      if (gs !== F.gripSig) { F.gripSig = gs; F.grip = gripFor(F.chord); }
      F.lastOnsetT = now();
    } else if (F.grip) { F.grip = null; F.gripSig = ""; }
    // follow my hand
    if (prefs.phand && onsets.length && F.pos && F.pos.kind !== "all") followHand();
    paintHand();
    if (autoWindow()) layout(); else paint();
  }

  function onset(midi) {
    const pl = place(midi, true);
    const cell = pl && pl.best.cell;
    const o = { midi, cell, alts: pl ? pl.alts : [], t: now() };
    F.onsets.push(o);
    if (F.onsets.length > 16) F.onsets.shift();
    F.lastOnsetT = o.t;
    if (prefs.unplayed) F.played.add(midi);
    if (cell) { addTrail(cell); ringString(cell); }
    if (prefs.targets && F.target && ((midi % 12) + 12) % 12 === F.target.pc && !F.target.hitT) {
      F.target.hitT = now(); F.tScore++;
      F.flash = { pc: F.target.pc, t: now() };
      setTimeout(() => { if (prefs.targets) { nextTarget(); paint(); } }, 900);
    }
  }
  function followHand() {
    const recent = F.onsets.slice(-6).filter((o) => o.cell);
    if (recent.length < 3) return;
    let best = prefs.pidx, bestN = -1;
    F.pos.list.forEach((p, i) => { const n = recent.filter((o) => p.keys.has(o.cell.key)).length; if (n > bestN || (n === bestN && i + 1 === prefs.pidx)) { bestN = n; best = i + 1; } });
    if (best !== prefs.pidx && bestN >= recent.length - 1) { prefs.pidx = best; save(); F.shapeSig = ""; }
  }
  function gripFor(ch) {
    const T = F.tun, capo = prefs.capo, n = T.notes.length;
    const pcs = new Set((Q[ch.quality] || Q.maj).iv.map((s) => (ch.root + s) % 12));
    const bass = ch.bass != null && ch.bass >= 0 ? ch.bass : ch.root;
    const third = (Q[ch.quality] || Q.maj).iv.find((s) => s === 3 || s === 4);
    const want = [ch.root, third != null ? (ch.root + third) % 12 : null].filter((x) => x != null);
    const hand = F.hand != null ? F.hand : capo + 2;
    let best = null;
    for (let w = capo; w <= T.maxF - 3; w++) {
      const opts = (s) => {
        const o = [];
        if (w <= capo + 2 && pcs.has((T.notes[s] + capo) % 12)) o.push(capo);
        for (let f = Math.max(w, capo + 1); f <= w + 3; f++) if (pcs.has((T.notes[s] + f) % 12)) o.push(f);
        return o;
      };
      let s0 = -1, f0 = 0;
      for (let s = 0; s < Math.min(n, 3) && s0 < 0; s++) { const o = opts(s).filter((f) => (T.notes[s] + f) % 12 === bass); if (o.length) { s0 = s; f0 = o[0]; } }
      if (s0 < 0) continue;
      const notes = [{ s: s0, f: f0 }];
      let mutes = 0;
      for (let s = s0 + 1; s < n; s++) {
        const o = opts(s);
        if (!o.length) { mutes++; continue; }
        o.sort((a, b) => (a === capo ? -0.5 : Math.abs(a - (w + 1))) - (b === capo ? -0.5 : Math.abs(b - (w + 1))));
        notes.push({ s, f: o[0] });
      }
      const have = new Set(notes.map((x) => (T.notes[x.s] + x.f) % 12));
      let cost = Math.abs(w - hand) * 0.5 + mutes * 1.3 + s0 * 0.5 + want.filter((p) => !have.has(p)).length * 3;
      const fr = notes.map((x) => x.f).filter((f) => f > capo);
      if (fr.length && Math.max(...fr) - Math.min(...fr) > 3) cost += 2;
      if (!best || cost < best.cost) best = { cost, notes };
    }
    if (!best) return null;
    return best.notes.map((x) => ({ key: x.s + ":" + x.f, s: x.s, f: x.f })).filter((x) => F.cellMap.has(x.key));
  }

  function paintHand() {
    const L = F.layers, G = F.G;
    if (!L || !G) return;
    const on = F.hand != null && now() - F.lastOnsetT < 2500;
    if (on) {
      const a = clamp(F.hand - 1.6, prefs.capo || 0, G.maxF), b = clamp(F.hand + 2.4, 0, G.maxF);
      const xa = G.X(G.fx(clamp(a, G.ws, G.we))), xb = G.X(G.fx(clamp(b, G.ws, G.we)));
      const sig = `${f1(Math.min(xa, xb))}|${f1(Math.abs(xb - xa))}`;
      if (sig !== F.handSig) { F.handSig = sig; L.hand.style.width = f1(Math.max(1, Math.abs(xb - xa))) + "px"; L.hand.style.transform = `translateX(${f1(Math.min(xa, xb))}px)`; }
    }
    tog(L.hand, "on", on);
  }
  /* The live effects are HTML on their own layer and animate only opacity / transform, which the compositor runs
     off the main thread; animating SVG would repaint the whole notes layer on every frame. */
  function fxEl(cls, x, y, w, h, rot) {
    const el = document.createElement("i");
    el.className = cls;
    el.style.width = f1(w) + "px"; el.style.height = f1(h) + "px";
    el.style.transform = `translate(${f1(x)}px, ${f1(y)}px)${rot ? ` rotate(${rot.toFixed(2)}rad)` : ""}`;
    return el;
  }
  function fxAdd(el, ms, cap) {
    const L = F.els.fx;
    L.appendChild(el);
    const all = L.querySelectorAll("." + el.className.split(" ")[0]);
    if (all.length > cap) all[0].remove();
    setTimeout(() => el.remove(), ms);
  }
  function addTrail(cell) {
    if (!prefs.trail || !F.els.fx) return;
    const prev = F.trailPrev;
    if (prev && now() - prev.t < 700 && prev.cell.key !== cell.key) {
      const dx = cell.x - prev.cell.x, dy = cell.y - prev.cell.y, len = Math.hypot(dx, dy), th = Math.max(2, cell.r * 0.3);
      const el = fxEl("fbv-tl", prev.cell.x, prev.cell.y - th / 2, len, th, Math.atan2(dy, dx));
      el.style.transformOrigin = `0 ${f1(th / 2)}px`;
      fxAdd(el, 2500, 10);
    }
    const d = cell.r * 2.1 + 2;
    fxAdd(fxEl("fbv-tr", cell.x - d / 2, cell.y - d / 2, d, d), 2500, 12);
    F.trailPrev = { cell, t: now() };
  }
  function ringString(cell) {
    if (!F.els.fx || !F.G) return;
    const G = F.G, x0 = prefs.lefty ? G.W - cell.x : cell.x;
    const pts = stringPts(G, cell.s, x0, G.boardEnd), a = pts[0], b = pts[pts.length - 1];
    const dx = b[0] - a[0], dy = b[1] - a[1], len = Math.hypot(dx, dy);
    const el = fxEl("fbv-rg", a[0], a[1] - 2, len, 4, Math.atan2(dy, dx));
    el.style.transformOrigin = "0 2px";
    fxAdd(el, 1300, 6);
  }
  // hot note, likely grip and practice targets: a small pool of markers moved with transforms
  function paintMarkers() {
    const L = F.els.fx;
    if (!L) return;
    const list = [];
    if (F.hot && F.hot.cell) list.push({ c: F.hot.cell, k: "hot" });
    (F.grip || []).forEach((g) => { const c = F.cellMap.get(g.key); if (c) list.push({ c, k: "grip" }); });
    if (prefs.targets && F.target && !prefs.tHide) F.cells.forEach((c) => { if (c.m % 12 === F.target.pc && c.cls.includes(" on") && !c.cls.includes(" dim")) list.push({ c, k: "tgt" }); });
    const sig = list.map((x) => x.k + x.c.key).join() + "|" + F.staticSig;
    if (sig === F.mkSig) return;
    F.mkSig = sig;
    F.mk = F.mk || [];
    list.forEach((x, i) => {
      let el = F.mk[i];
      if (!el) { el = document.createElement("i"); L.appendChild(el); F.mk[i] = el; }
      const R = x.k === "tgt" ? x.c.r + 5 : x.c.r * 2.3;
      const cls = "fbv-mk on " + x.k;
      if (el.className !== cls) el.className = cls;
      el.style.width = el.style.height = f1(R * 2) + "px";
      el.style.transform = `translate(${f1(x.c.x - R)}px, ${f1(x.c.y - R)}px)`;
    });
    for (let i = list.length; i < F.mk.length; i++) if (F.mk[i].className !== "fbv-mk") F.mk[i].className = "fbv-mk";
  }

  /* ---------- targets ---------- */
  function nextTarget() {
    const M = F.M;
    if (!M) return;
    const pcs = M.pcs.filter((pc) => pc !== (F.target && F.target.pc));
    let pc = pcs[Math.floor(Math.random() * pcs.length)];
    if (M.pcs.length > 1 && pc === M.key.root && Math.random() < 0.6) pc = pcs[Math.floor(Math.random() * pcs.length)];
    F.target = { pc, t: now(), hitT: 0 };
  }

  /* =====================================================================
     SOUND (WebAudio in the page; never the engine)
     ===================================================================== */
  const A = { ctx: null, out: null, idleT: 0 };
  function actx() {
    if (!A.ctx) {
      const AC = window.AudioContext || window.webkitAudioContext;
      if (!AC) return null;
      A.ctx = new AC();
      const comp = A.ctx.createDynamicsCompressor();
      A.out = A.ctx.createGain(); A.out.gain.value = 0.2;
      A.out.connect(comp); comp.connect(A.ctx.destination);
    }
    if (A.ctx.state === "suspended") A.ctx.resume();
    clearTimeout(A.idleT);
    A.idleT = setTimeout(audioSleep, 4000);   // a running context keeps the audio device busy
    return A.ctx;
  }
  // suspend even while a resume is still pending (otherwise it would finish resuming and keep the device busy)
  function audioSleep() { clearTimeout(A.idleT); if (A.ctx && A.ctx.state !== "closed") A.ctx.suspend().catch(() => {}); }
  function pluck(midi, when = 0, vel = 0.9) {
    const ctx = actx();
    if (!ctx) return;
    const a4 = (F.d && F.d.a4) || 440, hz = a4 * Math.pow(2, (midi - 69) / 12), t = ctx.currentTime + 0.015 + when;
    const o1 = ctx.createOscillator(), o2 = ctx.createOscillator(), g2 = ctx.createGain(), lp = ctx.createBiquadFilter(), env = ctx.createGain();
    o1.type = "triangle"; o1.frequency.value = hz;
    o2.type = "sine"; o2.frequency.value = hz * 2.003; g2.gain.value = 0.22;
    lp.type = "lowpass"; lp.Q.value = 0.7;
    lp.frequency.setValueAtTime(Math.min(9000, hz * 9), t); lp.frequency.exponentialRampToValueAtTime(Math.max(260, hz * 1.4), t + 1.1);
    env.gain.setValueAtTime(0.0001, t); env.gain.exponentialRampToValueAtTime(vel, t + 0.005); env.gain.exponentialRampToValueAtTime(vel * 0.35, t + 0.25); env.gain.exponentialRampToValueAtTime(0.0001, t + 1.7);
    o1.connect(lp); o2.connect(g2); g2.connect(lp); lp.connect(env); env.connect(A.out);
    o1.start(t); o2.start(t); o1.stop(t + 1.75); o2.stop(t + 1.75);
  }
  function strum(rootPc, ivs) {
    let m = 48 + ((rootPc - 48 % 12 + 12) % 12);
    if (m > 53) m -= 12;
    ivs.forEach((s, i) => pluck(m + s, i * 0.045, 0.7));
    pluck(m + 12, ivs.length * 0.045, 0.55);
  }

  /* =====================================================================
     CONTROLS
     ===================================================================== */
  function set(k, v) { prefs[k] = v; save(); }
  function menuAt(el, items) { if (typeof showMenu !== "function") return; const r = el.getBoundingClientRect(); showMenu(r.left, r.bottom + 4, items); }
  const ck = (on, label) => (on ? "✓ " : "") + label;
  function refresh(hard) { F.posSig = ""; F.staticSig = hard ? "" : F.staticSig; F.M = computeModel(); computePositions(); layout(); }

  function scaleMenu(el) {
    const items = [];
    const key = curKey();
    SCALE_MENU.forEach(([h, ids]) => {
      items.push({ h });
      ids.forEach((id) => {
        const a = AUTO[id];
        const sub = a ? (key.minor ? SC[a.min].n : SC[a.maj].n).toLowerCase() + " now" : SC[id].degs.map(pretty).join(" ");
        items.push({ label: ck(prefs.scale === id, a ? a.n : SC[id].n), sub, onClick: () => { set("scale", id); F.target = null; if (prefs.targets) nextTarget(); refresh(); } });
      });
    });
    menuAt(el, items);
  }
  function stepScale(dir) {
    const i = SCALE_CYCLE.indexOf(prefs.scale);
    set("scale", SCALE_CYCLE[(i + dir + SCALE_CYCLE.length) % SCALE_CYCLE.length]);
    if (prefs.targets) nextTarget();
    refresh();
  }
  function tuningMenu(el) {
    const T = F.tun, lp = window.ListenPane;
    const items = [{ h: "Tuning · shared with the tuner" }];
    T.list.filter((i) => !UNFRETTED.has(i.id)).forEach((I) => {
      items.push({ h: I.label });
      I.tunings.forEach((t) => items.push({ label: ck(T.inst === I.id && T.tid === t.id, t.label), sub: t.notes.replace(/b(\d)/g, "♭$1").replace(/#/g, "♯"), onClick: () => {
        if (lp && lp.setTuning) lp.setTuning(I.id, t.id); else say("The tuner isn't available");
        F.hand = null; refresh(true);
      } }));
    });
    menuAt(el, items);
  }
  function capoMenu(el) {
    const items = [{ h: "Capo" }, { label: ck(!prefs.capo, "No capo"), onClick: () => { set("capo", 0); refresh(); } }];
    for (let c = 1; c <= 9; c++) items.push({ label: ck(prefs.capo === c, `Fret ${c}`), sub: c === 2 ? "" : "", onClick: () => { set("capo", c); F.hand = null; refresh(); } });
    menuAt(el, items);
  }
  function neckMenu(el) {
    const T = F.tun;
    menuAt(el, [
      { h: "Fingerboard" },
      ...Object.entries(WOODS).map(([k, w]) => ({ label: ck(prefs.wood === k, w.n), onClick: () => { set("wood", k); refresh(); } })),
      { h: "Inlays" },
      { label: ck(prefs.inlay === "dots", "Dots"), onClick: () => { set("inlay", "dots"); refresh(); } },
      { label: ck(prefs.inlay === "blocks", "Blocks"), onClick: () => { set("inlay", "blocks"); refresh(); } },
      { h: "Neck" },
      { label: ck(prefs.frets === 22, "22 frets"), disabled: T.maxF < 22 && prefs.frets !== 22, onClick: () => { set("frets", 22); refresh(); } },
      { label: ck(prefs.frets === 24, "24 frets"), onClick: () => { set("frets", 24); refresh(); } },
      { label: ck(prefs.spacing === "real", "Real fret spacing"), sub: "equal temperament", onClick: () => { set("spacing", "real"); refresh(); } },
      { label: ck(prefs.spacing === "even", "Even fret spacing"), sub: "easier to read", onClick: () => { set("spacing", "even"); refresh(); } },
      { label: ck(prefs.lefty, "Left-handed"), onClick: () => { set("lefty", !prefs.lefty); refresh(); } },
      { label: ck(!prefs.highTop, "Low string on top"), sub: prefs.highTop ? "now: like tab" : "as you look down", onClick: () => { set("highTop", !prefs.highTop); refresh(); } },
      { h: "Dots" },
      { label: ck(prefs.color === "degree", "Colour by degree"), sub: prefs.color === "degree" ? "" : "now: root only", onClick: () => { set("color", prefs.color === "degree" ? "root" : "degree"); refresh(); } },
      { label: ck(prefs.trail, "Trail of what you play"), onClick: () => { set("trail", !prefs.trail); refresh(); } },
    ]);
  }

  function keyPop(anchor) {
    const pop = F.els.pop;
    if (!pop.hidden) { closePop(); return; }
    const k = curKey();
    const minor = pop._minor != null ? pop._minor : k.minor;
    pop._minor = minor;
    const roots = [0, 7, 2, 9, 4, 11, 6, 1, 8, 3, 10, 5];   // circle of fifths
    const det = F.det;
    pop.innerHTML = `<button class="fbv-follow${prefs.follow ? " on" : ""}" data-pk="follow"><i></i><span><b>Follow the detected key</b><small>${det ? `hearing ${escH(keyName(det))} ${det.minor ? "minor" : "major"} · ${Math.round((det.conf || 0) * 100)}%` : "listening — play for a few seconds"}</small></span></button>
      <div class="seg wide"><button data-pq="0" class="${minor ? "" : "on"}">Major</button><button data-pq="1" class="${minor ? "on" : ""}">Minor</button></div>
      <div class="fbv-roots">${roots.map((r) => { const on = k.root === r && k.minor === minor; return `<button data-pr="${r}" class="${on ? "on" : ""}${det && det.root === r && det.minor === minor ? " det" : ""}">${escH(keyName({ root: r, minor }))}${minor ? "m" : ""}</button>`; }).join("")}</div>
      <div class="fbv-pop-hint">Picking a key locks it. Circle of fifths order.</div>`;
    const r = anchor.getBoundingClientRect(), hr = F.root.getBoundingClientRect();
    pop.style.left = `${r.left - hr.left}px`;
    pop.style.top = `${r.bottom - hr.top + 6}px`;
    pop.hidden = false;
    setTimeout(() => document.addEventListener("pointerdown", popOutside, true));
  }
  function popOutside(e) { if (!F.els.pop.contains(e.target) && !e.target.closest('[data-act="key"]')) closePop(); }
  function closePop() { if (!F.els.pop) return; F.els.pop.hidden = true; F.els.pop._minor = null; document.removeEventListener("pointerdown", popOutside, true); }

  function wire() {
    const R = F.root, E = F.els;
    R.addEventListener("click", (e) => {
      const b = e.target.closest("[data-act],[data-lab],[data-ci],[data-om],[data-ps],[data-z],[data-tone],[data-pk],[data-pq],[data-pr]");
      if (!b || b.disabled) return;
      const ds = b.dataset;
      if (ds.lab) { set("labels", ds.lab); paint(); return; }
      if (ds.om) { set("omode", ds.om); paint(); return; }
      if (ds.ps) { set("psys", ds.ps); if (ds.ps !== "all") set("pidx", 1); F.posSig = ""; F.shapeSig = ""; computePositions(); layout(); return; }
      if (ds.z) { set("zoom", ds.z === "all" ? "all" : +ds.z); layout(); return; }
      if (ds.ci != null) {
        const c = F.M.dia[+ds.ci];
        const same = prefs.chordSel && prefs.chordSel.root === c.root && prefs.chordSel.quality === c.quality;
        set("chordSel", same ? null : { root: c.root, quality: c.quality });
        if (!same && prefs.sound) strum(c.root, Q[c.quality].iv);
        paint(); return;
      }
      if (ds.tone != null) {
        const pc = +ds.tone;
        if (prefs.targets) { F.target = { pc, t: now(), hitT: 0 }; }
        F.flash = { pc, t: now() };
        if (prefs.sound) pluck(52 + ((pc - 52 % 12 + 12) % 12));
        paint(); setTimeout(() => { if (F.visible) paint(); }, 950);
        return;
      }
      if (ds.pk === "follow") { set("follow", true); closePop(); refresh(); return; }
      if (ds.pq != null) { E.pop._minor = ds.pq === "1"; const m = E.pop._minor; E.pop.hidden = true; keyPop(E.kname); E.pop._minor = m; return; }
      if (ds.pr != null) { set("follow", false); set("lock", { root: +ds.pr, minor: !!E.pop._minor }); closePop(); if (prefs.targets) nextTarget(); refresh(); return; }
      switch (ds.act) {
        case "key": keyPop(E.kname); break;
        case "lock": {
          if (prefs.follow) { const k = curKey(); set("lock", { root: k.root, minor: k.minor }); set("follow", false); }
          else set("follow", true);
          refresh(); break;
        }
        case "scale": scaleMenu(b); break;
        case "livechord":
          if (prefs.chordSel) { set("chordSel", null); set("chordLive", true); }
          else set("chordLive", !prefs.chordLive);
          paint(); break;
        case "sev": set("sev", !prefs.sev); if (prefs.chordSel) set("chordSel", null); F.M = computeModel(); paint(); break;
        case "targets": set("targets", !prefs.targets); F.tScore = 0; if (prefs.targets) nextTarget(); else F.target = null; paint(); break;
        case "unplayed": set("unplayed", !prefs.unplayed); F.played.clear(); paint(); break;
        case "reset": F.played.clear(); paint(); break;
        case "sound": set("sound", !prefs.sound); if (prefs.sound) pluck(57, 0, 0.6); paint(); break;
        case "hear": if (F.target && F.M) { const r = 52 + ((F.M.key.root - 52 % 12 + 12) % 12); pluck(r, 0, 0.7); pluck(r + ((F.target.pc - F.M.key.root + 12) % 12), 0.55, 0.8); } break;
        case "thide": set("tHide", !prefs.tHide); paint(); break;
        case "tskip": nextTarget(); paint(); break;
        case "pprev": case "pnext": stepPos(ds.act === "pnext" ? 1 : -1); break;
        case "phide": set("phide", !prefs.phide); paint(); break;
        case "phand": set("phand", !prefs.phand); paint(); break;
        case "tuning": tuningMenu(b); break;
        case "capo": capoMenu(b); break;
        case "neck": neckMenu(b); break;
      }
    });
    E.pslider.addEventListener("input", () => { set("pidx", +E.pslider.value); F.shapeSig = ""; if (autoWindow()) layout(); else paint(); });
    // notes: click to hear, hover to read
    const cellOf = (e) => { const g = e.target.closest && e.target.closest(".fbc"); return g ? F.cellMap.get(g.dataset.c) : null; };
    E.notes.addEventListener("click", (e) => {
      const c = cellOf(e);
      if (!c || !c.cls.includes(" on")) return;
      if (prefs.sound) pluck(c.m);
      c.el.classList.remove("tap"); void c.el.getBBox(); c.el.classList.add("tap");
      setTimeout(() => c.el.classList.remove("tap"), 450);
    });
    E.notes.addEventListener("pointerover", (e) => { const c = cellOf(e); const v = c && c.cls.includes(" on") ? c : null; if (v !== F.hover) { F.hover = v; paintStatus(); } });
    E.notes.addEventListener("pointerleave", () => { if (F.hover) { F.hover = null; paintStatus(); } });
    // pan a zoomed neck with the wheel / trackpad
    let acc = 0;
    E.neck.addEventListener("wheel", (e) => {
      if (!F.G || F.G.zoomN >= F.G.maxF) return;
      const dx = Math.abs(e.deltaX) > Math.abs(e.deltaY) ? e.deltaX : e.shiftKey ? e.deltaY : 0;
      if (!dx) return;
      e.preventDefault();
      acc += (prefs.lefty ? -dx : dx);
      if (Math.abs(acc) < 36) return;
      const st = Math.sign(acc); acc = 0;
      const nw = clamp(F.ws + st, 0, F.G.maxF - F.G.zoomN);
      if (nw !== F.ws) { F.ws = nw; layout(); }
    }, { passive: false });
    document.addEventListener("keydown", (e) => {
      if (!F.visible || e.metaKey || e.ctrlKey || e.altKey || typingIn(document.activeElement)) return;
      if (typeof ui !== "undefined" && ui.modalOpen) return;
      if (e.key === "Escape" && !E.pop.hidden) { closePop(); return; }
      if ((e.key === "ArrowLeft" || e.key === "ArrowRight") && curPos() && !(document.activeElement && document.activeElement.closest && document.activeElement.closest("#view-tabs"))) { e.preventDefault(); stepPos(e.key === "ArrowRight" ? 1 : -1); }
      else if (e.key === "[" || e.key === "]") { e.preventDefault(); stepScale(e.key === "]" ? 1 : -1); }
    });
    if (window.ListenPane && ListenPane.onChange) ListenPane.onChange(() => { if (!F.visible) return; const t = tuningInfo(); if (!F.tun || t.notes.join() !== F.tun.notes.join() || t.maxF !== F.tun.maxF) { F.hand = null; refresh(true); } });
  }
  function stepPos(dir) {
    const P = F.pos;
    if (!P || P.kind === "all" || !P.list.length) return;
    const n = P.list.length;
    set("pidx", ((clamp(prefs.pidx, 1, n) - 1 + dir + n) % n) + 1);
    set("phand", false);
    F.shapeSig = "";
    if (autoWindow()) layout(); else paint();
  }

  /* =====================================================================
     LOOP (only while the tab is shown and the window awake)
     ===================================================================== */
  function active() { return F.visible && awake(); }
  function tick() {
    F.loopT = 0;
    if (!active()) return;
    F.stats.ticks++;
    const lp = window.ListenPane;
    if (lp && lp.want && lp.latest) {
      lp.want();
      const d = lp.latest();
      if (d && d !== F.lastD) { F.lastD = d; onData(d); }
    } else ownPoll();
    // the wood picture carries the theme's shadow: redraw it when the appearance changes
    const lt = document.body.classList.contains("light");
    if (lt !== F.light) { const had = F.light != null; F.light = lt; if (had) schedule(true); }
    // a held note that stopped: let it go
    if (F.hot && now() - (F.hotT || 0) > 400) { F.hot = null; F.hotAlt = []; paintCells(); paintStatus(); }
    if (F.layers && F.layers.hand.classList.contains("on") && now() - F.lastOnsetT > 2500) F.layers.hand.classList.remove("on");
    if (F.flash && now() - F.flash.t > 950) { F.flash = null; paintCells(); }
    const busy = (F.d && F.d.level && F.d.level.signal) || now() - F.lastOnsetT < 3000;
    F.loopT = setTimeout(tick, busy ? 40 : 220);
  }
  async function ownPoll() {
    if (F.ownBusy) return;
    F.ownBusy = true;
    try { const r = await fetch(base() + "/api/listen", { cache: "no-store" }); const d = await r.json(); if (d && d.ok && !d.warming && F.visible) onData(d); } catch (e) { /* engine away */ }
    F.ownBusy = false;
  }
  function start() { if (!F.loopT && active()) F.loopT = setTimeout(tick, 0); }
  function stop() { clearTimeout(F.loopT); F.loopT = 0; }
  if (window.HRActivity) HRActivity.on((e) => { if (!F.visible) return; if (e.awake) start(); else { stop(); audioSleep(); } });

  function enter() {
    if (!build()) return;
    F.visible = true;
    const lp = window.ListenPane;
    if (lp && lp.latest) { const d = lp.latest(); if (d && d !== F.lastD) { F.lastD = d; F.d = d; const k = d.key; if (k && k.root != null && k.root >= 0) F.det = { root: k.root, minor: !!k.minor, conf: k.conf, alt_root: k.alt_root, alt_minor: k.alt_minor, state: k.state }; } }
    F.tun = tuningInfo();
    F.M = computeModel();
    if (prefs.targets && !F.target) nextTarget();
    requestAnimationFrame(() => layout());
    start();
  }
  function leave() {
    F.visible = false;
    stop();
    closePop();
    F.hot = null; F.grip = null;
    audioSleep();
  }

  function reg() { if (window.HRViews && HRViews.register) { HRViews.register("fretboard", { enter, leave }); return true; } return false; }
  if (!reg()) document.addEventListener("DOMContentLoaded", reg, { once: true });

  // for tests / other views
  window.HRFretboard = {
    stats: () => Object.assign({ loop: !!F.loopT, visible: F.visible, audio: A.ctx ? A.ctx.state : "none" }, F.stats),
    state: () => ({ key: F.M && F.M.key, title: F.M && F.M.title, scale: prefs.scale, pos: curPos() && { idx: curPos().idx, name: curPos().name }, hand: F.hand, hot: F.hot && F.hot.cell && F.hot.cell.key, grip: F.grip && F.grip.map((g) => g.key), ws: F.ws }),
    set: (k, v) => { prefs[k] = v; save(); if (F.visible) { F.posSig = ""; F.staticSig = ""; layout(); } },
  };
})();
