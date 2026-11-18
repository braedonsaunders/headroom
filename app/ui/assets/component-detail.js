/* =====================================================================
   COMPONENT DETAIL PAGE
   Click a card in Components → the centre pane becomes a page about that
   unit: the hardware render (its knobs drive the page), how Headroom models
   it, an interactive schematic generated from the engine's exact model
   (/api/component/model), live analysis curves rendered offline by the
   engine (/api/component/curves), and — for tube amps and drive pedals —
   a customiser whose overrides can be auditioned on a rack slot and saved
   as "My Components".
   Relies on index.html globals: API_BASE, comps, rackState, ui, esc, $,
   compSlot, unitPreviewHTML, layoutDesc, paintEl, localFmt, snapNorm,
   paramKind, stepCount, displayVal, showTip, hideTipSoon, insertComponent,
   engineRack, getSlot, toast, renderComponents, markDirty.
   ===================================================================== */
(function () {
  "use strict";

  /* ---------- what each unit is modelled after (descriptive references) ---------- */
  const ABOUT = {
    34000: ["Marshall 1959 Super Lead “Plexi” (non-master, late 1960s)", "Late-60s British rock in its purest form: two cascaded triodes and a cathode follower into a power section you open up with the volume knobs."],
    92000: ["Marshall 1959 Super Lead on a Variac — the “brown sound”", "The Plexi with its mains voltage dialled down: more sag, earlier power-tube breakup and the soft, chewy compression of late-70s hard rock."],
    91000: ["Hot-rodded Marshall 1959 (extra gain stage + master volume)", "A modded Plexi with a third gain stage and a master volume — Plexi voicing at a usable volume, with real lead gain."],
    56000: ["Marshall JCM800 2203 (1981)", "The definitive 80s British lead amp: a cold-clipping second stage drives a Marshall tone stack and an EL34 power section that barks."],
    101000: ["Marshall JCM800 with jumped inputs / added gain mode", "The 800 with a gain switch that cascades one more stage — tighter and hotter, the template for countless modded Marshalls."],
    134000: ["Peavey 5150 (1992)", "A five-stage cascade built for chugging: tight low end, aggressive upper mids and a lead channel that saturates without going mushy."],
    122000: ["Soldano SLO-100 Super Lead Overdrive", "The original Californian hot-rod: a normal channel with sparkle and an overdrive channel that cascades four triodes into a smooth, singing lead."],
    163000: ["Soldano SLO-100", "The SLO circuit in its later form — crunch on the normal side, a fat cascaded overdrive channel on the other."],
    81000: ["Bogner Überschall", "A German-voiced modern metal head: massive gain, a scooped low-mid and a hard, focused attack."],
    16000: ["Mesa/Boogie Dual Rectifier (three-channel era)", "The nu-metal wall of sound: four modes from clean to modern, a heavy-sagging power section and a loose, enormous bottom end."],
    96000: ["Modified Rectifier-style rig for industrial metal", "A tightened, hotter take on the Rectifier platform — more pre-gain, less sag, punchier palm mutes."],
    80000: ["Orange OR120 (1970s)", "British, thick and woolly: an Orange-voiced preamp and a fat, loose power section with a low-cut to rein in the bottom."],
    87000: ["Hiwatt DR103 Custom 100", "Enormous clean headroom, a mid-forward tone stack and a stiff, tightly regulated power section — loud, clear and articulate."],
    30000: ["Fender Twin Reverb (1965 blackface)", "Pristine blackface clean: Fender tone stack ahead of the volume control, huge headroom, spring reverb and bias tremolo."],
    172000: ["Fender Deluxe Reverb (1965 blackface)", "The studio favourite: the blackface circuit in a small combo that breaks up sweetly when pushed, with reverb and tremolo."],
    62000: ["Fender 5F6-A Bassman (1959 tweed 4×10)", "The tweed that became the blueprint for the Marshall: a cathode follower drives the tone stack, and everything compresses gracefully."],
    79000: ["Fender 5E3 Deluxe (tweed)", "Small, open-loop and raw: no negative feedback, one tone control and a power section that sags and growls early."],
    152000: ["Valco / Supro combos of the early 1960s", "Mid-century American lo-fi: a single tone control, no feedback and a dirty, saturated character from the first note."],
    38000: ["Vox AC30 Top Boost", "Chime: EL84s in class A with no negative feedback, a bright top-boost tone network and a cut control on the power section."],
    176000: ["Vox AC15 (handwired)", "The AC30’s little sibling — the same class-A, no-feedback voicing in a smaller box, with tremolo and vibrato."],
    151000: ["Friedman BE-100 Deluxe", "Three gain modes from crunchy rhythm to a fully cascaded lead, voiced tight and articulate for modern rock and metal."],
    300000: ["Friedman BE-100 Brown Eye", "Modern hot-rodded British plexi with cascaded high-gain stages, tight low end, and singing midrange."],
    162000: ["Bogner Ecstasy 101B", "A dynamic, touch-sensitive overdrive channel with three gain modes, from edge-of-breakup to a saturated lead."],
    49000: ["Roland JC-120 Jazz Chorus", "The solid-state clean standard: flat, stiff and glassy, with its famous stereo chorus and vibrato."],
    54000: ["Ampeg SVT (1969, 300 W tube bass head)", "The rock bass reference: a big tube power section, ultra-low/high switches and a 9-band graphic EQ."],
    177000: ["Ampeg SVT-CL", "The classic all-tube SVT voicing with ultra-low and ultra-high switches."],
    150000: ["Solid-state bass head in the Gallien-Krueger 800RB tradition", "A stiff solid-state bass amp with boost, low-cut and contour switching."],
    15000: ["Ibanez TS808 Tube Screamer", "The mid-hump overdrive: diodes in the op-amp feedback loop clip only the band above ~720 Hz, and the clean signal is summed back in."],
    167000: ["Ibanez TS808 with extended EQ and clipping modes", "The Screamer circuit with bass, mid and treble controls and modes that change the diode knee and the feedback bandwidth."],
    40000: ["Dallas Rangemaster treble booster", "One germanium transistor biased hot, fed through a tiny input cap — the 60s British lead secret for kicking an amp into treble-rich overdrive."],
    65000: ["Clean FET boosters (MXR Micro Amp class)", "Up to 20 dB of clean gain, soft-limited only at the rails."],
    17000: ["Op-amp hard-clip distortion (DS-1 / Distortion+ family)", "Gain into diodes to ground with a tone blend — the classic stompbox distortion."],
    84000: ["Marshall Guv’nor / Jackhammer", "The amp-in-a-box Marshall pedal: hard clipping with a contour control for the mids."],
    42000: ["ProCo RAT", "An LM308 op-amp whose gain-bandwidth limit darkens the sound as you turn up the gain, into hard silicon diodes and a sweepable low-pass filter."],
    41000: ["Arbiter Fuzz Face (silicon)", "Two transistors in feedback: bias starve makes it sputter on attack and clean up with the guitar’s volume knob."],
    166000: ["Dallas-Arbiter Fuzz Face (germanium)", "The warm, round germanium Fuzz Face — softer, with more sag and clean-up."],
    43000: ["Electro-Harmonix Big Muff Pi (triangle-knob era)", "Two cascaded clipping stages and a scooped tone control: the sustaining wall of fuzz."],
    174000: ["Interfax Harmonic Percolator", "A germanium/silicon transistor pair into hard diodes — a gnarly, harmonically rich fuzz."],
    44000: ["High-gain metal distortion pedals", "A hot op-amp stage with pre-EQ into hard diodes and a parametric mid for modern metal."],
    46000: ["Boss MT-2 Metal Zone", "Cascaded high gain with a sweepable semi-parametric mid control."],
    45000: ["Tech 21 SansAmp Classic", "Analog tube-amp emulation: two asymmetric stages, a Baxandall-style EQ and a speaker-simulation switch."],
    170000: ["Instrument preamp pedals", "Two tube-style stages and EQ in a pedal format — warmth and drive without a full amp."],
    111000: ["Dual-stage boutique overdrives", "Two soft, asymmetric stages for a layered, amp-like overdrive."],
    165000: ["Boss HM-2 Heavy Metal (the “Swedish chainsaw”)", "Huge boosts at 95 Hz and 1.5 kHz ahead of hard diodes: the buzzsaw sound of Swedish death metal."],
    173000: ["Analog character / saturation boxes", "Gentle analog-style colour and saturation with EQ, for adding density rather than distortion."],
    123000: ["UREI 1176LN FET limiting amplifier", null], 124000: ["Teletronix LA-2A optical leveller", null], 125000: ["dbx 160 VCA compressor", null],
    154000: ["Lexicon 224XL digital reverb", null], 155000: ["Lexicon 480L digital reverb", null], 121000: ["SSL 4000 E-Series channel EQ", "The 82E242 card has separate HP In and LP In switches. Each filter frequency knob sets the cutoff when its filter is engaged."],
    130000: ["SSL G-series bus compressor", null], 82000: ["Electro-Harmonix Deluxe Memory Man", null], 78000: ["Roland RE-201 Space Echo", null],
    31000: ["Leslie rotating-speaker cabinet", null], 37000: ["DigiTech Whammy", null], 71000: ["Dunlop Cry Baby wah", null], 72000: ["Vox Clyde McCoy wah", null],
    75000: ["MXR Phase 90", null], 74000: ["Electro-Harmonix Electric Mistress", null], 142000: ["Roland Dimension D", null], 48000: ["Ross / MXR Dyna Comp style compressors", null],
    24000: ["Vari-mu tube compressors", null], 27000: ["Spring reverb tanks", null], 127000: ["Classic EMT-style plates and spring tanks (measured IRs)", null],
    156000: ["Guitar cabinets matched to each amp (measured IRs)", null], 119000: ["A studio live room with eight mic channels (measured IRs)", null],
    168000: ["Third-party cabinet impulse responses", null], 88000: ["Guitar cabinets matched to each amp (measured IRs)", null],
  };
  const KIND_LABEL = { amp: "Tube amplifier", drive: "Drive pedal", nam: "Neural capture", cab: "Cabinet", ir_reverb: "Convolution reverb", fdn_reverb: "Algorithmic reverb",
    delay: "Delay", mod: "Modulation", dynamics: "Dynamics", eq: "EQ", fallback: "EQ stand-in", midi: "MIDI", instrument: "Instrument", plugin: "Hosted plug-in" };
  const TAB_KEY = "headroom.component.tab"; // the last sub-tab chosen (Circuit / Analysis / Details), across units and sessions
  const SERIES = ["var(--cd-s1)", "var(--cd-s2)", "var(--cd-s3)", "var(--cd-s4)", "var(--cd-s5)"];

  const S = {
    open: false, tid: 0, ucid: null, uc: null, comp: null, params: [], norms: {}, model: null, curves: null, overrides: {}, edit: false, sel: null,
    slot: -1, audOn: false, slotOrig: null, tone: { level: 0.25, freq: 330 }, mseq: 0, cseq: 0, users: [], usersLoaded: false, applyT: 0, refT: 0, name: "",
    tab: (() => { try { return localStorage.getItem(TAB_KEY) || "schem"; } catch (e) { return "schem"; } })(), anaCount: -1,
    editEpoch: 0, tseq: 0, // circuit editor: a new epoch resets its draft (open, Reset); tseq orders topology checks
  };
  let root = null;
  const enc = encodeURIComponent;
  const api = (p) => fetch(API_BASE + p, { cache: "no-store" }).then((r) => (r.ok ? r.json() : Promise.reject(new Error(r.status))));
  const isEmpty = (o) => !o || !Object.keys(o).length;
  const r1 = (v) => (Math.abs(v) >= 100 ? v.toFixed(0) : Math.abs(v) >= 10 ? v.toFixed(1) : v.toFixed(2));
  const fmtHz = (f) => (f >= 1000 ? `${(f / 1000).toFixed(f >= 10000 ? 0 : 1)}k` : `${Math.round(f)}`);
  const fmtDb = (d) => `${d > 0.05 ? "+" : ""}${d.toFixed(1)} dB`;

  /* =====================================================================
     overrides (nested JSON, addressed by the engine's dotted edit paths)
     ===================================================================== */
  function getPath(o, path) { return path.split(".").reduce((a, k) => (a && typeof a === "object" ? a[k] : undefined), o); }
  function setPath(o, path, v) {
    const ks = path.split(".");
    let a = o;
    ks.slice(0, -1).forEach((k) => { if (!a[k] || typeof a[k] !== "object") a[k] = {}; a = a[k]; });
    a[ks[ks.length - 1]] = v;
  }
  function delPath(o, path) {
    const ks = path.split("."), stack = [];
    let a = o;
    for (const k of ks.slice(0, -1)) { if (!a || typeof a !== "object") return; stack.push([a, k]); a = a[k]; }
    if (a && typeof a === "object") delete a[ks[ks.length - 1]];
    for (let i = stack.length - 1; i >= 0; i--) { const [p, k] = stack[i]; if (p[k] && typeof p[k] === "object" && isEmpty(p[k])) delete p[k]; }
  }
  function leaves(o, pre = "", out = []) {
    Object.entries(o || {}).forEach(([k, v]) => { const p = pre ? `${pre}.${k}` : k; if (v && typeof v === "object" && !Array.isArray(v)) leaves(v, p, out); else out.push(p); });
    return out;
  }
  // Edits counted for the edit bar: each value override is one, and so is each circuit-editor operation (an added,
  // removed or rewired part, a renamed net) rather than every field inside it.
  function changeCount() {
    const t = getPath(S.overrides, "circuit.topology") || {};
    const topo = Object.keys(t.add || {}).length + (t.remove || []).length + Object.keys(t.wire || {}).length + Object.keys(t.rename || {}).length;
    return leaves(S.overrides).filter((p) => !p.startsWith("circuit.topology.")).length + topo;
  }
  function modelStr(ov) {
    ov = ov || S.overrides;
    if (isEmpty(ov) && !S.ucid) return "";
    const m = JSON.parse(JSON.stringify(ov));
    m.schema = "headroom.overrides/1";
    if (S.ucid) Object.assign(m, { name: S.uc ? S.uc.name : S.name, id: S.ucid, base: S.tid });
    return JSON.stringify(m);
  }
  const editable = () => !!(S.model && S.model.editable);

  /* =====================================================================
     user components ("My Components")
     ===================================================================== */
  function applyUsers() {
    if (typeof comps === "undefined") return;
    comps.list = comps.list.filter((c) => !c.ucid);
    comps.categories = comps.categories.filter((c) => c !== "My Components");
    const items = S.users.map((u) => {
      const base = comps.byTid.get(u.base);
      return base ? { ...base, ucid: u.id, uname: u.name, name: `Custom ${base.ref || base.name}`, category: "My Components", engine: `${u.name} · ${base.engine || ""}` } : null;
    }).filter(Boolean);
    if (items.length) { comps.list.unshift(...items); comps.categories.unshift("My Components"); }
  }
  async function loadUsers() {
    try {
      const d = await api("/api/usercomponents");
      S.users = Array.isArray(d.components) ? d.components : [];
      S.usersLoaded = true;
      applyUsers();
      if (typeof renderComponents === "function") renderComponents();
    } catch (e) { /* engine without the endpoint */ }
  }
  function injectUserComponents() { applyUsers(); if (!S.usersLoaded) loadUsers(); }

  /* =====================================================================
     open / close
     ===================================================================== */
  function ensureRoot() {
    if (root) return root;
    root = document.createElement("section");
    root.className = "cd-page";
    root.id = "cd-page";
    root.hidden = true;
    root.setAttribute("aria-label", "Component details");
    $("stage").appendChild(root);
    wire();
    return root;
  }
  function paramsFrom(comp, slot) {
    const src = (slot && slot.params && slot.params.length) ? slot.params : (comp.params || []);
    return src.map((p) => {
      let n = slot ? p.norm : (typeof p.default === "number" ? p.default : (typeof p.norm === "number" ? p.norm : 0.5));
      const q = { ...p, norm: n, val: p.min + n * (p.max - p.min) };
      q.fmt = slot ? p.fmt : localFmt(q);
      return q;
    });
  }
  function open(tid, ucid) {
    tid = +tid;
    const comp = comps.byTid.get(tid);
    if (!comp) return;
    ensureRoot();
    if (S.open && S.tid === tid && (S.ucid || null) === (ucid || null)) { root.querySelector(".cd-scroll").scrollTop = 0; return; }
    const uc = ucid ? S.users.find((u) => u.id === ucid) : null;
    Object.assign(S, { open: true, tid, ucid: uc ? uc.id : null, uc, comp, model: null, curves: null, sel: null, edit: false, audOn: false, slotOrig: null, anaCount: -1, editEpoch: S.editEpoch + 1, tseq: S.tseq + 1,
      overrides: uc && uc.model ? stripMeta(uc.model) : {}, name: uc ? uc.name : `My ${comp.ref || comp.name}` });
    // A rack slot of the same unit (prefer the selected one, or one already carrying this user component).
    const same = (rackState.slots || []).filter((s) => s.table_id === tid);
    const pick = same.find((s) => uc && s.custom_name === uc.name) || same.find((s) => s.slot_idx === ui.selected) || same[0];
    S.slot = pick ? pick.slot_idx : -1;
    S.params = paramsFrom(comp, null);
    if (uc && uc.norms) S.params.forEach((p) => { if (typeof uc.norms[p.id] === "number") { if (typeof p.on === "boolean" && Math.abs(uc.norms[p.id] - p.norm) > 1e-6) p.on = uc.norms[p.id] > 0.5; p.norm = uc.norms[p.id]; p.val = p.min + p.norm * (p.max - p.min); p.fmt = localFmt(p); } });
    S.norms = Object.fromEntries(S.params.map((p) => [p.id, p.norm]));
    root.hidden = false;
    document.body.classList.add("cd-open");
    if (window.HRViews) HRViews.onDetailOpen(S.uc ? S.uc.name : (comp.ref || comp.name)); // shows the centre pane's Component tab
    renderShell();
    refresh(true);
  }
  function stripMeta(m) { const o = JSON.parse(JSON.stringify(m || {})); delete o.name; delete o.id; delete o.base; delete o.schema; return o; }
  function close() {
    if (!S.open) return;
    if (S.audOn && changeCount() && !S.ucid) toast(`Your changes stay on slot ${posOf(S.slot)} — save them as a component to reuse them.`);
    S.open = false;
    root.hidden = true;
    root.innerHTML = "";
    document.body.classList.remove("cd-open");
    hideTipSoon(0);
    if (window.HRViews) HRViews.onDetailClose(); // back to the Signal Chain tab
  }
  const isOpen = () => S.open;
  const posOf = (slotIdx) => (rackState.slots || []).findIndex((s) => s.slot_idx === slotIdx) + 1;

  /* =====================================================================
     engine I/O
     ===================================================================== */
  function query(ov) { return `tid=${S.tid}&norms=${enc(JSON.stringify(S.norms))}&model=${enc(modelStr(ov))}`; }
  // Circuit editor (circuit-view.js): a topology draft is checked by the engine before it becomes an edit. The page
  // asks for the model with the draft; the engine compiles it, settles it and smoke-renders it. Accepted: it joins the
  // overrides (audition, save, presets). Rejected: the overrides keep the last accepted circuit and the reason is returned.
  async function applyTopology(topo) {
    const seq = ++S.tseq, tid = S.tid, ucid = S.ucid;
    const next = JSON.parse(JSON.stringify(S.overrides));
    if (topo) setPath(next, "circuit.topology", topo); else delPath(next, "circuit.topology");
    let m;
    try { m = await api(`/api/component/model?${query(next)}`); } catch (e) { return seq === S.tseq ? { ok: false, error: `the engine did not answer (${e.message || e})` } : { stale: true }; }
    if (seq !== S.tseq || !S.open || S.tid !== tid || S.ucid !== ucid) return { stale: true };
    if (m.error) return { ok: false, error: m.error };
    S.overrides = next;
    ++S.mseq; // a model request still in flight was for the previous circuit
    S.model = m;
    renderModelParts();
    fetchCurves();
    scheduleApply();
    return { ok: true };
  }
  function refresh(now) {
    clearTimeout(S.refT);
    S.refT = setTimeout(() => { fetchModel(); fetchCurves(); }, now ? 0 : 90);
  }
  async function fetchModel() {
    const seq = ++S.mseq;
    try {
      const m = await api(`/api/component/model?${query()}`);
      if (seq !== S.mseq || !S.open) return;
      S.model = m.error ? { error: m.error, kind: "generic", blocks: [] } : m;
    } catch (e) { if (seq === S.mseq) S.model = { error: String(e.message || e), kind: "generic", blocks: [] }; }
    renderModelParts();
  }
  let curvesBusy = false, curvesAgain = false;
  async function fetchCurves() {
    if (curvesBusy) { curvesAgain = true; return; }
    curvesBusy = true;
    root.querySelectorAll(".cd-plot").forEach((p) => p.classList.add("busy"));
    try {
      const c = await api(`/api/component/curves?${query()}&what=all&level=${S.tone.level}&freq=${S.tone.freq}`);
      if (S.open && !c.error) {
        S.curves = c; renderAnalysis(); renderHead();
        if (S.model && S.model.kind === "circuit" && window.CircuitView) window.CircuitView.updateCurves(c.static);
      } else if (S.open && !S.curves) { S.anaCount = 0; renderTabs(); } // nothing to plot for this unit: no Analysis tab
    } catch (e) { if (S.open && !S.curves) { S.anaCount = 0; renderTabs(); } /* else keep the previous curves */ }
    curvesBusy = false;
    root && root.querySelectorAll(".cd-plot").forEach((p) => p.classList.remove("busy"));
    if (curvesAgain) { curvesAgain = false; fetchCurves(); }
  }
  // Audition: push the overrides to the linked rack slot (debounced; the engine pre-computes amp level trims).
  function scheduleApply() {
    if (!S.audOn || S.slot < 0) return;
    clearTimeout(S.applyT);
    S.applyT = setTimeout(async () => {
      try { await api(`/api/component/apply?slot=${S.slot}&model=${enc(modelStr())}`); if (typeof markDirty === "function") markDirty(); } catch (e) { /* ignore */ }
    }, 260);
  }
  async function setAudition(on) {
    if (S.slot < 0) return;
    const s = getSlot(S.slot);
    if (!s || s.table_id !== S.tid) { S.slot = -1; heroLive(); renderEditBar(); return; }
    if (on) {
      // Take over the slot: the page's knobs now mirror (and drive) the unit in the chain.
      S.slotOrig = { model: "" };
      try { const m = await api(`/api/component/model?tid=${S.tid}&slot=${S.slot}`); S.slotOrig.model = m.overrides && !isEmpty(m.overrides) ? JSON.stringify(m.overrides) : ""; } catch (e) { /* ignore */ }
      S.params = paramsFrom(S.comp, s);
      S.norms = Object.fromEntries(S.params.map((p) => [p.id, p.norm]));
      S.audOn = true;
      renderHero();
      scheduleApply();
      refresh();
    } else {
      S.audOn = false;
      if (S.slotOrig) api(`/api/component/apply?slot=${S.slot}&model=${enc(S.slotOrig.model)}`).catch(() => {});
    }
    renderEditBar();
  }
  function changed() {
    renderEditBar();
    refresh();
    scheduleApply();
  }

  /* =====================================================================
     shell
     ===================================================================== */
  const ICO = {
    back: `<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"><path d="M10 3.5 5.5 8l4.5 4.5"/></svg>`,
    plus: `<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"><path d="M8 3v10M3 8h10"/></svg>`,
    wrench: `<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"><path d="M10.4 2.6a3.2 3.2 0 0 0-4 4L2.6 10.4a1.4 1.4 0 0 0 2 2l3.8-3.8a3.2 3.2 0 0 0 4-4l-2 2-1.9-.5-.5-1.9z"/></svg>`,
  };
  function renderShell() {
    const c = S.comp;
    root.innerHTML = `
      <div class="cd-bar">
        <button class="cd-back" data-act="back" title="Close and go back to the signal chain (Esc)">${ICO.back}Signal Chain<kbd>esc</kbd></button>
        <span class="cd-crumb">${esc(c.category || "Components")} / <b>${esc(S.uc ? S.uc.name : (c.ref || c.name))}</b></span>
        <span class="sp"></span>
        <span id="cd-bar-actions"></span>
      </div>
      <div class="cd-scroll">
        <div class="cd-hero"><div class="cd-hero-stage" id="cd-hero-stage"></div><div class="cd-engine-controls" id="cd-engine-controls"></div><div class="cd-hero-hint">Turn the knobs on the unit — the schematic and every plot below follow, computed by the engine.</div></div>
        <div class="cd-head" id="cd-head"></div>
        <div class="cd-sec cd-about-sec"><div class="cd-about" id="cd-about"><div class="cd-card txt cd-loading" style="padding:30px">Reading the model from the engine…</div></div></div>
        <div class="cd-tabs" id="cd-tabs" role="tablist" aria-label="Component sections"></div>
        <div class="cd-sec cd-pane" id="cd-schem-sec" data-pane="schem" role="tabpanel" hidden></div>
        <div class="cd-sec cd-pane" id="cd-ana-sec" data-pane="analysis" role="tabpanel" hidden><div class="cd-card txt cd-loading" style="padding:40px">Rendering the analysis on a private copy of the unit…</div></div>
        <div class="cd-sec cd-pane" id="cd-details-sec" data-pane="details" role="tabpanel" hidden></div>
        <div id="cd-edit-slot"></div>
      </div>`;
    renderBarActions();
    renderHero();
    renderHead();
    renderTabs();
  }

  /* ---------- sub-tabs: Circuit (or Signal path) · Analysis · Details ---------- */
  function tabList() {
    const m = S.model;
    const t = [];
    if (m && m.kind === "circuit" && m.circuit) t.push(["schem", "Circuit"]);
    else if (m && !m.error && (m.blocks || []).length) t.push(["schem", "Signal path"]);
    if (S.anaCount !== 0) t.push(["analysis", "Analysis", S.anaCount > 0 ? S.anaCount : ""]);
    t.push(["details", "Details"]);
    return t;
  }
  function currentTab() { const t = tabList(); return (t.find((x) => x[0] === S.tab) || t[0])[0]; }
  function renderTabs() {
    const bar = $("cd-tabs");
    if (!bar) return;
    const t = tabList(), cur = currentTab();
    bar.innerHTML = t.map(([id, label, n]) => `<button role="tab" data-cd-tab="${id}" class="${id === cur ? "on" : ""}" aria-selected="${id === cur}" aria-controls="cd-${id === "schem" ? "schem" : id === "analysis" ? "ana" : "details"}-sec">${esc(label)}${n ? `<span class="n">${n}</span>` : ""}</button>`).join("");
    root.querySelectorAll(".cd-pane").forEach((p) => { p.hidden = p.dataset.pane !== cur; });
  }
  function setTab(id) {
    const prev = currentTab();
    S.tab = id;
    try { localStorage.setItem(TAB_KEY, id); } catch (e) { /* private mode */ }
    renderTabs();
    const cur = currentTab();
    if (prev === "schem" && cur !== "schem" && window.CircuitView) window.CircuitView.hidden();
    if (cur === "schem") {
      if (S.model && S.model.kind === "circuit") { if (window.CircuitView) window.CircuitView.shown(); }
      else renderSchematic(); // the block diagram sizes itself to the pane
    }
    // keep the tab bar in view when the page was scrolled past it
    const bar = $("cd-tabs"), sc = root.querySelector(".cd-scroll");
    if (bar && sc && sc.scrollTop > bar.offsetTop) sc.scrollTop = bar.offsetTop;
  }
  function renderBarActions() {
    const box = $("cd-bar-actions");
    if (!box) return;
    const canEdit = S.model ? editable() : false;
    const where = ui.selected != null && getSlot(ui.selected) ? `after ${getSlot(ui.selected).reference_name}` : "at the end of the chain";
    box.style.display = "inline-flex";
    box.style.gap = "8px";
    box.innerHTML = `${canEdit ? `<button class="cd-btn${S.edit ? " on" : ""}" data-act="edit" title="Edit this unit's model values and save your own version">${ICO.wrench}${S.edit ? "Customizing" : "Customize"}</button>` : ""}
      <button class="cd-btn primary" data-act="add" title="Insert ${esc(where)}">${ICO.plus}Add to chain</button>`;
  }

  /* ---------- hero: the real hardware render, interactive ---------- */
  function heroSlot() {
    const c = S.comp;
    return { slot_idx: -1, table_id: c.table_id, reference_name: c.ref || c.name, display_name: c.name || c.ref, archetype: c.archetype || "", bypassed: false,
      params: S.params, primary: c.primary, engine: c.engine || "" };
  }
  function renderHero() {
    const st = $("cd-hero-stage");
    if (!st) return;
    let html = "";
    try { html = unitPreviewHTML(heroSlot()); } catch (e) { html = `<div class="cd-err">No hardware render for this unit.</div>`; }
    st.innerHTML = `<div class="cd-hero-unit" id="cd-hero-unit" data-ua-slot="${S.slot}">${html}</div><div class="cd-hero-floor"></div>`;
    $("cd-hero-unit").curveQuery = query;
    heroLive();
    renderEngineControls();
    requestAnimationFrame(fitHero);
  }
  // The hero's live meters, lamps and screens (assets/unit-art.js) read the rack slot that carries this unit, if any.
  function heroLive() {
    const u = $("cd-hero-unit");
    if (u) u.dataset.uaSlot = String(S.slot);
    if (window.HRUnitArt && HRUnitArt.indexMeters) HRUnitArt.indexMeters();
  }
  function renderEngineControls() {
    const box = $("cd-engine-controls");
    if (!box) return;
    const ids = ["amp_core", "input_voltage_scale", "input_gate", "level_matching"];
    box.innerHTML = ids.map((id) => {
      const p = S.params.find((v) => v.id === id);
      if (!p || !p.labels || !p.labels.length) return "";
      const forced = id === "amp_core" && !!S.overrides.circuit;
      const selected = forced ? 1 : Math.round(p.norm * (p.labels.length - 1));
      return `<label><span>${esc(p.name || id)}</span><select data-engine-param="1" data-pid="${esc(id)}"${forced ? ' title="Reset electrical component edits to return to the voicing model"' : ""}>${p.labels.map((label, i) => `<option value="${i}"${i === selected ? " selected" : ""}${forced && i === 0 ? " disabled" : ""}>${esc(label)}</option>`).join("")}</select></label>`;
    }).join("");
  }
  function fitHero() {
    const st = $("cd-hero-stage"), u = $("cd-hero-unit");
    if (!st || !u) return;
    u.style.transform = "none";
    const w = u.offsetWidth || 1, h = u.offsetHeight || 1;
    const d = layoutDesc(heroSlot());   // the form the hero was drawn in (unitPreviewHTML)
    const maxK = d.form === "pedal" || d.form === "wah" ? 1.9 : d.form === "midi" ? 1.15 : 1.12;
    const avail = Math.max(240, st.clientWidth - 40);
    const k = Math.min(maxK, avail / w, 470 / h);
    u.style.setProperty("--k", k.toFixed(3));
    u.style.transform = `scale(${k.toFixed(3)})`;
    u.style.marginTop = `${(h * k - h).toFixed(0)}px`;
    u.style.marginLeft = u.style.marginRight = `${((w * k - w) / 2).toFixed(0)}px`;
  }
  function heroParam(el) { return S.params.find((p) => p.id === el.dataset.pid) || null; }
  function setHero(el, n) {
    const p = heroParam(el);
    if (!p) return;
    n = snapNorm(p, n);
    if (Math.abs(n - p.norm) < 1e-6) { showTip(el, p.name, displayVal(p)); return; }
    p.norm = n;
    p.val = p.min + n * (p.max - p.min);
    if (typeof p.on === "boolean") p.on = n > 0.5;   // a switch moved off the midpoint: every unit reads it alike
    p.fmt = localFmt(p);
    root.querySelectorAll(`#cd-hero-unit .ctl[data-pid="${CSS.escape(p.id)}"]`).forEach((e) => paintEl(e, p));
    showTip(el, p.name, displayVal(p));
    S.norms[p.id] = n;
    if (S.audOn && S.slot >= 0) {   // coalesced like the rack knobs: one request in flight per control, latest wins
      const url = `/api/param?slot=${S.slot}&id=${enc(p.id)}&norm=${n.toFixed(4)}`;
      if (typeof queueSend === "function") queueSend(`p:${S.slot}:${p.id}`, url);
      else fetch(API_BASE + url).catch(() => {});
    }
    refresh();
  }
  let hdrag = null;
  function onHeroDown(e) {
    const el = e.target.closest("#cd-hero-unit .ctl");
    if (!el || e.button !== 0) return;
    e.stopPropagation();
    const p = heroParam(el);
    if (!p) return;
    const k = el.dataset.kind;
    if (k === "tog") { const st = +el.dataset.steps || 2; let i = (paramKind(p) === "switch" ? (switchOn(p) ? 1 : 0) : Math.round(p.norm * (st - 1))) + 1; if (i > st - 1) i = 0; setHero(el, i / (st - 1)); hideTipSoon(700); return; }
    if (k === "btnrow") { const b = e.target.closest("button[data-i]"); if (b) { setHero(el, +b.dataset.i / Math.max(1, +el.dataset.steps - 1)); hideTipSoon(700); } return; }
    if (k === "sel" || k === "sound" || k === "capture") { const b = e.target.closest("button[data-d]"); if (b) { setHero(el, p.norm + (+b.dataset.d) / Math.max(1, stepCount(p) - 1)); hideTipSoon(700); } return; }
    e.preventDefault();
    hdrag = { el, y: e.clientY, x: e.clientX, acc: p.norm };
    document.body.classList.add("dragging");
    showTip(el, p.name, displayVal(p));
  }
  function onHeroMove(e) {
    if (!hdrag) return;
    let d = (hdrag.y - e.clientY + (e.clientX - hdrag.x) * 0.25) / 190;
    hdrag.y = e.clientY; hdrag.x = e.clientX;
    if (e.shiftKey || e.altKey) d *= 0.15;
    if (!d) return;
    hdrag.acc = Math.max(0, Math.min(1, hdrag.acc + d));
    setHero(hdrag.el, hdrag.acc);
  }
  function onHeroUp() { if (!hdrag) return; hdrag = null; document.body.classList.remove("dragging"); hideTipSoon(650); }
  function onHeroWheel(e) {
    const el = e.target.closest("#cd-hero-unit .ctl");
    if (!el) return;
    const p = heroParam(el);
    if (!p) return;
    e.preventDefault();
    const raw = -e.deltaY || e.deltaX;
    if (paramKind(p) !== "cont") {
      el._acc = (el._acc || 0) + raw;
      if (Math.abs(el._acc) < 30) return;
      const steps = paramKind(p) === "switch" ? 2 : stepCount(p), dir = Math.sign(el._acc);
      el._acc = 0;
      setHero(el, p.norm + dir / Math.max(1, steps - 1));
    } else setHero(el, p.norm + Math.max(-0.05, Math.min(0.05, raw * (e.shiftKey ? 0.0003 : 0.0016))));
    hideTipSoon(800);
  }

  /* ---------- title, stats, about ---------- */
  function renderHead() {
    const box = $("cd-head");
    if (!box) return;
    const c = S.comp, m = S.model || {}, kind = m.impl || "";
    const about = ABOUT[S.tid] || [];
    const eyebrow = [KIND_LABEL[kind] || (c.category || "Unit")];
    if (m.kind === "amp") { const ch = activeChannel(m); if (ch) eyebrow.push(`${ch.n} preamp stages`); }
    if (m.kind === "drive" && m.topology_label) eyebrow.push(m.topology === m.designed_topology ? "as designed" : "re-wired");
    const stats = statTiles(m);
    box.innerHTML = `<div>
        <div class="cd-eyebrow">${eyebrow.map((x, i) => `${i ? `<span class="dot"></span>` : ""}<span class="${i ? "mute" : ""}">${esc(x)}</span>`).join("")}</div>
        <h1 class="cd-h1">${esc(S.uc ? S.uc.name : (c.ref || c.name))}${S.uc || (m.custom && !isEmpty(S.overrides)) ? `<span class="cust">${S.uc ? `Custom ${esc(c.ref)}` : "Modified"}</span>` : ""}</h1>
        <div class="cd-sub">${esc(c.name || "")}</div>
        ${about[0] ? `<div class="cd-after"><span class="k">${m.backend === "hybrid-circuit" ? "Voicing reference" : "Modelled after"}</span><span class="v">${esc(about[0])}</span></div>` : ""}
      </div>
      <div class="cd-stats">${stats.map(([v, k]) => `<div class="cd-stat"><div class="v">${v}</div><div class="k">${esc(k)}</div></div>`).join("")}</div>`;
  }
  function activeChannel(m) { return (m.channels || []).find((x) => x.index === m.active_channel) || (m.channels || [])[0]; }
  function statTiles(m) {
    const t = [];
    const os = m.oversampling || 1, sr = m.sample_rate || 48000;
    if (m.backend === "hybrid-circuit") {
      return [[`${r1(m.circuit_sample_rate / 1000)} kHz`, "electrical section"],
              ["hybrid", "model architecture"], ["software", "reference verification"]];
    }
    if (m.kind === "amp") {
      const ch = activeChannel(m);
      if (ch) t.push([ch.n, "triode stages"]);
      t.push([m.power && m.power.nfb_beta ? `β ${m.power.nfb_beta.toFixed(2)}` : "open", "negative feedback"]);
    } else if (m.kind === "drive" && m.values) {
      t.push([fmtDb(20 * Math.log10(Math.max(1e-6, m.values.gain))).replace(" dB", "<small style='font-size:11px'> dB</small>"), "clipper drive"]);
      t.push([m.values.vd.toFixed(2), "diode knee"]);
    } else if (m.kind === "reverb" && S.curves && S.curves.decay && S.curves.decay.rt60) {
      t.push([`${S.curves.decay.rt60.toFixed(2)} s`, "RT60 (measured)"]);
    } else if (m.kind === "nam" && m.architecture) {
      t.push([esc(m.architecture), "architecture"]);
    }
    if (os > 1) t.push([`${os}×`, "oversampling"]);
    t.push([`${Math.round((sr * os) / 1000)} kHz`, "internal rate"]);
    return t.slice(0, 4);
  }
  function renderAbout() {
    const box = $("cd-about");
    if (!box) return;
    const m = S.model || {};
    const about = ABOUT[S.tid] || [];
    const lead = m.backend === "hybrid-circuit" ? "An editable electrical reference section with upstream amp voicing." : about[1];
    renderEngineControls();
    // the header keeps to the essentials; how it is modelled, its parts and sources live in the Details tab
    const more = S.model ? `<button class="cd-more" data-cd-tab="details">${m.kind === "circuit" ? "Parts list, sources and how it is solved" : "How Headroom models it"}<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"><path d="M6 3.5 10.5 8 6 12.5"/></svg></button>` : "";
    const circ = m.circuit ? `<p>${esc(`Simulated part by part: all ${m.circuit.parts.filter((p) => p.type !== "V").length} components of the schematic, each one on the Circuit tab below.`)}</p>` : "";
    box.innerHTML = `<div class="cd-card txt">${lead ? `<p class="lead">${esc(lead)}</p>` : `<p class="lead">${esc(S.comp.name || "")}</p>`}${circ}${more}</div>
      <div class="cd-card cd-facts">${facts(m).map(([k, v]) => `<div class="row"><span class="k">${esc(k)}</span><span class="v">${esc(v)}</span></div>`).join("")}</div>`;
  }
  function describeModel(m) {
    const out = [];
    if (m.error) { out.push(`This engine build has no model export for this unit (${esc(m.error)}), so only its front panel is shown.`); return out; }
    if (m.backend === "hybrid-circuit") {
      out.push(esc(m.description || "A coupled electrical reference section follows the upstream amp voicing stages."));
      out.push(`Component edits change the values used by the electrical solver. The connectivity is fixed; supported topology changes are separate from scalar edits. ${esc(m.verification_scope || "Software circuit-reference checks; no hardware verification.")}`);
      out.push(`The electrical section runs at ${r1(m.circuit_sample_rate / 1000)} kHz. The upstream section runs at ${r1(m.internal_rate / 1000)} kHz. ${esc(m.output_reference || "")}`);
      if (S.overrides.circuit) out.push("Electrical component edits select this engine. Reset those edits to return to the voicing model.");
      return out;
    }
    if (m.kind === "amp") {
      const ch = activeChannel(m);
      if (ch) {
        const parts = ch.stages.map((s) => {
          const g = s.spec.gain;
          if (s.role === "triode") return `<b>${s.label}</b> is a ${s.rk < 2000 ? "hot" : "warm"} triode on the dynamic-cathode Koren 12AX7 model (Rk ${fmtR(s.rk)} ∥ ${fmtC(s.ck)}, ${r1(g)}× grid drive)`;
          if (s.role === "cold_clipper") return `<b>${s.label}</b> is a cold clipper — 10 kΩ unbypassed cathode, strongly asymmetric, ${r1(g)}× drive`;
          if (s.role === "solid_state") return `<b>${s.label}</b> is a solid-state gain stage`;
          return `<b>${s.label}</b> is a cathode follower that clips on the positive swing`;
        });
        const chName = (m.channels || []).length > 1 ? ` on the <b>${esc(ch.name)}</b> channel` : "";
        out.push(`Headroom builds this amp${chName} from ${ch.n} stages solved from the Koren triode equations (B+ 250 V, 100 kΩ plate load, 68 kΩ grid source): ${parts.join("; ")}. ` +
          `Every bypassed stage integrates its own cathode capacitor each sample, so bass tightening, bias shift under hard playing and recovery come out of the circuit itself, with grid-current blocking on top.`);
        const ts = m.tone_stack || {};
        out.push(`The gain pot sits ${ch.pot_after ? `after ${ch.stages[ch.pot_after - 1].label}` : "at the input"}${ch.bright_db ? ` with a bright cap worth up to ${fmtDb(ch.bright_db)}` : ""}. ` +
          `The tone stack is the passive FMV network solved in closed form (Yeh & Smith) with <b>${esc(ts.preset || "custom")}</b> values — ${fmtC(ts.values.C1)} / ${fmtC(ts.values.C2)} / ${fmtC(ts.values.C3)}, ${fmtR(ts.values.R1)} slope resistor — ` +
          `placed ${ch.stack_after > ch.n ? "after the last stage" : `after ${ch.stages[ch.stack_after - 1].label}`}.`);
      }
      const p = m.power || {}, sp = p.spec || {};
      out.push(`The power section is a long-tailed-pair phase inverter into a ${sp.class_a ? "push-pull pair with a class-A character blend" : "push-pull pentode pair"} with supply sag that also dips the preamp rail. ` +
        (p.nfb_beta ? `A global negative-feedback loop (β ${p.nfb_beta.toFixed(2)}) wraps PI, output stage and transformer, and presence and depth act inside that loop — so they interact with clipping like the real circuit. `
          : `It runs open loop — no negative feedback — so presence and depth are plain shelves ahead of the inverter. `) +
        `The output transformer band-limits to ${fmtHz(sp.ot_hp_hz)} Hz – ${fmtHz(sp.ot_lp_hz)}Hz and the amp drives a reactive speaker load resonating at ${Math.round(sp.spk_fs)} Hz, with cone-excursion limiting and voice-coil power compression.`);
      out.push(`Everything runs at ${m.oversampling}× oversampling (${Math.round(m.internal_rate / 1000)} kHz). Output trims are calibrated so every amp — including your custom ones — swaps in at a comparable loudness. ` +
        `The power tubes are not a separate tube-specific model: they are one push-pull clipper whose drive, headroom, imbalance and sag come from the voicing.`);
    } else if (m.kind === "drive") {
      const v = m.values || {};
      out.push(`Headroom models it as: <b>${esc(m.topology_label)}</b>${m.topology !== m.designed_topology ? " (you re-wired it; the knobs keep their original mapping)" : ""}. ` +
        `At the current settings the clipping path runs at ${fmtDb(20 * Math.log10(Math.max(1e-6, v.gain)))} with a ${v.vd.toFixed(2)} diode knee` + (v.clip_shape >= 0 ? `, forced to ${v.clip_shape ? "hard" : "soft"} diodes.` : "."));
      const tone = (m.blocks || []).find((b) => b.id === "postlp");
      if (tone) out.push(`After the clipper, the ${esc(tone.label.toLowerCase())} is a 2-pole low-pass at ${esc(tone.sub)}${(m.blocks || []).some((b) => b.id === "eq") ? ", followed by a three-band EQ" : ""}.`);
      out.push(`The whole circuit runs at ${m.oversampling}× oversampling (${Math.round(m.internal_rate / 1000)} kHz); knee values are normalised (1.0 ≈ 1 V at the guitar jack).`);
    } else if (m.kind === "nam") {
      out.push(m.loaded ? `Plays the Neural Amp Modeler capture <b>${esc(m.capture)}</b>${m.architecture ? ` — a ${esc(m.architecture)} network with ${m.weights.toLocaleString()} weights` : ""}${m.gear ? `, captured from ${esc(m.gear)}` : ""}.`
        : "Plays Neural Amp Modeler (.nam) captures of real amps, pedals and full rigs. Import captures to the NAM Captures folder.");
      out.push(`Headroom wraps the network with an input gain and gate, ${m.resampled ? `a cubic-Hermite rate bridge (${m.host_rate} → ${m.model_rate} Hz), ` : ""}loudness normalisation from the capture's metadata${m.normalize ? ` (${m.loudness_db.toFixed(1)} dB → -18)` : ""} and a three-band post EQ.`);
    } else if (m.summary) {
      out.push(esc(m.summary));
      out.push("The schematic below is derived from the engine's implementation of this unit; its values are computed with the same formulas the engine runs for the current knob settings.");
    }
    return out;
  }
  const fmtR = (r) => (r >= 1e6 ? `${(r / 1e6).toFixed(1)} MΩ` : r >= 1e3 ? `${(r / 1e3).toFixed(r >= 1e5 ? 0 : 1)} kΩ` : `${Math.round(r)} Ω`);
  const fmtC = (c) => (c >= 1e-6 ? `${(c * 1e6).toFixed(c >= 1e-5 ? 0 : 1)} µF` : c >= 1e-9 ? `${(c * 1e9).toFixed(c >= 1e-8 ? 0 : 1)} nF` : `${Math.round(c * 1e12)} pF`);
  // What a player wants to know at a glance, in plain words (no internal ids or class names).
  function facts(m) {
    const f = [];
    f.push(["Type", KIND_LABEL[m.impl] || S.comp.category || "Unit"]);
    if (m.error) { f.push(["Model", "Front panel only in this build"]); return f; }
    const how = { "circuit-engine": "Full circuit simulation, part by part", "hybrid-circuit": "Electrical reference section + voicing", "opaque-neural": "Neural network capture" }[m.backend]
      || (m.kind === "amp" ? "Stage-by-stage tube model" : m.kind === "cab" || m.kind === "reverb" ? "Measured impulse responses" : "Signal-processing model");
    f.push(["Modelling", how]);
    if (m.circuit) f.push(["Schematic", m.circuit.name]);
    if (m.backend === "hybrid-circuit") {
      const values = (m.circuit || {}).components || {};
      f.push(["Triodes", "Coupled Koren device equations"]);
      f.push(["Power section", "Reduced push-pull · reactive load"]);
      if (Number.isFinite(values["control.feedback"])) f.push(["Feedback control", String(values["control.feedback"])]);
      f.push(["Processing", `${r1(m.circuit_sample_rate / 1000)} kHz electrical · ${r1(m.internal_rate / 1000)} kHz voicing`]);
    } else if (m.kind === "amp") {
      const ts = m.tone_stack || {}, p = m.power || {};
      f.push(["Preamp tubes", "12AX7, solved on their load lines"]);
      f.push(["Tone stack", `${ts.preset || "custom"} values`]);
      f.push(["Power section", p.spec && p.spec.class_a ? "push-pull, class-A blend" : "push-pull"]);
      f.push(["Negative feedback", p.nfb_beta ? `yes (presence and depth in the loop)` : "none"]);
      if ((m.channels || []).length > 1) f.push(["Channels", m.channels.map((c) => c.name).join(" · ")]);
      if (m.builtin && m.builtin.spring) f.push(["Built in", "spring reverb (measured tank)"]);
    } else if (m.kind === "drive" && m.topologies) {
      f.push(["Clipping", (m.topologies || []).find((t) => t.v === m.topology)?.label || m.topology]);
    } else if (m.kind === "cab" && m.cabinet) f.push(["Cabinet", m.cabinet]);
    else if (m.kind === "reverb" && m.space) f.push(["Space", m.space]);
    else if (m.kind === "nam") { f.push(["Capture", m.capture || "none loaded"]); if (m.gear_type) f.push(["Gear type", m.gear_type]); }
    if (m.oversampling && m.backend !== "hybrid-circuit") {
      const rate = Math.round(((m.sample_rate || 48000) * m.oversampling) / 1000);
      f.push(["Processing", m.oversampling > 1 ? `${m.oversampling}× oversampled · runs at ${rate} kHz` : `${rate} kHz`]);
    }
    const cpu = Number.isFinite(m.cpu_pct) ? m.cpu_pct : Number.isFinite(m.cpu) ? m.cpu : null;
    if (cpu != null) f.push(["CPU", `${cpu < 1 ? cpu.toFixed(2) : cpu.toFixed(1)} %`]);
    if (m.verification === "spice-validated") f.push(["Accuracy", "Checked against SPICE"]);
    else if (m.verification === "hardware") f.push(["Accuracy", "Measured against the hardware"]);
    f.push(["Customizable", m.editable ? "Yes, with Customize" : "Not yet"]);
    return f;
  }

  /* =====================================================================
     schematic (generated from model.blocks)
     ===================================================================== */
  const NW = 126, NH = 106, GX = 30, GY = 74, PAD = 26, TOP = 30;
  function sym(b, m) {
    const t = b.type, heat = (g) => Math.max(0.35, Math.min(1, 0.35 + Math.log10(Math.max(1, g)) / 1.3));
    const tube = (x, y, r, g, cold) => `<circle class="glass" cx="${x}" cy="${y}" r="${r}"/><circle class="heat${cold ? " cold" : ""}" cx="${x}" cy="${y + r * 0.35}" r="${r * 0.82}" style="--h:${heat(g).toFixed(2)};animation-delay:-${(x * 0.37 + y) % 3}s"/>
      <path class="s" d="M${x - r * 0.5} ${y - r * 0.5} H${x + r * 0.5} M${x} ${y - r * 0.5} V${y - r} M${x - r * 0.6} ${y} H${x + r * 0.6}" /><path class="s" d="M${x - r * 0.6} ${y} H${x + r * 0.6}" stroke-dasharray="2 2.2"/>
      <path class="s" d="M${x - r * 0.45} ${y + r * 0.55} Q${x} ${y + r * 0.3} ${x + r * 0.45} ${y + r * 0.55}"/><path class="s" d="M${x - r} ${y} H${x - r * 0.6}"/>`;
    switch (t) {
      case "jack": return `<circle class="s" cx="-6" cy="0" r="10"/><circle class="f" cx="-6" cy="0" r="3.4"/><path class="s" d="M4 -4 H22 M4 4 H22"/>`;
      case "out": return `<path class="s" d="M-22 -4 H-4 M-22 4 H-4"/><circle class="s" cx="6" cy="0" r="10"/><circle class="af" cx="6" cy="0" r="3.4"/>`;
      case "triode": return tube(0, 0, 17, b.gain || 2, b.role === "cold_clipper");
      case "follower": return tube(0, 0, 15, 1.4, false) + `<path class="s a" d="M6 9 L18 16"/>`;
      case "transistor": return `<circle class="glass" r="16"/><path class="s" d="M-16 0 H-5 M-5 -9 V9 M-5 -3 L8 -13 M-5 3 L8 13"/><path class="f" d="M8 13 L2.6 12.2 L5.8 8.4 Z"/>`;
      case "opamp": return `<path class="glass" d="M-15 -17 L18 0 L-15 17 Z"/><path class="s" d="M-11 -7 h6 M-8 -10 v6 M-11 7 h6"/><path class="s" d="M-22 -7 H-15 M-22 7 H-15 M18 0 H24"/>`;
      case "diodes": return `<path class="s" d="M-20 -8 H20 M-20 8 H20 M-20 -8 V8 M20 -8 V8"/><path class="glass" d="M-6 -14 L6 -8 L-6 -2 Z"/><path class="s a" d="M6 -14 V-2"/><path class="glass" d="M6 2 L-6 8 L6 14 Z"/><path class="s a" d="M-6 2 V14"/>`;
      case "pot": return `<path class="s" d="M-24 6 H-16 l3 -6 l4 12 l4 -12 l4 12 l4 -12 l4 12 l4 -12 l3 6 H24"/><path class="s a" d="M0 -16 V-3"/><path class="af" d="M0 -1 L-3.6 -7 H3.6 Z"/>`;
      case "tonestack": {
        const c = (m && m.controls) || {}, kn = (x, v) => { const a = (-135 + 270 * (v ?? 0.5)) * Math.PI / 180; return `<circle class="glass" cx="${x}" cy="-4" r="7.5"/><path class="s a" d="M${x} -4 L${(x + Math.sin(a) * 6).toFixed(1)} ${(-4 - Math.cos(a) * 6).toFixed(1)}"/>`; };
        return kn(-17, c.bass) + kn(0, c.mid) + kn(17, c.treble) + `<path class="s" d="M-24 12 H24" stroke-dasharray="3 3"/>`;
      }
      case "pi": return tube(-11, 2, 10, 1.6, false) + tube(11, 2, 10, 1.6, false);
      case "power": {
        const bottle = (x) => `<rect class="glass" x="${x - 8}" y="-18" width="16" height="34" rx="7"/><ellipse class="heat" cx="${x}" cy="2" rx="7" ry="12" style="--h:0.95;animation-delay:-${x > 0 ? 1.3 : 0.2}s"/><path class="s" d="M${x - 5} -11 H${x + 5} M${x - 5} -2 H${x + 5} M${x - 5} 6 H${x + 5}" stroke-dasharray="2 1.6"/>`;
        return bottle(-11) + bottle(11);
      }
      case "transformer": return `<path class="s" d="M-12 -16 a4 4 0 0 1 0 8 a4 4 0 0 1 0 8 a4 4 0 0 1 0 8 a4 4 0 0 1 0 8 M12 -16 a4 4 0 0 0 0 8 a4 4 0 0 0 0 8 a4 4 0 0 0 0 8 a4 4 0 0 0 0 8"/><path class="s a" d="M-2 -17 V17 M2 -17 V17"/>`;
      case "speaker": return `<path class="glass" d="M-16 -6 H-8 L6 -17 V17 L-8 6 H-16 Z"/><path class="s a" d="M11 -8 Q16 0 11 8 M15 -13 Q23 0 15 13"/>`;
      case "filter_hp": return `<rect class="glass" x="-20" y="-14" width="40" height="28" rx="6"/><path class="s a" d="M-14 8 Q-6 8 -3 0 T6 -6 H14"/>`;
      case "filter_lp": return `<rect class="glass" x="-20" y="-14" width="40" height="28" rx="6"/><path class="s a" d="M-14 -6 H-6 Q3 -6 5 2 T14 8"/>`;
      case "filter_bp": return `<rect class="glass" x="-20" y="-14" width="40" height="28" rx="6"/><path class="s a" d="M-14 8 Q-6 8 -3 -2 Q0 -8 3 -2 Q6 8 14 8"/>`;
      case "eq": return `<rect class="glass" x="-20" y="-14" width="40" height="28" rx="6"/><path class="s a" d="M-14 2 Q-10 -6 -6 2 T2 2 Q6 8 9 0 T14 -2"/>`;
      case "tone": return `<circle class="glass" r="12"/><path class="s a" d="M0 0 L7 -8"/><circle class="f" cx="-15" cy="10" r="1.2"/><circle class="f" cx="15" cy="10" r="1.2"/><circle class="f" cx="0" cy="-16" r="1.2"/>`;
      case "mix": return `<circle class="glass" r="12"/><path class="s a" d="M-6 0 H6 M0 -6 V6"/>`;
      case "gain": case "vca": return `<path class="glass" d="M-13 -13 L15 0 L-13 13 Z"/>${t === "vca" ? `<path class="s a" d="M1 -16 V-6"/>` : ""}`;
      case "convolver": return `<path class="s" d="M-20 12 H22"/>${[0, 5, 9, 13, 16, 19, 22, 25, 28, 31, 34, 37].map((x, i) => `<path class="s${i ? "" : " a"}" d="M${x - 18} 12 V${(12 - 26 * Math.exp(-i / 3.2)).toFixed(1)}"/>`).join("")}`;
      case "mic": return `<rect class="glass" x="-7" y="-18" width="14" height="22" rx="7"/><path class="s" d="M-11 -2 Q-11 10 0 10 Q11 10 11 -2 M0 10 V17 M-6 17 H6"/>`;
      case "delay": return `<circle class="glass" cx="-11" cy="-2" r="9"/><circle class="glass" cx="11" cy="-2" r="9"/><circle class="f" cx="-11" cy="-2" r="2"/><circle class="f" cx="11" cy="-2" r="2"/><path class="s a" d="M-11 7 H11"/>`;
      case "fdn": return [-12, -4, 4, 12].map((y) => `<path class="s${y === -12 ? " a" : ""}" d="M-20 ${y} H12 a4 4 0 0 0 0 -0.1"/><circle class="f" cx="16" cy="${y}" r="1.6"/>`).join("");
      case "matrix": return [-10, 0, 10].map((y) => [-10, 0, 10].map((x) => `<circle class="${(x + y) % 20 === 0 ? "af" : "f"}" cx="${x}" cy="${y}" r="2.4"/>`).join("")).join("");
      case "lfo": return `<path class="s a" d="M-22 0 C-16 -16 -8 -16 -2 0 S12 16 18 0"/><path class="s" d="M-22 0 H22" stroke-dasharray="2 3"/>`;
      case "detector": return `<path class="s" d="M-20 10 L-14 -12 L-8 6 L-2 -8 L4 4 L10 -4 L16 2"/><path class="s a" d="M-20 -12 Q-6 -14 4 -6 T20 0"/>`;
      case "midi": return `<circle class="glass" r="15"/>${[150, 180, 210, 120, 240].map((a) => `<circle class="f" cx="${(Math.sin(a * Math.PI / 180) * 9).toFixed(1)}" cy="${(-Math.cos(a * Math.PI / 180) * 9).toFixed(1)}" r="1.8"/>`).join("")}<rect class="f" x="-2" y="-14" width="4" height="3" rx="1"/>`;
      case "synth": return `<rect class="glass" x="-22" y="-12" width="44" height="24" rx="4"/>${[-15, -7, 1, 9, 17].map((x) => `<path class="s" d="M${x} -12 V12"/>`).join("")}${[-11, -3, 13].map((x) => `<rect class="f" x="${x - 2}" y="-12" width="4" height="13"/>`).join("")}`;
      case "nam": return [[-15, -8], [-15, 8], [0, -13], [0, 0], [0, 13], [15, -6], [15, 6]].map(([x, y]) => `<circle class="${x === 0 && y === 0 ? "af" : "f"}" cx="${x}" cy="${y}" r="2.6"/>`).join("") +
        `<path class="s" d="M-15 -8 L0 -13 M-15 -8 L0 0 M-15 -8 L0 13 M-15 8 L0 -13 M-15 8 L0 0 M-15 8 L0 13 M0 -13 L15 -6 M0 0 L15 -6 M0 0 L15 6 M0 13 L15 6" stroke-opacity="0.45"/>`;
      case "resampler": return `<path class="s a" d="M-18 10 V4 H-10 V-2 H-2 V-8 H6 V-14 H14"/>`;
      case "spring": return `<path class="s a" d="M-22 0 ${Array.from({ length: 8 }, (_, i) => `q2.5 ${i % 2 ? 9 : -9} 5.5 0`).join(" ")}"/>`;
      case "clip": return `<rect class="glass" x="-20" y="-14" width="40" height="28" rx="6"/><path class="s a" d="M-14 8 H-8 C-2 8 2 -8 8 -8 H14"/>`;
      case "plugin": return `<rect class="glass" x="-14" y="-12" width="28" height="24" rx="5"/><path class="s a" d="M-5 -12 V-18 M5 -12 V-18 M0 12 V18"/>`;
      default: return `<rect class="glass" x="-18" y="-13" width="36" height="26" rx="6"/>`;
    }
  }
  function overridePaths() { return new Set(leaves(S.overrides)); }
  function blockModified(b, ov) { return (b.values || []).some((v) => v.edit && [...ov].some((p) => p === v.edit.path || p.startsWith(v.edit.path + "."))); }
  // Circuit-engine units get the netlist view (assets/circuit-view.js, loaded on first use).
  function circuitView(sec, m) {
    if (window.CircuitView) {
      window.CircuitView.render(sec, m, {
        edit: S.edit, getPath: (p) => getPath(S.overrides, p), overridePaths, editEpoch: S.editEpoch, onTopology: applyTopology,
        onEdit: (path, value) => editValue(path, value), onReset: (path) => { delPath(S.overrides, path); changed(); },
        // turning a pot on the schematic turns the unit's knob (same path as dragging the knob itself)
        setControl: (pid, n) => {
          const el = root.querySelector(`#cd-hero-unit .ctl[data-pid="${CSS.escape(pid)}"]`);
          if (el) { setHero(el, n); return; }
          const p = S.params.find((q) => q.id === pid);
          if (!p) return;
          p.norm = n; p.val = p.min + n * (p.max - p.min); S.norms[pid] = n;
          if (S.audOn && S.slot >= 0) fetch(API_BASE + `/api/param?slot=${S.slot}&id=${enc(pid)}&norm=${n.toFixed(4)}`).catch(() => {});
          refresh();
        },
      });
      if (S.curves && S.curves.static) window.CircuitView.updateCurves(S.curves.static);
      return;
    }
    if (!circuitView.loading) {
      circuitView.loading = true;
      const s = document.createElement("script");
      s.src = "assets/circuit-view.js";
      s.onload = () => { if (S.open) { renderSchematic(); renderDetails(); } };
      document.head.appendChild(s);
    }
    sec.innerHTML = `<div class="cd-card txt cd-loading" style="padding:30px">Drawing the circuit…</div>`;
  }
  function renderSchematic() {
    const sec = $("cd-schem-sec");
    if (!sec) return;
    const m = S.model;
    if (!m) return;
    if (m.kind === "circuit" && m.circuit) { circuitView(sec, m); return; }
    let blocks = m.blocks || [];
    if (!blocks.length) blocks = [{ id: "in", type: "jack", label: "Input", sub: "", values: [], section: "Input" }, { id: "u", type: S.comp.category === "Plugins" ? "plugin" : "gain", label: S.comp.ref || "Unit", sub: "", values: [], section: "Unit" }, { id: "out", type: "out", label: "Output", sub: "", values: [], section: "Output" }];
    const chans = m.kind === "amp" && (m.channels || []).length > 1 ? `<span class="cd-chan" title="Select the channel with the amp's own switches">${m.channels.map((c) => `<span class="${c.index === m.active_channel ? "on" : ""}">${esc(c.name)}</span>`).join("")}</span>` : "";
    sec.innerHTML = `<div class="cd-sec-h"><h2>Signal path</h2><span class="s">exactly as the engine models it · hover any part, click to inspect${S.edit ? " and edit" : ""}</span><span class="sp"></span>${chans}</div>
      <div class="cd-card"><div class="cd-schem" id="cd-schem"></div></div><div class="cd-card cd-part" id="cd-part"></div>`;
    const box = $("cd-schem");
    const width = Math.max(520, box.clientWidth - 12);
    const n = blocks.length;
    let per = Math.max(3, Math.min(10, Math.floor((width - 2 * PAD + GX) / (NW + GX))));
    if (n <= per) per = n;
    else per = Math.ceil(n / Math.ceil(n / per)); // balance the rows
    const rows = Math.ceil(n / per);
    const rowW = per * NW + (per - 1) * GX;
    const ox = Math.max(PAD, (width - rowW) / 2);
    const P = blocks.map((b, i) => {
      const r = Math.floor(i / per), c = i % per, vc = r % 2 ? per - 1 - c : c;
      const x = ox + vc * (NW + GX), y = TOP + r * (NH + GY);
      return { x, y, cx: x + NW / 2, cy: y + NH / 2 - 14, r };
    });
    const H = TOP + rows * (NH + GY) - GY + ((m.loops || []).length ? 54 : 16);
    // wire through the symbol centres
    const first = P[0], last = P[n - 1];
    const lastDir = last.r % 2 ? -1 : 1;
    let d = `M${(first.cx - NW / 2 - 22).toFixed(1)} ${first.cy}`;
    P.forEach((p, i) => {
      if (i && P[i - 1].r !== p.r) d += ` L${P[i - 1].cx} ${P[i - 1].cy + 34} L${p.cx} ${p.cy - 34}`;
      d += ` L${p.cx} ${p.cy}`;
    });
    d += ` L${(last.cx + lastDir * (NW / 2 + 22)).toFixed(1)} ${last.cy}`;
    const ov = overridePaths();
    let secs = "", lastSec = null;
    blocks.forEach((b, i) => {
      if (b.section && b.section !== lastSec) {
        const p = P[i];
        secs += `<text class="sec-lbl" x="${p.x + 2}" y="${p.y - 12}">${esc(b.section)}</text>`;
      }
      lastSec = b.section;
    });
    const nodes = blocks.map((b, i) => {
      const p = P[i], sel = S.sel === b.id, mod = blockModified(b, ov);
      const badge = b.type === "triode" && b.gain ? `<rect class="badge-bg" x="9" y="9" rx="5" width="${String(r1(b.gain)).length * 5.6 + 12}" height="13"/><text class="badge" x="15" y="18.6">${r1(b.gain)}×</text>` : "";
      return `<g class="cd-node${sel ? " sel" : ""}" data-b="${i}" transform="translate(${p.x.toFixed(1)} ${p.y.toFixed(1)})" tabindex="0" role="button" aria-label="${esc(b.label)}">
        <rect class="bx-ring" x="-3" y="-3" width="${NW + 6}" height="${NH + 6}" rx="16"/>
        <rect class="bx" width="${NW}" height="${NH}" rx="14"/><rect class="bx-hl" x="1" y="1" width="${NW - 2}" height="${NH / 2}" rx="13"/>
        <g class="sym" transform="translate(${NW / 2} ${NH / 2 - 14})">${sym(b, m)}</g>
        <text class="lbl" x="${NW / 2}" y="${NH - 27}" text-anchor="middle">${esc(trunc(b.label, 17))}</text>
        <text class="sub" x="${NW / 2}" y="${NH - 12}" text-anchor="middle">${esc(trunc(b.sub || "", 22))}</text>
        ${badge}${mod ? `<circle class="mod" cx="${NW - 12}" cy="12" r="4"/>` : ""}</g>`;
    }).join("");
    let loops = "";
    (m.loops || []).forEach((L) => {
      const a = blocks.findIndex((b) => b.id === L.from), z = blocks.findIndex((b) => b.id === L.to);
      if (a < 0 || z < 0) return;
      const A = P[a], Z = P[z], yb = Math.max(A.y, Z.y) + NH;
      const dip = yb + 30;
      loops += `<path class="loop" d="M${A.cx} ${A.y + NH} C${A.cx} ${dip}, ${Z.cx} ${dip}, ${Z.cx} ${Z.y + NH}"/><path class="f" d="M${Z.cx} ${Z.y + NH + 1} l-4 8 h8 Z" style="fill:color-mix(in srgb, var(--accent) 70%, var(--text-3))"/>
        <text class="loop-lbl" x="${(A.cx + Z.cx) / 2}" y="${dip + 8}" text-anchor="middle">${esc(L.label)}</text>`;
    });
    box.innerHTML = `<svg viewBox="0 0 ${width} ${H}" role="img" aria-label="Signal path">
      <defs>
        <radialGradient id="cd-heat"><stop offset="0" stop-color="var(--cd-glow)" stop-opacity="0.95"/><stop offset="0.55" stop-color="var(--cd-glow)" stop-opacity="0.35"/><stop offset="1" stop-color="var(--cd-glow)" stop-opacity="0"/></radialGradient>
        <radialGradient id="cd-heat-cold"><stop offset="0" stop-color="var(--cd-cold)" stop-opacity="0.8"/><stop offset="0.6" stop-color="var(--cd-cold)" stop-opacity="0.25"/><stop offset="1" stop-color="var(--cd-cold)" stop-opacity="0"/></radialGradient>
        <linearGradient id="cd-node-hl" x1="0" x2="0" y1="0" y2="1"><stop offset="0" stop-color="#fff" stop-opacity="0.045"/><stop offset="1" stop-color="#fff" stop-opacity="0"/></linearGradient>
      </defs>
      ${secs}<path class="wire" d="${d}"/><path class="flow" d="${d}"/>${loops}${nodes}</svg><div class="cd-tip" id="cd-tip"></div>`;
    renderPart();
  }
  const trunc = (s, n) => (String(s).length > n ? String(s).slice(0, n - 1) + "…" : String(s));
  function showNodeTip(g) {
    const b = (S.model.blocks || [])[+g.dataset.b];
    const tip = $("cd-tip");
    if (!b || !tip) return;
    const vals = (b.values || []).slice(0, 7);
    tip.innerHTML = `<h4>${esc(b.label)}<span>${esc(b.section || "")}</span></h4>${vals.map((v) => `<div class="r"><span class="k">${esc(v.k)}</span><span class="v">${esc(v.v)}</span></div>`).join("")}${(b.values || []).length > 7 ? `<div class="more">+${b.values.length - 7} more — click to inspect</div>` : vals.length ? "" : `<div class="more">No adjustable model values.</div>`}`;
    const box = $("cd-schem").getBoundingClientRect(), r = g.getBoundingClientRect();
    let x = r.left - box.left + r.width / 2 - 140, y = r.bottom - box.top + 8;
    x = Math.max(6, Math.min(box.width - 296, x));
    if (y + 200 > box.height) y = Math.max(4, r.top - box.top - tip.offsetHeight - 8);
    tip.style.left = `${x}px`;
    tip.style.top = `${y}px`;
    tip.classList.add("show");
  }

  /* ---------- part inspector (and editor) ---------- */
  function currentFor(v, m) {
    const e = v.edit, ovv = getPath(S.overrides, e.path);
    if (ovv !== undefined) return ovv;
    if (/\.preset$/.test(e.path)) return (m.tone_stack || {}).preset;
    if (/\.clip$/.test(e.path)) return "design";
    if (/\.type$/.test(e.path)) return v.raw ? "follower" : "triode";
    if (/\.off$/.test(e.path)) return false;
    if (/class_a$/.test(e.path)) return !!v.raw;
    if (/^drive\.kind$/.test(e.path)) return m.topology;
    return v.raw;
  }
  function unitFor(path) {
    const value = S.model ? findVal(path) : null;
    const unit = value?.edit?.unit;
    const magnitude = Math.abs(Number(value?.raw));
    if (unit === "F") return magnitude < 1e-9 ? { s: 1e-12, u: "pF" } : magnitude < 1e-6 ? { s: 1e-9, u: "nF" } : { s: 1e-6, u: "µF" };
    if (unit === "ohm" || unit === "Ω") return magnitude >= 1e6 ? { s: 1e6, u: "MΩ" } : magnitude >= 1e3 ? { s: 1e3, u: "kΩ" } : { s: 1, u: "Ω" };
    if (unit === "H") return magnitude < 1 ? { s: 1e-3, u: "mH" } : { s: 1, u: "H" };
    if (unit === "A") return magnitude < 1 ? { s: 1e-3, u: "mA" } : { s: 1, u: "A" };
    if (unit) return { s: 1, u: unit === "ratio" ? "×" : unit };
    if (/\.C1$/.test(path)) return { s: 1e-12, u: "pF" };
    if (/\.C[23]$/.test(path)) return { s: 1e-9, u: "nF" };
    if (/\.R\d$/.test(path)) return { s: 1e3, u: "kΩ" };
    if (/_hz$|input_hp|bright_hz/.test(path)) return { s: 1, u: "Hz" };
    if (/_db$/.test(path)) return { s: 1, u: "dB" };
    if (/\.gain$/.test(path)) return { s: 1, u: "×" };
    return { s: 1, u: "" };
  }
  const sliderPos = (e, val) => (e.scale === "log" && e.min > 0 ? Math.log(val / e.min) / Math.log(e.max / e.min) : (val - e.min) / (e.max - e.min));
  const sliderVal = (e, t) => {
    let v = e.scale === "log" && e.min > 0 ? e.min * Math.pow(e.max / e.min, t) : e.min + t * (e.max - e.min);
    if (e.step >= 1) v = Math.round(v / e.step) * e.step;
    return v;
  };
  const numTxt = (v, u) => { const x = v / u.s; return Number.isFinite(x) ? String(Number(x.toPrecision(8))) : ""; };
  function valHTML(v, m) {
    const e = v.edit, ov = e ? getPath(S.overrides, e.path) !== undefined : false;
    const u = e?.unit ? unitFor(e.path) : null;
    const shown = u && Number.isFinite(v.raw) ? `${numTxt(v.raw, u)} ${u.u}`.trim() : v.v;
    let ed = "";
    if (S.edit && e && e.editable !== false) {
      const cur = currentFor(v, m);
      if (e.options) {
        ed = `<div class="ed"><select data-path="${esc(e.path)}">${e.options.map((o, i) => `<option value="${i}"${String(o.v) === String(cur) || (typeof o.v === "number" && typeof cur === "number" && Math.abs(o.v - cur) < 1e-4) ? " selected" : ""}>${esc(o.label)}</option>`).join("")}</select></div>`;
      } else {
        const u = unitFor(e.path), t = Math.max(0, Math.min(1, sliderPos(e, +cur)));
        ed = `<div class="ed"><input type="range" min="0" max="1000" value="${Math.round(t * 1000)}" data-path="${esc(e.path)}" aria-label="${esc(v.k)}"><input class="num" type="text" value="${numTxt(+cur, u)}" data-path="${esc(e.path)}" data-scale="${u.s}" title="${esc(u.u)}"><span style="font-size:10.5px;color:var(--text-3);min-width:24px">${esc(u.u)}</span></div>`;
      }
    }
    return `<div class="cd-val${ov ? " mod" : ""}"><div class="k">${esc(v.k)}${ov && S.edit ? `<button class="rst" data-reset="${esc(e.path)}" title="Back to the factory value">Reset</button>` : ""}</div><div class="v">${esc(shown)}</div>${v.note ? `<div class="n">${esc(v.note)}</div>` : ""}${ed}</div>`;
  }
  function renderPart() {
    const box = $("cd-part");
    if (!box || !S.model) return;
    const blocks = S.model.blocks || [];
    const b = blocks.find((x) => x.id === S.sel);
    if (!b) {
      box.innerHTML = `<div class="cd-empty-part">${S.edit ? "Click a tube, pot, tone stack, diode or the power section to edit its model values." : "Click any part of the signal path to see the values the engine uses for it."}</div>`;
      return;
    }
    let extra = "";
    if (S.edit && b.id === "power" && S.model.power_profiles) {
      const cur = getPath(S.overrides, "amp.power");
      extra = `<div class="cd-val"><div class="k">Power section profile</div><div class="v">${cur ? "custom" : "as voiced"}</div><div class="n">Copies the power-section values the engine uses for a factory amp built around these output tubes.</div>
        <div class="ed"><select data-profile="1"><option value="">As voiced for ${esc(S.comp.ref)}</option>${S.model.power_profiles.map((p, i) => `<option value="${i}">${esc(p.label)}</option>`).join("")}</select></div></div>`;
    }
    const vals = b.values || [];
    box.innerHTML = `<div class="cd-part-h"><span class="sw"></span><span class="t">${esc(b.label)}</span><span class="s">${esc(b.section || "")} · ${esc(b.sub || "")}</span><span class="sp"></span>
      ${S.edit && vals.some((v) => v.edit && getPath(S.overrides, v.edit.path) !== undefined) ? `<button class="cd-btn ghost" data-reset-block="1">Reset part</button>` : ""}</div>
      <div class="cd-vals">${extra}${vals.length ? vals.map((v) => valHTML(v, S.model)).join("") : `<div class="cd-empty-part">This part has no separate model values.</div>`}</div>`;
  }
  function editValue(path, value) {
    const v = findVal(path);
    if (/\.preset$/.test(path)) ["C1", "C2", "C3", "R1", "R2", "R3", "R4"].forEach((k) => delPath(S.overrides, path.replace(/preset$/, k)));
    if (/\.clip$/.test(path) && value === "design") delPath(S.overrides, path);
    else if (/^drive\.kind$/.test(path) && value === S.model.designed_topology) delPath(S.overrides, path);
    else if (v && getPath(S.overrides, path) === undefined && String(currentFor(v, S.model)) === String(value) && !/\.off$/.test(path)) return;
    else if (/\.off$/.test(path) && value === false) delPath(S.overrides, path);
    else setPath(S.overrides, path, value);
    changed();
  }
  function findVal(path) { for (const b of S.model.blocks || []) for (const v of b.values || []) if (v.edit && v.edit.path === path) return v; return null; }

  /* ---------- edit bar ---------- */
  function renderEditBar() {
    const box = $("cd-edit-slot");
    if (!box) return;
    renderBarActions();
    if (!S.edit || !S.model) { box.innerHTML = ""; return; }
    const m = S.model, nch = changeCount();
    const slot = S.slot >= 0 ? getSlot(S.slot) : null;
    const topo = m.kind === "drive" ? `<select data-topology="1" title="Clipping topology">${(m.topologies || []).map((t) => `<option value="${esc(t.v)}"${t.v === m.topology ? " selected" : ""}>${t.v === m.designed_topology ? "● " : ""}${esc(t.label)}</option>`).join("")}</select>` : "";
    const ch = m.kind === "amp" ? activeChannel(m) : null;
    const removed = ch && ch.removed && ch.removed.length ? `<span class="cd-removed">Removed:${ch.removed.map((r) => `<button data-restore="amp.channels.${ch.index}.stages.${r.orig}.off" title="Put ${esc(r.label)} back in the signal path">${esc(r.label)} ↺</button>`).join("")}</span>` : "";
    const aud = slot ? `<span class="cd-aud"><button class="sw${S.audOn ? " on" : ""}" data-act="aud" aria-pressed="${S.audOn}" aria-label="Audition on the chain"></button>Hear it on slot ${posOf(S.slot)}</span>`
      : `<button class="cd-btn ghost" data-act="add-aud" title="Insert this unit into the chain and audition your changes on it">Add to chain to hear it</button>`;
    box.innerHTML = `<div class="cd-edit-bar">
      <span class="t"><span class="pip"></span>${nch ? `${nch} change${nch > 1 ? "s" : ""}` : "No changes yet"}</span>${topo}${removed}<span class="sp"></span>${aud}
      <input type="text" id="cd-name" value="${esc(S.name)}" placeholder="Component name" spellcheck="false" aria-label="Component name">
      ${nch ? `<button class="cd-btn ghost" data-act="reset">Reset</button>` : ""}
      ${S.ucid ? `<button class="cd-btn ghost danger" data-act="delete" title="Delete this component">Delete</button><button class="cd-btn" data-act="saveas">Save as new</button><button class="cd-btn primary" data-act="save">Update</button>`
        : `<button class="cd-btn primary" data-act="save"${nch ? "" : " disabled"}>Save as my component</button>`}</div>`;
  }
  async function save(asNew) {
    const name = ($("cd-name") && $("cd-name").value.trim()) || S.name;
    S.name = name;
    const id = asNew ? "" : (S.ucid || "");
    try {
      const d = await api(`/api/usercomponents/save?tid=${S.tid}&name=${enc(name)}&model=${enc(JSON.stringify(S.overrides))}&norms=${enc(JSON.stringify(S.norms))}${id ? `&id=${enc(id)}` : ""}`);
      if (!d.ok) { toast(d.error || "Couldn’t save the component."); return; }
      S.ucid = d.component.id;
      S.uc = d.component;
      await loadUsers();
      S.uc = S.users.find((u) => u.id === S.ucid) || d.component;
      scheduleApply();
      renderHead();
      renderEditBar();
      const cr = root.querySelector(".cd-crumb b");
      if (cr) cr.textContent = name;
      toast(`Saved “${name}” to My Components`);
    } catch (e) { toast("Couldn’t save the component."); }
  }
  async function del() {
    if (!S.ucid) return;
    try { await api(`/api/usercomponents/delete?id=${enc(S.ucid)}`); } catch (e) { /* ignore */ }
    const name = S.uc ? S.uc.name : "component";
    S.ucid = null; S.uc = null;
    await loadUsers();
    toast(`Deleted “${name}”`);
    renderHead();
    renderEditBar();
  }

  /* ---------- insert into the chain ---------- */
  async function insertWith(tid, target, model, norms) {
    const before = new Set((rackState.slots || []).map((s) => s.slot_idx));
    await insertComponent(tid, target);
    const added = (rackState.slots || []).find((s) => s.slot_idx === ui.selected && s.table_id === tid) || (rackState.slots || []).find((s) => s.table_id === tid && !before.has(s.slot_idx));
    if (!added) return -1;
    const comp = comps.byTid.get(tid);
    const defaults = comp ? Object.fromEntries((comp.params || []).map((p) => [p.id, typeof p.default === "number" ? p.default : p.norm])) : {};
    const sends = Object.entries(norms || {}).filter(([k, n]) => defaults[k] === undefined || Math.abs(defaults[k] - n) > 1e-4)
      .map(([k, n]) => fetch(`${API_BASE}/api/param?slot=${added.slot_idx}&id=${enc(k)}&norm=${(+n).toFixed(4)}`).catch(() => {}));
    await Promise.all(sends);
    if (model) { try { await api(`/api/component/apply?slot=${added.slot_idx}&model=${enc(model)}`); } catch (e) { /* ignore */ } }
    if (sends.length || model) await engineRack("/api/rack");
    return added.slot_idx;
  }
  function insertTarget() {
    const all = rackState.slots || [];
    const i = ui.selected != null ? all.findIndex((s) => s.slot_idx === ui.selected) : -1;
    return i >= 0 ? i + 1 : all.length;
  }
  async function addFromPage(thenAudition) {
    const slot = await insertWith(S.tid, insertTarget(), modelStr(), S.norms);
    if (slot < 0) return;
    S.slot = slot;
    heroLive();
    if (thenAudition) { S.audOn = true; S.slotOrig = { model: "" }; }
    toast(`${S.uc ? S.uc.name : S.comp.ref} added to the chain`);
    renderBarActions();
    renderEditBar();
  }
  async function insertUser(ucid, target) {
    const u = S.users.find((x) => x.id === ucid);
    if (!u) return;
    await insertWith(u.base, target, JSON.stringify(u.model || {}), u.norms || {});
  }

  /* =====================================================================
     analysis plots
     ===================================================================== */
  const CH = new Map();
  function lineChart(id, o) {
    const W = o.w || 520, Hh = o.h || 200, L = 40, R = o.endLabels ? 46 : 12, T = 10, B = 24;
    const xs = o.xs, xr = o.xr || [xs[0], xs[xs.length - 1]], yr = o.yr;
    const X = o.xLog ? (x) => L + (Math.log(x / xr[0]) / Math.log(xr[1] / xr[0])) * (W - L - R) : (x) => L + ((x - xr[0]) / (xr[1] - xr[0])) * (W - L - R);
    const Y = (y) => T + (1 - (Math.max(yr[0], Math.min(yr[1], y)) - yr[0]) / (yr[1] - yr[0])) * (Hh - T - B);
    const grid = (o.yTicks || []).map((y) => `<line class="${y === 0 ? "z" : ""}" x1="${L}" x2="${W - R}" y1="${Y(y).toFixed(1)}" y2="${Y(y).toFixed(1)}"/>`).join("") +
      (o.xTicks || []).map((x) => `<line x1="${X(x).toFixed(1)}" x2="${X(x).toFixed(1)}" y1="${T}" y2="${Hh - B}"/>`).join("");
    const ax = (o.yTicks || []).map((y) => `<text x="${L - 6}" y="${(Y(y) + 3).toFixed(1)}" text-anchor="end">${o.yFmt ? o.yFmt(y) : y}</text>`).join("") +
      (o.xTicks || []).map((x) => `<text x="${X(x).toFixed(1)}" y="${Hh - 8}" text-anchor="middle">${o.xFmt ? o.xFmt(x) : x}</text>`).join("");
    const pts = (ys) => ys.map((y, i) => `${X(xs[i]).toFixed(1)},${Y(y).toFixed(1)}`).join(" ");
    let areas = "", lines = "", labels = "";
    o.series.forEach((s, k) => {
      if (s.area) areas += `<polygon class="ar" points="${X(xs[0]).toFixed(1)},${Y(yr[0])} ${pts(s.ys)} ${X(xs[xs.length - 1]).toFixed(1)},${Y(yr[0])}" fill="url(#g-${id}-${k})"/>`;
      lines += `<polyline class="ln${s.dash ? " dash" : ""}" data-s="${k}" points="${pts(s.ys)}" stroke="${s.color}"/>`;
    });
    if (o.endLabels) {
      // Direct labels at the line ends, nudged apart so they never collide.
      const ls = o.series.map((s) => ({ s, y: Y(s.ys[s.ys.length - 1]) + 3.5 })).filter((l) => !l.s.dash).sort((a, b) => a.y - b.y);
      for (let i = 1; i < ls.length; i++) if (ls[i].y - ls[i - 1].y < 11) ls[i].y = ls[i - 1].y + 11;
      labels = ls.map((l) => `<text class="elbl" x="${W - R + 6}" y="${l.y.toFixed(1)}">${esc(l.s.name)}</text>`).join("");
    }
    const guides = (o.guides || []).map((g) => g.x != null ? `<line class="guide" x1="${X(g.x)}" x2="${X(g.x)}" y1="${T}" y2="${Hh - B}"/>${g.label ? `<text class="guide-t" x="${X(g.x) + 3}" y="${T + 9}">${esc(g.label)}</text>` : ""}`
      : `<line class="guide" x1="${L}" x2="${W - R}" y1="${Y(g.y)}" y2="${Y(g.y)}"/>`).join("");
    const defs = o.series.map((s, k) => s.area ? `<linearGradient id="g-${id}-${k}" x1="0" x2="0" y1="0" y2="1"><stop offset="0" stop-color="${s.color}" stop-opacity="0.28"/><stop offset="1" stop-color="${s.color}" stop-opacity="0"/></linearGradient>` : "").join("");
    CH.set(id, { o, X, Y, W, Hh, L, R, T, B });
    const legend = o.series.length > 1 ? `<div class="cd-legend">${o.series.map((s, k) => `<span data-leg="${id}:${k}" style="color:${s.color}"><i class="${s.dash ? "dash" : ""}"></i><b style="color:var(--text-2);font-weight:550">${esc(s.name)}</b></span>`).join("")}</div>` : "";
    return `${legend}<div class="cd-chart" data-ch="${id}"><svg viewBox="0 0 ${W} ${Hh}" preserveAspectRatio="xMidYMid meet" role="img" aria-label="${esc(o.title || "")}"><defs>${defs}</defs>
      <g class="grid">${grid}</g><g class="ax">${ax}</g>${guides}${areas}${lines}${labels}
      <line class="xh" x1="0" x2="0" y1="${T}" y2="${Hh - B}"/>${o.series.map((s, k) => `<circle class="hd" data-hd="${k}" r="4" fill="${s.color}"/>`).join("")}
      <rect x="${L}" y="${T}" width="${W - L - R}" height="${Hh - T - B}" fill="transparent" data-hit="1"/></svg><div class="cd-ctip"></div></div>`;
  }
  function chartHover(e) {
    const el = e.target.closest(".cd-chart[data-ch]");
    root.querySelectorAll(".cd-chart.hover").forEach((c) => { if (c !== el) { c.classList.remove("hover"); c.querySelector(".cd-ctip").classList.remove("show"); } });
    if (!el) return;
    const tip = el.querySelector(".cd-ctip");
    const bar = e.target.closest("[data-tip]");
    if (bar) {
      tip.innerHTML = bar.dataset.tip;
      const r = el.getBoundingClientRect(), br = bar.getBoundingClientRect();
      tip.style.left = `${Math.min(r.width - 120, Math.max(0, br.left - r.left - 30))}px`;
      tip.style.top = `${Math.max(0, br.top - r.top - 34)}px`;
      tip.classList.add("show");
      return;
    }
    const c = CH.get(el.dataset.ch);
    if (!c) { tip.classList.remove("show"); return; }
    const svg = el.querySelector("svg"), r = svg.getBoundingClientRect();
    const vx = ((e.clientX - r.left) / r.width) * c.W;
    if (vx < c.L || vx > c.W - c.R) { el.classList.remove("hover"); tip.classList.remove("show"); return; }
    const xs = c.o.xs;
    let bi = 0, bd = 1e9;
    xs.forEach((x, i) => { const d = Math.abs(c.X(x) - vx); if (d < bd) { bd = d; bi = i; } });
    const px = c.X(xs[bi]);
    el.classList.add("hover");
    const xh = el.querySelector(".xh");
    xh.setAttribute("x1", px); xh.setAttribute("x2", px);
    c.o.series.forEach((s, k) => { const hd = el.querySelector(`[data-hd="${k}"]`); hd.setAttribute("cx", px); hd.setAttribute("cy", c.Y(s.ys[bi])); });
    tip.innerHTML = `<div class="h">${c.o.tipX ? c.o.tipX(xs[bi]) : xs[bi]}</div>${c.o.series.map((s) => `<div class="r"><span><i style="background:${s.color}"></i>${esc(s.name)}</span><b>${c.o.tipY ? c.o.tipY(s.ys[bi]) : s.ys[bi]}</b></div>`).join("")}`;
    const sx = (px / c.W) * r.width;
    tip.style.left = `${sx + 14 + 150 > r.width ? sx - 160 : sx + 14}px`;
    tip.style.top = `6px`;
    tip.classList.add("show");
  }
  const F_TICKS = [50, 100, 200, 500, 1000, 2000, 5000, 10000];
  const fx = (f) => (f >= 1000 ? `${f / 1000}k` : `${f}`);
  const tipF = (f) => `${f >= 1000 ? (f / 1000).toFixed(2) + " kHz" : Math.round(f) + " Hz"}`;
  const tipDb = (y) => `${y > 0 ? "+" : ""}${(+y).toFixed(1)} dB`;
  function respRange(arrs, lo = -48) {
    let mn = 0, mx = -200;
    arrs.forEach((a) => a.forEach((v) => { mn = Math.min(mn, v); mx = Math.max(mx, v); }));
    const top = Math.max(6, Math.ceil((mx + 1) / 6) * 6), bot = Math.max(lo, Math.floor((mn - 1) / 6) * 6);
    return [bot, top];
  }
  function dbTicks(yr) { const t = []; const step = yr[1] - yr[0] > 40 ? 12 : 6; for (let y = Math.ceil(yr[0] / step) * step; y <= yr[1]; y += step) t.push(y); return t; }
  function respChart(id, title, series, o = {}) {
    const yr = o.yr || respRange(series.map((s) => s.ys), o.lo);
    return lineChart(id, { title, xs: series[0].xs, xLog: true, xr: [20, 20000], yr, xTicks: F_TICKS, yTicks: dbTicks(yr), xFmt: fx, yFmt: (y) => `${y}`, tipX: tipF, tipY: tipDb,
      series: series.map((s) => ({ ...s })) });
  }
  function plotCard(title, sub, body, big = "") {
    return `<div class="cd-card cd-plot"><div class="cd-plot-h"><span class="t">${title}</span><span class="sp"></span>${big}</div><div class="cd-plot-s">${sub}</div>${body}</div>`;
  }
  function harmonicsCard(hm) {
    if (!hm) return "";
    const H = hm.h.slice(0, 16), Wd = 520, Hh = 170, L = 34, R = 8, T = 8, B = 22, bw = (Wd - L - R) / H.length;
    const Y = (d) => T + (Math.min(0, Math.max(-90, d)) / -90) * (Hh - T - B);
    const bars = H.map((d, i) => {
      const k = i + 1, x = L + i * bw + 2, w = bw - 4, y = Y(d), base = Hh - B;
      const col = k === 1 ? "var(--accent)" : k % 2 ? "var(--cd-s1)" : "var(--cd-s2)";
      const hgt = Math.max(1, base - y);
      return `<path class="bar" d="M${x} ${base} V${y + Math.min(3, hgt)} q0 -3 3 -3 h${w - 6} q3 0 3 3 V${base} Z" fill="${col}" data-tip="<div class='h'>H${k} · ${Math.round(hm.f0 * k)} Hz</div><b>${k === 1 ? "fundamental" : `${d.toFixed(1)} dB re H1`}</b>"/>
        <text x="${x + w / 2}" y="${Hh - 8}" text-anchor="middle">${k}</text>`;
    }).join("");
    const grid = [0, -20, -40, -60, -80].map((d) => `<line class="${d === 0 ? "z" : ""}" x1="${L}" x2="${Wd - R}" y1="${Y(d)}" y2="${Y(d)}"/>`).join("");
    const ax = [0, -20, -40, -60, -80].map((d) => `<text x="${L - 6}" y="${Y(d) + 3}" text-anchor="end">${d}</text>`).join("");
    const svg = `<div class="cd-legend"><span style="color:var(--accent)"><i></i><b style="color:var(--text-2);font-weight:550">Fundamental</b></span><span style="color:var(--cd-s2)"><i></i><b style="color:var(--text-2);font-weight:550">Even (warmth)</b></span><span style="color:var(--cd-s1)"><i></i><b style="color:var(--text-2);font-weight:550">Odd (edge)</b></span></div>
      <div class="cd-chart" data-bars="1"><svg viewBox="0 0 ${Wd} ${Hh}"><g class="grid">${grid}</g><g class="ax">${ax}${bars}</g></svg><div class="cd-ctip"></div></div>`;
    const wave = (ys, id, color, name) => lineChart(id, { title: name, w: 250, h: 90, xs: ys.map((_, i) => i), xr: [0, ys.length - 1], yr: [-1.1, 1.1], yTicks: [0], xTicks: [],
      series: [{ name, ys, color }], tipX: (i) => `${Math.round((i / (ys.length - 1)) * 360)}°`, tipY: (y) => (+y).toFixed(2) });
    const freqs = [82, 110, 165, 220, 330, 440, 660, 1000];
    const ctl = `<div class="cd-tone-ctl">Test tone <select data-tone-f="1">${freqs.map((f) => `<option value="${f}"${Math.abs(f - S.tone.freq) < 1 ? " selected" : ""}>${f} Hz</option>`).join("")}</select>
      at <input type="range" min="-40" max="0" step="1" value="${Math.round(20 * Math.log10(S.tone.level))}" data-tone-l="1" aria-label="Test tone level"><b style="color:var(--text-2)">${Math.round(20 * Math.log10(S.tone.level))} dBFS</b></div>`;
    return plotCard("Harmonic spectrum", `A ${Math.round(hm.f0)} Hz sine through the whole unit, rendered offline · output ${hm.out_rms_db.toFixed(1)} dBFS RMS`,
      ctl + svg + `<div class="cd-wave"><div><div class="lb">Input · one period</div>${wave(hm.wave_in, "wv-in", "var(--text-3)", "Input")}</div><div><div class="lb">Output · one period (normalised)</div>${wave(hm.wave_out, "wv-out", "var(--accent)", "Output")}</div></div>`,
      `<span class="big">${hm.thd_pct < 10 ? hm.thd_pct.toFixed(1) : Math.round(hm.thd_pct)}<small>% THD</small></span>`);
  }
  function renderAnalysis() {
    const sec = $("cd-ana-sec");
    if (!sec) return;
    const c = S.curves, m = S.model || {};
    if (!c) { sec.innerHTML = ""; return; }
    CH.clear();
    const st = c.static || {}, cards = [];
    if (st.tonestack) {
      const t = st.tonestack;
      cards.push(plotCard("Tone stack response", `FMV · ${esc((m.tone_stack || {}).preset || "")} values · Bass ${(t.bass * 10).toFixed(1)} · Mid ${(t.mid * 10).toFixed(1)} · Treble ${(t.treble * 10).toFixed(1)}`,
        respChart("ts", "Tone stack", [{ name: "Current", xs: t.f, ys: t.db, color: "var(--accent)", area: true }, { name: "All at noon", xs: t.f, ys: t.noon_db, color: "var(--text-3)", dash: true }], { lo: -42 })));
    }
    if (st.stages && st.stages.curves) {
      const u = st.stages.u, cs = st.stages.curves;
      let mn = 0, mx = 0;
      cs.forEach((s) => s.y.forEach((v) => { mn = Math.min(mn, v); mx = Math.max(mx, v); }));
      const yr = [Math.floor(mn - 0.5), Math.ceil(mx + 0.5)];
      const yt = []; for (let y = Math.ceil(yr[0]); y <= yr[1]; y += Math.max(1, Math.round((yr[1] - yr[0]) / 5))) yt.push(y);
      cards.push(plotCard("Preamp stage transfer curves", "Static curve of every stage at its operating point · u = 1 is grid conduction · badges show grid drive",
        lineChart("stg", { title: "Stage curves", xs: u, xr: [-8, 8], yr, xTicks: [-8, -4, -1, 0, 1, 4, 8], yTicks: yt, xFmt: (x) => `${x}`, guides: [{ x: 1, label: "knee" }, { x: -1 }], endLabels: true,
          tipX: (x) => `grid drive u = ${(+x).toFixed(1)}`, tipY: (y) => (+y).toFixed(2),
          series: cs.map((s, i) => ({ name: s.label, ys: s.y, color: SERIES[i % SERIES.length] })) })));
    }
    if (c.harmonics) cards.push(harmonicsCard(c.harmonics));
    if (st.power) {
      const p = st.power;
      cards.push(plotCard("Power stage", `Phase inverter + ${(m.power && m.power.spec && m.power.spec.class_a) ? "class-A blend" : "push-pull pair"} at the current master · open loop · drive ${p.drive.toFixed(2)}, headroom ${p.headroom.toFixed(2)}`,
        lineChart("pw", { title: "Power stage", xs: p.x, xr: [-1.5, 1.5], yr: [-p.headroom * 1.15, p.headroom * 1.15], xTicks: [-1.5, -1, -0.5, 0, 0.5, 1, 1.5], yTicks: [-1, 0, 1].map((v) => +(v * p.headroom).toFixed(2)),
          yFmt: (y) => y.toFixed(1), tipX: (x) => `preamp out ${(+x).toFixed(2)}`, tipY: (y) => (+y).toFixed(3),
          series: [{ name: "Output", ys: p.y, color: "var(--accent)" }, { name: "Unclipped", ys: p.x.map((x) => x * p.drive), color: "var(--text-3)", dash: true }] })));
    }
    if (st.unit) {
      cards.push(plotCard("Clipping curve", "Input at the jack → clipper output, with the filters at unity (current drive)",
        lineChart("clip", { title: "Clipping curve", xs: st.unit.x, xr: [-1, 1], yr: respRangeLin(st.unit.y), xTicks: [-1, -0.5, 0, 0.5, 1], yTicks: linTicks(respRangeLin(st.unit.y)),
          yFmt: (y) => y.toFixed(1), tipX: (x) => `in ${(+x).toFixed(2)} V`, tipY: (y) => (+y).toFixed(3), series: [{ name: "Clipper", ys: st.unit.y, color: "var(--accent)" }] })));
    }
    if (st.pre) cards.push(plotCard("Into the clipper", "Filters ahead of / between the clipping stages (normalised)", respChart("pre", "Pre-clip", [{ name: "Voicing", xs: st.pre.f, ys: st.pre.db, color: "var(--cd-s1)", area: true }])));
    if (st.tone) cards.push(plotCard("Tone section", "Everything after the clipper, from the real filter objects (normalised)", respChart("tone", "Tone", [{ name: "Tone", xs: st.tone.f, ys: st.tone.db, color: "var(--accent)", area: true }])));
    if (c.response) {
      const t = { amp: "Whole-amp small-signal response", drive: "Whole-pedal small-signal response", cab: "Cabinet frequency response", eq: "EQ response", fallback: "EQ response", ir_reverb: "Reverb spectrum (first 0.7 s)", fdn_reverb: "Reverb spectrum (first 0.7 s)" }[c.impl] || "Frequency response";
      const ctl = m.controls || {};
      const fx = c.impl === "amp" && (ctl.mod_mode || ctl.reverb_on) ? " · includes the built-in " + [ctl.mod_mode ? "modulation" : "", ctl.reverb_on ? "reverb" : ""].filter(Boolean).join(" and ") + " as set, so it is time-varying" : "";
      const sub = (c.impl === "amp" || c.impl === "drive" ? "A -60 dBFS impulse through the whole unit minus its response to silence — the voicing without distortion" : "The unit's impulse response, measured on a private instance (normalised)") + fx;
      cards.push(plotCard(t, sub, respChart("resp", t, [{ name: "Response", xs: c.response.f, ys: c.response.db, color: "var(--accent)", area: true }], { lo: -54 })));
    }
    if (c.decay) {
      const d = c.decay, n = Math.min(d.t.length, c.impl === "delay" ? 250 : 300);
      cards.push(plotCard(c.impl === "delay" ? "Echo pattern" : "Decay", c.impl === "delay" ? "Energy of the impulse response in 10 ms windows" : "Energy envelope and Schroeder decay curve of the measured impulse response",
        lineChart("dec", { title: "Decay", xs: d.t.slice(0, n), xr: [0, d.t[n - 1]], yr: [-72, 0], xTicks: [0, 0.5, 1, 1.5, 2, 2.5].filter((x) => x <= d.t[n - 1]), yTicks: [0, -24, -48, -72],
          xFmt: (x) => `${x}s`, tipX: (x) => `${(+x).toFixed(2)} s`, tipY: tipDb,
          series: c.impl === "delay" ? [{ name: "Energy", ys: d.env_db.slice(0, n), color: "var(--accent)", area: true }]
            : [{ name: "Envelope", ys: d.env_db.slice(0, n), color: "var(--accent)", area: true }, { name: "Energy decay", ys: d.edc_db.slice(0, n), color: "var(--cd-s1)" }] }),
        d.rt60 ? `<span class="big">${d.rt60.toFixed(2)}<small>s RT60</small></span>` : ""));
    }
    if (c.dynamics) {
      const d = c.dynamics;
      cards.push(plotCard("Compression curve", "Steady 1 kHz sines at rising level through the unit (measured)",
        lineChart("dyn", { title: "Dynamics", xs: d.in_db, xr: [-60, 0], yr: [-60, 6], xTicks: [-60, -48, -36, -24, -12, 0], yTicks: [-60, -48, -36, -24, -12, 0], tipX: (x) => `in ${x} dBFS`, tipY: tipDb,
          series: [{ name: "Output", ys: d.out_db, color: "var(--accent)" }, { name: "Unity", ys: d.in_db, color: "var(--text-3)", dash: true }] })));
    }
    if (c.lfo) {
      const l = c.lfo;
      cards.push(plotCard("Modulation", `Delay time of the left and right lines over two LFO periods · ${l.rate_hz.toFixed(2)} Hz`,
        lineChart("lfo", { title: "LFO", xs: l.t, xr: [0, l.t[l.t.length - 1]], yr: [0, Math.ceil(Math.max(...l.l_ms) + 1)], xTicks: [], yTicks: [0, 3.5, Math.ceil(Math.max(...l.l_ms))], xFmt: (x) => `${x}s`,
          tipX: (x) => `${(+x).toFixed(2)} s`, tipY: (y) => `${(+y).toFixed(2)} ms`, series: [{ name: "Left", ys: l.l_ms, color: "var(--cd-s1)" }, { name: "Right", ys: l.r_ms, color: "var(--cd-s2)" }] })));
    }
    if (S.anaCount !== cards.length) { S.anaCount = cards.length; renderTabs(); }
    sec.innerHTML = cards.length ? `<div class="cd-sec-h"><h2>Live analysis</h2><span class="s">computed by the engine for the current knob settings${S.edit && leaves(S.overrides).length ? " and your changes" : ""}</span></div><div class="cd-grid">${cards.join("")}</div>` : "";
  }
  function respRangeLin(ys) { const m = Math.max(0.2, ...ys.map(Math.abs)); const t = Math.ceil(m * 1.15 * 2) / 2; return [-t, t]; }
  function linTicks(yr) { const s = yr[1] > 2 ? 1 : 0.5; const t = []; for (let y = Math.ceil(yr[0] / s) * s; y <= yr[1] + 1e-9; y += s) t.push(+y.toFixed(2)); return t; }

  function renderModelParts() {
    if (!S.open) return;
    renderBarActions();
    renderHead();
    renderAbout();
    renderTabs();
    renderSchematic();
    renderDetails();
    renderEditBar();
  }
  const FOOT = "Values come from the engine's own model of this unit. Where a number is a modelling parameter rather than a physical part (grid drive, knee voltages, blocking) it is labelled as normalised. " +
    "Curves are rendered on a private instance of the unit, never on the live signal chain.";
  // Details tab: for circuit units the circuit's own documentation (circuit-view.js); otherwise how Headroom
  // models the unit and every value of its model, block by block.
  function renderDetails() {
    const sec = $("cd-details-sec");
    if (!sec || !S.model) return;
    const m = S.model;
    if (m.kind === "circuit" && m.circuit) {
      if (!window.CircuitView) { sec.innerHTML = `<div class="cd-card txt cd-loading" style="padding:30px">Reading the circuit…</div>`; return; }
      sec.innerHTML = window.CircuitView.details(m) + `<p class="cd-dt-foot">${esc(FOOT)}</p>`;
      return;
    }
    const sect = (title, sub, body) => `<section class="cd-card cd-dt-sec"><h3>${esc(title)}${sub ? `<span>${esc(sub)}</span>` : ""}</h3>${body}</section>`;
    const paras = describeModel(m);
    const how = sect(m.backend === "hybrid-circuit" ? "How the hybrid model works" : "How Headroom models it", "", paras.length ? paras.map((p) => `<p>${p}</p>`).join("") : `<p>${esc(S.comp.engine || S.comp.name || "")}</p>`);
    const blocks = (m.blocks || []).filter((b) => (b.values || []).length);
    const vals = blocks.length ? sect("Model values", "at the current knob settings · open a block on the Signal path tab to edit it", blocks.map((b) =>
      `<div class="cd-dt-blk"><div class="h"><b>${esc(b.label)}</b><span>${esc([b.section, b.sub].filter(Boolean).join(" · "))}</span></div>${b.values.map((v) =>
        `<div class="cd-dt-row"><div class="k">${esc(v.k)}</div><div class="v"><span class="num">${esc(v.v)}</span>${v.note ? `<div class="n">${esc(v.note)}</div>` : ""}</div></div>`).join("")}</div>`).join("")) : "";
    const facts2 = sect("At a glance", "", facts(m).map(([k, v]) => `<div class="cd-dt-row"><div class="k">${esc(k)}</div><div class="v">${esc(v)}</div></div>`).join(""));
    sec.innerHTML = `<div class="cd-dt"><div class="col">${how}${vals}</div><div class="col">${facts2}</div></div><p class="cd-dt-foot">${esc(FOOT)}</p>`;
  }

  /* =====================================================================
     events
     ===================================================================== */
  function wire() {
    root.addEventListener("pointerdown", onHeroDown, true);
    // illustrated faces (assets/unit-art.js) load their moving parts on first use: redraw the hero once they arrive
    window.addEventListener("hr-unit-art", () => { const u = $("cd-hero-unit"); if (S.open && u && u.querySelector(".ua-loading")) renderHero(); });
    window.addEventListener("pointermove", onHeroMove);
    window.addEventListener("pointerup", onHeroUp);
    root.addEventListener("wheel", onHeroWheel, { passive: false });
    root.addEventListener("click", (e) => {
      const tb = e.target.closest("[data-cd-tab]");
      if (tb) { setTab(tb.dataset.cdTab); return; }
      const gp = e.target.closest("[data-goto-part]");
      if (gp && window.CircuitView) { const ref = gp.dataset.gotoPart; setTab("schem"); requestAnimationFrame(() => window.CircuitView.select(ref)); return; }
      const a = e.target.closest("[data-act]");
      if (a) {
        const act = a.dataset.act;
        if (act === "back") close();
        else if (act === "add") addFromPage(false);
        else if (act === "add-aud") addFromPage(true).then(() => scheduleApply());
        else if (act === "edit") {
          S.edit = !S.edit;
          if (S.edit && !S.sel) { const b = (S.model.blocks || []).find((x) => (x.values || []).some((v) => v.edit)); if (b) S.sel = b.id; }
          if (S.edit && S.slot >= 0 && !S.audOn) setAudition(true);
          renderModelParts();
        } else if (act === "aud") setAudition(!S.audOn);
        else if (act === "reset") { S.overrides = {}; S.editEpoch++; changed(); }
        else if (act === "save") save(false);
        else if (act === "saveas") save(true);
        else if (act === "delete") del();
        return;
      }
      const rs = e.target.closest("[data-reset]");
      if (rs) { delPath(S.overrides, rs.dataset.reset); changed(); return; }
      if (e.target.closest("[data-reset-block]")) {
        const b = (S.model.blocks || []).find((x) => x.id === S.sel);
        (b ? b.values : []).forEach((v) => v.edit && delPath(S.overrides, v.edit.path));
        if (b && b.id === "power") delPath(S.overrides, "amp.power");
        changed();
        return;
      }
      const rst = e.target.closest("[data-restore]");
      if (rst) { delPath(S.overrides, rst.dataset.restore); changed(); return; }
      const g = e.target.closest(".cd-node");
      if (g) {
        const b = (S.model.blocks || [])[+g.dataset.b];
        if (!b) return;
        S.sel = S.sel === b.id ? null : b.id;
        root.querySelectorAll(".cd-node").forEach((n) => n.classList.toggle("sel", n === g && S.sel === b.id));
        renderPart();
      }
    });
    root.addEventListener("keydown", (e) => {
      const tb = e.target.closest && e.target.closest("[data-cd-tab]");
      if (tb && tb.parentElement.id === "cd-tabs" && (e.key === "ArrowLeft" || e.key === "ArrowRight")) {
        const all = [...tb.parentElement.querySelectorAll("[data-cd-tab]")], i = all.indexOf(tb);
        const nx = all[(i + (e.key === "ArrowRight" ? 1 : all.length - 1)) % all.length];
        e.preventDefault(); setTab(nx.dataset.cdTab);
        const f = $("cd-tabs").querySelector(`[data-cd-tab="${nx.dataset.cdTab}"]`); if (f) f.focus();
        return;
      }
      const g = e.target.closest && e.target.closest(".cd-node");
      if (g && (e.key === "Enter" || e.key === " ")) { e.preventDefault(); g.dispatchEvent(new MouseEvent("click", { bubbles: true })); }
    });
    root.addEventListener("input", (e) => {
      const t = e.target;
      if (t.matches("input[type=range][data-path]")) {
        const v = findVal(t.dataset.path);
        if (!v) return;
        const val = sliderVal(v.edit, +t.value / 1000);
        const num = t.parentElement.querySelector(".num");
        // Preserve the scale shown beside this input until its row is rebuilt.
        // A refreshed model may choose another SI prefix while dragging.
        if (num) num.value = numTxt(val, { s: +num.dataset.scale });
        setPath(S.overrides, t.dataset.path, +val.toPrecision(5));
        renderEditBarCount();
        refresh();
        scheduleApply();
      } else if (t.matches("[data-tone-l]")) {
        S.tone.level = Math.pow(10, +t.value / 20);
        t.nextElementSibling.textContent = `${t.value} dBFS`;
        clearTimeout(S.toneT);
        S.toneT = setTimeout(fetchCurves, 160);
      }
    });
    root.addEventListener("change", (e) => {
      const t = e.target;
      if (t.matches("select[data-engine-param]")) {
        const p = heroParam(t);
        if (p) { setHero(t, +t.value / Math.max(1, p.labels.length - 1)); hideTipSoon(700); }
        return;
      }
      if (t.matches("input[type=range][data-path]")) { renderModelParts(); return; }
      if (t.matches(".num[data-path]")) {
        const v = findVal(t.dataset.path), x = parseFloat(t.value);
        if (v && isFinite(x)) editValue(t.dataset.path, Math.max(v.edit.min, Math.min(v.edit.max, x * +t.dataset.scale)));
        return;
      }
      if (t.matches("select[data-path]")) { const v = findVal(t.dataset.path); if (v) editValue(t.dataset.path, v.edit.options[+t.value].v); return; }
      if (t.matches("[data-profile]")) {
        if (t.value === "") delPath(S.overrides, "amp.power");
        else setPath(S.overrides, "amp.power", { ...S.model.power_profiles[+t.value].spec });
        changed();
        return;
      }
      if (t.matches("[data-topology]")) { editValue("drive.kind", t.value); return; }
      if (t.matches("[data-tone-f]")) { S.tone.freq = +t.value; fetchCurves(); return; }
      if (t.id === "cd-name") S.name = t.value;
    });
    root.addEventListener("pointermove", (e) => {
      chartHover(e);
      const g = e.target.closest(".cd-node");
      const tip = $("cd-tip");
      if (g && S.model) { if (tip && tip._g !== g) { tip._g = g; showNodeTip(g); } }
      else if (tip && tip._g) { tip._g = null; tip.classList.remove("show"); }
    });
    root.addEventListener("pointerleave", () => { root.querySelectorAll(".cd-chart.hover").forEach((c) => c.classList.remove("hover")); root.querySelectorAll(".cd-ctip").forEach((t) => t.classList.remove("show")); });
    root.addEventListener("pointerover", (e) => {
      const lg = e.target.closest("[data-leg]");
      root.querySelectorAll(".cd-chart .ln.dim").forEach((l) => l.classList.remove("dim"));
      if (!lg) return;
      const [id, k] = lg.dataset.leg.split(":");
      const ch = root.querySelector(`.cd-chart[data-ch="${id}"]`);
      if (ch) ch.querySelectorAll(".ln").forEach((l) => l.classList.toggle("dim", l.dataset.s !== k));
    });
    // Esc goes back to the chain (unless a menu / modal / text field has it).
    document.addEventListener("keydown", (e) => {
      if (!S.open || e.key !== "Escape" || !root.offsetParent) return;   // only while the Component tab is showing
      const menu = $("menu");
      if ((menu && !menu.hidden) || ui.modalOpen) return;
      const a = document.activeElement;
      if (a && !root.contains(a) && (a.tagName === "INPUT" || a.tagName === "SELECT" || a.tagName === "TEXTAREA")) return;
      if (a && root.contains(a) && (a.tagName === "INPUT" || a.tagName === "SELECT")) { a.blur(); e.stopImmediatePropagation(); return; }
      e.preventDefault();
      e.stopImmediatePropagation();
      close();
    }, true);
    // Picking a unit in the right-hand chain list returns to the chain.
    const tf = $("topology-flow");
    if (tf) tf.addEventListener("click", (e) => { if (S.open && e.target.closest("[data-sel]") && !e.target.closest("[data-byp]")) { if (window.HRViews) HRViews.show("chain"); else close(); } }, true);
    let rz = 0;
    window.addEventListener("resize", () => { if (!S.open) return; clearTimeout(rz); rz = setTimeout(() => { fitHero(); renderSchematic(); }, 120); });
  }
  function renderEditBarCount() {
    const t = root.querySelector(".cd-edit-bar .t");
    const n = changeCount();
    if (t) t.innerHTML = `<span class="pip"></span>${n ? `${n} change${n > 1 ? "s" : ""}` : "No changes yet"}`;
    const sv = root.querySelector('.cd-edit-bar [data-act="save"]');
    if (sv && n) sv.removeAttribute("disabled");
  }

  window.ComponentDetail = {
    open, close, isOpen, injectUserComponents, insertUser, reloadUsers: loadUsers,
    // read-only knowledge for the Guitar Tech (guitar-tech.js): what a unit is modelled after, and the
    // same plain-language model description this page shows (HTML) for an /api/component/model response
    about: (tid) => ABOUT[tid] || null,
    describeModel: (m) => { try { return describeModel(m || {}); } catch (e) { return []; } },
  };
})();
