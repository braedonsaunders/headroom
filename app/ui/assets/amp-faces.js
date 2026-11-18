/* =====================================================================
   AMP FACES — per-unit faceplates for every amplifier, modelled on the
   real hardware family each unit emulates (see ABOUT in component-detail.js).
   Each face is built from plain block elements (front/grille, panel), so the
   controls can never ride up into the grille cloth at any width.
   Wordmarks are styled after each maker's lettering, never their artwork.

   Hooks (index.html):
     describe()  -> HRAmpFaces.describe(slot, d)   tolex / cloth / piping for the matched cab
     ampHTML()   -> HRAmpFaces.ampHTML(s, d, i)     the faceplate itself
   Helpers used at call time (globals from index.html): ctlHTML, faceParams, esc,
   moreBtn, drawerHTML, unitCls, XBTN, RENDER, isTogParam.
   ===================================================================== */
(function () {
  "use strict";

  /* ---------- printed scales (static, on the panel) and rotating Fender skirts ---------- */
  const svgURI = (inner) => `url("data:image/svg+xml,${encodeURIComponent(`<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'>${inner}</svg>`)}")`;
  function ringSVG(labels, ink, r = 45, fs = 9.5) {
    let t = "";
    labels.forEach((l, i) => {
      const a = ((-135 + (270 * i) / (labels.length - 1)) * Math.PI) / 180;
      const x = 50 + r * Math.sin(a), y = 50 - r * Math.cos(a) + fs * 0.36;
      t += `<text x='${x.toFixed(1)}' y='${y.toFixed(1)}' font-size='${fs}' text-anchor='middle' fill='${ink}' font-family='Helvetica Neue,Arial,sans-serif' font-weight='700'>${l}</text>`;
    });
    return svgURI(t);
  }
  // Fender blackface skirt: 1..10 printed on the skirt, counter-clockwise, so the number at 12 o'clock is the setting.
  function skirtSVG(ink) {
    let t = "";
    for (let k = 1; k <= 10; k++) {
      const phi = 135 - 30 * (k - 1), a = (phi * Math.PI) / 180, r = 41;
      const x = 50 + r * Math.sin(a), y = 50 - r * Math.cos(a);
      t += `<text x='${x.toFixed(1)}' y='${(y + 3.4).toFixed(1)}' font-size='10' text-anchor='middle' fill='${ink}' font-family='Helvetica Neue,Arial,sans-serif' font-weight='700' transform='rotate(${phi} ${x.toFixed(1)} ${y.toFixed(1)})'>${k}</text>`;
    }
    return svgURI(t);
  }
  const NUM = (a, b) => Array.from({ length: b - a + 1 }, (_, i) => a + i);

  /* ---------- Orange-style pictograms (drawn, not lettered) ---------- */
  const PIC = {
    gain: "<path d='M14 70 L86 70 L86 30 Z' fill='#111'/>",
    bass: "<path d='M10 50 Q30 10 50 50 T90 50' fill='none' stroke='#111' stroke-width='9' stroke-linecap='round'/>",
    treble: "<path d='M8 50 Q16 22 24 50 T40 50 T56 50 T72 50 T88 50' fill='none' stroke='#111' stroke-width='7' stroke-linecap='round'/>",
    presence: "<path d='M10 70 L30 70 L30 30 L50 30 L50 70 L70 70 L70 30 L90 30' fill='none' stroke='#111' stroke-width='8' stroke-linejoin='round'/>",
    master: "<circle cx='50' cy='50' r='30' fill='none' stroke='#111' stroke-width='9'/><path d='M50 6 L50 46' stroke='#111' stroke-width='9' stroke-linecap='round'/>",
    locut: "<path d='M8 78 Q40 78 50 40 L92 40' fill='none' stroke='#111' stroke-width='9' stroke-linecap='round'/>",
    voicing: "<path d='M10 50 L35 50 M65 50 L90 50' stroke='#111' stroke-width='8'/><circle cx='50' cy='50' r='13' fill='#111'/>",
  };

  /* ---------- per-unit specs ---------- */
  // Order arrays: [paramId, panel label]. Anything on the face but not listed is appended; svc = engine controls
  // that no real panel has (power-amp voltage, sag, legacy mode): they sit in a recessed "voicing" well.
  const MARSHALL_SVC = [["G4M", "Mode"], ["Pw", "Power"], ["Var", "VAC"], ["Sg", "Sag"]];
  const SPECS = {
    /* ---- Marshall 1959 Super Lead: gold perspex, vinyl front with gold piping, power left / inputs right ---- */
    34000: { brand: "Marshall", fam: "marshall plexi", layout: "head", tolex: "#141413", pipe: "#cfae62", cloth: "gx-saltpepper", front: { kind: "vinyl", h: 86 },
      logo: { cls: "lg-mscript" }, panel: "pn-plexi", ink: "#17120a", knob: "marshall", scale: "dots",
      order: [["Pr", "Presence"], ["B", "Bass"], ["M", "Middle"], ["T", "Treble"], ["V1", "Loudness 1"], ["V2", "Loudness 2"]], svc: MARSHALL_SVC,
      left: ["model", "rockers", "jewel:red"], right: ["jacks4:high treble|normal"] },
    92000: { brand: "Marshall", fam: "marshall plexi", layout: "head", tolex: "#2f4a6b", pipe: "#e2c27e", cloth: "gx-saltpepper", front: { kind: "vinyl", h: 86 },
      logo: { cls: "lg-mscript" }, panel: "pn-plexi", ink: "#17120a", knob: "marshall", scale: "dots",
      order: [["Pr", "Presence"], ["B", "Bass"], ["M", "Middle"], ["T", "Treble"], ["V1", "Loudness 1"], ["V2", "Loudness 2"]],
      svc: [["Var", "Variac"], ["Pw", "Power"], ["Sg", "Sag"]], svcCap: "Variac",
      left: ["model", "rockers", "jewel:red"], right: ["jacks4:high treble|normal"] },
    91000: { brand: "Marshall", fam: "marshall plexi", layout: "head", tolex: "#7a1f1a", pipe: "#e2c27e", cloth: "gx-saltpepper", front: { kind: "vinyl", h: 86 },
      logo: { cls: "lg-mscript" }, panel: "pn-gold", ink: "#17120a", knob: "marshall", scale: "dots",
      order: [["Pr", "Presence"], ["B", "Bass"], ["M", "Middle"], ["T", "Treble"], ["V1", "Pre-amp"], ["V2", "Master"]], svc: MARSHALL_SVC,
      left: ["model", "rockers", "jewel:red"], right: ["jacks2:high|low"] },
    /* ---- JCM800 2203: brushed gold full-width panel, black cloth + white piping, big white script ---- */
    56000: { brand: "Marshall", fam: "marshall jcm", layout: "head", tolex: "#121212", pipe: "#f1f1ee", cloth: "gx-blackgold", front: { kind: "cloth", cls: "gx-blackgold", h: 88 },
      logo: { cls: "lg-mscript big" }, panel: "pn-gold", ink: "#17120a", knob: "marshall", scale: "dots",
      order: [["Pr", "Presence"], ["B", "Bass"], ["M", "Middle"], ["T", "Treble"], ["V2", "Master Volume"], ["V1", "Pre-amp Volume"]], svc: MARSHALL_SVC,
      left: ["series:Lead Series", "rockers"], right: ["jacks2:high|low"] },
    101000: { brand: "Marshall", fam: "marshall jcm", layout: "head", tolex: "#121212", pipe: "#f1f1ee", cloth: "gx-saltpepper", front: { kind: "vinyl", h: 86 },
      logo: { cls: "lg-mscript big" }, panel: "pn-gold", ink: "#17120a", knob: "marshall", scale: "dots",
      order: [["Hg", "Gain Boost"], ["Pr", "Presence"], ["B", "Bass"], ["M", "Middle"], ["T", "Treble"], ["V2", "Master Volume"], ["V1", "Pre-amp Volume"]], svc: MARSHALL_SVC,
      left: ["series:Master Lead · Mk II", "rockers"], right: ["jacks2:high|low"] },
    /* ---- Modern boutique heads ---- */
    151000: { brand: "FRIEDMAN", fam: "friedman", layout: "head", tolex: "#151515", pipe: "#d8b56a", cloth: "gx-saltpepper", front: { kind: "cloth", cls: "gx-saltpepper", h: 84 },
      logo: { cls: "lg-plate fire" }, panel: "pn-blackgold", ink: "#e9cf8f", knob: "marshall", scale: "dots",
      order: [["gain", "Gain"], ["gainMode", "Gain Mode"], ["low", "Bass"], ["mid", "Middle"], ["treble", "Treble"], ["master", "Master"]],
      left: ["jacks1:input", "model"], right: ["jewel:red", "toggles"] },
    300000: { brand: "FRIEDMAN", fam: "friedman", layout: "head", tolex: "#151515", pipe: "#d8b56a", cloth: "gx-saltpepper", front: { kind: "cloth", cls: "gx-saltpepper", h: 84 },
      logo: { cls: "lg-plate fire" }, panel: "pn-blackgold", ink: "#e9cf8f", knob: "marshall", scale: "dots",
      order: [["gain", "Gain"], ["bass", "Bass"], ["mid", "Middle"], ["treble", "Treble"], ["presence", "Presence"], ["master", "Master"], ["bright", "Bright"], ["fat", "Fat"]],
      left: ["jacks1:input", "model"], right: ["jewel:red", "toggles"] },
    162000: { brand: "Fender", brand: "BOGNER", fam: "bogner ecstasy", layout: "head", tolex: "#24201c", pipe: "#c8976a", cloth: "gx-saltpepper", front: { kind: "cloth", cls: "gx-saltpepper", h: 84 },
      logo: { cls: "lg-plate seeker" }, panel: "pn-copper", ink: "#1c120a", knob: "pedal", scale: "dots",
      order: [["gain", "Gain"], ["gainMode", "Gain Mode"], ["low", "Bass"], ["mid", "Middle"], ["treble", "Treble"], ["master", "Master"]],
      left: ["jacks1:input", "model"], right: ["jewel:amber", "toggles"] },
    /* ---- Soldano SLO: expanded-metal grille, lowercase plate, knurled black/silver knobs ---- */
    122000: { brand: "soldano", fam: "soldano", layout: "head", tolex: "#111", pipe: "#c9cdd1", cloth: "gx-expanded", front: { kind: "metal", cls: "gx-expanded", h: 84 },
      logo: { cls: "lg-slo" }, panel: "pn-chrome", ink: "#111", knob: "silvercap", scale: "dots",
      order: [["O2", "Channel"], ["P0", "Normal"], ["P1", "Overdrive"], ["B3", "Bass"], ["M4", "Middle"], ["T5", "Treble"], ["M6", "Master"], ["P7", "Presence"], ["D8", "Depth"]],
      left: ["jacks1:input", "model"], right: ["toggles", "jewel:blue"] },
    163000: { brand: "soldano", fam: "soldano white", layout: "head", tolex: "#111", pipe: "#c9cdd1", cloth: "gx-expanded", front: { kind: "metal", cls: "gx-expanded", h: 84 },
      logo: { cls: "lg-slo" }, panel: "pn-white", ink: "#141414", knob: "silvercap", scale: "dots",
      order: [["channel", "Channel"], ["normalMode", "Crunch"], ["overdrivePreamp", "Overdrive"], ["bass", "Bass"], ["middle", "Middle"], ["treble", "Treble"], ["master", "Master"], ["presence", "Presence"], ["depth", "Depth"]],
      left: ["jacks1:input", "model"], right: ["toggles", "jewel:red"] },
    /* ---- 5150: perforated metal, block-letter plate, silver panel, inputs left, LEDs right ---- */
    134000: { brand: "5150", fam: "peavey", layout: "head", tolex: "#0f0f0f", pipe: "#9ea2a6", cloth: "gx-perf", front: { kind: "metal", cls: "gx-perf", h: 84 },
      logo: { cls: "lg-block51" }, panel: "pn-silver", ink: "#121212", knob: "chicken", ring: { labels: NUM(0, 10), ink: "#1b1b1b" },
      order: [["R0", "Rhythm Pre Gain"], ["H14", "Hi Gain"], ["L1", "Lead Pre Gain"], ["B2", "Low"], ["M3", "Mid"], ["T4", "High"], ["P5", "Post Gain"], ["R6", "Resonance"], ["P7", "Presence"]],
      left: ["jacks2:high|normal", "model"], right: ["leds", "toggles"] },
    /* ---- Rectifier: polished diamond plate + vent plate, black panel, chrome knobs, input far right ---- */
    16000: { brand: "MESA/BOOGIE", fam: "recto", layout: "head", tolex: "#141414", pipe: "#9aa0a6", cloth: "gx-blackmesh", front: { kind: "diamond", h: 92 },
      logo: { cls: "lg-badge" }, panel: "pn-black", ink: "#ecebe6", knob: "chrome", scale: "dots",
      order: [["Mast", "Master"], ["Prs", "Presence"], ["M", "Mode"], ["Gn", "Gain"], ["Bs", "Bass"], ["Mid", "Mid"], ["Trb", "Treble"]],
      svc: [["Var", "VAC"], ["Sg", "Sag"]],
      left: ["toggles", "jewel:white", "script"], right: ["jacks1:input"] },
    96000: { brand: "MESA/BOOGIE", fam: "recto revc", layout: "head", tolex: "#0d0d0d", pipe: "#5c5c5c", cloth: "gx-blackmesh", front: { kind: "diamond dark", h: 92 },
      logo: { cls: "lg-badge red" }, panel: "pn-black", ink: "#ff5a3c", knob: "chrome", scale: "dots",
      order: [["Mast", "Master"], ["Prs", "Presence"], ["M", "Mode"], ["Gn", "Gain"], ["Bs", "Bass"], ["Mid", "Mid"], ["Trb", "Treble"]],
      svc: [["Var", "VAC"], ["Sg", "Sag"]],
      left: ["toggles", "jewel:red", "script"], right: ["jacks1:input"] },
    /* ---- Überschall: black mirror panel with blue fibre-optic lettering, chrome-metal wordmark ---- */
    81000: { brand: "BOGNER", fam: "bogner", layout: "head", tolex: "#121211", pipe: "#3a3a3a", cloth: "gx-blackmesh", front: { kind: "cloth", cls: "gx-blackmesh", h: 86 },
      logo: { cls: "lg-chrome" }, panel: "pn-mirror", ink: "#8fd3ff", knob: "chrome", scale: "dots",
      order: [["c", "Channel"], ["g", "Gain"], ["b", "Bass"], ["m", "Middle"], ["t", "Treble"], ["p", "Presence"], ["v", "Volume"], ["Mst", "Master"]],
      svc: [["Pw", "Power"]],
      left: ["led:blue", "model"], right: ["jacks1:input", "toggles"] },
    /* ---- Orange OR120: orange tolex, beige basketweave, pictogram white panel ---- */
    80000: { brand: "ORANGE", fam: "orange", layout: "head", tolex: "#e3741b", pipe: "#f3d9b4", cloth: "gx-basket", front: { kind: "cloth", cls: "gx-basket", h: 88 },
      logo: { cls: "lg-orange", pos: "l" }, panel: "pn-white", ink: "#111", knob: "pedal", scale: "dots",
      order: [["f", "@locut"], ["b", "@bass"], ["t", "@treble"], ["p", "@presence"], ["g", "@gain"], ["m", "@master"]],
      svc: [["Pw", "Power"], ["Var", "VAC"], ["Sg", "Sag"]],
      left: ["jacks2:hi|lo"], right: ["jewel:red", "toggles1"] },
    /* ---- Hiwatt DR103: black vinyl front, rectangular plate, recessed black panel ---- */
    87000: { brand: "HIWATT", fam: "hiwatt", layout: "head", tolex: "#121212", pipe: "#efefef", cloth: "gx-hiwatt", front: { kind: "vinyl", h: 86 },
      logo: { cls: "lg-hiplate" }, panel: "pn-navy", ink: "#f2f2f2", knob: "silvercap", scale: "dots",
      order: [["nv", "Normal Vol"], ["bv", "Brilliant Vol"], ["bs", "Bass"], ["tr", "Treble"], ["md", "Middle"], ["pr", "Presence"], ["Mst", "Master Vol"]],
      svc: [["Pw", "Power"], ["Var", "VAC"]],
      left: ["jacks4:normal|brilliant", "series:Custom 100"], right: ["jewel:red", "toggles"] },
    /* ---- Fender blackface combos: black panel on top, numbered skirted knobs, silver grille, chrome script ---- */
    30000: { brand: "Fender", fam: "blackface", layout: "combo", tolex: "#141414", pipe: "#2a2a2a", cloth: "gx-silver", front: { kind: "cloth", cls: "gx-silver", h: 74 },
      logo: { cls: "lg-fscript", pos: "ul" }, panel: "pn-blackface", ink: "#f4f4f2", knob: "fendersk",
      order: [["Br", "Bright"], ["Vol", "Volume"], ["Tr", "Treble"], ["Md", "Middle"], ["Bs", "Bass"], ["RvL", "Reverb"], ["ViSpd", "Speed"]],
      svc: [["RvTm", "Decay"], ["RvSz", "Tank"]], svcCap: "Spring tank",
      left: ["jacks2:1|2"], right: ["ampscript", "jewel:red:power"] },
    172000: { brand: "Fender", fam: "blackface", layout: "combo", tolex: "#141414", pipe: "#2a2a2a", cloth: "gx-silver", front: { kind: "cloth", cls: "gx-silver", h: 74 },
      logo: { cls: "lg-fscript", pos: "ul" }, panel: "pn-blackface", ink: "#f4f4f2", knob: "fendersk",
      order: [["bright", "Bright"], ["volume", "Volume"], ["treble", "Treble"], ["bass", "Bass"], ["reverb on", "Reverb On"], ["reverb mode", "Tank"], ["reverb intensity", "Reverb"], ["speed", "Speed"], ["intensity", "Intensity"]],
      left: ["jacks2:1|2"], right: ["ampscript", "jewel:red:power"] },
    /* ---- Tweed: lacquered tweed, oxblood grille with gold stripe, chrome top panel, pointer knobs on a printed 1-12 ---- */
    62000: { fam: "tweed", layout: "combo", tolex: "#c7a868", pipe: "#5a3b1f", cloth: "gx-oxblood", front: { kind: "cloth", cls: "gx-oxblood", h: 78 },
      logo: { cls: "lg-tscript", pos: "ul" }, panel: "pn-tweedchrome", ink: "#191512", knob: "chicken", ring: { labels: NUM(1, 12), ink: "#1d1a16" },
      order: [["VB", "Bright Vol"], ["VN", "Normal Vol"], ["T", "Treble"], ["B", "Bass"], ["M", "Middle"], ["Pr", "Presence"]], svc: MARSHALL_SVC,
      left: ["jacks4:bright|normal"], right: ["toggles", "jewel:red"] },
    79000: { brand: "Fender", fam: "tweed", layout: "combo", tolex: "#c9ab6c", pipe: "#5a3b1f", cloth: "gx-oxblood", front: { kind: "cloth", cls: "gx-oxblood", h: 78 },
      logo: { cls: "lg-tscript", pos: "ul" }, panel: "pn-tweedchrome", ink: "#191512", knob: "chicken", ring: { labels: NUM(1, 12), ink: "#1d1a16" },
      order: [["vb", "Bright Vol"], ["vn", "Normal Vol"], ["t", "Tone"]],
      svc: [["Pw", "Power"], ["Var", "VAC"], ["Sg", "Sag"], ["Rsp", "Response"], ["Bi", "Bias"]],
      left: ["jacks4:inst|mic"], right: ["jewel:red", "toggles1"] },
    /* ---- Valco / Supro: midnight-blue rhino hide, grey striped grille, chrome script ---- */
    152000: { brand: "Supro", fam: "supro", layout: "combo", tolex: "#28374a", pipe: "#b8bcc2", cloth: "gx-supro", front: { kind: "cloth", cls: "gx-supro", h: 78 },
      logo: { cls: "lg-supro", pos: "ul" }, panel: "pn-supro", ink: "#e9efe9", knob: "cream", ring: { labels: NUM(1, 10), ink: "#e9efe9" },
      order: [["gain", "Volume"], ["tone", "Tone"], ["mod", "Tremolo"], ["bright", "Bright"]],
      left: ["jacks2:1|2", "model"], right: ["jewel:red", "toggles1"] },
    /* ---- Vox AC30 Top Boost: black band + gold strip, brown diamond grille, copper panel strip below ---- */
    38000: { brand: "VOX", fam: "vox", layout: "vox", tolex: "#151413", pipe: "#efe8d8", cloth: "gx-diamond", front: { kind: "cloth", cls: "gx-diamond", h: 56 },
      logo: { cls: "lg-vox" }, panel: "pn-voxcopper", ink: "#1b120b", knob: "chicken", scale: "dots",
      order: [["TSp", "Speed"], ["TDt", "Vib-Trem"], ["Vol", "Normal"], ["Br", "Brilliant"], ["Tb", "Treble"], ["Bs", "Bass"], ["Tc", "Cut"]],
      svc: [["G5M", "Mode"], ["Hz", "Power"]],
      left: ["jacks6:vib-trem|normal|brilliant"], right: ["jewel:red", "toggles"] },
    176000: { brand: "VOX", fam: "vox fawn", layout: "vox", tolex: "#d8c9a4", pipe: "#f3ecda", cloth: "gx-diamond", front: { kind: "cloth", cls: "gx-diamond", h: 56 },
      logo: { cls: "lg-vox" }, panel: "pn-voxgrey", ink: "#f1ece0", knob: "chickenw", scale: "dots",
      order: [["volume", "Volume"], ["brilliance", "Brilliance"], ["brillianceFrequency", "Bright Freq"], ["cut", "Tone Cut"], ["effectsSelector", "Vib · Trem"], ["vibratoSpeed", "Vib Speed"], ["tremoloSpeed", "Trem Speed"], ["depth", "Depth"], ["tempoSync", "Sync"]],
      left: ["jacks2:normal|top boost"], right: ["jewel:red", "toggles"] },
    /* ---- Roland JC-120: dark panel, aluminium caps, printed 0-10, olive FX section, grid-weave grille ---- */
    49000: { brand: "Roland", fam: "jc", layout: "combo", tolex: "#121212", pipe: "#a8acb0", cloth: "gx-jc", front: { kind: "cloth", cls: "gx-jc", h: 70 },
      logo: { cls: "lg-jc", pos: "ul" }, panel: "pn-jc", ink: "#eef0ef", knob: "silvercap", ring: { labels: NUM(0, 10), ink: "#e7e9e6" },
      order: [["B", "Bright"], ["V", "Volume"], ["T", "Treble"], ["M", "Middle"], ["a", "Bass"], ["R", "Speed"], ["d", "Depth"], ["em", "Vib · Chorus"]],
      fx: ["R", "d", "em"],
      left: ["jacks2:high|low"], right: ["badge", "jewel:red", "pushpower"] },
    /* ---- Ampeg SVT 1969 "blue line": silver panel with blue engraving on top, silver face ---- */
    54000: { brand: "Ampeg", fam: "svt blueline", layout: "combo", head: true, tolex: "#151515", pipe: "#c4c8cc", cloth: "gx-svtsilver", front: { kind: "cloth", cls: "gx-svtsilver", h: 76 },
      logo: { cls: "lg-ascript blue", pos: "ul" }, panel: "pn-blueline", ink: "#1d3e8a", knob: "silvercap", scale: "ticks",
      order: [["G", "Gain"], ["D", "Drive"], ["T", "Treble"], ["M", "Midrange"], ["mF", "Frequency"], ["B", "Bass"], ["V", "Master"], ["eQv", "EQ Level"]],
      svc: [["G4M", "Mode"]],
      left: ["jacks4:normal|bright"], right: ["rockers", "jewel:red"] },
    /* ---- Ampeg SVT-CL: dark panel with silver trim, script plate lower-left on black grille ---- */
    177000: { brand: "Ampeg", fam: "svt classic", layout: "combo", head: true, tolex: "#141414", pipe: "#b9bdc1", cloth: "gx-svtblack", front: { kind: "cloth", cls: "gx-svtblack", h: 76 },
      logo: { cls: "lg-ascript", pos: "ll" }, panel: "pn-svtcl", ink: "#e9ebee", knob: "silvercap", scale: "ticks",
      order: [["gain", "Gain"], ["ultra high", "Ultra Hi"], ["ultra low", "Ultra Lo"], ["bass", "Bass"], ["midrange", "Midrange"], ["frequency", "Frequency"], ["treble", "Treble"], ["volume", "Master"], ["output selector", "Output"]],
      left: ["jacks2:0 dB|-15 dB"], right: ["led:red", "rockerbox"] },
    /* ---- GK 800RB: black rack-style panel, grey cap discs, red LEDs, vent strip ---- */
    150000: { brand: "GK", fam: "gk", layout: "head", tolex: "#151515", pipe: "#2a2a2a", cloth: "gx-blackmesh", front: { kind: "vent", h: 40 },
      logo: { cls: "lg-gk" }, panel: "pn-gk", ink: "#f0f0f0", knob: "gk", scale: "ticks",
      order: [["volume", "Volume"], ["loCut", "Lo Cut"], ["midContour", "Contour"], ["treble", "Treble"], ["hiMid", "Hi Mid"], ["loMid", "Lo Mid"], ["bass", "Bass"], ["boost", "Boost"], ["master", "Master"]],
      left: ["jacks1:input", "series:Bass 800"], right: ["led:red", "pushpower"], logoOnPanel: true },
  };

  /* ---------- injected generated CSS (printed scales, Fender skirts) ---------- */
  (function injectCSS() {
    let css = `.k-fendersk .kn-rot { background: ${skirtSVG("#f1f1f1")} center / 100% 100% no-repeat; }\n`;
    Object.entries(SPECS).forEach(([tid, sp]) => {
      if (sp.ring) css += `.amp.af[data-af="${tid}"] .af-ctls .kn::before { background-image: ${ringSVG(sp.ring.labels.map(String), sp.ring.ink)}; }\n`;
    });
    css += `.af-pic { background-repeat: no-repeat; background-position: center; background-size: contain; }\n`;
    Object.entries(PIC).forEach(([k, v]) => { css += `.af-pic-${k} { background-image: ${svgURI(v)}; }\n`; });
    const st = document.createElement("style");
    st.id = "amp-faces-gen";
    st.textContent = css;
    (document.head || document.documentElement).appendChild(st);
  })();

  /* ---------- describe hook: what the matched cabinet inherits ---------- */
  function specFor(s) { return SPECS[s && s.table_id] || null; }
  function describe(slot, d) {
    const sp = specFor(slot);
    if (!sp) return d;
    return { ...d, form: "amp", img: null, tolexImg: null, combo: sp.layout !== "head" && !sp.head, tolex: sp.tolex, pipe: sp.pipe, grille: sp.cloth, cabGrille: sp.cloth,
      panel: d.panel || "transparent", ink: sp.ink, knob: sp.knob, af: true };
  }

  /* ---------- small hardware bits ---------- */
  const jewel = (c, isPower, s) => isPower
    ? `<button class="jewel af-jewel jw-${c}" data-act="bypass" title="Pilot lamp — click to bypass" aria-label="Power ${esc(s.reference_name)}"></button>`
    : `<span class="jewel af-jewel jw-${c}" title="Pilot lamp"></span>`;
  const powerBtn = (s, lbl = "On") => `<div class="pwr"><button class="toggle-sw" data-act="bypass" title="Power: bypass" aria-label="Power ${esc(s.reference_name)}"></button><span class="pwr-lbl">${esc(lbl)}</span></div>`;
  function jacks(n, lbls) {
    const L = (lbls || "").split("|").filter(Boolean);
    if (n === 1) return `<div class="af-jacks j1"><span class="ajack"></span><span class="af-jl">${esc(L[0] || "Input")}</span></div>`;
    if (n === 2) return `<div class="af-jacks j2">${[0, 1].map((k) => `<div class="af-jc"><span class="ajack"></span><span class="af-jl">${esc(L[k] || String(k + 1))}</span></div>`).join("")}</div>`;
    // pairs: one column per channel, two jacks stacked
    const cols = n / 2;
    let h = "";
    for (let c = 0; c < cols; c++) h += `<div class="af-jc"><span class="ajack"></span><span class="ajack"></span><span class="af-jl">${esc(L[c] || ["I", "II", "III"][c])}</span></div>`;
    return `<div class="af-jacks jp">${h}</div>`;
  }
  function token(t, s, sp) {
    const [k, a, b] = t.split(":");
    if (k === "model") return `<div class="af-model">${esc(s.display_name)}</div>`;
    if (k === "series") return `<div class="af-series">${esc(a)}</div>`;
    if (k === "script") return `<div class="af-pscript">${esc(s.reference_name)}</div>`;
    if (k === "ampscript") return `<div class="af-ampscript">${esc(s.reference_name)}<span>-Amp</span></div>`;
    if (k === "badge") return `<div class="af-badge">${esc(s.reference_name)}<b>120</b></div>`;
    if (k === "jewel") return jewel(a, b === "power", s);
    if (k === "led") return `<span class="led af-led led-${a}"></span>`;
    if (k === "leds") return `<div class="af-leds"><span><i class="led"></i>Standby</span><span><i class="led"></i>Power</span></div>`;
    if (k === "toggles") return `<div class="af-togs"><div class="pwr"><span class="toggle-sw af-dummy" aria-hidden="true"></span><span class="pwr-lbl">Standby</span></div>${powerBtn(s, "Power")}</div>`;
    if (k === "toggles1") return powerBtn(s, "Power");
    if (k === "pushpower") return `<div class="pwr"><button class="af-push" data-act="bypass" title="Power: bypass" aria-label="Power ${esc(s.reference_name)}"></button><span class="pwr-lbl">Power</span></div>`;
    if (k === "rockers") return `<div class="af-rockers"><div class="af-rk"><button class="af-rocker mains" data-act="bypass" title="Mains: bypass" aria-label="Power ${esc(s.reference_name)}"></button><span>Mains</span></div><div class="af-rk"><span class="af-rocker sb" aria-hidden="true"></span><span>Standby</span></div></div>`;
    if (k === "rockerbox") return `<div class="af-rockerbox"><div class="af-rk"><span class="af-rocker sb" aria-hidden="true"></span><span>Standby</span></div><div class="af-rk"><button class="af-rocker mains" data-act="bypass" title="Power: bypass" aria-label="Power ${esc(s.reference_name)}"></button><span>Power</span></div><div class="af-rbname">${esc(s.reference_name)}</div></div>`;
    const m = /^jacks(\d)$/.exec(k);
    if (m) return jacks(+m[1], a);
    return "";
  }

  /* ---------- control layout ---------- */
  // Lanes (compact): the real panel's own controls first, in panel order, rather than the engine's ranking.
  function compactFace(s, sp) {
    const all = panelOnly(s).filter(isVisibleParam).filter((p) => !(hasLabels(p) && p.labels.length > 12));
    const rank = new Map();
    (sp.order || []).forEach(([id], k) => rank.set(id, k));
    (sp.svc || []).forEach(([id], k) => rank.set(id, 1000 + k));
    const r = (p) => (rank.has(p.id) ? rank.get(p.id) : 500 + all.indexOf(p));
    const pick = all.slice().sort((a, b) => r(a) - r(b)).slice(0, 6);
    return all.filter((p) => pick.includes(p)).sort((a, b) => r(a) - r(b));
  }
  function splitControls(s, sp) {
    const face = RENDER.compact ? compactFace(s, sp) : faceParams(s);
    const byId = new Map(face.map((p) => [p.id, p]));
    const used = new Set();
    const pick = (list) => (list || []).map(([id, lbl]) => { const p = byId.get(id); if (!p || used.has(id)) return null; used.add(id); return { p, lbl }; }).filter(Boolean);
    const main = pick(sp.order);
    const svc = pick(sp.svc);
    face.forEach((p) => { if (!used.has(p.id)) main.push({ p, lbl: p.name }); });
    return { main, svc };
  }
  function ctl(s, sp, c, size, extraCls) {
    const pic = typeof c.lbl === "string" && c.lbl[0] === "@" ? c.lbl.slice(1) : null;
    let h = ctlHTML(s, c.p, { knob: sp.knob, size, scale: sp.ring ? "none" : (sp.scale || "dots"), label: pic ? `AFPIC${pic}AFPIC` : c.lbl, cls: extraCls || "" });
    if (pic) h = h.replace(`AFPIC${pic}AFPIC`, `<i class="af-pic af-pic-${pic}" title="${esc(c.p.name)}"></i><em>${esc(c.p.name)}</em>`);
    return h;
  }

  function ampHTML(s, d, i) {
    const sp = specFor(s);
    if (!sp) return null;
    const C = RENDER.compact;
    const { main, svc } = splitControls(s, sp);
    const nKnobs = main.filter((c) => !isTogParam(c.p)).length;
    const size = C ? 34 : nKnobs > 7 ? 38 : 42;
    const fxSet = new Set(sp.fx || []);
    let ctls = "";
    if (C) ctls = main.concat(svc).map((c) => ctl(s, sp, c, size)).join("");
    else {
      const a = main.filter((c) => !fxSet.has(c.p.id)), fx = main.filter((c) => fxSet.has(c.p.id));
      ctls = a.map((c) => ctl(s, sp, c, size)).join("");
      if (fx.length) ctls += `<div class="af-fx"><span class="af-cap">Effect</span><div class="af-row">${fx.map((c) => ctl(s, sp, c, size)).join("")}</div></div>`;
      if (svc.length) ctls += `<div class="af-svc" title="Engine voicing controls (not on the original panel)"><span class="af-cap">${esc(sp.svcCap || "Voicing")}</span><div class="af-row">${svc.map((c) => ctl(s, sp, c, 28, "small")).join("")}</div></div>`;
    }
    const name = esc(s.reference_name);
    const brand = esc(sp.brand || s.reference_name);
    const logoCls = `af-logo ${sp.logo.cls}${sp.logo.pos ? " pos-" + sp.logo.pos : ""}`;
    const logoHTML = `<div class="${logoCls}"><span>${brand}</span></div>`;

    const left = C ? `<div class="af-mini ${sp.logo.cls}"><span>${name}</span></div>${moreBtn(s)}`
      : `${(sp.left || []).map((t) => token(t, s, sp)).join("")}${sp.logoOnPanel ? `<div class="af-logo ${sp.logo.cls}"><span>${brand}</span></div>` : ""}${moreBtn(s)}`;
    const rightTokens = C ? (sp.right || []).filter((t) => /^(jewel|led|toggles1|pushpower)/.test(t)) : (sp.right || []);
    let right = rightTokens.map((t) => token(t, s, sp)).join("");
    if (!/data-act="bypass"/.test(left + right)) right += powerBtn(s, "On");
    const panel = `<div class="af-panel ${sp.panel}" style="--ink:${sp.ink}">
        <div class="af-left">${left}</div>
        <div class="af-ctls">${ctls}</div>
        <div class="af-right">${right}</div>
      </div>`;

    const f = sp.front;
    let front = "";
    if (!C) {
      if (f.kind === "vinyl") front = `<div class="af-front vinyl" style="--fh:${f.h}px">${logoHTML}</div>`;
      else if (f.kind.startsWith("diamond")) front = `<div class="af-front ${f.kind}" style="--fh:${f.h}px"><div class="af-vent">${logoHTML}</div></div>`;
      else if (f.kind === "vent") front = `<div class="af-front vent" style="--fh:${f.h}px"></div>`;
      else front = `<div class="af-front cloth ${f.kind === "metal" ? "metal " : ""}${f.cls}" style="--fh:${f.h}px">${logoHTML}</div>`;
    }
    let body;
    if (C) body = panel;
    else if (sp.layout === "combo") body = panel + `<div class="af-gap"></div>` + front;
    else if (sp.layout === "vox") body = `<div class="af-band">${logoHTML}</div><div class="af-strip"></div>${front.replace(logoHTML, "")}<div class="af-gap"></div>${panel}`;
    else body = front + `<div class="af-gap"></div>` + panel;

    const fam = sp.fam.split(" ").map((x) => "af-" + x).join(" ");
    const vars = `--i:${i};--tolex:${sp.tolex};--ink:${sp.ink};--pipe:${sp.pipe}`;
    const handle = C ? "" : `<div class="amp-handle af-handle"></div>`;
    return `<div class="${unitCls(s, `amp af ${fam}${sp.ring ? " af-ring" : ""}${sp.layout !== "head" && !sp.head ? " combo" : ""}${C ? " compact" : ""}`)}" data-unit="${s.slot_idx}" data-af="${s.table_id}" style="${vars}">
    ${handle}
    <div class="hw-body">${body}</div>${drawerHTML(s)}${XBTN}</div>`;
  }

  window.HRAmpFaces = { describe, ampHTML, specs: SPECS };
})();
